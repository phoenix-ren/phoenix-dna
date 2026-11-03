# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Offline tests of the synthesis engines' plumbing (no network, no audio).

The MiniMax engine is exercised with a faked ``urlopen`` and a decoder
stub; the local engine with a stub venv whose "interpreter" speaks the
worker protocol from the stdlib. What is verified is the wiring — SSE
hex-decoding, the request body's shape, the format fallback, the framed
protocol and its stop-drain — not any vendor's behaviour.
"""

from __future__ import annotations

import json
import stat
import textwrap
from pathlib import Path

import pytest
import urllib.request

import phoenix_dna.speech.minimax_engine as minimax_engine


# -- minimax --------------------------------------------------------------------


class FakeResponse:
    """The SSE body the t2a_v2 stream would send."""

    def __init__(self, frames, status_code=0):
        payload = b"".join(
            b"data: "
            + json.dumps(
                {
                    "data": {"audio": frame.hex(), "status": 2},
                    "base_resp": {"status_code": status_code},
                }
            ).encode()
            + b"\n\n"
            for frame in frames
        )
        self._lines = iter(payload.splitlines(keepends=True))

    def __iter__(self):
        return self._lines

    def close(self):
        return None


def _passthrough_decoder(tmp_path: Path) -> Path:
    """A stand-in ffmpeg: copies stdin to stdout whatever the args."""
    script = tmp_path / "fakeffmpeg"
    script.write_text("#!/bin/sh\nexec cat\n")
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return script


def test_minimax_streams_sse_audio_as_pcm(monkeypatch, tmp_path):
    opened = {}

    def fake_urlopen(request, timeout=0):
        opened["body"] = json.loads(request.data)
        opened["headers"] = request.headers
        return FakeResponse([b"\x01\x00" * 100, b"\x02\x00" * 50])

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = minimax_engine.MinimaxEngine(
        api_key="k", ffmpeg_bin=str(_passthrough_decoder(tmp_path))
    )
    pcm = b"".join(engine.synthesize("one short sentence"))
    assert pcm == b"\x01\x00" * 100 + b"\x02\x00" * 50

    # The request body is the documented contract.
    body = opened["body"]
    assert body["model"] == "speech-2.8-turbo"
    assert body["stream"] is True
    assert body["language_boost"] == "auto"
    assert body["text_normalization_mode"] == "basic"
    assert body["voice_setting"]["voice_id"] == "English_expressive_narrator"
    assert body["audio_setting"]["format"] == "opus"
    assert body["audio_setting"]["sample_rate"] == 32000
    assert body["audio_setting"]["channel"] == 1
    assert opened["headers"]["Authorization"] == "Bearer k"
    engine.close()


def test_minimax_falls_back_to_mp3_when_opus_refused(monkeypatch, tmp_path):
    calls = []

    def fake_urlopen(request, timeout=0):
        body = json.loads(request.data)
        calls.append(body["audio_setting"]["format"])
        if body["audio_setting"]["format"] == "opus":
            import io

            raise urllib.error.HTTPError(
                "url",
                400,
                "bad request",
                hdrs=None,
                fp=io.BytesIO(b"unsupported audio format opus"),
            )
        return FakeResponse([b"\x03\x00" * 10])

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = minimax_engine.MinimaxEngine(
        api_key="k",
        ffmpeg_bin=str(_passthrough_decoder(tmp_path)),
    )
    pcm = b"".join(engine.synthesize("fallback sentence"))
    assert calls == ["opus", "mp3"]
    assert pcm == b"\x03\x00" * 10
    engine.close()


def test_minimax_speaks_service_errors(monkeypatch, tmp_path):
    def fake_urlopen(request, timeout=0):
        return FakeResponse([b"\x00"], status_code=1004)

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = minimax_engine.MinimaxEngine(
        api_key="k", ffmpeg_bin=str(_passthrough_decoder(tmp_path))
    )
    with pytest.raises(minimax_engine.SynthesisError, match="1004"):
        list(engine.synthesize("too long"))
    engine.close()


# -- local (stub venv) ------------------------------------------------------------

_STUB_PROTOCOL = textwrap.dedent(
    """
    import json, struct, sys
    sys.stdout.write(json.dumps({"engine": "stub", "sample_rate": 24000,
                                 "voices": ["af_heart"]}) + "\\n")
    sys.stdout.flush()
    for line in sys.stdin:
        req = json.loads(line)
        if req.get("op") == "synthesize":
            pcm = ((req.get("text") or "") + "|").encode() * 40
            while pcm:
                frame, pcm = pcm[:4096], pcm[4096:]
                sys.stdout.buffer.write(struct.pack("<I", len(frame)) + frame)
            sys.stdout.buffer.write(struct.pack("<I", 0))
            sys.stdout.buffer.flush()
    """
)


def _stub_venv(tmp_path: Path) -> Path:
    venv = tmp_path / "venv"
    (venv / "bin").mkdir(parents=True)
    (venv / "bin" / "protocol.py").write_text(_STUB_PROTOCOL)
    launcher = venv / "bin" / "python"
    launcher.write_text(
        f"#!/bin/sh\nexec /usr/bin/python3 {venv / 'bin' / 'protocol.py'}\n"
    )
    launcher.chmod(launcher.stat().st_mode | stat.S_IXUSR)
    return venv


def test_local_engine_round_trip(tmp_path):
    from phoenix_dna.speech.local_engine import LocalEngine

    engine = LocalEngine(
        venv_dir=_stub_venv(tmp_path), voice_model=Path("/nonexistent.onnx")
    )
    assert engine.voices() == ["af_heart"]
    expected = len("hello stub|".encode()) * 40
    assert sum(len(c) for c in engine.synthesize("hello stub")) == expected
    assert engine.sample_rate == 24000

    # A stop mid-stream must not desync the next request: the engine
    # drains the abandoned request's frames before reading on.
    stream = engine.synthesize("second sentence")
    next(stream)
    engine.stop()
    assert list(stream) == []
    third = len("third|".encode()) * 40
    assert sum(len(c) for c in engine.synthesize("third")) == third
    engine.close()


def test_local_engine_without_venv_is_helpful(tmp_path):
    from phoenix_dna.speech.local_engine import LocalEngine, LocalEngineError

    engine = LocalEngine(venv_dir=tmp_path / "missing")
    with pytest.raises(LocalEngineError, match="not installed"):
        list(engine.synthesize("anything"))

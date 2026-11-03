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
    """The SSE body the t2a_v2 stream would send.

    ``error_code`` makes the first line carry an in-band base_resp
    error (HTTP 200, the stream-shaped refusal the service actually
    uses); ``trailing_error`` serves audio segments *and then* an
    error — the mid-stream failure; ``plain_json`` serves a whole JSON
    error body with no SSE framing at all — content-type
    application/json, which is what "invalid params" looks like; and
    ``final_chunk`` selects the protocol shape of the audio: the real
    service feeds status-1 segments and finishes with a status-2 chunk
    carrying the *complete* clip (``complete``, the default), while
    ``segment`` serves a final chunk that is only the last segment.
    """

    def __init__(
        self,
        frames,
        status_code=0,
        plain_json=False,
        trailing_error=0,
        final_chunk="complete",
    ):
        if plain_json:
            payload = json.dumps(
                {"base_resp": {"status_code": status_code, "status_msg": "boom"}}
            ).encode()
        elif status_code:
            payload = (
                b"data: "
                + json.dumps(
                    {"base_resp": {"status_code": status_code, "status_msg": "nope"}}
                ).encode()
                + b"\n\n"
            )
        else:
            def line(data: dict) -> bytes:
                return b"data: " + json.dumps(
                    dict(data, base_resp={"status_code": 0})
                ).encode() + b"\n\n"

            payload = b""
            if final_chunk == "complete":
                # The real shape: segments first …
                for frame in frames:
                    payload += line(
                        {"data": {"audio": frame.hex(), "status": 1, "ced": ""}}
                    )
                # … then the complete clip once more.
                payload += line(
                    {
                        "data": {
                            "audio": b"".join(frames).hex(),
                            "status": 2,
                            "ced": "",
                        },
                        "extra_info": {"audio_size": sum(len(f) for f in frames)},
                    }
                )
            elif final_chunk == "segment":
                # A stream whose final chunk is only the last segment.
                for index, frame in enumerate(frames):
                    last = index == len(frames) - 1
                    payload += line(
                        {
                            "data": {
                                "audio": frame.hex(),
                                "status": 2 if last else 1,
                                "ced": "",
                            }
                        }
                    )
            else:
                # Segments with no final chunk at all — a stream that
                # dies mid-flight (pair it with trailing_error).
                for frame in frames:
                    payload += line(
                        {"data": {"audio": frame.hex(), "status": 1, "ced": ""}}
                    )
            if trailing_error:
                payload += (
                    b"data: "
                    + json.dumps(
                        {
                            "base_resp": {
                                "status_code": trailing_error,
                                "status_msg": "died mid-stream",
                            }
                        }
                    ).encode()
                    + b"\n\n"
                )
        self._lines = iter(
            [payload] if plain_json else payload.splitlines(keepends=True)
        )

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

    # The request body is the documented contract — with the voice
    # parameters as integers: the service's schema types them that way
    # and refuses a JSON 1.0 outright.
    body = opened["body"]
    assert body["model"] == "speech-2.8-turbo"
    assert body["stream"] is True
    assert body["language_boost"] == "auto"
    assert body["text_normalization_mode"] == "basic"
    assert body["voice_setting"]["voice_id"] == "English_expressive_narrator"
    for field in ("speed", "vol", "pitch"):
        assert body["voice_setting"][field] == int(body["voice_setting"][field])
    assert isinstance(body["voice_setting"]["speed"], int)
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


def test_minimax_falls_back_on_in_band_error_and_sticks(monkeypatch, tmp_path):
    """The live failure this was built on: opus refused with an opaque
    HTTP-200 in-band error, mp3 fine. The engine retries once (whole
    request, no audio lost), and remembers the format that worked."""
    calls = []

    def fake_urlopen(request, timeout=0):
        body = json.loads(request.data)
        fmt = body["audio_setting"]["format"]
        calls.append(fmt)
        if fmt == "opus":
            return FakeResponse([], status_code=1000)
        return FakeResponse([b"\x03\x00" * 10])

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = minimax_engine.MinimaxEngine(
        api_key="k", ffmpeg_bin=str(_passthrough_decoder(tmp_path))
    )
    assert b"".join(engine.synthesize("first")) == b"\x03\x00" * 10
    assert calls == ["opus", "mp3"]
    # The working format stuck: the next sentence skips the probe.
    assert b"".join(engine.synthesize("second")) == b"\x03\x00" * 10
    assert calls == ["opus", "mp3", "mp3"]
    engine.close()


def test_minimax_no_retry_after_partial_audio(monkeypatch, tmp_path):
    """A stream that dies after its segments never yielded them (the
    parser buffers until the clip is complete), so nothing was heard
    and one bounded retry is correct — then the failure is raised."""
    calls = []

    def fake_urlopen(request, timeout=0):
        calls.append(1)
        return FakeResponse(
            [b"\x01\x00" * 10], trailing_error=1000, final_chunk="none"
        )

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = minimax_engine.MinimaxEngine(
        api_key="k", ffmpeg_bin=str(_passthrough_decoder(tmp_path))
    )
    with pytest.raises(minimax_engine.SynthesisError, match="1000"):
        list(engine.synthesize("partial"))
    assert len(calls) == 2  # opus, then the single mp3 retry
    engine.close()


def test_minimax_speaks_the_clip_once_not_twice(monkeypatch, tmp_path):
    """The live bug this guards against: the stream feeds status-1
    segments and then a status-2 chunk carrying the complete clip. The
    engine must use the complete clip exactly once — feeding both made
    every sentence play twice."""
    opened = {}

    def fake_urlopen(request, timeout=0):
        opened["body"] = json.loads(request.data)
        return FakeResponse([b"\x01\x00" * 100, b"\x02\x00" * 50])

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = minimax_engine.MinimaxEngine(
        api_key="k",
        ffmpeg_bin=str(_passthrough_decoder(tmp_path)),
        audio_format="mp3",  # single attempt: the shape is the point
    )
    pcm = b"".join(engine.synthesize("say this once"))
    engine.close()
    assert pcm == b"\x01\x00" * 100 + b"\x02\x00" * 50  # once, complete


def test_minimax_concatenates_when_final_chunk_is_a_segment(monkeypatch, tmp_path):
    """A final chunk smaller than the accumulated segments is the last
    segment, not the whole clip — concatenate, nothing dropped."""

    def fake_urlopen(request, timeout=0):
        return FakeResponse(
            [b"\x01\x00" * 100, b"\x02\x00" * 50], final_chunk="segment"
        )

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = minimax_engine.MinimaxEngine(
        api_key="k",
        ffmpeg_bin=str(_passthrough_decoder(tmp_path)),
        audio_format="mp3",
    )
    pcm = b"".join(engine.synthesize("segments only"))
    engine.close()
    assert pcm == b"\x01\x00" * 100 + b"\x02\x00" * 50


def test_minimax_raises_on_plain_json_error_body(monkeypatch, tmp_path):
    """HTTP 200 with a whole JSON error body (no SSE framing) must not
    fail in silence — this shape is what "invalid params" looks like."""

    def fake_urlopen(request, timeout=0):
        return FakeResponse([], status_code=2013, plain_json=True)

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = minimax_engine.MinimaxEngine(
        api_key="k", ffmpeg_bin=str(_passthrough_decoder(tmp_path))
    )
    with pytest.raises(minimax_engine.SynthesisError, match="2013.*boom"):
        list(engine.synthesize("bad params"))
    engine.close()


def test_minimax_raises_when_stream_runs_dry(monkeypatch, tmp_path):
    """A stream that yields not one byte of audio is a silent failure —
    the worst kind; it must be heard."""

    def fake_urlopen(request, timeout=0):
        return FakeResponse([])

    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)
    engine = minimax_engine.MinimaxEngine(
        api_key="k",
        ffmpeg_bin=str(_passthrough_decoder(tmp_path)),
        audio_format="mp3",  # single attempt: the shape is the point
    )
    with pytest.raises(minimax_engine.SynthesisError, match="no audio"):
        list(engine.synthesize("empty"))
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

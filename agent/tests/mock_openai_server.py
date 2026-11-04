# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""A mock OpenAI-compatible server, used to check the whole path.

The real endpoints cannot be reached from a test, but the *shape* of
them can be: a local HTTP server that answers like OpenAI does — the
chat completions as an SSE stream, the speech endpoint as an audio
body — is enough to check that the agent sends what it should and
reads what comes back.

The server records every request it served, so a test can assert on
the wire format rather than on the internals.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def _silent_mp3() -> bytes:
    """A short silent MP3, made with ffmpeg.

    The speech engine decodes the answer with ffmpeg, so the mock has
    to serve something that is really an MP3 — a byte string would be
    rejected by the decoder and the test would prove nothing. When
    ffmpeg is not installed the bytes are empty and the caller skips.
    """
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return b""
    try:
        result = subprocess.run(
            [
                ffmpeg,
                "-hide_banner",
                "-loglevel",
                "error",
                "-f",
                "lavfi",
                "-i",
                "anullsrc=r=16000:cl=mono",
                "-t",
                "0.2",
                "-f",
                "mp3",
                "pipe:1",
            ],
            capture_output=True,
            timeout=15,
        )
    except (OSError, subprocess.SubprocessError):
        return b""
    return result.stdout


class _Handler(BaseHTTPRequestHandler):
    """Answers like the OpenAI API, and records what it was asked."""

    protocol_version = "HTTP/1.1"

    def log_message(self, *args):  # silence the default logging
        return None

    def _record(self, body: bytes) -> None:
        self.server.requests.append(  # type: ignore[attr-defined]
            {
                "path": self.path,
                "headers": {k.lower(): v for k, v in self.headers.items()},
                "body": body,
            }
        )

    def do_POST(self) -> None:  # noqa: N802 - the name is fixed
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        self._record(body)

        if self.path.endswith("/chat/completions"):
            self._chat(body)
        elif self.path.endswith("/audio/speech"):
            self._speech(body)
        elif self.path.endswith("/audio/transcriptions"):
            self._transcription(body)
        else:
            self.send_error(404)

    def _chat(self, body: bytes) -> None:
        """A streaming chat completion, in OpenAI's SSE shape."""
        chunks = [
            {
                "choices": [{"delta": {"content": "PHOENIX-"}, "index": 0}],
            },
            {
                "choices": [{"delta": {"content": "OK"}, "index": 0}],
            },
            {"choices": [{"delta": {}, "finish_reason": "stop", "index": 0}]},
        ]
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for chunk in chunks:
            self._write_chunk(f"data: {json.dumps(chunk)}\n\n".encode())
        self._write_chunk(b"data: [DONE]\n\n")
        self._write_chunk(b"")

    def _speech(self, body: bytes) -> None:
        """An audio body, as /audio/speech answers."""
        audio = self.server.audio_bytes  # type: ignore[attr-defined]
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Content-Length", str(len(audio)))
        self.end_headers()
        self.wfile.write(audio)

    def _transcription(self, body: bytes) -> None:
        """A transcription, as /audio/transcriptions answers."""
        payload = json.dumps({"text": "hello there", "duration": 1.5}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _write_chunk(self, data: bytes) -> None:
        """One chunk of a chunked body."""
        if data:
            self.wfile.write(f"{len(data):X}\r\n".encode())
            self.wfile.write(data)
            self.wfile.write(b"\r\n")
        else:
            self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()


class MockOpenAiServer:
    """A local server that behaves like the OpenAI API."""

    def __init__(self):
        self._server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self._server.requests = []  # type: ignore[attr-defined]
        # The audio the speech endpoint serves: a real MP3, so the
        # engine's decoder has something it can actually decode.
        self._server.audio_bytes = _silent_mp3()  # type: ignore[attr-defined]
        self._thread = threading.Thread(
            target=self._server.serve_forever, daemon=True
        )

    def __enter__(self) -> "MockOpenAiServer":
        self._thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self._server.shutdown()
        self._server.server_close()
        self._thread.join(timeout=2.0)

    @property
    def base_url(self) -> str:
        """The server's OpenAI-compatible base URL."""
        host, port = self._server.server_address[:2]
        return f"http://{host}:{port}/v1"

    @property
    def requests(self) -> list:
        """What the server was asked, in order."""
        return self._server.requests  # type: ignore[attr-defined]

    def json_body(self, index: int = 0) -> dict:
        """The parsed body of the n-th request."""
        return json.loads(self.requests[index]["body"])

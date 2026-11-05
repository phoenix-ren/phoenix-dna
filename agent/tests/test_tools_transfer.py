# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Transfer tool tests: a local HTTP server, a fake FTP and a fake
libtorrent stand in for the far ends, and the metering is recorded the
way the daemon would receive it."""

from __future__ import annotations

import http.server
import threading
import time
from pathlib import Path

import pytest

from phoenix_dna.tools import ToolError
from phoenix_dna.tools.transfer import TransferTool

PAYLOAD = bytes(range(256)) * 80  # 20 KiB, so a chunk boundary is crossed
SLOW_CHUNK = b"s" * 1024
SLOW_CHUNKS = 5


class RecordingNotifier:
    """Collects the (direction, done, total) reports of a transfer."""

    def __init__(self):
        self.calls = []
        self.fail = False

    def __call__(self, direction, done, total):
        if self.fail:
            raise RuntimeError("the UI is having a bad day")
        self.calls.append((direction, int(done), int(total)))


class Handler(http.server.BaseHTTPRequestHandler):
    """A small far end: a file, a slow drip, a lie about a length, and
    somewhere an upload can land."""

    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):  # noqa: A002 - http.server API
        pass

    def _send(self, code, body=b"", content_type="application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self):
        if self.path == "/file.bin":
            self._send(200, PAYLOAD)
        elif self.path == "/slow":
            # A drip: a chunk every 0.3 s, so cancellation and the
            # budget land mid-transfer, not before it starts.
            self.send_response(200)
            self.send_header("Content-Length", str(SLOW_CHUNKS * len(SLOW_CHUNK)))
            self.end_headers()
            for _ in range(SLOW_CHUNKS):
                time.sleep(0.3)
                self.wfile.write(SLOW_CHUNK)
        elif self.path == "/truncated":
            # Promises more than it sends: the tool must notice.
            body = PAYLOAD[:1024]
            self.send_response(200)
            self.send_header("Content-Length", str(len(PAYLOAD)))
            self.end_headers()
            self.wfile.write(body)
            self.close_connection = True
        else:
            self._send(404, b"no such thing", content_type="text/plain")

    def _receive(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        self.server.uploads.setdefault(self.command, []).append(
            (self.path, body)
        )
        self._send(200, b"stored")

    def do_PUT(self):
        self._receive()

    def do_POST(self):
        self._receive()


@pytest.fixture
def server():
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    httpd.uploads = {}
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    yield httpd
    httpd.shutdown()
    thread.join(timeout=5)


@pytest.fixture
def url(server):
    host, port = server.server_address
    return f"http://{host}:{port}"


def make_tool(home_dir, **kw):
    return TransferTool(home_dir, **kw)


# -- downloads -----------------------------------------------------------------


def test_download_lands_in_downloads_and_meters(home_dir, url):
    notifier = RecordingNotifier()
    tool = make_tool(home_dir, transfer_notifier=notifier)
    result = tool.execute({"url": f"{url}/file.bin"})
    dest = Path(result.details["dest"])
    assert dest == home_dir / "Downloads" / "file.bin"
    assert dest.read_bytes() == PAYLOAD
    # the last report is the whole truth, and the totals were known
    assert notifier.calls[-1] == ("download", len(PAYLOAD), len(PAYLOAD))
    dones = [done for _, done, _ in notifier.calls]
    assert dones == sorted(dones), "the count only ever grows"
    assert result.details["bytes"] == len(PAYLOAD)
    assert "downloaded" in result.output and "file.bin" in result.output
    # the partial file is gone: the download is the thing, or nothing
    assert not (home_dir / "Downloads" / "file.bin.part").exists()


def test_download_honours_a_named_dest(home_dir, url):
    tool = make_tool(home_dir)
    dest = home_dir / "somewhere" / "iso.img"
    result = tool.execute({"url": f"{url}/file.bin", "dest": str(dest)})
    assert dest.read_bytes() == PAYLOAD
    assert Path(result.details["dest"]) == dest


def test_download_into_a_directory_takes_the_url_name(home_dir, url):
    tool = make_tool(home_dir)
    target = home_dir / "pile"
    target.mkdir()
    tool.execute({"url": f"{url}/file.bin", "dest": str(target) + "/"})
    assert (target / "file.bin").read_bytes() == PAYLOAD


def test_download_404_reports_and_cleans_up(home_dir, url):
    tool = make_tool(home_dir)
    with pytest.raises(ToolError, match="HTTP 404"):
        tool.execute({"url": f"{url}/missing.iso", "dest": "missing.iso"})
    assert not (home_dir / "missing.iso").exists()
    assert not (home_dir / "missing.iso.part").exists()


def test_download_notices_a_truncated_body(home_dir, url):
    tool = make_tool(home_dir)
    with pytest.raises(ToolError, match="closed early"):
        tool.execute({"url": f"{url}/truncated", "dest": "short.bin"})
    assert not (home_dir / "short.bin").exists()
    assert not (home_dir / "short.bin.part").exists()


def test_download_is_cancelled_and_cleaned_up(home_dir, url):
    tool = make_tool(home_dir)
    cancel = threading.Event()
    threading.Timer(0.5, cancel.set).start()
    start = time.monotonic()
    with pytest.raises(ToolError, match="cancelled"):
        tool.execute(
            {"url": f"{url}/slow", "dest": "slow.bin", "timeout": 60},
            cancel=cancel,
        )
    assert time.monotonic() - start < 10, "cancellation stops the read, not the timeout"
    assert not (home_dir / "slow.bin.part").exists()


def test_download_times_out(home_dir, url):
    tool = make_tool(home_dir)
    with pytest.raises(ToolError, match="timed out"):
        tool.execute({"url": f"{url}/slow", "dest": "slow.bin", "timeout": 1})
    assert not (home_dir / "slow.bin.part").exists()


def test_broken_notifier_does_not_break_the_transfer(home_dir, url):
    notifier = RecordingNotifier()
    notifier.fail = True
    tool = make_tool(home_dir, transfer_notifier=notifier)
    result = tool.execute({"url": f"{url}/file.bin", "dest": "fine.bin"})
    assert Path(result.details["dest"]).read_bytes() == PAYLOAD


# -- uploads ---------------------------------------------------------------------


def test_upload_put_arrives_and_meters(home_dir, url, server):
    source = home_dir / "report.tar"
    source.write_bytes(PAYLOAD)
    notifier = RecordingNotifier()
    tool = make_tool(home_dir, transfer_notifier=notifier)
    result = tool.execute(
        {"url": f"{url}/up/report.tar", "source": "report.tar"}
    )
    assert server.uploads["PUT"] == [("/up/report.tar", PAYLOAD)]
    assert notifier.calls[-1] == ("upload", len(PAYLOAD), len(PAYLOAD))
    assert result.details["direction"] == "upload"
    assert "uploaded" in result.output


def test_upload_post_is_honoured(home_dir, url, server):
    source = home_dir / "report.tar"
    source.write_bytes(PAYLOAD)
    tool = make_tool(home_dir)
    result = tool.execute(
        {
            "url": f"{url}/up/report.tar",
            "source": str(source),
            "method": "POST",
        }
    )
    assert server.uploads["POST"] == [("/up/report.tar", PAYLOAD)]
    assert result.details["direction"] == "upload"


def test_upload_of_a_missing_file_is_an_error(home_dir, url):
    tool = make_tool(home_dir)
    with pytest.raises(ToolError, match="not a file"):
        tool.execute({"url": f"{url}/up/ghost", "source": "ghost.bin"})


def test_upload_rejects_a_dest_argument(home_dir, url):
    source = home_dir / "report.tar"
    source.write_bytes(PAYLOAD)
    tool = make_tool(home_dir)
    with pytest.raises(ToolError, match="dest is for downloads"):
        tool.execute(
            {
                "url": f"{url}/up/report.tar",
                "source": "report.tar",
                "dest": "nowhere",
            }
        )


# -- FTP (a fake server stands in) -------------------------------------------------


class FakeFTP:
    """The ftplib.FTP surface the tool uses, with canned data."""

    payload = PAYLOAD

    def __init__(self):
        self.stored = b""
        self.aborted = False

    def connect(self, host, port, timeout=None):
        return "220 ready"

    def login(self, user, password):
        return "230 logged in"

    def size(self, path):
        return len(self.payload)

    def retrbinary(self, cmd, callback, blocksize=8192):
        for i in range(0, len(self.payload), blocksize):
            callback(self.payload[i : i + blocksize])

    def storbinary(self, cmd, fp, blocksize=8192, callback=None):
        while True:
            block = fp.read(blocksize)
            if not block:
                break
            if callback:
                callback(block)
            self.stored += block

    def quit(self):
        return "221 bye"

    def close(self):
        self.aborted = True


@pytest.fixture
def fake_ftp(monkeypatch):
    instances = []
    original = FakeFTP

    def factory():
        ftp = original()
        instances.append(ftp)
        return ftp

    monkeypatch.setattr("phoenix_dna.tools.transfer.ftplib.FTP", factory)
    return instances


def test_ftp_download(home_dir, fake_ftp):
    notifier = RecordingNotifier()
    tool = make_tool(home_dir, transfer_notifier=notifier)
    result = tool.execute(
        {"url": "ftp://127.0.0.1/pub/file.bin", "dest": "file.bin"}
    )
    assert (home_dir / "file.bin").read_bytes() == PAYLOAD
    assert notifier.calls[-1] == ("download", len(PAYLOAD), len(PAYLOAD))
    assert "file.bin" in result.output


def test_ftp_upload(home_dir, fake_ftp):
    source = home_dir / "report.tar"
    source.write_bytes(PAYLOAD)
    tool = make_tool(home_dir)
    tool.execute(
        {"url": "ftp://127.0.0.1/pub/report.tar", "source": "report.tar"}
    )
    assert fake_ftp[0].stored == PAYLOAD


# -- magnets (a fake libtorrent stands in) -------------------------------------------


class FakeTorrentStatus:
    def __init__(self, handle):
        self._handle = handle
        self.name = "Phoenix 26.04 Live"
        self.total_wanted = 3 * 1024 * 1024
        self.total_wanted_done = min(
            handle.polls * 1024 * 1024, self.total_wanted
        )
        self.is_finished = self.total_wanted_done >= self.total_wanted
        self.total_payload_download = self.total_wanted_done


class FakeTorrentHandle:
    def __init__(self):
        self.polls = 0

    def status(self):
        self.polls += 1
        return FakeTorrentStatus(self)

    def is_seed(self):
        return self.status().is_finished


class FakeTorrentSession:
    def __init__(self, settings=None):
        self.torrents = []

    def add_torrent(self, params):
        handle = FakeTorrentHandle()
        self.torrents.append(handle)
        return handle

    def pop_alerts(self):
        return []

    def remove_torrent(self, handle):
        self.torrents.remove(handle)


@pytest.fixture
def fake_libtorrent(monkeypatch):
    import sys
    import types

    module = types.ModuleType("libtorrent")
    module.session = FakeTorrentSession
    monkeypatch.setitem(sys.modules, "libtorrent", module)
    return module


def test_magnet_download_without_libtorrent_says_so(home_dir, monkeypatch):
    import sys

    monkeypatch.setitem(sys.modules, "libtorrent", None)  # import fails
    tool = make_tool(home_dir)
    with pytest.raises(ToolError, match="libtorrent"):
        tool.execute({"url": "magnet:?xt=urn:btih:deadbeef"})


def test_magnet_download_meters_and_lands(home_dir, fake_libtorrent):
    notifier = RecordingNotifier()
    tool = make_tool(home_dir, transfer_notifier=notifier)
    result = tool.execute(
        {"url": "magnet:?xt=urn:btih:deadbeef", "timeout": 30}
    )
    dest = Path(result.details["dest"])
    assert dest == home_dir / "Downloads" / "Phoenix 26.04 Live"
    assert "downloaded" in result.output and "Phoenix 26.04 Live" in result.output
    dones = [done for _, done, _ in notifier.calls]
    assert dones == sorted(dones) and dones[-1] == 3 * 1024 * 1024
    assert notifier.calls[-1][2] == 3 * 1024 * 1024, "the total was reported"


def test_magnet_download_is_cancelled(home_dir, fake_libtorrent):
    # The fake never finishes before its third poll; a cancel set
    # immediately must beat it.
    tool = make_tool(home_dir)
    cancel = threading.Event()
    cancel.set()
    with pytest.raises(ToolError, match="cancelled"):
        tool.execute({"url": "magnet:?xt=urn:btih:deadbeef"}, cancel=cancel)


# -- argument checking --------------------------------------------------------------


def test_unsupported_scheme_is_an_error(home_dir):
    tool = make_tool(home_dir)
    with pytest.raises(ToolError, match="unsupported url scheme"):
        tool.execute({"url": "gopher://example.com/file"})


def test_bad_method_is_an_error(home_dir, url):
    tool = make_tool(home_dir)
    (home_dir / "x").write_bytes(b"x")
    with pytest.raises(ToolError, match="PUT or POST"):
        tool.execute(
            {"url": f"{url}/up/x", "source": "x", "method": "DELETE"}
        )


def test_empty_url_is_an_error(home_dir):
    tool = make_tool(home_dir)
    with pytest.raises(ToolError, match="url"):
        tool.execute({"url": "  "})

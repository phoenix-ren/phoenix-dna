# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The transfer tool: downloads and uploads, metered.

A download through the bash tool (``wget``, ``curl``) has two problems:
the call times out on anything slower than its cap, and the workaround
the model reaches for — a detached command — moves the work somewhere
the UI cannot see it. An ISO that takes twenty minutes is not a bash
command; it is a transfer, and the user should watch it happen.

So this tool owns the moving of bytes:

* downloads from http/https/ftp URLs and torrent ``magnet:`` links,
* uploads of a local file to an http(s) URL (PUT or POST) or onto an
  FTP server (STOR),

and reports the bytes as they move through an optional
``transfer_notifier(direction, done, total)`` callback — the daemon
turns that into D-Bus signals, and the UI meters the transfer the same
way it meters a command's CPU (see :mod:`phoenix_dna.tools.shell`).

The whole transfer is orchestrated by asyncio (``execute`` runs an
event loop on the worker thread): the sockets themselves are pumped by
blocking reads on a daemon thread of their own, which the coroutine
supervises — cancellation, the overall budget and the metering cadence
live on the loop, one blocking read away from the metal. HTTP and FTP
come from the standard library (the daemon stays dependency-free;
libtorrent, for magnets, is optional and reported clearly when it is
missing).

A download lands beside its destination as ``<dest>.part`` and is
renamed into place only when it is complete, so an interrupted transfer
never passes for the finished thing.
"""

from __future__ import annotations

import asyncio
import ftplib
import mimetypes
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from ..http import headers as http_headers
from . import ToolError, ToolResult, ToolSpec, validate_required

#: Read/write block size of the pumps.
_CHUNK = 65536

#: How long a single socket read may block before the transfer is
#: considered stalled (a healthy connection says something — anything —
#: at least this often, if only by keeping quiet less).
_SOCKET_TIMEOUT = 90.0

#: The overall budget, unless the call names one: an hour covers an
#: ISO on a mediocre line. The cap (a day) keeps a forgotten transfer
#: from holding the run forever.
_DEFAULT_TIMEOUT = 3600.0
_MAX_TIMEOUT = 86400.0

#: How often the metering is reported, in seconds — often enough that
#: the bar moves, rarely enough that the report is not the traffic.
_NOTIFY_INTERVAL = 0.2

#: Where a download without a ``dest`` lands.
_DOWNLOADS_DIR = "Downloads"

#: The schemes a download can speak.
_HTTP_SCHEMES = ("http", "https")


class _Aborted(Exception):
    """Raised inside a pump the supervisor has told to stop."""


def _fmt_bytes(count: float) -> str:
    """A byte count as a person reads it ("4.4 GiB")."""
    count = float(count)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if count < 1024.0 or unit == "TiB":
            return f"{int(count)} B" if unit == "B" else f"{count:.1f} {unit}"
        count /= 1024.0
    return f"{count:.1f} TiB"  # pragma: no cover - the loop returns first


class _Progress:
    """The byte counter a pump and its supervisor share.

    The pump (a worker thread) counts what moves and reports it through
    the notifier at a sane cadence; the supervisor (the event loop)
    reads the counts and can abort the pump. The notifier is called
    with ``(direction, done, total)`` — ``done`` cumulative, ``total``
    zero when unknown — and its failures are swallowed: metering must
    never disturb the transfer it meters.
    """

    def __init__(self, notifier, direction: str):
        self._notifier = notifier
        self._direction = direction
        self._lock = threading.Lock()
        self._done = 0
        self._total = 0
        self._last_report = 0.0
        self._aborted = False
        self._abort_hooks: list = []

    # -- the pump's side (a worker thread) ---------------------------------

    def add(self, count: int) -> None:
        """Count bytes that moved; report them at the cadence."""
        with self._lock:
            self._done += int(count)
            if time.monotonic() - self._last_report >= _NOTIFY_INTERVAL:
                self._report_locked()

    def set_done(self, count: int) -> None:
        """Name the cumulative count (polling engines, not incremental)."""
        with self._lock:
            self._done = max(0, int(count))

    def set_total(self, total: int) -> None:
        with self._lock:
            self._total = max(0, int(total))
            if total > 0:
                # A total arriving is news in itself: the bar can turn
                # from indeterminate to honest at once.
                self._report_locked()

    def raise_if_aborted(self) -> None:
        if self._aborted:
            raise _Aborted()

    def on_abort(self, hook) -> None:
        """Register what abort() calls to unblock a stuck read."""
        self._abort_hooks.append(hook)

    # -- the supervisor's side (the event loop) ----------------------------

    def abort(self) -> None:
        """Stop the pump: the flag first, then whatever unblocks it."""
        self._aborted = True
        for hook in list(self._abort_hooks):
            try:
                hook()
            except Exception:  # noqa: BLE001 - aborting must never raise
                pass

    def counts(self) -> tuple[int, int]:
        with self._lock:
            return self._done, self._total

    def finish(self) -> None:
        """The last report: the exact totals, whatever the cadence."""
        with self._lock:
            self._report_locked()

    def _report_locked(self) -> None:
        self._last_report = time.monotonic()
        if self._notifier is None:
            return
        try:
            self._notifier(self._direction, self._done, self._total)
        except Exception:  # noqa: BLE001 - the UI must not break the tool
            pass


class _CountingFile:
    """A file wrapper that meters (and can abort) as the body is sent.

    http.client and ftplib read a body through this in blocks; every
    block that leaves is counted, so an upload is metered as honestly
    as a download.
    """

    def __init__(self, fp, progress: _Progress):
        self._fp = fp
        self._progress = progress

    def read(self, size=-1):
        self._progress.raise_if_aborted()
        chunk = self._fp.read(size)
        if chunk:
            self._progress.add(len(chunk))
        return chunk

    def close(self) -> None:
        self._fp.close()


class TransferTool:
    spec = ToolSpec(
        name="transfer",
        description=(
            "Download or upload a file over the network, with metered "
            "progress the user can watch while it runs. Downloads from "
            "http/https/ftp URLs and torrent magnet: links; uploads a "
            "local file to an http(s) URL (PUT or POST) or onto an FTP "
            "server. Use this tool for EVERY download and upload, however "
            "small - never curl, wget, ftp, aria2c or a torrent client "
            "through bash: their progress hides from the user, they can "
            "outlive the bash timeout, and large or slow items (ISO "
            "images, torrents) are exactly what this tool is for. The "
            "default budget is one hour; pass a larger timeout (up to "
            "86400) for very big or slow transfers. A download writes "
            "<dest>.part and only becomes <dest> when it is complete."
        ),
        parameters={
            "type": "object",
            "properties": {
                "url": {
                    "type": "string",
                    "description": (
                        "The address of the transfer: an http/https/ftp "
                        "URL to download from — or, together with "
                        "source, to upload to — or a torrent magnet: link"
                    ),
                },
                "dest": {
                    "type": "string",
                    "description": (
                        "Where a download lands: a file path, or a "
                        "directory (existing or with a trailing /) whose "
                        "name comes from the URL. Default: "
                        "~/Downloads/<file name from the URL>. For a "
                        "magnet link, the directory to save into"
                    ),
                },
                "source": {
                    "type": "string",
                    "description": (
                        "The local file to upload; naming it makes the "
                        "transfer an upload (url is then the destination)"
                    ),
                },
                "method": {
                    "type": "string",
                    "enum": ["PUT", "POST"],
                    "description": "The HTTP method of an upload (default PUT)",
                },
                "timeout": {
                    "type": "integer",
                    "description": (
                        "Overall budget in seconds (default 3600, "
                        "maximum 86400)"
                    ),
                    "minimum": 1,
                    "maximum": 86400,
                },
            },
            "required": ["url"],
        },
    )

    def __init__(
        self,
        cwd: Path,
        *,
        transfer_notifier=None,
        default_timeout: float = _DEFAULT_TIMEOUT,
        max_timeout: float = _MAX_TIMEOUT,
    ):
        self.cwd = Path(cwd)
        # Optional metering for the UI: called with
        # (direction, done, total) while the bytes move. Failures in the
        # notifier are swallowed (see _Progress): metering must never
        # disturb the transfer it meters.
        self.transfer_notifier = transfer_notifier
        self.default_timeout = default_timeout
        self.max_timeout = max_timeout

    # -- public API -----------------------------------------------------------

    def execute(
        self, args: dict, *, cancel: threading.Event | None = None
    ) -> ToolResult:
        validate_required(self.spec, args)
        url = str(args.get("url") or "").strip()
        if not url:
            raise ToolError("url must be a non-empty string")
        source = str(args.get("source") or "").strip()
        dest = str(args.get("dest") or "").strip()
        method = str(args.get("method") or "PUT").strip().upper()
        if method not in ("PUT", "POST"):
            raise ToolError(f"method must be PUT or POST, not {method!r}")
        try:
            timeout = float(args.get("timeout") or self.default_timeout)
        except (TypeError, ValueError):
            timeout = self.default_timeout
        timeout = max(1.0, min(timeout, self.max_timeout))

        started = time.monotonic()
        if source:
            result = asyncio.run(
                self._upload(url, source, dest, method, timeout, cancel)
            )
            direction = "upload"
        elif url.lower().startswith("magnet:"):
            result = asyncio.run(self._magnet(url, dest, timeout, cancel))
            direction = "download"
        else:
            scheme = urllib.parse.urlsplit(url).scheme.lower()
            if scheme in _HTTP_SCHEMES:
                result = asyncio.run(self._download_http(url, dest, timeout, cancel))
            elif scheme == "ftp":
                result = asyncio.run(self._download_ftp(url, dest, timeout, cancel))
            else:
                raise ToolError(
                    f"unsupported url scheme {scheme!r}: transfer speaks "
                    "http, https, ftp and magnet: links"
                )
            direction = "download"

        elapsed = max(0.001, time.monotonic() - started)
        done, total = result["done"], result["total"]
        output = self._compose_output(direction, result, done, total, elapsed)
        return ToolResult(
            output=output,
            details={
                "direction": direction,
                "url": url,
                "dest": result["dest"],
                "source": result.get("source", ""),
                "bytes": done,
                "total": total,
                "elapsed_seconds": round(elapsed, 1),
            },
        )

    # -- composition ------------------------------------------------------------

    @staticmethod
    def _compose_output(
        direction: str, result: dict, done: int, total: int, elapsed: float
    ) -> str:
        verb = "uploaded" if direction == "upload" else "downloaded"
        size = _fmt_bytes(done)
        if total > 0:
            size += f" of {_fmt_bytes(total)} ({min(100, done * 100 // total)}%)"
        name = result.get("name")
        if name:
            size += f" ({name})"
        rate = _fmt_bytes(done / elapsed) + "/s"
        target = result.get("url") if direction == "upload" else result["dest"]
        action = "to" if direction == "upload" else "into"
        return f"{verb} {size} in {elapsed:.0f}s at {rate} {action} {target}"

    # -- the shared supervision ---------------------------------------------------

    async def _pump_in_thread(self, pump, direction: str, timeout: float, cancel):
        """Run one blocking ``pump(progress)`` in a thread, supervised.

        The loop owns what the pump cannot see from inside a blocking
        read: cancellation, the overall budget and the final metering
        report. The pump is unblocked by ``progress.abort()`` — its
        socket is closed under it — so a cancelled transfer stops at
        the next read, not at the next timeout.
        """
        if cancel is not None and cancel.is_set():
            raise ToolError("transfer cancelled")
        progress = _Progress(self.transfer_notifier, direction)
        loop = asyncio.get_running_loop()
        finished = asyncio.Event()
        outcome: dict = {}

        def runner() -> None:
            try:
                outcome["result"] = pump(progress)
            except BaseException as exc:  # noqa: BLE001 - handed back below
                outcome["error"] = exc
            finally:
                loop.call_soon_threadsafe(finished.set)

        threading.Thread(target=runner, name="phoenix-transfer", daemon=True).start()

        deadline = time.monotonic() + timeout
        while not finished.is_set():
            try:
                await asyncio.wait_for(finished.wait(), timeout=0.2)
            except asyncio.TimeoutError:
                pass
            if finished.is_set():
                break
            if cancel is not None and cancel.is_set():
                progress.abort()
                await self._await_end(finished)
                raise ToolError("transfer cancelled") from None
            if time.monotonic() >= deadline:
                progress.abort()
                await self._await_end(finished)
                raise ToolError(f"transfer timed out after {timeout:.0f}s") from None

        progress.finish()
        if "error" in outcome:
            raise outcome["error"]
        return outcome["result"], progress

    @staticmethod
    async def _await_end(finished: asyncio.Event) -> None:
        """Wait (bounded) for an aborted pump to unwind; the socket
        close does the waking, the bound is only for a pump that
        refuses to die."""
        try:
            await asyncio.wait_for(finished.wait(), timeout=15.0)
        except asyncio.TimeoutError:  # pragma: no cover - defensive
            pass

    # -- HTTP downloads ------------------------------------------------------------

    async def _download_http(self, url: str, dest: str, timeout: float, cancel):
        target = self._dest_path(dest, url)
        part = target.with_name(target.name + ".part")
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            result, progress = await self._pump_in_thread(
                self._http_download_pump(url, part, target),
                "download",
                timeout,
                cancel,
            )
        except BaseException:
            part.unlink(missing_ok=True)  # a partial file must not pass for it
            raise
        done, total = progress.counts()
        return {"dest": str(result), "done": done, "total": total}

    def _http_download_pump(self, url: str, part: Path, target: Path):
        def pump(progress: _Progress) -> Path:
            request = urllib.request.Request(url, headers=http_headers())
            try:
                response = urllib.request.urlopen(
                    request, timeout=_SOCKET_TIMEOUT
                )
            except urllib.error.HTTPError as exc:
                raise ToolError(f"download failed: HTTP {exc.code} {exc.reason}") from exc
            except (urllib.error.URLError, OSError, ValueError) as exc:
                raise ToolError(f"download failed: {exc}") from exc
            with response:
                # Closing the response is what unblocks a cancelled
                # read: the socket goes away under it.
                progress.on_abort(response.close)
                progress.set_total(int(response.headers.get("Content-Length") or 0))
                done = 0
                try:
                    with open(part, "wb") as out:
                        while True:
                            progress.raise_if_aborted()
                            chunk = response.read(_CHUNK)
                            if not chunk:
                                break
                            out.write(chunk)
                            done += len(chunk)
                            progress.add(len(chunk))
                except (OSError, TimeoutError) as exc:
                    raise ToolError(f"download failed: {exc}") from exc
            if progress.counts()[1] and done < progress.counts()[1]:
                raise ToolError(
                    f"download failed: the connection closed early "
                    f"({done} of {progress.counts()[1]} bytes)"
                )
            os.replace(part, target)
            return target

        return pump

    # -- FTP downloads --------------------------------------------------------------

    def _ftp_parts(self, url: str) -> tuple[str, int, str, str, str]:
        """(host, port, user, password, path) of an ftp:// URL."""
        parts = urllib.parse.urlsplit(url)
        if not parts.hostname:
            raise ToolError(f"download failed: no host in {url}")
        return (
            parts.hostname,
            parts.port or 21,
            urllib.parse.unquote(parts.username or "anonymous"),
            urllib.parse.unquote(parts.password or "phoenix-dna@"),
            urllib.parse.unquote(parts.path.lstrip("/")) or ".",
        )

    async def _download_ftp(self, url: str, dest: str, timeout: float, cancel):
        target = self._dest_path(dest, url)
        part = target.with_name(target.name + ".part")
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            result, progress = await self._pump_in_thread(
                self._ftp_download_pump(url, part, target),
                "download",
                timeout,
                cancel,
            )
        except BaseException:
            part.unlink(missing_ok=True)
            raise
        done, total = progress.counts()
        return {"dest": str(result), "done": done, "total": total}

    def _ftp_download_pump(self, url: str, part: Path, target: Path):
        host, port, user, password, path = self._ftp_parts(url)

        def pump(progress: _Progress) -> Path:
            ftp = ftplib.FTP()
            try:
                ftp.connect(host, port, timeout=_SOCKET_TIMEOUT)
                ftp.login(user, password)
                progress.on_abort(ftp.close)
                try:
                    progress.set_total(int(ftp.size(path) or 0))
                except (ftplib.all_errors, TypeError, ValueError):
                    pass  # SIZE is a courtesy, not a promise
                with open(part, "wb") as out:

                    def receive(chunk: bytes) -> None:
                        progress.raise_if_aborted()
                        out.write(chunk)
                        progress.add(len(chunk))

                    ftp.retrbinary(f"RETR {path}", receive, blocksize=_CHUNK)
            except _Aborted:
                raise
            except ftplib.all_errors as exc:
                raise ToolError(f"download failed: ftp: {exc}") from exc
            except OSError as exc:
                raise ToolError(f"download failed: {exc}") from exc
            finally:
                try:
                    ftp.quit()
                except ftplib.all_errors:
                    try:
                        ftp.close()
                    except Exception:  # pragma: no cover - defensive
                        pass
            os.replace(part, target)
            return target

        return pump

    # -- magnet downloads -------------------------------------------------------------

    async def _magnet(self, magnet: str, dest: str, timeout: float, cancel):
        lt = self._load_libtorrent()
        save_dir = self._save_dir(dest)
        progress = _Progress(self.transfer_notifier, "download")
        deadline = time.monotonic() + timeout
        handle = None
        session = None
        name = ""
        try:
            session = lt.session(
                {
                    "listen_interfaces": "0.0.0.0:6881",
                    "alert_queue_size": 400,
                }
            )
            handle = session.add_torrent({"url": magnet, "save_path": str(save_dir)})
            while True:
                if cancel is not None and cancel.is_set():
                    raise ToolError("transfer cancelled") from None
                if time.monotonic() >= deadline:
                    raise ToolError(
                        f"transfer timed out after {timeout:.0f}s"
                    ) from None
                status = handle.status()
                name = str(getattr(status, "name", "") or name)
                progress.set_total(int(getattr(status, "total_wanted", 0) or 0))
                progress.set_done(int(getattr(status, "total_wanted_done", 0) or 0))
                session.pop_alerts()
                if bool(getattr(status, "is_finished", False)) or handle.is_seed():
                    break
                await asyncio.sleep(1.0)
        finally:
            if session is not None and handle is not None:
                try:
                    session.remove_torrent(handle)
                except Exception:  # noqa: BLE001 - cleaning up a dead session
                    pass
            progress.finish()
        done, total = progress.counts()
        # The torrent names its own files; a magnet cannot, before the
        # metadata arrives.
        target = str(save_dir / name) if name else str(save_dir)
        return {"dest": target, "done": done, "total": total, "name": name}

    @staticmethod
    def _load_libtorrent():
        try:
            import libtorrent  # noqa: PLC0415 - optional, by design
        except ImportError:
            raise ToolError(
                "torrent support needs libtorrent, which is not installed "
                "(dnf install python3-libtorrent, apt install "
                "python3-libtorrent or pacman -S python-libtorrent); fall "
                "back to a torrent client through bash for this one"
            ) from None
        return libtorrent

    # -- uploads ----------------------------------------------------------------------

    async def _upload(self, url: str, source: str, dest: str, method: str,
                      timeout: float, cancel):
        if dest:
            raise ToolError(
                "dest is for downloads; an upload's destination is its url"
            )
        path = Path(source).expanduser()
        if not path.is_absolute():
            path = self.cwd / path
        if not path.is_file():
            raise ToolError(f"cannot upload {source}: not a file")

        scheme = urllib.parse.urlsplit(url).scheme.lower()
        if scheme in _HTTP_SCHEMES:
            result, progress = await self._pump_in_thread(
                self._http_upload_pump(url, path, method), "upload", timeout, cancel
            )
        elif scheme == "ftp":
            result, progress = await self._pump_in_thread(
                self._ftp_upload_pump(url, path), "upload", timeout, cancel
            )
        else:
            raise ToolError(
                f"unsupported upload destination {scheme!r}: transfer "
                "uploads to http, https and ftp URLs"
            )
        done, total = progress.counts()
        return {
            "url": url,
            "source": str(path),
            "dest": result,
            "done": done,
            "total": total,
            "name": path.name,
        }

    def _http_upload_pump(self, url: str, path: Path, method: str):
        def pump(progress: _Progress) -> str:
            size = path.stat().st_size
            progress.set_total(size)
            mime, _ = mimetypes.guess_type(path.name)
            fp = path.open("rb")
            counting = _CountingFile(fp, progress)
            request = urllib.request.Request(
                url,
                data=counting,
                method=method,
                headers=http_headers(
                    {
                        "Content-Type": mime or "application/octet-stream",
                        "Content-Length": str(size),
                    }
                ),
            )
            try:
                with urllib.request.urlopen(
                    request, timeout=_SOCKET_TIMEOUT
                ) as response:
                    response.read(_CHUNK)  # the answer, drained
            except urllib.error.HTTPError as exc:
                raise ToolError(f"upload failed: HTTP {exc.code} {exc.reason}") from exc
            except (urllib.error.URLError, OSError, ValueError) as exc:
                raise ToolError(f"upload failed: {exc}") from exc
            finally:
                counting.close()
            return url

        return pump

    def _ftp_upload_pump(self, url: str, path: Path):
        host, port, user, password, remote = self._ftp_parts(url)

        def pump(progress: _Progress) -> str:
            size = path.stat().st_size
            progress.set_total(size)
            ftp = ftplib.FTP()
            try:
                ftp.connect(host, port, timeout=_SOCKET_TIMEOUT)
                ftp.login(user, password)
                progress.on_abort(ftp.close)
                with path.open("rb") as fp:

                    def sent(block: bytes) -> None:
                        progress.raise_if_aborted()
                        progress.add(len(block))

                    ftp.storbinary(
                        f"STOR {remote}", fp, blocksize=_CHUNK, callback=sent
                    )
            except _Aborted:
                raise
            except ftplib.all_errors as exc:
                raise ToolError(f"upload failed: ftp: {exc}") from exc
            except OSError as exc:
                raise ToolError(f"upload failed: {exc}") from exc
            finally:
                try:
                    ftp.quit()
                except ftplib.all_errors:
                    try:
                        ftp.close()
                    except Exception:  # pragma: no cover - defensive
                        pass
            return url

        return pump

    # -- destinations -----------------------------------------------------------------

    def _dest_path(self, dest: str, url: str) -> Path:
        """Where a URL download lands: the named path, or ~/Downloads."""
        if dest:
            path = Path(dest).expanduser()
            if not path.is_absolute():
                path = self.cwd / path
            if path.is_dir() or dest.endswith("/"):
                return path / _url_filename(url)
            return path
        downloads = self.cwd / _DOWNLOADS_DIR
        downloads.mkdir(parents=True, exist_ok=True)
        return downloads / _url_filename(url)

    def _save_dir(self, dest: str) -> Path:
        """The directory a magnet saves into (its files name themselves)."""
        if dest:
            path = Path(dest).expanduser()
            if not path.is_absolute():
                path = self.cwd / path
            path.mkdir(parents=True, exist_ok=True)
            return path
        downloads = self.cwd / _DOWNLOADS_DIR
        downloads.mkdir(parents=True, exist_ok=True)
        return downloads


def _url_filename(url: str) -> str:
    """The file name a download takes from its URL ("download" at worst)."""
    path = urllib.parse.urlsplit(url).path
    name = os.path.basename(urllib.parse.unquote(path))
    name = name.strip().lstrip(".")
    return name or "download"

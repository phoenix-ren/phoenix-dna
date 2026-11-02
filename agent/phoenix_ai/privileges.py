# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Root-mode privileges: KDE password dialog + sudo password caching.

Security properties (v1):
* the password lives only in process memory (never logged, never written
  to disk, ``__repr__`` is redacted);
* it is validated immediately via ``sudo -S -k -v`` before use;
* it is re-used for ``credential_ttl_seconds`` (default 900s, like sudo)
  so that multi-prompt root sessions only ask once;
* three failed attempts give up.
"""

from __future__ import annotations

import logging
import subprocess
import threading
import time
from typing import Callable, Protocol

log = logging.getLogger("phoenix.privileges")


class PasswordProvider(Protocol):
    def ask(self, message: str) -> str | None: ...


class KDialogPasswordProvider:
    """Asks the user for a password with the standard KDE dialog."""

    def __init__(self, kdialog_bin: str = "/usr/bin/kdialog", timeout: float = 180.0):
        self.kdialog_bin = kdialog_bin
        self.timeout = timeout

    def ask(self, message: str) -> str | None:
        argv = [
            self.kdialog_bin,
            "--password",
            message,
            "--title",
            "Phoenix AI",
        ]
        try:
            result = subprocess.run(
                argv, capture_output=True, text=True, timeout=self.timeout
            )
        except (subprocess.TimeoutExpired, FileNotFoundError, OSError) as exc:
            log.warning("kdialog failed: %s", exc)
            return None
        if result.returncode != 0:
            return None  # user pressed Cancel / Esc
        password = result.stdout.rstrip("\n")
        return password or None


class StaticPasswordProvider:
    """Scripted provider for tests: returns passwords one by one, then None."""

    def __init__(self, passwords):
        if isinstance(passwords, str):
            passwords = [passwords]
        self._passwords = list(passwords)
        self.calls = 0

    def ask(self, message: str) -> str | None:
        self.calls += 1
        if not self._passwords:
            return None
        return self._passwords.pop(0)


class AuthNotifier:
    """Callbacks the daemon wires up to emit D-Bus auth events."""

    def __init__(self):
        self.on_started: Callable[[], None] = lambda: None
        self.on_finished: Callable[[bool], None] = lambda ok: None


class SudoCredentialCache:
    """Obtains, validates and caches the sudo password for root mode."""

    def __init__(
        self,
        password_provider: PasswordProvider,
        *,
        sudo_bin: str = "/usr/bin/sudo",
        ttl_seconds: float = 900.0,
        clock: Callable[[], float] = time.monotonic,
        max_attempts: int = 3,
    ):
        self._provider = password_provider
        self._sudo_bin = sudo_bin
        self._ttl = ttl_seconds
        self._clock = clock
        self._max_attempts = max_attempts
        self._lock = threading.Lock()
        self._password: str | None = None
        self._cached_at: float = -1.0
        self.notifier = AuthNotifier()

    def __repr__(self) -> str:  # never leak the password in logs
        return (
            f"<SudoCredentialCache cached={self._password is not None} "
            f"ttl={self._ttl}s>"
        )

    def get_password(self) -> str | None:
        """Return a validated password, or None if the user gave up."""
        with self._lock:
            if (
                self._password is not None
                and (self._clock() - self._cached_at) < self._ttl
            ):
                return self._password
            self._password = None
            for _ in range(self._max_attempts):
                self.notifier.on_started()
                candidate = self._provider.ask(
                    "Phoenix AI needs administrator privileges " "to perform this task."
                )
                if not candidate:
                    self.notifier.on_finished(False)
                    return None
                if self._validate(candidate):
                    self._password = candidate
                    self._cached_at = self._clock()
                    self.notifier.on_finished(True)
                    return self._password
                self.notifier.on_finished(False)
            return None

    def invalidate(self) -> None:
        with self._lock:
            self._password = None
            self._cached_at = -1.0

    def _validate(self, password: str) -> bool:
        argv = [self._sudo_bin, "-S", "-p", "", "-k", "-v"]
        try:
            result = subprocess.run(
                argv,
                input=password + "\n",
                capture_output=True,
                text=True,
                timeout=30,
            )
        except (subprocess.TimeoutExpired, FileNotFoundError, OSError) as exc:
            log.warning("sudo validation failed: %s", exc)
            return False
        return result.returncode == 0

# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Sudo credential cache tests (validation, TTL, retries, notifications)."""

from __future__ import annotations

from phoenix_dna.privileges import SudoCredentialCache, StaticPasswordProvider


def make_cache(passwords, fake_sudo, **kw):
    provider = StaticPasswordProvider(passwords)
    cache = SudoCredentialCache(provider, sudo_bin=str(fake_sudo), **kw)
    return cache, provider


def test_valid_password_cached(fake_sudo):
    cache, provider = make_cache(["hunter2"], fake_sudo)
    assert cache.get_password() == "hunter2"
    # second call within the TTL does not re-ask
    assert cache.get_password() == "hunter2"
    assert provider.calls == 1


def test_wrong_password_then_success(fake_sudo):
    cache, provider = make_cache(["wrong", "hunter2"], fake_sudo)
    assert cache.get_password() == "hunter2"
    assert provider.calls == 2


def test_gives_up_after_three_attempts(fake_sudo):
    cache, provider = make_cache(["bad1", "bad2", "bad3", "hunter2"], fake_sudo)
    assert cache.get_password() is None
    assert provider.calls == 3


def test_user_cancel(fake_sudo):
    cache, provider = make_cache([], fake_sudo)  # ask() returns None
    assert cache.get_password() is None


def test_invalidate_forces_reask(fake_sudo):
    cache, provider = make_cache(["hunter2"], fake_sudo)
    cache.get_password()
    cache.invalidate()
    cache._provider = StaticPasswordProvider(["hunter2"])  # second ask source
    assert cache.get_password() == "hunter2"


def test_ttl_expiry_with_fake_clock(fake_sudo):
    now = [100.0]
    provider = StaticPasswordProvider(["hunter2"])
    second = StaticPasswordProvider(["hunter2"])
    cache = SudoCredentialCache(
        provider, sudo_bin=str(fake_sudo), ttl_seconds=900.0, clock=lambda: now[0]
    )
    assert cache.get_password() == "hunter2"
    now[0] += 899
    assert cache.get_password() == "hunter2"
    assert provider.calls == 1
    now[0] += 2  # past the TTL
    cache._provider = second
    assert cache.get_password() == "hunter2"
    assert second.calls == 1


def test_notifier_events(fake_sudo):
    provider = StaticPasswordProvider(["wrong", "hunter2"])
    cache = SudoCredentialCache(provider, sudo_bin=str(fake_sudo))
    started = []
    finished = []
    cache.notifier.on_started = lambda: started.append(1)
    cache.notifier.on_finished = lambda ok: finished.append(ok)

    assert cache.get_password() == "hunter2"
    assert len(started) == 2  # asked twice (bad, then good)
    assert finished == [False, True]


def test_missing_sudo_binary(fake_sudo):
    cache = SudoCredentialCache(
        StaticPasswordProvider(["hunter2", "hunter2", "hunter2"]),
        sudo_bin="/nonexistent/sudo",
    )
    assert cache.get_password() is None


def test_repr_never_leaks_password(fake_sudo):
    cache, _ = make_cache(["hunter2"], fake_sudo)
    cache.get_password()
    assert "hunter2" not in repr(cache)

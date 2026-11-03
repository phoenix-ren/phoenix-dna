# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The synthesis engine interface, and how config becomes an engine.

An engine turns one piece of text into mono 16-bit PCM (s16le) at its
declared :attr:`SynthesisEngine.sample_rate` — one uniform contract for
the HTTP engine and the local one, so the Speaker never knows which is
below it:

* ``synthesize(text)`` yields PCM chunks as they arrive (streaming: the
  player starts sounding before the whole clip is back);
* ``stop()`` abandons the in-flight synthesis (from any thread);
* ``close()`` releases what the engine holds.

The daemon is stdlib-only on this path; the optional local engine keeps
its heavy imports in a separate interpreter entirely.
"""

from __future__ import annotations

import logging
from typing import Iterator, Protocol

from ..config import Config

log = logging.getLogger("phoenix.speech.synth")


class SynthesisEngine(Protocol):
    """Anything that can turn text into PCM chunks."""

    #: The sample rate of the PCM ``synthesize`` yields (Hz).
    sample_rate: int

    def synthesize(self, text: str) -> Iterator[bytes]:
        """Yield mono s16le PCM chunks for ``text``; may raise RuntimeError."""
        ...  # pragma: no cover - protocol

    def voices(self) -> list[str]:
        """Voice ids this engine offers (informational, for settings UIs)."""
        ...  # pragma: no cover - protocol

    def stop(self) -> None:
        """Abandon the in-flight synthesis, if any (thread-safe)."""
        ...  # pragma: no cover - protocol

    def close(self) -> None:
        """Release the engine's resources (idempotent)."""
        ...  # pragma: no cover - protocol


def build_engine(config: Config) -> SynthesisEngine | None:
    """The configured engine, or None when speech has no engine.

    Never raises: a missing key or an unknown provider logs and returns
    None, and the Speaker built around None is simply disabled — a
    half-configured feature must not take the daemon down with it.
    """
    provider = config.tts_provider.strip().lower()
    if provider == "none":
        return None
    if provider == "minimax":
        from .minimax_engine import MinimaxEngine

        api_key = config.tts_key()
        if not api_key:
            log.warning(
                "tts_provider is 'minimax' but no key is set "
                "(PHOENIX_TTS_API_KEY, MINIMAX_API_KEY or keys.json); "
                "speech stays off"
            )
            return None
        return MinimaxEngine(
            api_key=api_key,
            model=config.tts_model,
            endpoint=config.tts_endpoint,
            voice=config.tts_voice,
            speed=config.tts_speed,
            sample_rate=32000,
            audio_format=config.tts_format,
        )
    if provider == "local":
        from .local_engine import LocalEngine, venv_python

        # An obviously absent venv disables speech now, with the reason
        # in the log, instead of failing per utterance later.
        if venv_python(config.tts_local_venv) is None:
            log.warning(
                "tts_provider is 'local' but tts_local_venv does not point "
                "at a venv (%s); speech stays off — see the README's local "
                "synthesis section",
                config.tts_local_venv or "unset",
            )
            return None
        return LocalEngine(
            venv_dir=config.tts_local_venv,
            voice_model=config.tts_voice_local,
            voice=config.tts_voice,
        )
    log.warning("unknown tts_provider %r; speech stays off", config.tts_provider)
    return None

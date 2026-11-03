# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Text-to-speech: the daemon reads its own answers aloud.

The package turns agent answers into spoken audio: normalization
(``normalize``), math made speakable (``mathspan``, ``latex_speech``),
sentence segmentation (``segment``), synthesis engines (``synth``,
``minimax_engine``, ``local_engine``), playback (``player``) and the
event-driven orchestrator behind it all (``speaker``).
"""

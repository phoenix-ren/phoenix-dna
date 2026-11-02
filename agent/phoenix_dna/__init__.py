# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Phoenix DNA desktop agent daemon.

A D-Bus daemon that backs the Phoenix DNA plasmoid. It receives already
transcribed prompts (speech-to-text happens in the plasmoid), runs them
through an LLM-driven agent loop in one of three modes (root / user / web)
and streams progress back as D-Bus signals.
"""

__version__ = "0.1.0"

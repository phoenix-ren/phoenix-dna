# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Phoenix AI desktop agent daemon.

A D-Bus daemon that backs the Phoenix plasmoid. It receives already
transcribed prompts (speech-to-text happens in the plasmoid), runs them
through an LLM-driven agent loop in one of three modes (root / user / web)
and streams progress back as D-Bus signals.
"""

__version__ = "0.1.0"

# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Phoenix DNA desktop agent daemon.

A D-Bus daemon that backs the Phoenix DNA plasmoid. It receives already
transcribed prompts (speech-to-text happens in the plasmoid), runs them
through an LLM-driven agent loop as the desktop user (commands that
need administrator rights are escalated on demand) and streams progress
back as D-Bus signals.
"""

__version__ = "0.1.0"

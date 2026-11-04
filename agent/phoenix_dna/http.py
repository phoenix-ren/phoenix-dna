# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""The HTTP conventions the daemon's own requests follow.

Shared so that every outbound request looks the same to the far end,
and so the reason lives in one place.

The User-Agent matters more than it looks: urllib sends
"Python-urllib/3.x" by default, and the bot protection some hosted
APIs sit behind refuses it. Groq's Cloudflare answers such a request
with a 403 and an HTML page — a refusal that has nothing to do with
the key or the request. A plain product name passes.
"""

from __future__ import annotations

#: What the daemon calls itself on the wire.
USER_AGENT = "PhoenixDNA/0.1"


def headers(extra: dict | None = None) -> dict:
    """The headers every outbound request carries.

    \a extra is added on top, so a caller can set what it needs
    (the authorization, the content type) without repeating the rest.
    """
    base = {"User-Agent": USER_AGENT}
    if extra:
        base.update(extra)
    return base

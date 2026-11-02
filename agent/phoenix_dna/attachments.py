# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Prompt attachments: the files the user added to a prompt.

The widget sends the *paths* of the files the user attached. The daemon
copies them into its own attachment directory - so a file survives the run
whatever happens to the original - and turns them into something the model
can work with:

* images are inlined into the message as base64 data URLs, but only when
  the model can see (see :func:`vision_enabled`);
* PDFs are converted to text with ``pdftotext``, when it is installed;
* every attachment is listed with its path, so the agent can also look at
  it with its tools.

A prompt without attachments is unchanged: it is still a plain text
message.
"""

from __future__ import annotations

import base64
import json
import logging
import mimetypes
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger("phoenix.attachments")

#: Images larger than this are not inlined (the base64 payload would bloat
#: the request); the file is still attached and listed for the agent.
MAX_INLINE_IMAGE_BYTES = 10 * 1024 * 1024

#: How much text is taken from a PDF before it is truncated.
MAX_PDF_TEXT_CHARS = 40_000

#: The image types the OpenAI-compatible vision API understands.
_VISION_MIME_TYPES = {"image/png", "image/jpeg", "image/gif", "image/webp"}

#: Model names that are known to accept image parts. Used when the config
#: does not say explicitly whether the model can see.
#:
#: The MiniMax entry matters: MiniMax-M3 and MiniMax-M3.1 accept image
#: parts, MiniMax-Text-01 does not. A bare "minimax" hint would wrongly
#: call the text-only model multimodal, so the family name is not enough.
_VISION_MODEL_HINTS = (
    "vl",
    "vision",
    "gpt-4o",
    "gpt-4.1",
    "gpt-5",
    "claude-3",
    "claude-4",
    "gemini",
    "llava",
    "pixtral",
    "qwen-vl",
    "internvl",
    "minimax-m3",
)


@dataclass(frozen=True)
class Attachment:
    """One file the user attached to a prompt."""

    path: Path
    name: str
    mime_type: str

    @property
    def is_image(self) -> bool:
        return self.mime_type.startswith("image/")

    @property
    def is_pdf(self) -> bool:
        return self.mime_type == "application/pdf" or self.path.suffix.lower() == ".pdf"


def vision_enabled(config) -> bool:
    """Whether the configured model may be sent image parts.

    ``config.vision`` decides when it is set; otherwise the model name is
    inspected, because sending an image to a text-only model makes the
    whole request fail.
    """
    override = getattr(config, "vision", None)
    if override is not None:
        return bool(override)
    model = (getattr(config, "model", "") or "").lower()
    return any(hint in model for hint in _VISION_MODEL_HINTS)


def parse_payload(payload) -> list[Attachment]:
    """Parse the attachment payload the widget sent.

    The payload is a JSON array of ``{"name", "path", "mimeType"}``
    objects. Anything unusable is skipped rather than fatal: a missing
    file must not take the whole prompt down.
    """
    if not payload:
        return []

    items = payload
    if isinstance(payload, (str, bytes)):
        try:
            items = json.loads(payload)
        except ValueError:
            log.warning("ignoring malformed attachment payload")
            return []

    if not isinstance(items, list):
        return []

    attachments: list[Attachment] = []
    for item in items:
        if not isinstance(item, dict):
            continue
        path = str(item.get("path") or "").strip()
        if not path:
            continue
        name = str(item.get("name") or "").strip() or Path(path).name
        mime = str(item.get("mimeType") or "").strip()
        if not mime:
            mime = mimetypes.guess_type(path)[0] or "application/octet-stream"
        attachments.append(Attachment(path=Path(path), name=name, mime_type=mime))
    return attachments


def stage(attachments: list[Attachment], directory: Path) -> list[Attachment]:
    """Copy the attachments into \a directory and return the copies.

    The copies are what the prompt refers to: the agent's tools can read
    them, and they stay put for the whole run. Files that cannot be read
    or copied are dropped with a warning.
    """
    if not attachments:
        return []

    staged: list[Attachment] = []
    try:
        directory.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        log.warning("cannot create the attachment directory %s: %s", directory, exc)
        return []

    for attachment in attachments:
        try:
            target = directory / _safe_name(attachment.name)
            shutil.copyfile(attachment.path, target)
        except OSError as exc:
            log.warning("cannot attach %s: %s", attachment.path, exc)
            continue
        staged.append(
            Attachment(
                path=target, name=attachment.name, mime_type=attachment.mime_type
            )
        )
    return staged


def _safe_name(name: str) -> str:
    """A file name that is safe to create inside the attachment directory."""
    cleaned = "".join(c if c.isalnum() or c in "._-" else "_" for c in name).strip("._")
    return cleaned or "attachment"


def build_user_message(
    prompt: str, attachments: list[Attachment], *, vision: bool = False
) -> dict:
    """The user message the model sees: the prompt plus its attachments.

    Without attachments this is the plain prompt, exactly as before. With
    attachments the text part also lists the files (name, type and path),
    and images are added as image parts when the model can see them.

    An image that is not sent - the model is text-only, or the file is
    unusable - is marked as such in the listing. Without that note the
    model tries to read the file, hits the binary-file refusal, and
    concludes that it cannot see images at all.
    """
    if not attachments:
        return {"role": "user", "content": prompt}

    parts: list[dict] = []
    listed: list[str] = []

    for attachment in attachments:
        if attachment.is_pdf:
            extracted = extract_pdf_text(attachment.path)
            if extracted:
                parts.append(
                    {
                        "type": "text",
                        "text": (
                            f"Text extracted from {attachment.name}:\n{extracted}"
                        ),
                    }
                )
            listed.append(
                f"- {attachment.name} ({attachment.mime_type}): {attachment.path}"
            )
            continue

        if attachment.is_image and vision:
            data_url = image_data_url(attachment)
            if data_url:
                parts.append({"type": "image_url", "image_url": {"url": data_url}})
                listed.append(
                    f"- {attachment.name} ({attachment.mime_type}): {attachment.path}"
                )
                continue
            note = " (image content could not be inlined)"
        elif attachment.is_image:
            note = " (image content is not sent to this model)"
        else:
            note = ""

        listed.append(
            f"- {attachment.name} ({attachment.mime_type}): {attachment.path}{note}"
        )

    text = "\n".join([prompt.rstrip(), "", "Attached files:", *listed])
    parts.insert(0, {"type": "text", "text": text})

    if len(parts) == 1:
        return {"role": "user", "content": text}
    return {"role": "user", "content": parts}


def image_data_url(attachment: Attachment) -> str:
    """The image as a ``data:`` URL, or an empty string when unusable."""
    mime = attachment.mime_type
    if mime not in _VISION_MIME_TYPES:
        # Some tools report odd image types (image/x-png, image/jpg...);
        # fall back to the type the API knows.
        suffix = attachment.path.suffix.lower()
        mime = {
            ".png": "image/png",
            ".jpg": "image/jpeg",
            ".jpeg": "image/jpeg",
            ".gif": "image/gif",
            ".webp": "image/webp",
        }.get(suffix, "")
        if not mime:
            return ""
    try:
        if attachment.path.stat().st_size > MAX_INLINE_IMAGE_BYTES:
            log.info("not inlining %s: the image is too large", attachment.name)
            return ""
        data = attachment.path.read_bytes()
    except OSError as exc:
        log.warning("cannot read the attachment %s: %s", attachment.path, exc)
        return ""
    return f"data:{mime};base64,{base64.b64encode(data).decode('ascii')}"


def extract_pdf_text(path: Path) -> str:
    """The text of a PDF, via ``pdftotext``; empty when that is impossible."""
    binary = shutil.which("pdftotext")
    if not binary:
        return ""
    try:
        result = subprocess.run(
            [binary, "-layout", "-q", str(path), "-"],
            capture_output=True,
            timeout=30,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        log.warning("cannot extract the text of %s: %s", path, exc)
        return ""
    if result.returncode != 0:
        log.warning(
            "pdftotext failed for %s: %s", path, result.stderr.decode(errors="replace")
        )
        return ""
    text = result.stdout.decode("utf-8", errors="replace").strip()
    if len(text) > MAX_PDF_TEXT_CHARS:
        text = text[:MAX_PDF_TEXT_CHARS] + "\n…[truncated]"
    return text

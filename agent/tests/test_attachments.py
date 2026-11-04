# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Prompt attachments: parsing, staging and the message the model sees."""

from __future__ import annotations

import base64
import dataclasses
import json
from pathlib import Path

from conftest import wait_for

from phoenix_dna.attachments import (
    Attachment,
    build_user_message,
    parse_payload,
    stage,
    vision_enabled,
)
from phoenix_dna.config import Config
from phoenix_dna.daemon import AgentDaemon
from phoenix_dna.provider import FakeProvider

PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4"
    "890000000a49444154789c6360000002000100ffff03000006000557bfabd400"
    "00000049454e44ae426082"
)


def write_png(path: Path) -> Path:
    path.write_bytes(PNG)
    return path


def test_parse_payload_reads_the_widget_format():
    payload = json.dumps(
        [
            {"name": "shot.png", "path": "/tmp/shot.png", "mimeType": "image/png"},
            {"path": "/tmp/notes.txt"},
        ]
    )
    attachments = parse_payload(payload)
    assert [a.name for a in attachments] == ["shot.png", "notes.txt"]
    assert attachments[0].mime_type == "image/png"
    # A missing mime type is guessed from the name.
    assert attachments[1].mime_type == "text/plain"


def test_parse_payload_ignores_garbage():
    assert parse_payload(None) == []
    assert parse_payload("") == []
    assert parse_payload("not json") == []
    assert parse_payload("[1, 2, 3]") == []
    assert parse_payload('[{"name": "x"}]') == []  # no path


def test_stage_copies_the_files(tmp_path):
    source = write_png(tmp_path / "shot.png")
    attachments = parse_payload(
        json.dumps([{"name": "shot.png", "path": str(source), "mimeType": "image/png"}])
    )

    staged = stage(attachments, tmp_path / "run" / "attachments")

    assert len(staged) == 1
    assert staged[0].path.read_bytes() == PNG
    assert staged[0].path.parent == tmp_path / "run" / "attachments"
    # The copy has a safe name and keeps the original's name for the model.
    assert staged[0].name == "shot.png"
    assert staged[0].path.name == "shot.png"


def test_stage_drops_unreadable_files(tmp_path):
    attachments = parse_payload(
        json.dumps([{"name": "gone.png", "path": str(tmp_path / "gone.png")}])
    )
    assert stage(attachments, tmp_path / "attachments") == []


def test_stage_sanitizes_the_file_name(tmp_path):
    source = tmp_path / "evil name;.txt"
    source.write_text("hello")
    attachments = parse_payload(json.dumps([{"path": str(source)}]))
    staged = stage(attachments, tmp_path / "attachments")
    assert staged[0].path.parent == tmp_path / "attachments"
    assert "/" not in staged[0].path.name
    assert staged[0].path.read_text() == "hello"


def test_build_user_message_without_attachments_is_the_plain_prompt():
    message = build_user_message("what is up?", [])
    assert message == {"role": "user", "content": "what is up?"}


def test_build_user_message_lists_the_attachments(tmp_path):
    attachment = Attachment(
        path=tmp_path / "notes.txt", name="notes.txt", mime_type="text/plain"
    )
    message = build_user_message("summarise this", [attachment])
    assert message["role"] == "user"
    text = message["content"]
    assert "summarise this" in text
    assert "notes.txt" in text
    assert str(attachment.path) in text


def test_build_user_message_inlines_images_only_with_vision(tmp_path):
    image = write_png(tmp_path / "shot.png")
    attachment = Attachment(path=image, name="shot.png", mime_type="image/png")

    blind = build_user_message("what is this?", [attachment], vision=False)
    assert isinstance(blind["content"], str)
    assert "shot.png" in blind["content"]

    seeing = build_user_message("what is this?", [attachment], vision=True)
    assert isinstance(seeing["content"], list)
    parts = seeing["content"]
    assert parts[0]["type"] == "text"
    image_parts = [p for p in parts if p["type"] == "image_url"]
    assert len(image_parts) == 1
    url = image_parts[0]["image_url"]["url"]
    assert url.startswith("data:image/png;base64,")
    assert base64.b64decode(url.split(",", 1)[1]) == PNG


def test_vision_enabled_follows_the_model_name():
    assert vision_enabled(Config(model="MiniMax-VL-01")) is True
    assert vision_enabled(Config(model="gpt-4o")) is True
    assert vision_enabled(Config(model="MiniMax-Text-01")) is False
    # An explicit setting wins over the guess.
    assert vision_enabled(Config(model="MiniMax-Text-01", vision=True)) is True
    assert vision_enabled(Config(model="MiniMax-VL-01", vision=False)) is False


def test_vision_enabled_knows_the_minimax_models():
    """MiniMax-M3 sees images; the text-only MiniMax model does not."""
    assert vision_enabled(Config(model="MiniMax-M3")) is True
    assert vision_enabled(Config(model="MiniMax-M3.1-Flash")) is True
    assert vision_enabled(Config(model="MiniMax-Text-01")) is False


def test_build_user_message_says_when_an_image_is_not_sent(tmp_path):
    """A blind model is told the image is not there, so it does not guess."""
    image = write_png(tmp_path / "shot.png")
    attachment = Attachment(path=image, name="shot.png", mime_type="image/png")

    blind = build_user_message("what is this?", [attachment], vision=False)
    assert isinstance(blind["content"], str)
    assert "(image content is not sent to this model)" in blind["content"]

    seeing = build_user_message("what is this?", [attachment], vision=True)
    assert isinstance(seeing["content"], list)
    assert "not sent to this model" not in seeing["content"][0]["text"]
    assert any(part["type"] == "image_url" for part in seeing["content"])


def test_daemon_sends_the_attachments_with_the_prompt(config, tmp_path):
    image = write_png(tmp_path / "shot.png")
    provider = FakeProvider(["I can see it."])
    events = []
    daemon = AgentDaemon(
        dataclasses.replace(config, vision=True),
        provider=provider,
        emitter=lambda event: events.append(event),
    )
    try:
        daemon.submit(
            "what is this?",
            "",
            json.dumps(
                [{"name": "shot.png", "path": str(image), "mimeType": "image/png"}]
            ),
        )
        assert wait_for(
            lambda: [e for e in events if e["kind"] == "run_finished"], timeout=10
        )
    finally:
        daemon.shutdown()

    messages = provider.last_messages
    user_message = messages[1]
    assert user_message["role"] == "user"
    assert isinstance(user_message["content"], list)
    assert any(part["type"] == "image_url" for part in user_message["content"])
    # The copy lives in the daemon's own attachment directory.
    staged = list((config.data_dir / "attachments").rglob("shot.png"))
    assert staged, "the attachment was not staged"


def test_daemon_lists_attachments_the_model_cannot_see(config, tmp_path):
    """A text-only model gets the file listed, not an image part."""
    image = write_png(tmp_path / "shot.png")
    provider = FakeProvider(["I see a file name."])
    events = []
    daemon = AgentDaemon(
        config,
        provider=provider,
        emitter=lambda event: events.append(event),
    )
    try:
        daemon.submit(
            "what is this?",
            "",
            json.dumps(
                [{"name": "shot.png", "path": str(image), "mimeType": "image/png"}]
            ),
        )
        assert wait_for(
            lambda: [e for e in events if e["kind"] == "run_finished"], timeout=10
        )
    finally:
        daemon.shutdown()

    content = provider.last_messages[1]["content"]
    assert isinstance(content, str)
    assert "shot.png" in content


def test_daemon_without_attachments_sends_a_plain_prompt(config):
    provider = FakeProvider(["Sure."])
    events = []
    daemon = AgentDaemon(
        config,
        provider=provider,
        emitter=lambda event: events.append(event),
    )
    try:
        daemon.submit("hello")
        assert wait_for(
            lambda: [e for e in events if e["kind"] == "run_finished"], timeout=10
        )
    finally:
        daemon.shutdown()

    assert provider.last_messages[1] == {"role": "user", "content": "hello"}

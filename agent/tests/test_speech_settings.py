# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Tests for the settings window's speech settings (keys.json) and the
live reload that applies them without a service restart."""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from conftest import wait_for  # noqa: E402
from phoenix_dna.config import load_config  # noqa: E402
from phoenix_dna.daemon import AgentDaemon  # noqa: E402
from phoenix_dna.provider import FakeProvider  # noqa: E402


def _write_keys(config_dir: Path, **entries) -> None:
    (config_dir / "keys.json").write_text(json.dumps(entries))


def test_keys_json_speech_settings_apply(tmp_path):
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    _write_keys(
        cfg,
        tts_enabled="true",
        tts_read_mode="stream",
        tts_voice="English_expressive_narrator",
    )
    config = load_config(config_dir=cfg, data_dir=tmp_path / "d", env={})
    assert config.tts_enabled is True
    assert config.tts_read_mode == "stream"
    assert config.tts_voice == "English_expressive_narrator"


def test_keys_json_tts_settings_tolerate_real_json_types(tmp_path):
    # The settings window writes strings, but a hand-edited file may
    # carry a real boolean or number; both must reach the config.
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / "keys.json").write_text(
        json.dumps({"tts_enabled": True, "tts_speed": 1.25, "tts_read_mode": "stream"})
    )
    config = load_config(config_dir=cfg, data_dir=tmp_path / "d", env={})
    assert config.tts_enabled is True
    assert config.tts_speed == 1.25
    assert config.tts_read_mode == "stream"


def test_keys_json_speech_settings_sit_below_toml_and_env(tmp_path):
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    _write_keys(cfg, tts_enabled="true", tts_read_mode="stream")
    (cfg / "agent.toml").write_text("[agent]\ntts_read_mode = \"final\"\n")

    # agent.toml beats keys.json …
    config = load_config(config_dir=cfg, data_dir=tmp_path / "d", env={})
    assert config.tts_enabled is True
    assert config.tts_read_mode == "final"

    # … and the environment beats both.
    config = load_config(
        config_dir=cfg, data_dir=tmp_path / "d", env={"PHOENIX_TTS_ENABLED": "false"}
    )
    assert config.tts_enabled is False


def test_daemon_reload_speech_applies_the_toggle_live(config):
    daemon = AgentDaemon(config, provider=FakeProvider(["spoken"]))
    try:
        assert daemon.speech_state() == {"enabled": False, "state": "idle"}

        # The settings window's write, then its ReloadSpeech call. (The
        # MiniMax key rides along: the window's toggle alone is not
        # enough to build an engine without a credential.)
        _write_keys(
            config.config_dir, tts_enabled="true", tts_voice="v", minimax_api_key="k"
        )
        daemon.reload_speech()
        assert daemon.speech_state()["enabled"] is True

        # Flipping it off mid-listen falls silent (the old speaker is
        # retired with a close, not left half-alive).
        _write_keys(
            config.config_dir,
            tts_enabled="false",
            minimax_api_key="k",
        )
        daemon.reload_speech()
        assert daemon.speech_state() == {"enabled": False, "state": "idle"}
    finally:
        daemon.shutdown()

# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Configuration loading tests."""

from __future__ import annotations

import json

from phoenix_dna.config import describe, load_config


def test_defaults_and_dir_creation(tmp_path):
    config = load_config(
        config_dir=tmp_path / "cfg", data_dir=tmp_path / "data", env={}
    )
    assert config.provider == "minimax"
    assert config.model == "MiniMax-Text-01"
    assert config.llm_base_url == "https://api.minimax.io/v1"
    assert config.data_dir.is_dir()
    assert config.config_dir.is_dir()


def test_keys_json_and_env_override(tmp_path):
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / "keys.json").write_text(
        json.dumps(
            {
                "minimax_api_key": "sk-file-key-abcdefgh",
                "tavily_api_key": "tvly-file",
                "exa_api_key": "exa-file",
            }
        )
    )
    config = load_config(config_dir=cfg, data_dir=tmp_path / "d", env={})
    assert config.llm_api_key == "sk-file-key-abcdefgh"
    assert config.keys["tavily_api_key"] == "tvly-file"

    # environment beats the file
    config = load_config(
        config_dir=cfg,
        data_dir=tmp_path / "d",
        env={"MINIMAX_API_KEY": "sk-env-key"},
    )
    assert config.llm_api_key == "sk-env-key"
    assert config.keys["minimax_api_key"] == "sk-env-key"


def test_toml_overrides_and_unknown_settings(tmp_path):
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / "agent.toml").write_text(
        '[agent]\nmodel = "MiniMax-New"\nmax_turns = 7\nbogus_setting = 1\n'
    )
    config = load_config(config_dir=cfg, data_dir=tmp_path / "d", env={})
    assert config.model == "MiniMax-New"
    assert config.max_turns == 7
    assert not hasattr(config, "bogus_setting")

    # environment beats agent.toml
    config = load_config(
        config_dir=cfg,
        data_dir=tmp_path / "d",
        env={"PHOENIX_MODEL": "MiniMax-Env"},
    )
    assert config.model == "MiniMax-Env"


def test_describe_masks_keys(tmp_path):
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / "keys.json").write_text(
        json.dumps({"minimax_api_key": "sk-supersecret-value"})
    )
    config = load_config(config_dir=cfg, data_dir=tmp_path / "d", env={})
    summary = describe(config)
    assert "sk-supersecret-value" not in summary
    assert "sk-sup..." in summary
    assert "not set" in summary  # tavily/exa unset

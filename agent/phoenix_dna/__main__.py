# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Entry point: ``phoenix-dna-agent`` or ``python -m phoenix_dna``."""

from __future__ import annotations

import argparse
import logging
import sys

from . import __version__
from .config import describe, load_config
from .daemon import AgentDaemon
from .provider import build_provider


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="phoenix-dna-agent",
        description="Phoenix DNA desktop agent daemon (D-Bus service)",
    )
    parser.add_argument("--version", action="version", version=__version__)
    parser.add_argument(
        "--provider",
        choices=["minimax", "fake"],
        help="LLM provider (overrides config; 'fake' needs no API key)",
    )
    parser.add_argument(
        "--llm-provider",
        choices=["minimax", "openai", "groq", "openrouter", "custom"],
        help="which hosted service the model calls go to (overrides config)",
    )
    parser.add_argument("--config-dir", help="configuration directory")
    parser.add_argument("--data-dir", help="state/data directory")
    parser.add_argument(
        "--check-config",
        action="store_true",
        help="print the effective config and exit",
    )
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(name)s %(levelname)s: %(message)s",
        stream=sys.stderr,
    )

    config = load_config(config_dir=args.config_dir, data_dir=args.data_dir)
    if args.provider:
        import dataclasses

        config = dataclasses.replace(config, provider=args.provider)
    if args.llm_provider:
        import dataclasses

        # The provider brings its own endpoint, model and key; a
        # --llm-provider on the command line replaces the configured
        # one, so the defaults follow it (unless agent.toml names an
        # endpoint or a model of its own).
        config = dataclasses.replace(config, llm_provider=args.llm_provider)
        from .config import LLM_PROVIDERS

        spec = LLM_PROVIDERS[args.llm_provider]
        if spec["base_url"]:
            config = dataclasses.replace(config, llm_base_url=spec["base_url"])
        if spec["model"]:
            config = dataclasses.replace(config, model=spec["model"])
        key = config.keys.get(spec["key_field"], "")
        if key:
            config = dataclasses.replace(config, llm_api_key=key)

    if args.check_config:
        print(describe(config))
        return 0

    try:
        provider = build_provider(config)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    from .service import run_service

    # The transcript cleaner, the session namer and the summariser of a
    # conversation that outgrew the model's window are their own provider
    # instances: their single-shot calls are independent of the run loop's
    # provider usage (and of the scripted call sequence a FakeProvider
    # replays).
    try:
        cleanup_provider = build_provider(config)
        title_provider = build_provider(config)
        compaction_provider = build_provider(config)
    except RuntimeError:
        cleanup_provider = None
        title_provider = None
        compaction_provider = None

    daemon = AgentDaemon(
        config,
        provider=provider,
        cleanup_provider=cleanup_provider,
        title_provider=title_provider,
        compaction_provider=compaction_provider,
    )
    try:
        run_service(daemon)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

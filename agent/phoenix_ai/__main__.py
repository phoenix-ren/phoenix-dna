# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

"""Entry point: ``phoenix-agent`` or ``python -m phoenix_ai``."""

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
        prog="phoenix-agent",
        description="Phoenix AI desktop agent daemon (D-Bus service)",
    )
    parser.add_argument("--version", action="version", version=__version__)
    parser.add_argument(
        "--provider",
        choices=["minimax", "fake"],
        help="LLM provider (overrides config; 'fake' needs no API key)",
    )
    parser.add_argument("--config-dir", help="configuration directory")
    parser.add_argument("--data-dir", help="state/data directory")
    parser.add_argument(
        "--check-config", action="store_true", help="print the effective config and exit"
    )
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(name)s %(levelname)s: %(message)s",
        stream=sys.stderr,
    )

    config = load_config(
        config_dir=args.config_dir, data_dir=args.data_dir
    )
    if args.provider:
        import dataclasses

        config = dataclasses.replace(config, provider=args.provider)

    if args.check_config:
        print(describe(config))
        return 0

    try:
        provider = build_provider(config)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    from .service import run_service

    daemon = AgentDaemon(config, provider=provider)
    try:
        run_service(daemon)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

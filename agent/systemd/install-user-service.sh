#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause
#
# Install the Phoenix DNA agent daemon as a systemd *user* service.
#
#   agent/systemd/install-user-service.sh [--agent-dir DIR] [--no-start]
#
# The daemon runs as the desktop user (michelle), not as root: it owns the
# session-bus name dna.phoenix.Agent that the plasmoid connects to, it needs
# the GUI session for the kdialog password prompt, and it elevates to root on
# demand through sudo (see agent/phoenix_dna/privileges.py).

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
template="$script_dir/phoenix.service.in"
agent_dir=$(dirname -- "$script_dir")
start_now=1

die() {
    echo "error: $*" >&2
    exit 1
}

usage() {
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --agent-dir)
            [ $# -ge 2 ] || die "--agent-dir needs a directory"
            agent_dir=$(readlink -f -- "$2")
            shift 2
            ;;
        --no-start)
            start_now=0
            shift
            ;;
        -h | --help)
            usage
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

[ -f "$template" ] || die "template not found: $template"
[ -d "$agent_dir/phoenix_dna" ] || die "no agent package in $agent_dir"

# Prefer the project's virtualenv (it carries the editable install of the
# package); fall back to the system interpreter, which needs the distro
# packages python3-dbus and python3-gobject.
if [ -x "$agent_dir/.venv/bin/python" ]; then
    python_bin="$agent_dir/.venv/bin/python"
else
    python_bin=$(command -v python3 || true)
    [ -n "$python_bin" ] || die "no python3 found"
fi

# "-m phoenix_dna" runs the package from WorkingDirectory, so it works with
# and without the editable install.
agent_exec="$python_bin -m phoenix_dna"

unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
unit_path="$unit_dir/phoenix.service"
mkdir -p "$unit_dir"

sed -e "s|@AGENT_EXEC@|$agent_exec|g" -e "s|@AGENT_DIR@|$agent_dir|g" \
    "$template" >"$unit_path"

echo "wrote $unit_path"
echo "  ExecStart=$agent_exec"
echo "  WorkingDirectory=$agent_dir"

# The daemon needs a MiniMax key to start; keys.json wins over the
# environment, and the service inherits neither of the keys an interactive
# shell may have exported.
keys_file="${PHOENIX_CONFIG_DIR:-$HOME/.config/phoenix-dna}/keys.json"
if [ ! -f "$keys_file" ]; then
    echo "warning: $keys_file not found - the daemon will refuse to start" >&2
    echo "         (see agent/README.md for the key format)" >&2
fi

systemctl --user daemon-reload

if [ "$start_now" -eq 1 ]; then
    # A daemon that is still running from a terminal owns the bus name; the
    # service cannot take over until it is gone.
    if systemctl --user is-active --quiet phoenix.service; then
        echo "restarting phoenix.service"
    fi
    systemctl --user enable --now phoenix.service
    systemctl --user --no-pager --lines=10 status phoenix.service || true
else
    systemctl --user enable phoenix.service
    echo "enabled phoenix.service; start it with: systemctl --user start phoenix.service"
fi

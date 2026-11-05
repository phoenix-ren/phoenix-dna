#!/bin/sh
# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause
#
# Check an installed Phoenix DNA package from the outside: file placement,
# launcher syntax, Python import, AppStream metadata, desktop entry, systemd
# unit and D-Bus activation. It does not need a running Plasma session.

set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
version=$(sed -n \
    's/^__version__[[:space:]]*=[[:space:]]*"\([^"]*\)".*$/\1/p' \
    "$root/agent/phoenix_dna/__init__.py")

passed=0
failed=0

pass() {
    passed=$((passed + 1))
    printf 'ok   %s\n' "$1"
}

fail() {
    failed=$((failed + 1))
    printf 'FAIL %s\n' "$1" >&2
}

check_file() {
    if [ -f "$1" ]; then pass "$1"; else fail "missing $1"; fi
}

check_exec() {
    if [ -x "$1" ]; then pass "$1"; else fail "not executable: $1"; fi
}

check_file_any() {
    label=$1
    shift
    for file in "$@"; do
        if [ -f "$file" ]; then
            pass "$file"
            return
        fi
    done
    fail "missing $label"
}

agent_dir=/usr/share/phoenix-dna/agent
unit=/usr/lib/systemd/user/phoenix.service
dbus=/usr/share/dbus-1/services/dna.phoenix.Agent.service

check_exec /usr/bin/phoenix-dna-agent
check_exec /usr/bin/phoenix-dna-repl
check_file "$agent_dir/phoenix_dna/__init__.py"
check_file "$agent_dir/README.md"
check_file "$agent_dir/config-example.toml"
check_file "$unit"
check_file "$dbus"
check_file /usr/share/applications/org.phoenixdna.agent.desktop
check_file /usr/share/metainfo/org.phoenixdna.agent.metainfo.xml
# Ubuntu's dpkg drops /usr/share/doc/* at unpack time (only copyright and
# changelog are re-included), so a missing README there is the distribution's
# doing, not a packaging bug.
doc_excluded=0
for conf in /etc/dpkg/dpkg.cfg.d/*.conf; do
    [ -f "$conf" ] || continue
    grep -q '^path-exclude=/usr/share/doc/\*' "$conf" && doc_excluded=1
done
if [ "$doc_excluded" -eq 1 ]; then
    printf 'skip installed README (dpkg path-exclude=/usr/share/doc/*)\n'
else
    check_file_any "installed README" \
        /usr/share/doc/phoenix-dna/README.md \
        /usr/share/doc/phoenix-dna/README.md.gz
fi
check_file /usr/share/licenses/phoenix-dna/BSD-2-Clause.txt

qml_root=
for candidate in /usr/lib64/qt6/qml /usr/lib/qt6/qml /usr/lib/*/qt6/qml; do
    if [ -d "$candidate/org/phoenixdna" ]; then
        qml_root="$candidate/org/phoenixdna"
        break
    fi
done
if [ -n "$qml_root" ]; then
    pass "$qml_root"
    for module in theme overlay resultswindow search settings; do
        check_file "$qml_root/$module/qmldir"
        check_file "$qml_root/$module/liborg.phoenixdna.$module.so"
    done
else
    fail "QML modules not found under /usr/lib*/qt6/qml/org/phoenixdna"
fi

plugin=$(find /usr/lib64/qt6/plugins/plasma/applets \
    /usr/lib/qt6/plugins/plasma/applets \
    /usr/lib/*/qt6/plugins/plasma/applets \
    -name org.phoenixdna.agent.so 2>/dev/null | head -n1)
if [ -n "$plugin" ]; then pass "$plugin"; else fail "applet plugin not found"; fi

if sh -n /usr/bin/phoenix-dna-agent; then
    pass "launcher syntax"
else
    fail "launcher syntax"
fi

if PYTHONPATH="$agent_dir" python3 -c \
        "import phoenix_dna; raise SystemExit(phoenix_dna.__version__ != '$version')" \
        2>/dev/null; then
    pass "python import (version $version)"
else
    fail "python import / version mismatch (expected $version)"
fi

if PYTHONPATH="$agent_dir" python3 -m phoenix_dna --check-config \
        >/dev/null 2>&1; then
    pass "python --check-config"
else
    fail "python --check-config"
fi

if grep -q '^ExecStart=/usr/bin/phoenix-dna-agent$' "$unit" &&
        grep -q "^WorkingDirectory=$agent_dir$" "$unit"; then
    pass "systemd unit paths"
else
    fail "systemd unit ExecStart/WorkingDirectory"
fi

if grep -q '^Name=dna.phoenix.Agent$' "$dbus" &&
        grep -q '^Exec=/usr/bin/phoenix-dna-agent$' "$dbus" &&
        grep -q '^SystemdService=phoenix.service$' "$dbus"; then
    pass "D-Bus activation paths"
else
    fail "D-Bus activation file"
fi

if command -v appstreamcli >/dev/null 2>&1; then
    if appstreamcli validate --no-net \
            /usr/share/metainfo/org.phoenixdna.agent.metainfo.xml \
            >/dev/null 2>&1; then
        pass "appstreamcli validate"
    else
        fail "appstreamcli validate"
    fi
else
    printf 'skip appstreamcli (not installed)\n'
fi

if command -v desktop-file-validate >/dev/null 2>&1; then
    if desktop-file-validate \
            /usr/share/applications/org.phoenixdna.agent.desktop \
            >/dev/null 2>&1; then
        pass "desktop-file-validate"
    else
        fail "desktop-file-validate"
    fi
else
    printf 'skip desktop-file-validate (not installed)\n'
fi

printf '\n%d passed, %d failed\n' "$passed" "$failed"
[ "$failed" -eq 0 ]

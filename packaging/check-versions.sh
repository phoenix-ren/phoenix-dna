#!/bin/sh
# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause
#
# The version has one home: agent/phoenix_dna/__init__.py. CMake reads it, but
# the native recipes cannot, so this check keeps them from drifting apart.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$(sed -n \
    's/^__version__[[:space:]]*=[[:space:]]*"\([^"]*\)".*$/\1/p' \
    "$root/agent/phoenix_dna/__init__.py")

[ -n "$version" ] || {
    echo "error: cannot read the version from agent/phoenix_dna/__init__.py" >&2
    exit 1
}

failed=0
check() {
    file=$1
    pattern=$2
    if ! grep -q "$pattern" "$root/$file"; then
        echo "error: $file does not carry version $version" >&2
        failed=1
    fi
}

check shell/metadata.json "\"Version\"[[:space:]]*:[[:space:]]*\"$version\""
check packaging/fedora/phoenix-dna.spec "^Version:[[:space:]]*$version\$"
check packaging/arch/PKGBUILD "^pkgver=$version\$"
check debian/changelog "^phoenix-dna ($version-"

# The AppStream file is a template; CMake substitutes the project version.
if ! grep -q '@PROJECT_VERSION@' \
        "$root/packaging/org.phoenixdna.agent.metainfo.xml.in"; then
    echo "error: the AppStream template no longer uses @PROJECT_VERSION@" >&2
    failed=1
fi

if [ "$failed" -ne 0 ]; then
    echo "bump every occurrence to $version, then re-run this check" >&2
    exit 1
fi

echo "all version references are $version"

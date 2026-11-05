#!/bin/sh
# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause
#
# Create the tarball the native recipes expect:
#
#   phoenix-dna-<version>.tar.gz
#   └── phoenix-dna-<version>/
#
# Usage:
#   packaging/make-source-archive.sh [--worktree] [OUTDIR]
#
# Without --worktree the archive comes from `git archive HEAD`, so it contains
# exactly the committed tree. --worktree packs the current working tree
# instead; that is what the test scripts use while the recipes are still being
# edited.

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
version=$(sed -n \
    's/^__version__[[:space:]]*=[[:space:]]*"\([^"]*\)".*$/\1/p' \
    "$root/agent/phoenix_dna/__init__.py")
[ -n "$version" ] || {
    echo "error: cannot read the version from agent/phoenix_dna/__init__.py" >&2
    exit 1
}

worktree=0
if [ "${1:-}" = "--worktree" ]; then
    worktree=1
    shift
fi

outdir=${1:-"$root/dist"}
mkdir -p "$outdir"
archive="$outdir/phoenix-dna-$version.tar.gz"

if [ "$worktree" -eq 0 ]; then
    git -C "$root" archive \
        --format=tar.gz \
        --prefix="phoenix-dna-$version/" \
        --output="$archive" \
        HEAD
else
    tar -C "$root" \
        --exclude=./.git \
        --exclude=./build \
        --exclude=./.rpmbuild \
        --exclude=./dist \
        --exclude=./agent/build \
        --exclude=./agent/phoenix_dna.egg-info \
        --exclude=./agent/.venv \
        --exclude=__pycache__ \
        --exclude='*.pyc' \
        --transform "s,^\.,phoenix-dna-$version," \
        -czf "$archive" .
fi

echo "$archive"

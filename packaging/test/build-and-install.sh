#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause
#
# Build the native package inside a test VM, install it, and run the smoke
# test. Run as the desktop user (not root) with sudo available.
#
#   packaging/test/build-and-install.sh
#
# The VM must have network access for the first run: the script installs the
# distribution's build dependencies from the recipe itself.

set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
version=$(sed -n \
    's/^__version__[[:space:]]*=[[:space:]]*"\([^"]*\)".*$/\1/p' \
    "$root/agent/phoenix_dna/__init__.py")
[ -n "$version" ] || {
    echo "error: cannot read the version from agent/phoenix_dna/__init__.py" >&2
    exit 1
}

die() {
    echo "error: $*" >&2
    exit 1
}

log() {
    printf '\n==> %s\n' "$*"
}

run_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

[ "$(id -u)" -ne 0 ] || die "run this as your desktop user; it uses sudo only for package installation"

. /etc/os-release

if [ "${ID:-}" = "kde-linux" ] || [ -e /etc/kde-linux-release ]; then
    die "KDE Linux is immutable; build the Arch package in the Arch VM and install it there with 'sudo pacman -U <package>' if your image permits it. Use a toolbox/container for a build test."
fi

build_fedora() {
    log "installing Fedora build dependencies"
    run_root dnf install -y rpm-build dnf-plugins-core
    run_root dnf builddep -y "$root/packaging/fedora/phoenix-dna.spec"

    log "creating source archive"
    "$root/packaging/make-source-archive.sh" --worktree "$root/dist"

    log "building RPM"
    topdir="$root/.rpmbuild"
    mkdir -p "$topdir"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
    rpmbuild -ba \
        --define "_topdir $topdir" \
        --define "_sourcedir $root/dist" \
        --define "_specdir $root/packaging/fedora" \
        "$root/packaging/fedora/phoenix-dna.spec"

    rpm=$(find "$topdir/RPMS" -name "phoenix-dna-$version-*.rpm" \
        ! -name '*debuginfo*' ! -name '*debugsource*' | sort | head -n1)
    [ -n "$rpm" ] || die "rpmbuild did not produce a binary RPM"
    log "installing $rpm"
    run_root dnf install -y "$rpm"
}

build_arch() {
    log "installing Arch build and runtime dependencies"
    run_root pacman -Syu --needed --noconfirm \
        base-devel cmake extra-cmake-modules boost \
        libplasma plasma-workspace kwin \
        kcoreaddons kconfig kconfigwidgets ki18n \
        kglobalaccel kpackage kwindowsystem kirigami ksvg \
        qt6-base qt6-declarative qt6-svg \
        python python-dbus python-gobject pipewire sudo

    log "creating source archive"
    "$root/packaging/make-source-archive.sh" --worktree "$root/dist"

    log "building package"
    (cd "$root/packaging/arch" &&
        SRCDEST="$root/dist" makepkg -f -C --noconfirm)

    pkg=$(find "$root/packaging/arch" -maxdepth 1 \
        -name "phoenix-dna-$version-*.pkg.tar.zst" | sort | head -n1)
    [ -n "$pkg" ] || die "makepkg did not produce a package"
    log "installing $pkg"
    run_root pacman -U --noconfirm "$pkg"
}

build_debian() {
    log "installing Debian build dependencies"
    run_root env DEBIAN_FRONTEND=noninteractive \
        apt-get update
    run_root env DEBIAN_FRONTEND=noninteractive \
        apt-get install -y devscripts equivs dpkg-dev

    if ! dpkg-checkbuilddeps "$root/debian/control" >/dev/null 2>&1; then
        (cd "$root" && run_root mk-build-deps --install --remove \
            --tool 'apt-get -y' debian/control)
    fi

    log "building Debian package"
    (cd "$root" && dpkg-buildpackage -b -uc -us)

    deb=$(find "$(dirname "$root")" -maxdepth 1 \
        -name "phoenix-dna_${version}-*.deb" | sort | head -n1)
    [ -n "$deb" ] || die "dpkg-buildpackage did not produce a .deb"
    log "installing $deb"
    run_root env DEBIAN_FRONTEND=noninteractive apt-get install -y "$deb"
}

case "${ID:-}" in
    fedora)
        build_fedora
        ;;
    arch | cachyos)
        build_arch
        ;;
    debian | ubuntu)
        build_debian
        ;;
    *)
        case " ${ID_LIKE:-} " in
            *arch*) build_arch ;;
            *debian* | *ubuntu*) build_debian ;;
            *fedora*) build_fedora ;;
            *) die "unsupported distribution: ${ID:-unknown}" ;;
        esac
        ;;
esac

log "running smoke test"
"$root/packaging/test/smoke.sh"

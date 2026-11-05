<!--
SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
SPDX-License-Identifier: BSD-2-Clause
-->

# Packaging Phoenix DNA

Phoenix DNA is two halves that must be installed together:

* the **Plasma applet** — a C++ plugin, five QML modules, the icon set and a
  desktop entry;
* the **agent daemon** — the Python package `agent/phoenix_dna`, two
  launchers, a systemd user unit and a D-Bus activation file.

One CMake project installs both. A distribution package should build with
the switches below and install into a `DESTDIR`; native packaging recipes are
preferred over CPack.

## Build and install

```sh
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=ON \
    -DPHOENIX_BUILD_DEVTOOLS=OFF \
    -DPHOENIX_UPDATE_CACHES=OFF
cmake --build build --parallel
DESTDIR="$PWD/instroot" cmake --install build
```

The options that matter to a packager:

| Option | Default | What it does |
|---|---|---|
| `BUILD_TESTING` | `ON` | Builds the CTest targets. Pass `OFF` to skip them entirely. |
| `PHOENIX_BUILD_DEVTOOLS` | `ON` | Builds `widgettest` and `waveformrender`. Never installed; pass `OFF` for a package. |
| `PHOENIX_INSTALL_AGENT` | `ON` | Installs the Python package, launchers, systemd user unit and D-Bus activation file. |
| `PHOENIX_INSTALL_APPDATA` | `ON` | Installs AppStream metadata, READMEs and licence texts. |
| `PHOENIX_UPDATE_CACHES` | `ON` | Refreshes the icon/desktop caches after a *non-DESTDIR* install. Always skipped under `DESTDIR`; pass `OFF` to make that explicit. |
| `PHOENIX_AGENT_PYTHON` | Python found by CMake | Interpreter the installed launchers use. Pass `/usr/bin/python3` in a package. |

`CMAKE_INSTALL_PREFIX=/usr` plus KDE's `KDEInstallDirs` puts the files where
the distribution expects them. Do not hard-code `lib` vs `lib64`: Fedora uses
`/usr/lib64`, Debian multiarch uses `/usr/lib/x86_64-linux-gnu`, and Arch uses
`/usr/lib`; the same build tree follows the platform's `CMAKE_INSTALL_LIBDIR`.

## Installed layout

| Path | Contents |
|---|---|
| `${KDE_INSTALL_QMLDIR}/org/phoenixdna/{theme,overlay,resultswindow,search,settings}` | The five QML modules |
| `${KDE_INSTALL_PLUGINDIR}/plasma/applets/org.phoenixdna.agent.so` | The applet plugin |
| `${KDE_INSTALL_ICONDIR}/hicolor/...` | `org.phoenixdna.agent.svg`, `org.phoenixdna.avatar.png` |
| `${KDE_INSTALL_APPDIR}/org.phoenixdna.agent.desktop` | The app's desktop entry (window identity/icon; `NoDisplay=true`) |
| `${KDE_INSTALL_METAINFODIR}/org.phoenixdna.agent.metainfo.xml` | AppStream add-on for `org.kde.plasmashell` |
| `${KDE_INSTALL_DATADIR}/phoenix-dna/agent/phoenix_dna` | The Python daemon, privately installed |
| `${KDE_INSTALL_BINDIR}/phoenix-dna-agent`, `phoenix-dna-repl` | Launchers |
| `${KDE_INSTALL_SYSTEMDUSERUNITDIR}/phoenix.service` | systemd **user** unit |
| `${KDE_INSTALL_DBUSSERVICEDIR}/dna.phoenix.Agent.service` | D-Bus session activation |
| `${KDE_INSTALL_DATADIR}/doc/phoenix-dna` | README, this file, `LICENSE` |
| `${KDE_INSTALL_DATADIR}/licenses/phoenix-dna` | BSD-2-Clause, FSFAP, ISC and MIT texts |

Two details are deliberate:

* **The Python package is not installed into site-packages.** It lives in
  `/usr/share/phoenix-dna/agent` (or the distribution's data dir) and the
  launchers put that directory on `PYTHONPATH`. This keeps the applet and the
  daemon on the same version and out of the interpreter's namespace. If the
  distribution wants to package the Python part with its own Python macros,
  pass `-DPHOENIX_INSTALL_AGENT=OFF` and install `agent/phoenix_dna` plus the
  service files yourself.
* **The daemon is not compiled and has no vendored dependencies.** It uses the
  distribution's `python3-dbus` and `python3-gobject` (`PyGObject`'s `GLib`).

## Runtime dependencies

Required:

* KDE Frameworks 6: CoreAddons, Config, ConfigWidgets, I18n, GlobalAccel,
  Package, WindowSystem, Kirigami, KSvg;
* Plasma: `libplasma` (the `org.kde.plasma.core`,
  `org.kde.plasma.components` and `org.kde.plasma.plasmoid` QML modules) and
  `plasma-workspace` (plasmashell);
* KWin: the `org.kde.kwin` QML module (screenshot picker);
* Qt 6.7 or newer: Core, Gui, Quick, Qml, QuickControls2, DBus, Network, Svg;
* Python 3.11 or newer, `dbus-python` and `PyGObject`;
* PipeWire's `pw-cat` (dictation and the completion chime).

Strongly recommended, because the feature is optional at runtime:

* `ffmpeg` — read-aloud answer synthesis/decoding;
* `kdialog` — the KDE password dialog for `sudo`;
* `sudo` — on-demand administrator rights;
* a TeX engine (`xelatex`, `lualatex` or `pdflatex`) — LaTeX formulas rendered
  to PDF. Without it the answers keep the formulas verbatim.

### Fedora

```spec
BuildRequires:  cmake extra-cmake-modules gcc-c++ gettext
BuildRequires:  qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtsvg-devel
BuildRequires:  kf6-kcoreaddons-devel kf6-kconfig-devel
BuildRequires:  kf6-kconfigwidgets-devel kf6-ki18n-devel
BuildRequires:  kf6-kglobalaccel-devel kf6-kpackage-devel
BuildRequires:  kf6-kwindowsystem-devel kf6-kirigami-devel kf6-ksvg-devel
BuildRequires:  libplasma-devel boost-devel python3
BuildRequires:  systemd-rpm-macros

Requires:       libplasma plasma-workspace kwin-common
Requires:       kf6-kcoreaddons kf6-kconfig kf6-kconfigwidgets kf6-ki18n
Requires:       kf6-kglobalaccel kf6-kpackage kf6-kwindowsystem
Requires:       kf6-kirigami kf6-ksvg
Requires:       qt6-qtbase qt6-qtdeclarative qt6-qtsvg
Requires:       python3-dbus python3-gobject
Requires:       pipewire-utils
Recommends:     ffmpeg-free kdialog sudo
Recommends:     texlive-xetex
```

`qt6-qtdeclarative-devel` carries the QuickControls2 development files on
Fedora. `ffmpeg-free` is Fedora's ffmpeg build; RPM Fusion calls it `ffmpeg`.

### Arch Linux (and derivatives)

The full recipe is [`packaging/arch/PKGBUILD`](packaging/arch/PKGBUILD); the
dependency list, verified on a real CachyOS install:

```bash
depends=(
    'libplasma' 'plasma-workspace' 'kwin'
    'kcoreaddons' 'kconfig' 'kconfigwidgets' 'ki18n'
    'kglobalaccel' 'kpackage' 'kwindowsystem'
    'kirigami' 'ksvg'
    'qt6-base' 'qt6-declarative' 'qt6-svg'
    'python' 'python-dbus' 'python-gobject' 'pipewire'
)
makedepends=(
    'cmake' 'extra-cmake-modules' 'boost'
    'kirigami' 'ksvg' 'libplasma' 'kwin'
)
```

Note the names: Arch's KF6 packages carry **no `kf6-` prefix** (`kcoreaddons`,
`kirigami`, `ksvg`, …) — `kf6-kcoreaddons` and friends do not exist. CachyOS
uses the same packages. `pipewire` provides `pw-cat`; `qt6-declarative`
carries QuickControls2.

### Debian and Ubuntu

The packaging lives in [`debian/`](debian) at the repository root — where
`dpkg-buildpackage` expects it. Highlights:

```text
Build-Depends: debhelper-compat (= 13), cmake, extra-cmake-modules,
 gettext, qt6-base-dev, qt6-declarative-dev, qt6-svg-dev,
 libkf6coreaddons-dev, libkf6config-dev, libkf6configwidgets-dev,
 libkf6i18n-dev, libkf6globalaccel-dev, libkf6package-dev,
 libkf6windowsystem-dev, libkirigami-dev, libkf6svg-dev,
 libplasma-dev (>= 6.4), libboost-dev,
 libxkbcommon-dev, libxkbcommon-x11-dev, python3,
 appstream, desktop-file-utils

Depends: ${shlibs:Depends}, ${misc:Depends}, plasma-workspace,
 kwin-common, kwin-wayland,
 qml6-module-qtcore, qml6-module-qtqml,
 qml6-module-qtquick, qml6-module-qtquick-controls,
 qml6-module-qtquick-dialogs, qml6-module-qtquick-layouts,
 qml6-module-qtquick-window,
 qml6-module-org-kde-kirigami, qml6-module-org-kde-ksvg,
 python3, python3-dbus, python3-gi, pipewire-bin

Recommends: ffmpeg, kdialog, sudo, texlive-xetex
```

Naming traps this tree ran into on real Debian-family systems: Kirigami's
dev package is `libkirigami-dev`, not `libkf6kirigami-dev`; there are **no**
`qml6-module-org-kde-kwin`, `-plasma-core` or `-plasma-components` packages
(those QML modules ship inside `kwin-common`, `libplasma` and
`plasma-workspace`); and the libplasma soname changes between releases
(`libplasma6`, `libplasma7`, …), so depend on `${shlibs:Depends}` instead of
pinning it. `libxkbcommon[-x11]-dev` is what `plasma_add_applet` needs to
find XKB.

The binding minimum is **libplasma 6.4** — `plasma_add_applet` first appeared
there — together with Qt 6.7; `QT_MIN_VERSION`/`KF6_MIN_VERSION` alone
understate it. In practice: Fedora with Plasma ≥ 6.4 (verified on Fedora 45),
current Arch and CachyOS (verified), Ubuntu 25.10 and 26.04 (verified on
26.04.1), Debian sid. Debian 13 (trixie) ships Plasma 6.3 and **cannot build
this tree**; Ubuntu 24.04 carries Qt 6.4/Plasma 5 and cannot either.

## Recipes and the test harness

The native recipes are real files, exercised end-to-end on fresh VMs — not
documentation snippets:

| Distro | Recipe | Verified on |
|---|---|---|
| Fedora | [`packaging/fedora/phoenix-dna.spec`](packaging/fedora/phoenix-dna.spec) | Fedora 45 (fresh VM): `dnf builddep`, `rpmbuild`, 10/10 tests, install, smoke 30/30 |
| Arch / CachyOS | [`packaging/arch/PKGBUILD`](packaging/arch/PKGBUILD) | CachyOS VM: pacman deps, `makepkg`, 10/10 tests, `pacman -U`, smoke 30/30 |
| Debian / Ubuntu | [`debian/`](debian) (`control`, `rules`, …) | Kubuntu 26.04.1 VM: `mk-build-deps`, `dpkg-buildpackage`, 10/10 tests, install, smoke 30/30 |

Supporting pieces:

* `packaging/make-source-archive.sh` — builds `phoenix-dna-<version>.tar.gz`,
  the `Source0` the spec and PKGBUILD reference. `--worktree` packs the
  current tree instead of `HEAD`; the test harness uses that while iterating.
* `packaging/check-versions.sh` — fails if the spec, the PKGBUILD,
  `debian/changelog` or `shell/metadata.json` drifted from the version in
  `agent/phoenix_dna/__init__.py`.
* `packaging/test/build-and-install.sh` — the whole per-distro cycle from one
  command: install the build dependencies *from the recipe itself*, build the
  package, install it, run the smoke test. Run it as a normal user with sudo
  rights, and say `bash packaging/test/build-and-install.sh` — Ubuntu's
  `/bin/sh` is dash and the script uses bash features.
* `packaging/test/smoke.sh` — checks an *installed* system: file layout,
  launcher syntax, Python import and `--check-config`, systemd unit paths,
  the D-Bus activation file, `appstreamcli` and `desktop-file-validate`. It
  skips `/usr/share/doc` when dpkg's `path-exclude` masks documentation
  (Ubuntu's default) and finds the QML/plugin directories on both `/usr/lib`
  and `/usr/lib64` layouts.

Fedora note: the `%files` globs (`%{_datadir}/icons/hicolor/*/apps/…`,
`%{_libdir}/qt6/…`) match what CMake installs under
`KDE_INSTALL_USE_QT_SYS_PATHS`, which `CMAKE_INSTALL_PREFIX=/usr` enables
automatically — `check-files` reported nothing unpackaged on the Fedora 45
build.

## Service and caching notes

* The daemon runs as a **systemd user service**, not a system service: it owns
  the session-bus name `dna.phoenix.Agent`, needs the GUI session for
  `kdialog`, and must write `~/.config/phoenix-dna` as the desktop user.
* The D-Bus activation file makes the first plasmoid call start the daemon even
  when the user unit has not been enabled. `SystemdService=phoenix.service`
  keeps the start under systemd. Do not ship a preset that enables the unit
  unless the distribution's policy wants the daemon at every login; both
  paths work.
* Under `DESTDIR`, `cmake --install` never runs `gtk-update-icon-cache` or
  `kbuildsycoca6`. Distro file triggers handle the icon cache and the desktop
  database; add a `kbuildsycoca6` scriptlet only if the distribution does not
  already trigger it.
* `phoenix-dna-agent` restarts cleanly with
  `systemctl --user restart phoenix.service`; the settings window tells the
  user this after an LLM key change. Keep the unit name if you can, or patch
  that message and the D-Bus `SystemdService=` entry together.

## Versioning

The version lives in `agent/phoenix_dna/__init__.py`. CMake reads it before
`project()`, so the QML modules' `kde-qmlmodule.version` files, the AppStream
release and the Python package all carry the same number. `shell/metadata.json`
still has its own `KPlugin.Version` and is checked with a warning at configure
time; bump it with the Python version.

## Checking the package

```sh
DESTDIR="$PWD/instroot" cmake --install build
appstreamcli validate instroot/usr/share/metainfo/org.phoenixdna.agent.metainfo.xml
desktop-file-validate instroot/usr/share/applications/org.phoenixdna.agent.desktop
sh -n instroot/usr/bin/phoenix-dna-agent
PYTHONPATH=instroot/usr/share/phoenix-dna/agent \
    python3 -m phoenix_dna --check-config
```

The last command prints the effective configuration; it needs a MiniMax key
only to actually run a prompt, not to check the configuration.

The Python test suite is independent of CMake:

```sh
cd agent
python3 -m venv .venv
./.venv/bin/pip install pytest
./.venv/bin/python -m pytest -q
```

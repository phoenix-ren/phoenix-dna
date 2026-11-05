// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#set document(title: "Quickstart — Phoenix DNA")
#import "/layout.typ": *
#show: page.with(title: "Quickstart", subtitle: "From nothing to a talking panel widget, on the three distributions the recipes were verified on.", active: "quickstart")

= Requirements

Phoenix DNA needs *KDE Plasma 6.4 or newer* (that is where `plasma_add_applet`
first appeared) with *Qt 6.7 or newer*, PipeWire's `pw-cat`, and Python 3.11+.

#table(
  columns: (1.4fr, 1fr),
  table.header([Distro], [Status]),
  [Fedora 45+], [verified on a fresh Fedora 45 VM],
  [Arch, CachyOS], [verified on a CachyOS VM],
  [Kubuntu 26.04, Ubuntu 25.10+, Debian sid], [verified on Kubuntu 26.04.1],
  [Debian 13 (trixie), Ubuntu 24.04], [✗ Plasma/Qt too old — cannot build],
)

Recommended companions, installed with the package where the distro allows:
`ffmpeg` (read answers aloud from files), `kdialog` + `sudo` (administrator
rights on demand) and a TeX engine (`texlive-xetex`) to render LaTeX formulas
to PDF. Everything optional stays optional at runtime.

= Fedora

From the release's binary package (built by the project's CI on `fedora:45`):

#cmd([one command, straight from the release], ```bash
dnf install \
  https://github.com/phoenix-ren/phoenix-dna/releases/download/v0.2.0/phoenix-dna-0.2.0-1.fc45.x86_64.rpm
```)

Or build it yourself from the recipe, the way a packager would:

#cmd([build the RPM from the released spec], ```bash
curl -LO https://github.com/phoenix-ren/phoenix-dna/releases/download/v0.2.0/phoenix-dna.spec
sudo dnf builddep phoenix-dna.spec
rpmbuild -ba --define "_sourcedir $PWD" phoenix-dna.spec
sudo dnf install ~/rpmbuild/RPMS/*/phoenix-dna-0.2.0-1.*.rpm
```)

= Arch Linux and CachyOS

#cmd([from the AUR], ```bash
yay -S phoenix-dna        # or: paru -S phoenix-dna
```)

Or with `makepkg` and the released recipe:

#cmd([build the package yourself], ```bash
curl -LO https://github.com/phoenix-ren/phoenix-dna/releases/download/v0.2.0/PKGBUILD
makepkg -si
```)

= Kubuntu 26.04 (and Debian sid)

From the release's binary package:

#cmd([download and install the deb], ```bash
curl -LO https://github.com/phoenix-ren/phoenix-dna/releases/download/v0.2.0/phoenix-dna_0.2.0-1_amd64.deb
sudo apt install ./phoenix-dna_0.2.0-1_amd64.deb
```)

Or build from the released source package:

#cmd([dpkg-buildpackage from the orig tarball], ```bash
base=https://github.com/phoenix-ren/phoenix-dna/releases/download/v0.2.0
curl -LO $base/phoenix-dna_0.2.0.orig.tar.gz
curl -LO $base/phoenix-dna_0.2.0-1.debian.tar.gz
tar xf phoenix-dna_0.2.0.orig.tar.gz && cd phoenix-dna-0.2.0
tar xf ../phoenix-dna_0.2.0-1.debian.tar.gz
sudo apt build-dep .          # or: mk-build-deps -irs
dpkg-buildpackage -us -uc -b
sudo apt install ../phoenix-dna_0.2.0-1_amd64.deb
```)

= After installing

+ *Add the widget*: right-click the panel → *Add widgets* → *Phoenix DNA*.
  The daemon needs no enabling: the first plasmoid call starts it through
  D-Bus activation, and `phoenix.service` (a *user* unit) keeps it across
  logins.

+ *Put a key in the settings*: the panel's gear button → *API keys*. Name any
  OpenAI-compatible endpoint (MiniMax, OpenAI, Groq, OpenRouter or your own)
  and paste its key. Keys live in `~/.config/phoenix-dna/keys.json`, mode 0600.

+ *Say something*: press the record button (or its global shortcut) and talk.
  Press again to transcribe and send. The typed-prompt window and the session
  search have their own shortcuts, rebindable in *System Settings → Shortcuts*.

Everything above is the short version. The
#html.elem("a", attrs: (href: "manual.html"), [manual]) walks every window,
every button and every shortcut in the order a conversation goes.

#cmd([the daemon, when you want to look at it], ```bash
systemctl --user status phoenix.service
journalctl --user -u phoenix.service -f
```)

Saving the LLM key restarts the daemon for you — the settings window
says so when it does. A changed LLM *endpoint* or *model* applies after
`systemctl --user restart phoenix.service`, and the window says so when
it matters.

= Building from a git checkout

#cmd([both halves of the app, from a checkout], ```bash
git clone https://github.com/phoenix-ren/phoenix-dna.git
cd phoenix-dna
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=ON \
    -DPHOENIX_BUILD_DEVTOOLS=OFF -DPHOENIX_UPDATE_CACHES=OFF
cmake --build build --parallel
sudo cmake --install build
systemctl --user restart plasma-plasmashell   # the panel keeps the old plugin otherwise
```)

The full packager's guide — every CMake switch, the installed layout, the
test harness that exercises the recipes end to end — is
#html.elem("a", attrs: (href: repo-url + "/blob/master/PACKAGING.md"), [PACKAGING.md in the repository]).

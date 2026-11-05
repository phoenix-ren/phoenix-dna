# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause
#
# Fedora/RHEL spec for Phoenix DNA.
#
# Build the source archive first:
#   packaging/make-source-archive.sh
# then point rpmbuild at the result:
#   rpmbuild -ba --define "_sourcedir $PWD/dist" packaging/fedora/phoenix-dna.spec
#
# The Source0 URL is the release asset produced by that script. Keep the
# version in this file, agent/phoenix_dna/__init__.py and the other recipes in
# step; packaging/check-versions.sh enforces it.

Name:           phoenix-dna
Version:        0.1.0
Release:        1%{?dist}
Summary:        Voice-first AI agent for the KDE Plasma panel
License:        BSD-2-Clause AND FSFAP AND ISC AND MIT
URL:            https://github.com/phoenix-ren/phoenix-dna
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  extra-cmake-modules
BuildRequires:  gcc-c++
BuildRequires:  gettext
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  qt6-qtsvg-devel
BuildRequires:  kf6-kcoreaddons-devel
BuildRequires:  kf6-kconfig-devel
BuildRequires:  kf6-kconfigwidgets-devel
BuildRequires:  kf6-ki18n-devel
BuildRequires:  kf6-kglobalaccel-devel
BuildRequires:  kf6-kpackage-devel
BuildRequires:  kf6-kwindowsystem-devel
BuildRequires:  kf6-kirigami-devel
BuildRequires:  kf6-ksvg-devel
BuildRequires:  libplasma-devel
BuildRequires:  boost-devel
BuildRequires:  python3
BuildRequires:  systemd-rpm-macros
BuildRequires:  appstream
BuildRequires:  desktop-file-utils

Requires:       libplasma
Requires:       plasma-workspace
Requires:       kwin
Requires:       kf6-kcoreaddons
Requires:       kf6-kconfig
Requires:       kf6-kconfigwidgets
Requires:       kf6-ki18n
Requires:       kf6-kglobalaccel
Requires:       kf6-kpackage
Requires:       kf6-kwindowsystem
Requires:       kf6-kirigami
Requires:       kf6-ksvg
Requires:       qt6-qtbase
Requires:       qt6-qtdeclarative
Requires:       qt6-qtsvg
Requires:       python3-dbus
Requires:       python3-gobject
Requires:       pipewire-utils

Recommends:     ffmpeg-free
Recommends:     kdialog
Recommends:     sudo
Recommends:     texlive-xetex

%description
Phoenix DNA is a desktop-native AI agent for KDE Plasma. It lives in the
panel, takes dictated or typed prompts, runs them through an LLM-driven agent
loop and shows the answer in a native window.

The agent works as the desktop user, can attach files and screenshots to a
prompt, asks for administrator rights through KDE's password dialog when a
command needs them, and can read its answers aloud.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake \
    -DBUILD_TESTING=ON \
    -DPHOENIX_BUILD_DEVTOOLS=OFF \
    -DPHOENIX_UPDATE_CACHES=OFF \
    -DPHOENIX_AGENT_PYTHON=%{_bindir}/python3
%cmake_build

%check
# The tests create QGuiApplication instances and must not need a display.
export QT_QPA_PLATFORM=offscreen
ctest --test-dir %{_vpath_builddir} --output-on-failure

%install
%cmake_install

%post
if [ -x /usr/bin/kbuildsycoca6 ]; then
    /usr/bin/kbuildsycoca6 --noincremental >/dev/null 2>&1 || :
fi

%postun
if [ -x /usr/bin/kbuildsycoca6 ]; then
    /usr/bin/kbuildsycoca6 --noincremental >/dev/null 2>&1 || :
fi

%files
%license %{_datadir}/licenses/phoenix-dna
%doc %{_datadir}/doc/phoenix-dna
%{_bindir}/phoenix-dna-agent
%{_bindir}/phoenix-dna-repl
%{_datadir}/phoenix-dna/agent
%{_datadir}/applications/org.phoenixdna.agent.desktop
%{_datadir}/icons/hicolor/*/apps/org.phoenixdna.*
%{_datadir}/metainfo/org.phoenixdna.agent.metainfo.xml
%{_datadir}/dbus-1/services/dna.phoenix.Agent.service
%{_userunitdir}/phoenix.service
%{_libdir}/qt6/plugins/plasma/applets/org.phoenixdna.agent.so
%{_libdir}/qt6/qml/org/phoenixdna/

%changelog
* Wed Nov 04 2026 Phoenix Ren <ren@phoenixren.space> - 0.1.0-1
- Initial package.

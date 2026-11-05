# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

cmake_minimum_required(VERSION 3.16)

# The version has one home: the Python package the daemon reports over D-Bus.
# Reading it here keeps the CMake project, the QML modules' version files and
# the installed Python package in step. A checkout without the agent (a
# stripped source tree) falls back to the last released number.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/agent/phoenix_dna/__init__.py"
     PHOENIX_VERSION_LINE
     REGEX "^__version__[ \t]*=[ \t]*\"[^\"]+\"")
if(PHOENIX_VERSION_LINE)
    string(REGEX REPLACE
           "^__version__[ \t]*=[ \t]*\"([^\"]+)\".*$" "\\1"
           PHOENIX_VERSION "${PHOENIX_VERSION_LINE}")
    string(STRIP "${PHOENIX_VERSION}" PHOENIX_VERSION)
else()
    set(PHOENIX_VERSION "0.1.0")
endif()

project(plasma-phoenixdna VERSION ${PHOENIX_VERSION} LANGUAGES CXX)

include(FeatureSummary)

# Standard CMake switch: defines BUILD_TESTING (ON by default) and calls
# enable_testing() when it is on. Distribution builds can pass
# -DBUILD_TESTING=OFF to leave the test executables out entirely.
include(CTest)

set(KF6_MIN_VERSION "6.0.0")
set(QT_MIN_VERSION "6.7.0")
set(BOOST_MIN_VERSION "1.83.0")

# Packaging switches ---------------------------------------------------------
#
# The defaults build everything a developer wants. A distribution package
# normally passes:
#
#   -DCMAKE_BUILD_TYPE=Release
#   -DPHOENIX_BUILD_DEVTOOLS=OFF
#   -DPHOENIX_UPDATE_CACHES=OFF
#
# and installs into a DESTDIR. See PACKAGING.md for the full story.

# widgettest/ and waveform/ are developer tools; they are never installed.
option(PHOENIX_BUILD_DEVTOOLS
    "Build the developer-only testbench and waveform renderer" ON)

# The Python daemon is not compiled, but it is part of the application: a
# plasmoid without a daemon cannot do anything. Install it here, or turn this
# off and let the distribution's Python packaging own it.
option(PHOENIX_INSTALL_AGENT
    "Install the Python agent daemon, its launchers and service files" ON)

# AppStream metadata, the READMEs and the licence texts. Turn off when the
# package recipe installs those itself.
option(PHOENIX_INSTALL_APPDATA
    "Install AppStream metadata, documentation and licence files" ON)

# Refreshing the icon and desktop caches makes a `sudo cmake --install` from a
# checkout work immediately. It must never run while DESTDIR is set, so a
# distribution build is safe even with this on; turning it off makes that
# explicit and silences the helper lookup.
option(PHOENIX_UPDATE_CACHES
    "Update the icon and desktop caches after a system install" ON)

find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

# The QML module helpers (ecm_add_qml_module and friends). They come in
# implicitly with plasma_add_applet otherwise, which only worked because
# shell/ happened to be the first subdirectory; the standalone modules
# need them regardless of ordering.
include(ECMQmlModule)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS
    Core
    Gui
    Quick
    Qml
    QuickControls2
    DBus
    Network
)

find_package(KF6 ${KF6_MIN_VERSION} REQUIRED COMPONENTS
    CoreAddons
    Config
    ConfigWidgets
    I18n
    GlobalAccel
    Package
    WindowSystem
)

find_package(Plasma REQUIRED)

# Boost.Histogram (<boost/histogram.hpp>) and the other header-only Boost
# libraries are used by the C++ components. Only the "headers" component is
# needed: Boost::headers carries the include path for every Boost library and
# links no shared objects. The config package that ships with Boost is used
# instead of the deprecated FindBoost module.
find_package(Boost ${BOOST_MIN_VERSION} REQUIRED COMPONENTS headers CONFIG)

# Makes a target use the header-only Boost libraries.
#
# KDE compiles everything with -fno-exceptions, which makes Boost define
# BOOST_NO_EXCEPTIONS and then look for an application-provided
# boost::throw_exception() that no longer ships with Boost.Exception. Turning
# exceptions back on for the target keeps Boost's header-only implementation,
# so the headers alone are enough to compile and link.
function(phoenixdna_use_boost target)
    target_link_libraries(${target} PRIVATE Boost::headers)
    KDE_TARGET_ENABLE_EXCEPTIONS(${target} PRIVATE)
endfunction()

# The plasmoid's metadata is a JSON file without comments; keep its version in
# step with the project. This is a warning, not an error, so a maintainer
# bumping the version in one place still gets a working build.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shell/metadata.json" PHOENIX_METADATA_JSON)
if(NOT PHOENIX_METADATA_JSON MATCHES
        "\"Version\"[ \t]*:[ \t]*\"${PROJECT_VERSION}\"")
    message(WARNING
        "shell/metadata.json does not carry the project version "
        "(${PROJECT_VERSION}); update its KPlugin.Version field.")
endif()

# All components of the widget live in this repository and are built
# together against one install prefix.
add_subdirectory(theme)
add_subdirectory(shell)
add_subdirectory(overlay)
add_subdirectory(resultswindow)
add_subdirectory(search)
add_subdirectory(settingswindow)

# The Python half: the D-Bus agent daemon, its launchers, the systemd user
# unit and the D-Bus activation file.
if(PHOENIX_INSTALL_AGENT)
    add_subdirectory(agent)
endif()

# Developer testbench: hosts the widget in a plain window without installing.
# waveform/ (the offline .wav -> .png renderer) is a developer component too.
if(PHOENIX_BUILD_DEVTOOLS)
    add_subdirectory(widgettest)
    add_subdirectory(waveform)
endif()

# Distribution-facing installed data ----------------------------------------

if(PHOENIX_INSTALL_APPDATA)
    # AppStream describes the applet to software centres. The file carries the
    # project version, so it lives as a template and is configured here.
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/org.phoenixdna.agent.metainfo.xml.in"
        "${CMAKE_CURRENT_BINARY_DIR}/org.phoenixdna.agent.metainfo.xml"
        @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.phoenixdna.agent.metainfo.xml"
            DESTINATION ${KDE_INSTALL_METAINFODIR})

    # The documents a packager and a user need. Distribution recipes may ship
    # their own copies instead (`-DPHOENIX_INSTALL_APPDATA=OFF`).
    install(FILES
                "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
                "${CMAKE_CURRENT_SOURCE_DIR}/PACKAGING.md"
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            DESTINATION ${KDE_INSTALL_DATADIR}/doc/phoenix-dna)

    # The Lucide drawings bring their own licences (see LICENSES/). Install
    # all of them so the notices travel with the package.
    install(FILES
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/BSD-2-Clause.txt"
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/FSFAP.txt"
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/ISC.txt"
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/MIT.txt"
            DESTINATION ${KDE_INSTALL_DATADIR}/licenses/phoenix-dna)
endif()

# CPack is a convenience for maintainers who want a tarball or a quick
# DEB/RPM from the build tree. Distributions should use their native recipes
# (PACKAGING.md); CPack is not a substitute for them.
set(CPACK_PACKAGE_NAME "phoenix-dna")
set(CPACK_PACKAGE_VENDOR "Phoenix DNA contributors")
set(CPACK_PACKAGE_CONTACT "ren@phoenixren.space")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Voice-first AI agent for the KDE Plasma panel")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/phoenix-ren/phoenix-dna")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_DEBIAN_PACKAGE_SECTION "kde")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_RPM_PACKAGE_LICENSE "BSD-2-Clause AND ISC AND MIT AND FSFAP")
set(CPACK_RPM_PACKAGE_GROUP "Unspecified")
include(CPack)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

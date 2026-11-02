# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

cmake_minimum_required(VERSION 3.16)

project(plasma-phoenixdna LANGUAGES CXX)

set(KF6_MIN_VERSION "6.0.0")
set(QT_MIN_VERSION "6.7.0")
set(BOOST_MIN_VERSION "1.83.0")

find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

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

# All components of the widget live in this repository and are built
# together against one install prefix.
add_subdirectory(shell)
add_subdirectory(overlay)
add_subdirectory(pane)
add_subdirectory(search)

# Developer testbench: hosts the widget in a plain window without installing.
add_subdirectory(widgettest)

# Offline .wav → .png waveform renderer (waveform component, step 1).
add_subdirectory(waveform)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

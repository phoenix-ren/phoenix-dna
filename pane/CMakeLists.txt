# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

# Component 3 of 3 — the results pane.
#
# A standalone QML module installed to ${KDE_INSTALL_QMLDIR}/org/phoenixai/pane.
#
# The pane is a transient, semi-transparent dark overlay: when the agent's
# result arrives it fades in above every other view, shows the result, and
# dismisses itself again. It is still a Plasma Dialog — the compositor-managed
# window type — because on Wayland an ordinary window cannot put itself above
# the shell's surfaces.

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Qml Quick QuickControls2)

ecm_add_qml_module(org.phoenixai.pane
    URI org.phoenixai.pane
    VERSION 1.0
    GENERATE_PLUGIN_SOURCE
)

ecm_target_qml_sources(org.phoenixai.pane
    SOURCES
        qml/Pane.qml
)

ecm_finalize_qml_module(org.phoenixai.pane)

# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

# Component 5 — the settings window.
#
# A standalone QML module installed to
# ${KDE_INSTALL_QMLDIR}/org/phoenixdna/settings.
#
# The SettingsWindow is the widget's configuration dialog: the LLM and
# speech-to-text API keys (stored in ~/.config/phoenix-dna/keys.json) and
# the three global hotkeys (rebound through KGlobalAccel). Like the
# results window it is a fully-fledged decorated toplevel, and purely
# presentational — the applet's shell binds the state and routes the
# interaction signals to the controller's settings store.

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Qml Quick QuickControls2)

ecm_add_qml_module(org.phoenixdna.settings
    URI org.phoenixdna.settings
    VERSION 1.0
    GENERATE_PLUGIN_SOURCE
)

ecm_target_qml_sources(org.phoenixdna.settings
    SOURCES
        qml/SettingsWindow.qml
        qml/KeySequenceButton.qml
)

ecm_finalize_qml_module(org.phoenixdna.settings)

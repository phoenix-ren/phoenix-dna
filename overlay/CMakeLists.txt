# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

# Component 2 of 3 — the dictation overlay.
#
# A standalone QML module installed to ${KDE_INSTALL_QMLDIR}/org/phoenixdna/overlay.
# The panel widget imports it and instantiates the Dialog; the Dialog itself is
# provided by org.kde.plasma.core so that it is compositor-managed.

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Qml Quick QuickControls2)

ecm_add_qml_module(org.phoenixdna.overlay
    URI org.phoenixdna.overlay
    VERSION 1.0
    GENERATE_PLUGIN_SOURCE
)

ecm_target_qml_sources(org.phoenixdna.overlay
    SOURCES
        qml/DictationOverlay.qml
        qml/WaitingOverlay.qml
)

ecm_finalize_qml_module(org.phoenixdna.overlay)

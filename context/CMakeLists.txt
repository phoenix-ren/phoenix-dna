# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

# Component 6 — the context view.
#
# A standalone QML module installed to
# ${KDE_INSTALL_QMLDIR}/org/phoenixdna/context.
#
# The context view is the session's context window seen: the ring in the
# corner of the results window says how much of the model's context the
# session on screen has used, and the overlay the ring opens has the
# details — the model, the size of its context window, the tokens the
# session spent, and what they cost.
#
# Both are presentational: the numbers come from the daemon (through
# the shell's agent bridge), and the overlay is a Plasma Dialog, because
# on Wayland an ordinary application window cannot put itself above the
# shell's surfaces.

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Qml Quick QuickControls2)

ecm_add_qml_module(org.phoenixdna.context
    URI org.phoenixdna.context
    VERSION 1.0
    GENERATE_PLUGIN_SOURCE
    DEPENDENCIES
        org.kde.kirigami
        org.kde.plasma.core
        org.phoenixdna.theme
)

ecm_target_qml_sources(org.phoenixdna.context
    SOURCES
        qml/ContextRing.qml
        qml/ContextOverlay.qml
)

ecm_finalize_qml_module(org.phoenixdna.context)

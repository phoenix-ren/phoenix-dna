# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

# Component 4 — the session search overlay.
#
# A standalone QML module installed to ${KDE_INSTALL_QMLDIR}/org/phoenixdna/search.
#
# The SessionSearchOverlay is the widget's history browser: it lists the
# user's previous sessions on the left, under the summary name the daemon
# gave them, and shows the queries and responses of the selected session
# on the right. Typing filters the list — the query is matched fuzzily
# against the names, so every further character narrows the list down.
#
# It is a Plasma Dialog — the compositor-managed window type — because on
# Wayland an ordinary window cannot put itself above the shell's surfaces.

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Qml Quick QuickControls2)

ecm_add_qml_module(org.phoenixdna.search
    URI org.phoenixdna.search
    VERSION 1.0
    GENERATE_PLUGIN_SOURCE
)

ecm_target_qml_sources(org.phoenixdna.search
    SOURCES
        qml/SessionSearchOverlay.qml
)

ecm_finalize_qml_module(org.phoenixdna.search)

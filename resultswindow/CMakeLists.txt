# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

# Component 3 of 3 — the results window.
#
# A standalone QML module installed to
# ${KDE_INSTALL_QMLDIR}/org/phoenixdna/resultswindow.
#
# The ResultsWindow is the widget's conversation window: it opens when a
# run produces its first output, streams the answer and the tool log, and
# stays until the user closes it. It is a fully-fledged, decorated
# toplevel — the compositor moves, resizes, maximizes and closes it with
# the ordinary window decorations, after the frameless Plasma Dialog
# predecessor turned out to be a window class the compositor refuses to
# move or resize interactively.

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Qml Quick QuickControls2)

ecm_add_qml_module(org.phoenixdna.resultswindow
    URI org.phoenixdna.resultswindow
    VERSION 1.0
    GENERATE_PLUGIN_SOURCE
)

ecm_target_qml_sources(org.phoenixdna.resultswindow
    SOURCES
        qml/ResultsWindow.qml
        qml/CpuMeterView.qml
        qml/LedBar.qml
        qml/WireMeterView.qml
        qml/WireRateRow.qml
        qml/WizardView.qml
        qml/QuestionInputs.qml
)

ecm_finalize_qml_module(org.phoenixdna.resultswindow)

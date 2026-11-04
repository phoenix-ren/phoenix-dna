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

# Headless check of the window's scroll-follow behaviour: the conversation
# is polished in several passes after every arrival (the Markdown import,
# the paragraph spacing, the link pass, the formula swap), and each pass
# changes the content's height — so the follow has to survive them, and
# the position has to come back after the formula swap's wholesale
# rewrite of the document.
#
# The test loads the real window from the module above, not a copy.
#
#   scrollfollowtest
add_executable(scrollfollowtest scrollfollowtest.cpp)
target_compile_definitions(scrollfollowtest PRIVATE
    PHOENIX_SCROLLFOLLOW_TEST="${CMAKE_CURRENT_SOURCE_DIR}/qml/ScrollFollowTest.qml")
target_link_libraries(scrollfollowtest PRIVATE
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
)
# The module is loaded by the QML engine from the import path (the build
# tree's bin directory), the way the app loads it.
add_dependencies(scrollfollowtest org.phoenixdna.resultswindow)
add_test(NAME scrollfollowtest COMMAND scrollfollowtest)
set_tests_properties(scrollfollowtest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/bin")

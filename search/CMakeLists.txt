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

if(BUILD_TESTING)

# Headless check of the search's session actions: a session that is
# thrown away cannot be brought back, so the delete button only asks —
# the confirmation names the session, and only the confirmation hands
# it over to be deleted. A session can also be named the user's way,
# and that name stands against the model's own.
#
# The test loads the real overlay from the module above, not a copy.
#
#   sessionactionstest
add_executable(sessionactionstest sessionactionstest.cpp)
target_compile_definitions(sessionactionstest PRIVATE
    PHOENIX_SESSION_ACTIONS_TEST="${CMAKE_CURRENT_SOURCE_DIR}/qml/SessionActionsTest.qml")
target_link_libraries(sessionactionstest PRIVATE
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
)
# The module is loaded by the QML engine from the import path (the build
# tree's bin directory), the way the app loads it.
add_dependencies(sessionactionstest org.phoenixdna.search org.phoenixdna.theme)
add_test(NAME sessionactionstest COMMAND sessionactionstest)
set_tests_properties(sessionactionstest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QML_IMPORT_PATH=${CMAKE_BINARY_DIR}/bin")

endif() # BUILD_TESTING

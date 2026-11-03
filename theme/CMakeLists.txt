# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

# The shared theme singleton and the Markdown presentation helpers.
#
# A standalone QML module installed to ${KDE_INSTALL_QMLDIR}/org/phoenixdna/theme.
# Every other component imports it for its colours, and the applet's shell
# pushes the user's colour-scheme preference into it — so one property
# recolours the whole UI, live. The C++ singletons are the presentation
# workbench for the agent's answers: MarkdownDocument spaces the native
# Markdown view's paragraphs; LatexRenderer compiles a finished answer
# (Markdown + LaTeX math) to PDF via a TeX engine.

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS
    Core
    Gui
    Qml
    Quick
    QuickControls2
)

ecm_add_qml_module(org.phoenixdna.theme
    URI org.phoenixdna.theme
    VERSION 1.0
    GENERATE_PLUGIN_SOURCE
    # Kirigami's theme names the system's own font family, which is what
    # the agent's answers fall back on when no family is chosen for the
    # mode a window is in.
    DEPENDENCIES
        org.kde.kirigami
    SOURCES
        latexrenderer.cpp
        latexrenderer.h
        markdowndocument.cpp
        markdowndocument.h
)

# A QML singleton by pragma, declared to the build the way qt_add_qml_module
# wants (the qmldir entry is only generated for flagged files).
set_source_files_properties(qml/PhoenixTheme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

ecm_target_qml_sources(org.phoenixdna.theme
    SOURCES
        qml/PhoenixTheme.qml
)

ecm_finalize_qml_module(org.phoenixdna.theme)

# QQuickTextDocument (Quick) and QTextDocument (Gui) of the helpers;
# the formula pixel work runs on the global QThreadPool (Core).
target_link_libraries(org.phoenixdna.theme PRIVATE
    Qt6::Gui
    Qt6::Quick
)

# Offline check of the shared math-span fixture: the speech daemon's
# Python detector (agent/phoenix_dna/speech/mathspan.py, exercised by
# the agent test suite) and this renderer must find the same spans and
# map the same Unicode symbols, or the daemon would speak different
# mathematics than the window renders. Both sides run against one JSON
# fixture; this target is the C++ half of that contract.
#
#   mathspantest [fixture.json]
add_executable(mathspantest mathspantest.cpp)
target_include_directories(mathspantest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(mathspantest PRIVATE
    MATHSPAN_FIXTURE="${CMAKE_SOURCE_DIR}/agent/tests/data/mathspans.json")
target_link_libraries(mathspantest PRIVATE Qt6::Core)
add_test(NAME mathspantest COMMAND mathspantest)

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

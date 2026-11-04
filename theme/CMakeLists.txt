# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

# The shared theme singleton, the app's own icon set, and the Markdown
# presentation helpers.
#
# A standalone QML module installed to ${KDE_INSTALL_QMLDIR}/org/phoenixdna/theme.
# Every other component imports it for its colours, and the applet's shell
# pushes the user's colour-scheme preference into it — so one property
# recolours the whole UI, live. The C++ singletons are the presentation
# workbench for the agent's answers: MarkdownDocument spaces the native
# Markdown view's paragraphs; LatexRenderer compiles a finished answer
# (Markdown + LaTeX math) to PDF via a TeX engine. LucideIcons hands out the
# app's own icons: Lucide's line drawings, inked in the colour of the place
# they stand in (see lucideicons.h).

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS
    Core
    Gui
    Qml
    Quick
    QuickControls2
    Svg
)

# The module's own address, in the resources as well: the icons are read
# from there by name.
set(PHOENIX_THEME_URI org.phoenixdna.theme)
string(REPLACE "." "/" PHOENIX_THEME_PATH "${PHOENIX_THEME_URI}")

ecm_add_qml_module(org.phoenixdna.theme
    URI ${PHOENIX_THEME_URI}
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
        lucideicons.cpp
        lucideicons.h
        markdowndocument.cpp
        markdowndocument.h
)

# The app's icon set: Lucide's line drawings, in Lucide's own licence (see
# icons/lucide/ and the LICENSES/ directory). They are shipped inside the
# module, so every window draws the same drawings from the same files,
# wherever the app is installed.
#
# The list is the one the app draws; a drawing that is added to it needs
# an entry here as well, or it is not in the module's resources.
set(PHOENIX_LUCIDE_ICONS
    arrow-left
    arrow-right
    arrow-up
    camera
    chevron-down
    chevron-right
    circle-stop
    circle-x
    clipboard-list
    eye
    eye-off
    info
    keyboard
    megaphone
    mic-audio-lines
    paperclip
    plus
    search
    send
    settings
    x
)

set(phoenix_lucide_files)
foreach(icon ${PHOENIX_LUCIDE_ICONS})
    list(APPEND phoenix_lucide_files "${CMAKE_SOURCE_DIR}/icons/lucide/${icon}.svg")
endforeach()

qt6_add_resources(org.phoenixdna.theme "lucideicons"
    PREFIX "/qt/qml/${PHOENIX_THEME_PATH}/lucide"
    BASE "${CMAKE_SOURCE_DIR}/icons/lucide"
    FILES ${phoenix_lucide_files}
)

# Where the drawings are, for the code that reads them.
target_compile_definitions(org.phoenixdna.theme PRIVATE
    PHOENIX_LUCIDE_RESOURCE_DIR="/qt/qml/${PHOENIX_THEME_PATH}/lucide")

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
# the formula pixel work runs on the global QThreadPool (Core);
# QSvgRenderer (Svg) draws the app's own icons.
target_link_libraries(org.phoenixdna.theme PRIVATE
    Qt6::Gui
    Qt6::Quick
    Qt6::Svg
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

# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

# Component 1 — the panel widget.
#
# Compiled Plasma::Applet that lives in the taskbar. It owns:
#   * the record button and the ring-buffered waveform (C++ side),
#   * the dictation overlay (component 2, `overlay/`),
#   * the results window (component 3, `resultswindow/`),
#   * the session search (component 4, `search/`),
#   * the settings window (component 5, `settingswindow/`).
#
# The other components are separate QML modules installed by their own
# subprojects; this applet imports them.

add_definitions(-DTRANSLATION_DOMAIN=\"plasma_applet_org.phoenixdna.agent\")

plasma_add_applet(org.phoenixdna.agent
    QML_SOURCES
        qml/main.qml
        qml/CompactRepresentation.qml
        qml/WaveformView.qml
    CPP_SOURCES
        phoenixcontroller.cpp
        phoenixcontroller.h
        audioringbuffer.cpp
        audioringbuffer.h
        audiocapture.cpp
        audiocapture.h
        agentbridge.cpp
        agentbridge.h
        conversationmodel.cpp
        conversationmodel.h
        sessiontranscriptmodel.cpp
        sessiontranscriptmodel.h
        cliprecorder.cpp
        cliprecorder.h
        promptattachments.cpp
        promptattachments.h
        pickerhoverservice.cpp
        pickerhoverservice.h
        windowpicture.cpp
        windowpicture.h
        windowscreenshotpicker.cpp
        windowscreenshotpicker.h
        shuttersound.cpp
        shuttersound.h
        settingsstore.cpp
        settingsstore.h
        transcriber.cpp
        transcriber.h
        speaker.cpp
        speaker.h
    RESOURCES
        metadata.json
    GENERATE_APPLET_CLASS
)

# The generated plugin source lives in the build directory, and moc resolves
# Q_PLUGIN_METADATA's FILE argument relative to that directory. Copy the
# metadata next to it so the plugin actually carries its metadata.
configure_file(metadata.json metadata.json COPYONLY)

# The applet's own icon.
#
# The widget picker shows an applet under the icon its metadata names, and
# that name is looked up in the icon theme -- so the artwork goes into the
# theme, under the name the metadata gives it, and the theme's cache is
# refreshed afterwards so a running plasmashell finds it.
#
# The artwork itself is drawn by icons/make-icon.py.
install(FILES ${CMAKE_SOURCE_DIR}/icons/phoenix-dna.svg
        DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps
        RENAME org.phoenixdna.agent.svg)

install(CODE "
    if (NOT DEFINED ENV{DESTDIR})
        find_program(PHOENIX_ICON_CACHE gtk-update-icon-cache)
        if (PHOENIX_ICON_CACHE)
            execute_process(
                COMMAND \${PHOENIX_ICON_CACHE} -q -t -i .
                WORKING_DIRECTORY ${CMAKE_INSTALL_PREFIX}/${KDE_INSTALL_ICONDIR}/hicolor)
        endif()
    endif()
")

# C++ classes exposed to QML must be declared in a header: qmltyperegistrar
# warns "Class X is declared in X.cpp ... registration to QML may fail".

target_include_directories(org.phoenixdna.agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(org.phoenixdna.agent PRIVATE
    Qt6::Gui
    Qt6::Quick
    Qt6::DBus
    Qt6::Network
    Plasma::Plasma
    KF6::CoreAddons
    KF6::I18n
    KF6::GlobalAccel
    KF6::ConfigCore
)

# Offline verifier for the dictation-clip half of the transcription path:
# it feeds a WAV file through ClipRecorder exactly as pcmReady() would
# (including pw-cat's start-up burst) and writes the mono 16 kHz WAV that
# would be uploaded — or, with --stt, uploads it through the real
# Transcriber using the same configuration the plasmoid uses.
#
#   cliptest input.wav [output.wav]
#   cliptest --stt input.wav
add_executable(cliptest
    cliptest.cpp
    cliprecorder.cpp
    cliprecorder.h
    transcriber.cpp
    transcriber.h
)
target_include_directories(cliptest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cliptest PRIVATE
    Qt6::Core
    Qt6::Network
)

# Header-only Boost, e.g. <boost/histogram.hpp>.
phoenixdna_use_boost(org.phoenixdna.agent)

# Check of the screenshot path: it asks KWin for the picture of the
# active window over D-Bus, reads it out of the pipe the way the picker
# does and writes it to a PNG.
#
#   screenshottest [output.png]
add_executable(screenshottest
    screenshottest.cpp
    windowpicture.cpp
    windowpicture.h
)
target_include_directories(screenshottest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(screenshottest PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
)

# The trail of a run — the steps the results window shows while the agent
# works — checked without a window: the order of the steps, the grouping
# of the tool calls, the answer's own place and the steps that are still
# running. Registers with CTest; run `ctest` from the build directory.
#
#   conversationmodeltest
add_executable(conversationmodeltest
    conversationmodeltest.cpp
    conversationmodel.cpp
    conversationmodel.h
)
target_include_directories(conversationmodeltest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(conversationmodeltest PRIVATE
    Qt6::Core
    Qt6::Qml
)
add_test(NAME conversationmodeltest COMMAND conversationmodeltest)

# The transcript of a session — the queries and responses the session
# search shows — checked without a window: the empty turns are dropped,
# the rows are handed over a chunk at a time, and the counters the bar at
# the top of the window reads are true. Registers with CTest; run `ctest`
# from the build directory.
#
#   sessiontranscriptmodeltest
add_executable(sessiontranscriptmodeltest
    sessiontranscriptmodeltest.cpp
    sessiontranscriptmodel.cpp
    sessiontranscriptmodel.h
)
target_include_directories(sessiontranscriptmodeltest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sessiontranscriptmodeltest PRIVATE
    Qt6::Core
    Qt6::Qml
)
add_test(NAME sessiontranscriptmodeltest COMMAND sessiontranscriptmodeltest)

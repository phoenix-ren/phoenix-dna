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
        cliprecorder.cpp
        cliprecorder.h
        promptattachments.cpp
        promptattachments.h
        pickerhoverservice.cpp
        pickerhoverservice.h
        windowscreenshotpicker.cpp
        windowscreenshotpicker.h
        settingsstore.cpp
        settingsstore.h
        transcriber.cpp
        transcriber.h
        agentmode.cpp
        agentmode.h
    RESOURCES
        metadata.json
    GENERATE_APPLET_CLASS
)

# The generated plugin source lives in the build directory, and moc resolves
# Q_PLUGIN_METADATA's FILE argument relative to that directory. Copy the
# metadata next to it so the plugin actually carries its metadata.
configure_file(metadata.json metadata.json COPYONLY)

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

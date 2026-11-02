# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

# A standalone testbench for the panel widget.
#
# It dlopens the applet plugin straight from the build tree, so the real QML
# module (controller singleton, mode enum, CompactRepresentation, waveform)
# registers exactly as it does inside plasmashell — without installing
# anything. The record button still exercises the real capture path.
#
# It also hosts the buffered waveform component (design_docs/
# WAVEFORM_COMPONENT_2.md): given a WAV file, a feed thread delivers it to a
# WaveformBuffer as realtime 25 ms PCM buffers while the WaveformItem draws
# snapshots of the three per-pixel arrays — the audio thread and the UI
# thread are synchronized only by the buffer's mutex, as in the real widget.
#
#   widgettest                                  # applet only, no file
#   widgettest sample.wav                       # + realtime waveform feed
#   widgettest --speed 4 sample.wav             # faster than realtime
#
# Headless verification (no display needed):
#
#   QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
#   widgettest --seconds 1.5 --grab early.png sample.wav

add_executable(widgettest
    widgettest.cpp
    widgettest.qrc
    waveformitem.h
    waveformitem.cpp
    wavfeed.h
    wavfeed.cpp
)

target_compile_definitions(widgettest PRIVATE
    PHOENIX_APPLET_PLUGIN="${CMAKE_BINARY_DIR}/bin/plasma/applets/$<TARGET_FILE_NAME:org.phoenixdna.agent>"
)

target_link_libraries(widgettest PRIVATE
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    waveform_core
)

# The plugin must exist before the testbench can be linked against its path.
add_dependencies(widgettest org.phoenixdna.agent)

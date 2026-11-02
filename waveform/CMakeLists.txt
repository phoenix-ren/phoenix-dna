# SPDX-FileCopyrightText: 2026 Phoenix AI contributors
# SPDX-License-Identifier: BSD-2-Clause

# The waveform component (design_docs/WAVEFORM_COMPONENT_1.md and
# WAVEFORM_COMPONENT_2.md).
#
# waveform_core is the thread-safe, QtCore-only heart of the component:
# the WAV reader and the ring of three per-pixel arrays (min, max, EWMA of
# |x|) with a mutex between the audio and UI threads. The developer tools
# below and widgettest/ link it; when the component is promoted into the
# applet (shell/), this target moves with it.

add_library(waveform_core STATIC
    palette.h
    wavreader.h
    wavreader.cpp
    waveformbuffer.h
    waveformbuffer.cpp
)

target_include_directories(waveform_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(waveform_core PUBLIC
    Qt6::Core
)

# Step 1 of the component: a bespoke offline renderer that turns .wav files
# into static .png waveform images with the same three per-pixel arrays the
# live display uses.
#
#   waveformrender [--width N] [--height N] [--tau-ms T] sample.wav ...
#
# Each render lands next to its input as <name>_waveform.png, ready for
# eyeballing against the tenacity benchmarks in audio_prompt_samples/.
#
# Not installed: this is a development tool.

add_executable(waveformrender
    waveformrender.cpp
)

target_link_libraries(waveformrender PRIVATE
    Qt6::Gui
    waveform_core
)

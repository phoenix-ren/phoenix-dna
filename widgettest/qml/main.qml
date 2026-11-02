// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import org.kde.kirigami as Kirigami

// The actual panel widget, exactly as the panel would render it.
import plasma.applet.org.phoenixai.agent as Phoenix

// The buffered waveform component (design_docs/WAVEFORM_COMPONENT_2.md).
import Phoenix.Waveform 1.0

Rectangle {
    color: Kirigami.Theme.backgroundColor

    Column {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        Phoenix.CompactRepresentation {
            width: parent.width
            height: 88 - Kirigami.Units.smallSpacing * 2
        }

        // Fed from C++ when a .wav argument is given: the buffer receives
        // the file as realtime 25 ms buffers on a separate thread.
        WaveformItem {
            visible: waveFeedRunning
            width: parent.width
            height: parent.height - 88 - Kirigami.Units.smallSpacing
            buffer: waveBuffer
        }
    }

    Component.onCompleted: {
        // Start live capture straight away so the waveform animates; the
        // record button toggles it just like on the real panel. Gated on a
        // context property so plain runs behave exactly as before (the
        // testbench sets it with --record to exercise the capture path
        // headlessly).
        if (waveAutoRecord) {
            Phoenix.PhoenixController.toggleRecording();
        }
    }

    // --stop-after: toggle recording off again mid-run so headless runs can
    // verify the stop path (the waveform must stay on screen, dimmed).
    Timer {
        running: waveAutoStopSeconds > 0
        interval: waveAutoStopSeconds * 1000
        onTriggered: Phoenix.PhoenixController.toggleRecording()
    }
}

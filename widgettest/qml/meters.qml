// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

//
// The meters of the results window, in a plain window: the CPU meter
// of a running command, the pair of rate meters of the conversation
// with the model, and the progress meter of a running transfer.
//
// The testbench loads the applet plugin, so the Plasma components (and
// the theme's colours) are there as they are in the panel — the bare
// qml runtime has no Plasma, and its PC3 labels do not draw.
//

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The meters, from the results window's own module.
import org.phoenixdna.resultswindow
import org.phoenixdna.theme

Rectangle {
    id: root

    color: Kirigami.Theme.backgroundColor

    Component.onCompleted: PhoenixTheme.systemPrefersDark = true

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Kirigami.Units.largeSpacing
        spacing: Kirigami.Units.largeSpacing

        // The CPU meter of a command.
        CpuMeterView {
            Layout.fillWidth: true
            Layout.maximumWidth: 300
            showing: true
            fraction: root.cpuFraction
            peak: root.cpuFraction
            cores: root.cpuFraction * 16
            maxCores: 16
        }

        // The pair of rate meters: the request on top, the answer below.
        WireMeterView {
            Layout.fillWidth: true
            Layout.maximumWidth: 300
            showing: true
            sendFraction: root.sendFraction
            receiveFraction: root.receiveFraction
            sendRate: root.sendRate
            receiveRate: root.receiveRate
        }

        // The transfer meter: the pace of the bytes and the share of
        // the thing that is here.
        TransferMeterView {
            Layout.fillWidth: true
            Layout.maximumWidth: 300
            showing: true
            uploading: false
            done: root.transferDone
            total: 47244640256
            rate: root.transferRate
            rateFraction: root.transferFraction
        }
    }

    // The values the testbench drives, so the meters can be seen at
    // several levels.
    property real cpuFraction: 0.42
    property real sendFraction: 0.25
    property real receiveFraction: 0.75
    property real sendRate: 4096
    property real receiveRate: 65536
    property real transferDone: 11811160064
    property real transferRate: 12582912
    property real transferFraction: 0.62

    // A slow sweep, so a grab catches the meters mid-animation.
    SequentialAnimation {
        running: true
        loops: Animation.Infinite
        NumberAnimation {
            target: root; property: "receiveFraction"
            from: 0.05; to: 1.0; duration: 2500
        }
        NumberAnimation {
            target: root; property: "receiveFraction"
            from: 1.0; to: 0.05; duration: 2500
        }
    }

    // The transfer's share of its total, swept the same way: the
    // progress bar filling is the meter's own animation.
    SequentialAnimation {
        running: true
        loops: Animation.Infinite
        NumberAnimation {
            target: root; property: "transferDone"
            from: 0; to: 47244640256; duration: 6000
        }
        NumberAnimation {
            target: root; property: "transferDone"
            from: 47244640256; to: 0; duration: 6000
        }
    }

    // The meters' geometry, checked where it can be checked exactly: the
    // bar of a level reads its own lit count, and the two readouts keep
    // their columns whatever they say.
    function checkGeometry() {
        const bar = ledBar
        const lit = bar.litCount
        const expected = Math.round(bar.fraction * bar.segmentCount)
        if (lit !== expected) {
            console.warn("meters: the bar lights", lit, "segments, expected", expected)
            return false
        }
        return true
    }

    LedBar {
        id: ledBar
        visible: false
        width: 300
        fraction: root.cpuFraction
    }
}

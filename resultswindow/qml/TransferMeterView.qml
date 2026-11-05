// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

//
// The progress of a running transfer, drawn the way the other meters
// are: a level meter for the pace of the bytes (the same row the wire
// meter uses, "DL" for a download and "UL" for an upload), and a
// progress bar for how much of the thing is here — determinate when
// the far end promised a size, a travelling stripe when it did not,
// with the count beside it either way.
//
// Pure presentation: the pace, the count and the total are handed in,
// already averaged by the metering side (see TransferMeter). The rate
// arrives as a three-second moving average, so the bar glides instead
// of jumping with every report.
//

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

ColumnLayout {
    id: root

    // Which way the bytes move: a download pulls them in, an upload
    // pushes them away. Decides the caption and the accent.
    property bool uploading: false

    // The bytes moved so far, and the bytes promised (0 when the far
    // end did not say). Reals, not ints: a download can outrun a
    // 32-bit count. Normally bound to the agent bridge's TransferMeter.
    property real done: 0.0
    property real total: 0.0

    // The pace of the bytes: the rate (bytes a second) and its share
    // of the peak, which is what the level meter shows.
    property real rate: 0.0
    property real rateFraction: 0.0

    // Whether the meter should show at all: it fades in and out, so a
    // transfer that just ended does not blink away but glides.
    property bool showing: false

    // The colours of the two directions, the ones the wire meter uses:
    // what goes out and what comes in are told apart at a glance.
    readonly property color accent: root.uploading
        ? PhoenixTheme.focusBorderColor
        : PhoenixTheme.successColor

    spacing: Math.round(Kirigami.Units.smallSpacing / 2)

    opacity: root.showing ? 1.0 : 0.0
    visible: root.showing || opacity > 0.0

    Behavior on opacity {
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.InOutQuad
        }
    }

    WireRateRow {
        Layout.fillWidth: true

        caption: root.uploading ? "UL" : "DL"
        fraction: root.rateFraction
        rate: root.rate
        accent: root.accent
    }

    RowLayout {
        Layout.fillWidth: true

        spacing: Kirigami.Units.smallSpacing

        PC3.ProgressBar {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter

            // A known total is an honest bar; an unknown one is a
            // travelling stripe, so the meter still says "alive".
            from: 0.0
            to: 1.0
            value: root.total > 0 ? Math.min(1.0, root.done / root.total) : 0.0
            indeterminate: root.total <= 0
        }

        PC3.Label {
            Layout.alignment: Qt.AlignVCenter

            text: root.total > 0
                ? i18n("%1 of %2 (%3%)",
                       root.formatBytes(root.done),
                       root.formatBytes(root.total),
                       Math.min(100, Math.floor(root.done * 100 / root.total)))
                : root.formatBytes(root.done)
            color: PhoenixTheme.secondaryTextColor
            font.pixelSize: Math.round(Qt.application.font.pixelSize * 0.8)
        }
    }

    // A byte count as a person reads it, in the units the wire meter's
    // readouts use.
    function formatBytes(bytes) {
        if (!bytes || bytes <= 0) {
            return "0 B"
        }
        if (bytes >= 1024 * 1024 * 1024) {
            return (bytes / (1024 * 1024 * 1024)).toFixed(1) + " GB"
        }
        if (bytes >= 1024 * 1024) {
            return (bytes / (1024 * 1024)).toFixed(1) + " MB"
        }
        if (bytes >= 1024) {
            return (bytes / 1024).toFixed(1) + " kB"
        }
        return Math.round(bytes) + " B"
    }
}

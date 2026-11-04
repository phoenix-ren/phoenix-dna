// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

//
// The CPU meter of a running bash command, drawn the way audio gear
// draws a level meter: a row of LED segments that light from the left,
// green through yellow into red, with a peak-hold segment standing
// above the level before it, too, decays.
//
// Pure presentation: the level (0..1 of the machine's whole capacity)
// and its held peak are handed in, already smoothed by the sampling
// side (see CpuMeter). The Behaviors below only keep the glide between
// two samples liquid, so the bar animates while /proc is read a
// modest ten times a second.
//

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

RowLayout {
    id: root

    // The level (0..1) and its peak hold, normally bound to the agent
    // bridge's CpuMeter.
    property real fraction: 0.0
    property real peak: 0.0

    // Whether the meter should show at all: it fades in and out, so a
    // command that just ended does not blink away but glides.
    property bool showing: false

    // An audio meter's scale: everything below 60% is fine, up to 85%
    // is busy, beyond is flat out.
    readonly property real warnFraction: 0.6
    readonly property real hotFraction: 0.85

    readonly property int segmentCount: 28
    // The gap between two LED segments: a hint of separation, no more.
    readonly property int segmentGap: Math.max(1, Math.round(Kirigami.Units.smallSpacing / 4))
    readonly property int litCount: Math.round(root.fraction * root.segmentCount)
    readonly property int peakIndex:
        Math.min(root.segmentCount - 1, Math.round(root.peak * (root.segmentCount - 1)))

    // The level restated for the readout beside the bar: a share of all
    // cores reads better as "how many cores" ("2.4x") than as a
    // percentage of the machine.
    property real cores: 0.0

    // How far \a cores can rise (the machine's core count, passed in by
    // the results window). The readouts' columns are reserved for their
    // widest possible values ("100%", "x<maxCores>"): a value that
    // changes width with every digit would take the bar with it, and
    // the meter would flicker exactly when it animates.
    property int maxCores: 16

    spacing: Kirigami.Units.smallSpacing

    opacity: root.showing ? 1.0 : 0.0
    visible: root.showing || opacity > 0.0

    Behavior on opacity {
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.InOutQuad
        }
    }
    // The samples arrive every 100 ms; the bar should not step with
    // them but glide.
    Behavior on fraction {
        NumberAnimation { duration: 130; easing.type: Easing.OutQuad }
    }
    Behavior on peak {
        NumberAnimation { duration: 160; easing.type: Easing.OutQuad }
    }

    PC3.Label {
        Layout.alignment: Qt.AlignVCenter

        text: "CPU"
        color: PhoenixTheme.secondaryTextColor
        font.pixelSize: Math.round(Qt.application.font.pixelSize * 0.8)
    }

    Item {
        id: bar

        Layout.fillWidth: true
        Layout.alignment: Qt.AlignVCenter
        implicitHeight: Math.round(Kirigami.Units.gridUnit * 0.45)

        // The unlit shell of the scale, so the meter reads as a row of
        // LEDs and not as a bar that grows out of nothing.
        Rectangle {
            anchors.fill: parent
            radius: Math.min(2, height / 4)
            color: PhoenixTheme.textColor
            opacity: 0.08
        }

        Repeater {
            model: root.segmentCount

            delegate: Rectangle {
                id: segment

                required property int index

                // Buckets must be positioned explicitly: a Repeater in a
                // plain Item leaves every delegate at x = 0 (see
                // WaveformView).
                x: segment.index * (width + root.segmentGap)
                width: (bar.width - (root.segmentCount - 1) * root.segmentGap)
                       / root.segmentCount
                height: bar.height
                radius: Math.min(2, height / 4)

                // The zone the segment belongs to: fine, busy or flat
                // out, lit or dark.
                readonly property color zoneColor: {
                    const at = (segment.index + 1) / root.segmentCount
                    if (at > root.hotFraction) {
                        return PhoenixTheme.errorColor
                    }
                    if (at > root.warnFraction) {
                        return PhoenixTheme.warningColor
                    }
                    return PhoenixTheme.successColor
                }
                readonly property bool lit: segment.index < root.litCount
                readonly property bool held: segment.index === root.peakIndex

                color: zoneColor
                opacity: lit ? 0.95 : (held ? 0.55 : 0.14)
            }
        }
    }

    // The readouts keep constant columns: measured for the widest value
    // they can show, right-aligned inside. Only the digits change, never
    // the layout — the LED bar must not breathe with the numbers.
    TextMetrics {
        id: percentMetrics
        font: percentLabel.font
        text: "100%"
    }
    TextMetrics {
        id: coresMetrics
        font: coresLabel.font
        // <maxCores> with a decimal ("x16.0"), or a value beyond it on a
        // machine that reports more cores than it has - the column grows
        // once, at most, and never shrinks back mid-command.
        text: "×" + Math.max(root.maxCores, root.cores).toFixed(1)
    }

    PC3.Label {
        id: percentLabel

        Layout.alignment: Qt.AlignVCenter
        Layout.preferredWidth: percentMetrics.width

        horizontalAlignment: Text.AlignRight
        text: Math.round(root.fraction * 100) + "%"
        color: PhoenixTheme.secondaryTextColor
        font.pixelSize: Math.round(Qt.application.font.pixelSize * 0.8)
    }

    PC3.Label {
        id: coresLabel

        Layout.alignment: Qt.AlignVCenter
        Layout.preferredWidth: coresMetrics.width

        horizontalAlignment: Text.AlignRight
        // Empty rather than hidden below one core: hiding the label
        // would hand its column back to the bar, and a command hovering
        // around one core would toggle the bar's width with it.
        text: root.cores >= 1.0 ? "×" + root.cores.toFixed(1) : ""
        color: PhoenixTheme.secondaryTextColor
        opacity: 0.7
        font.pixelSize: Math.round(Qt.application.font.pixelSize * 0.75)
    }
}

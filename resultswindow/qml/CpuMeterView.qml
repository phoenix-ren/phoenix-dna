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
// The bar itself is LedBar; the wire meter beside it in the trail uses
// the same one, twice.
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

    PC3.Label {
        Layout.alignment: Qt.AlignVCenter

        text: "CPU"
        color: PhoenixTheme.secondaryTextColor
        font.pixelSize: Math.round(Qt.application.font.pixelSize * 0.8)
    }

    LedBar {
        id: bar

        Layout.fillWidth: true
        Layout.alignment: Qt.AlignVCenter

        fraction: root.fraction
        peak: root.peak
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

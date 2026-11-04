// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

//
// One row of the wire meter: a caption, the bar, the rate.
//
// Its own file rather than an inline component of WireMeterView: a
// compiled inline component does not reach the ids of the file that
// declares it, and the row needs its own metrics (the columns of its
// readouts are reserved, so the bar never breathes with the digits).
//

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

RowLayout {
    id: root

    // The caption of the row ("OUT", "IN").
    property string caption: ""

    // The level of the bar (0..1) and the rate it stands for (bytes a
    // second).
    property real fraction: 0.0
    property real rate: 0.0

    // The colour of this direction's bar.
    property color accent: "#ffffff"

    spacing: Kirigami.Units.smallSpacing

    // The readouts' columns are reserved for their widest values, so the
    // bar never breathes with the digits.
    TextMetrics {
        id: captionMetrics
        font.pixelSize: Math.round(Qt.application.font.pixelSize * 0.8)
        text: "888"
    }
    TextMetrics {
        id: rateMetrics
        font: rateLabel.font
        text: "888.9 kB/s"
    }

    PC3.Label {
        Layout.alignment: Qt.AlignVCenter
        Layout.preferredWidth: captionMetrics.width

        horizontalAlignment: Text.AlignRight
        text: root.caption
        color: PhoenixTheme.secondaryTextColor
        font.pixelSize: Math.round(Qt.application.font.pixelSize * 0.8)
    }

    LedBar {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignVCenter

        fraction: root.fraction
        peak: 0.0
        zoned: false
        lowColor: root.accent
        shellColor: root.accent
    }

    PC3.Label {
        id: rateLabel

        Layout.alignment: Qt.AlignVCenter
        Layout.preferredWidth: rateMetrics.width

        horizontalAlignment: Text.AlignRight
        text: formatRate(root.rate)
        color: PhoenixTheme.secondaryTextColor
        font.pixelSize: Math.round(Qt.application.font.pixelSize * 0.8)
    }

    // A rate in bytes a second, as the meter reads it: the number
    // itself, in the unit that fits it.
    function formatRate(bytes) {
        if (!bytes || bytes <= 0) {
            return "0 B/s"
        }
        if (bytes >= 1024 * 1024) {
            return (bytes / (1024 * 1024)).toFixed(1) + " MB/s"
        }
        if (bytes >= 1024) {
            return (bytes / 1024).toFixed(1) + " kB/s"
        }
        return Math.round(bytes) + " B/s"
    }
}

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

//
// One LED bar, the way audio gear draws a level meter: a row of
// segments that light from the left, green through yellow into red.
//
// The bar itself knows nothing about what it measures: it is given a
// level (0..1), a peak hold and the colours to wear. The meters that
// use it — the CPU of a running command, the rate of the conversation
// with the model — decide what those mean, and what the numbers beside
// the bar say.
//

import QtQuick
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

Item {
    id: root

    // The level (0..1) and its peak hold.
    property real fraction: 0.0
    property real peak: 0.0

    // An audio meter's scale: everything below 60% is fine, up to 85%
    // is busy, beyond is flat out.
    readonly property real warnFraction: 0.6
    readonly property real hotFraction: 0.85

    // The colours of the three zones. A meter that does not measure
    // load (the wire's rate, say) passes its own: one colour for the
    // lit segments, one for the shell they sit in.
    property color lowColor: PhoenixTheme.successColor
    property color midColor: PhoenixTheme.warningColor
    property color highColor: PhoenixTheme.errorColor
    property color shellColor: PhoenixTheme.textColor

    // Whether the segments wear the three zones, or a single colour.
    property bool zoned: true

    readonly property int segmentCount: 28
    // The gap between two LED segments: a hint of separation, no more.
    readonly property int segmentGap: Math.max(1, Math.round(Kirigami.Units.smallSpacing / 4))
    readonly property int litCount: Math.round(root.fraction * root.segmentCount)
    readonly property int peakIndex:
        Math.min(root.segmentCount - 1, Math.round(root.peak * (root.segmentCount - 1)))

    implicitHeight: Math.round(Kirigami.Units.gridUnit * 0.45)

    // The samples arrive every 100 ms; the bar should not step with
    // them but glide.
    Behavior on fraction {
        NumberAnimation { duration: 130; easing.type: Easing.OutQuad }
    }
    Behavior on peak {
        NumberAnimation { duration: 160; easing.type: Easing.OutQuad }
    }

    // The unlit shell of the scale, so the meter reads as a row of LEDs
    // and not as a bar that grows out of nothing.
    Rectangle {
        anchors.fill: parent
        radius: Math.min(2, height / 4)
        color: root.shellColor
        opacity: 0.08
    }

    Repeater {
        model: root.segmentCount

        delegate: Rectangle {
            id: segment

            required property int index

            // Buckets must be positioned explicitly: a Repeater inside a
            // plain Item leaves every delegate at x = 0, stacking the whole
            // envelope on top of itself at the left edge.
            x: segment.index * (width + root.segmentGap)
            width: (root.width - (root.segmentCount - 1) * root.segmentGap)
                   / root.segmentCount
            height: root.height
            radius: Math.min(2, height / 4)

            // The zone the segment belongs to: fine, busy or flat out,
            // lit or dark.
            readonly property color zoneColor: {
                if (!root.zoned) {
                    return root.lowColor
                }
                const at = (segment.index + 1) / root.segmentCount
                if (at > root.hotFraction) {
                    return root.highColor
                }
                if (at > root.warnFraction) {
                    return root.midColor
                }
                return root.lowColor
            }
            readonly property bool lit: segment.index < root.litCount
            readonly property bool held: segment.index === root.peakIndex

            color: segment.zoneColor
            opacity: segment.lit ? 0.95 : (segment.held ? 0.55 : 0.14)
        }
    }
}

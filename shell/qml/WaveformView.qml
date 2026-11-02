// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import org.kde.kirigami as Kirigami

/*
 * A DAW-style scrolling waveform.
 *
 * The model exposes one row per bucket, each carrying the minimum and the
 * maximum amplitude observed in that slice of time. Only the envelope is
 * drawn — never raw PCM — so the view stays cheap no matter how much audio is
 * being captured.
 *
 * The oldest sample is on the left and the newest on the right, so as audio
 * arrives the waveform scrolls leftwards, the way it does in a DAW.
 */
Item {
    id: root

    // The AudioRingBuffer model.
    property var model: null

    // Whether the waveform is currently being fed.
    property bool active: false

    implicitHeight: Kirigami.Units.iconSizes.medium
    implicitWidth: Kirigami.Units.gridUnit * 8

    // Bars are normalised against the loudest bucket currently in the ring
    // (its peak-to-peak span, exposed as \c peakSpan), so the loudest slice
    // of recent audio fills the widget and quieter speech keeps its shape
    // whatever the microphone level. Normalising against the running RMS
    // instead saturated almost every bar: speech routinely peaks three to
    // four times its RMS, which drove every bar into the full-height clamp
    // and turned the waveform into a solid block.
    //
    // The floor (≈2% of full scale) keeps pure silence from being amplified
    // into a wall of noise.
    readonly property real levelScale:
        model ? Math.max(model.peakSpan, 655) : 1

    readonly property int rowCount: model ? model.count : 0

    // The ring's fixed capacity (buckets/s × window; 160/s × 0.75 s = 120).
    // Buckets are laid out against it, not against the current count: each
    // slice of time keeps its place on the timeline, the waveform fills
    // left-to-right, and once the ring wraps it scrolls, DAW-style.
    readonly property int capacity: model ? model.capacity : 0

    // The mean band must stand out from the envelope whatever the panel's
    // theme: lighten the base colour on a dark one, darken it on a light
    // one — Qt.lighter() alone would wash the band out on a light panel,
    // where the theme's text and highlight colours are already light.
    function emphasized(baseColor, factor) {
        const luminance = 0.299 * baseColor.r
            + 0.587 * baseColor.g
            + 0.114 * baseColor.b
        return luminance > 0.5 ? Qt.darker(baseColor, factor)
                               : Qt.lighter(baseColor, factor)
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: 1
        color: Kirigami.Theme.textColor
        opacity: 0.15
    }

    Repeater {
        id: repeater

        model: root.model

        delegate: Rectangle {
            required property int index
            required property real minimum
            required property real maximum
            required property real meanLevel

            // Buckets must be positioned explicitly: a Repeater inside a
            // plain Item leaves every delegate at x = 0, stacking the whole
            // envelope on top of itself at the left edge.
            x: root.capacity > 0 ? index * root.width / root.capacity : 0

            // Each bucket occupies an equal slice of the width.
            width: root.capacity > 0 ? root.width / root.capacity : 0
            height: root.height

            color: "transparent"

            Rectangle {
                // The envelope bar for this bucket: its vertical extent spans
                // the minimum and maximum amplitude of the slice. The darker
                // of the two tones goes down first.
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter

                width: Math.max(1, parent.width)
                height: Math.max(1, Math.min(parent.height,
                    (parent.maximum - parent.minimum) / root.levelScale * parent.height))

                color: root.active ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
                opacity: root.active ? 0.9 : 0.25
                // No corner radius: at a few pixels wide, rounded bars read
                // as pills; crisp columns match the DAW look.
                radius: 0
            }

            Rectangle {
                // The mean |level| band, symmetric about the centre line and
                // painted on top of the envelope (painter's algorithm): the
                // lighter tone sandwiched inside the darker one, as in the
                // waveform component design.
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter

                width: Math.max(1, parent.width)
                height: Math.max(1, Math.min(parent.height,
                    parent.meanLevel * 2 / root.levelScale * parent.height))

                color: root.active ? root.emphasized(Kirigami.Theme.highlightColor, 1.45)
                                   : root.emphasized(Kirigami.Theme.textColor, 1.6)
                opacity: root.active ? 0.95 : 0.35
                radius: 0
            }
        }
    }
}

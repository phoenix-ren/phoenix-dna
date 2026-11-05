// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

/*
 * The context ring: how much of the model's context window the session
 * on screen has used.
 *
 * An arc with the share of the window inside it — the percentage the
 * conversation has used, drawn the way a gauge is: from the top,
 * clockwise. The track behind it is the whole window, so what is left
 * of the circle is what is still free.
 *
 * The numbers come from the daemon (see AgentBridge::sessionContext):
 * the tokens the session spent and the size of the model's context
 * window. A window whose size is not known has no share to draw — the
 * ring says so rather than guessing.
 *
 * The arc is coloured by how full the window is: the accent while
 * there is room, the warning when it is nearly full, the error when it
 * is. A conversation that is about to run out of context says so
 * before it does.
 */
Item {
    id: root

    // The context view's picture of the session: the map the daemon
    // answered with (see AgentBridge::sessionContext).
    property var context: null

    // Emitted when the ring is clicked: the details are wanted.
    signal clicked()

    // The size of the model's context window, in tokens; zero when it
    // is not known.
    readonly property int contextLimit: context
        ? Number(context.contextLimit || 0) : 0

    // How much of that window the session takes up: the size of the
    // biggest prompt it sent. This is not the sum of every request's
    // prompt - the conversation is sent again with every turn, so that
    // sum counts the same tokens over and over and says nothing about
    // how much room is left.
    readonly property int contextUsed: context
        ? Number(context.contextUsed || 0) : 0

    // Whether the window's size is known at all: without it there is
    // no share to draw.
    readonly property bool known: contextLimit > 0

    // The share of the window in use, zero to one.
    readonly property real fraction: known
        ? Math.max(0, Math.min(1, contextUsed / contextLimit))
        : 0

    // The share in per cent, as the ring shows it.
    readonly property int percent: Math.round(fraction * 100)

    // The colour of the arc: the accent while there is room, the
    // warning when the window is nearly full, the error when it is.
    readonly property color arcColor: !known
        ? PhoenixTheme.secondaryTextColor
        : (percent >= 90 ? PhoenixTheme.errorColor
                         : (percent >= 75 ? PhoenixTheme.warningColor
                                          : PhoenixTheme.linkColor))

    // The width of the arc's stroke.
    readonly property real strokeWidth: Math.max(2, Math.round(width * 0.1))

    // Whether the pointer is on the ring: the tooltip is up then.
    readonly property bool hovered: pointer.containsMouse

    implicitWidth: Kirigami.Units.iconSizes.large
    implicitHeight: Kirigami.Units.iconSizes.large

    // The arc: the window behind, the used share in front of it.
    Canvas {
        id: arc

        anchors.fill: parent
        antialiasing: true

        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)

            const line = root.strokeWidth
            const radius = Math.min(width, height) / 2 - line / 2
            const cx = width / 2
            const cy = height / 2

            // The track: the whole window, dimmed.
            const track = PhoenixTheme.secondaryTextColor
            ctx.strokeStyle = Qt.rgba(track.r, track.g, track.b, 0.25)
            ctx.lineWidth = line
            ctx.lineCap = "round"
            ctx.beginPath()
            ctx.arc(cx, cy, radius, 0, Math.PI * 2)
            ctx.stroke()

            // The used share: from the top, clockwise.
            if (root.fraction > 0) {
                ctx.strokeStyle = root.arcColor
                ctx.beginPath()
                ctx.arc(cx, cy, radius, -Math.PI / 2,
                        -Math.PI / 2 + Math.PI * 2 * root.fraction)
                ctx.stroke()
            }
        }
    }

    // The share inside the ring, in per cent. A window whose size is
    // not known has no percentage to show.
    Text {
        anchors.centerIn: parent
        width: parent.width - root.strokeWidth * 2
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter

        text: root.known ? root.percent + "%" : "—"
        color: root.known ? PhoenixTheme.textColor
                          : PhoenixTheme.secondaryTextColor
        font.pixelSize: Math.max(8, Math.round(root.width * 0.27))
    }

    // The numbers behind the ring, for the pointer that asks.
    PC3.ToolTip {
        visible: root.hovered
        text: root.known
            ? i18n("Context: %1%% of the window used — %2 of %3 tokens",
                   root.percent,
                   root.contextUsed.toLocaleString(Qt.locale(), "f", 0),
                   root.contextLimit.toLocaleString(Qt.locale(), "f", 0))
            : i18n("Context: the size of the model's context window is not known")
    }

    MouseArea {
        id: pointer

        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor

        onClicked: root.clicked()
    }

    // The arc is painted by hand, so a change of the share asks for a
    // new paint.
    onFractionChanged: arc.requestPaint()
    onKnownChanged: arc.requestPaint()
    onArcColorChanged: arc.requestPaint()

    Component.onCompleted: arc.requestPaint()

    onWidthChanged: arc.requestPaint()
    onHeightChanged: arc.requestPaint()
}

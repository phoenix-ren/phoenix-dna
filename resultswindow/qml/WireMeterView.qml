// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

//
// The rate of the conversation with the model, as a pair of level
// meters: what goes out and what comes in.
//
// The two bars stand one above the other — the request on top, the
// answer below — and each is scaled against its own peak since the run
// began, so a bar shows how close this moment is to the busiest the
// conversation has been. The numbers beside them are the real rates in
// bytes a second, which is what the bars are about.
//
// The rates arrive already averaged (see WireMeter): a three-second
// exponentially weighted moving average over the bytes that crossed
// the wire, computed over the time that actually passed between them,
// so the bursts of a streamed answer are read as the rate they are.
//

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

ColumnLayout {
    id: root

    // The two rates (bytes a second) and their peaks, normally bound to
    // the agent bridge's WireMeter.
    property real sendRate: 0.0
    property real receiveRate: 0.0
    property real sendPeak: 0.0
    property real receivePeak: 0.0

    // The share of the peak each rate is: what the bars show.
    property real sendFraction: 0.0
    property real receiveFraction: 0.0

    // Whether the meter should show at all: it fades in and out, so a
    // turn that just ended does not blink away but glides.
    property bool showing: false

    // The turn of the conversation the traffic belongs to.
    property int turn: 0

    // The colours of the two directions: the request that goes out and
    // the answer that comes in are told apart at a glance.
    property color sendColor: PhoenixTheme.focusBorderColor
    property color receiveColor: PhoenixTheme.successColor

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
        caption: "OUT"
        fraction: root.sendFraction
        rate: root.sendRate
        accent: root.sendColor
    }

    WireRateRow {
        Layout.fillWidth: true
        caption: "IN"
        fraction: root.receiveFraction
        rate: root.receiveRate
        accent: root.receiveColor
    }
}

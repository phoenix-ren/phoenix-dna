// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

/*
 * The waiting overlay.
 *
 * A prompt the daemon has not started yet: it waits its turn in the
 * daemon's queue, behind the run that is going on. The results pane
 * opens when the daemon *starts* a run, so a queued prompt leaves the
 * screen unchanged — and a screen that does not react is the one thing
 * that makes a user press the button again.
 *
 * This is that reaction: a small, centred window with a spinning wheel
 * and a line of text, saying that the agent is getting to it. It is
 * transient (the daemon's own pacing decides how long it is up) and it
 * takes no input: there is nothing to press, only to wait for.
 */
PlasmaCore.Dialog {
    id: waitingOverlay

    // Centred on the display: the wait belongs to no window in
    // particular, and the middle of the screen is where an eye lands
    // when nothing else is happening.
    location: PlasmaCore.Types.Floating
    type: PlasmaCore.Dialog.WindowType.DialogWindow

    // The dialog surface itself is bare: the card below paints its own
    // themed backdrop (dark in the dark scheme, light in the light one).
    backgroundHints: PlasmaCore.Dialog.NoBackground

    // Above everything: the wait is the app's news.
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    hideOnWindowDeactivate: false

    visible: false

    // The size of the card: small enough to stay out of the way, big
    // enough to hold the wheel and its line of text.
    readonly property real cardWidth: 320
    readonly property real cardHeight: 140

    // The line under the wheel. The default names what the agent is
    // doing — it is busy with the run before this one, and the prompt
    // waits behind it.
    property string message: i18n("Reticulating splines...")

    // How many prompts wait, when the daemon says. Zero means none:
    // the overlay is not up.
    property int queuedPrompts: 0

    // The overlay is up while a prompt waits in the daemon's queue. It
    // goes down as soon as the queue is empty — the results pane opens
    // then, with the run in it.
    readonly property bool waiting: queuedPrompts > 0

    onWaitingChanged: {
        if (waiting) {
            openOverlay();
        } else {
            closeOverlay();
        }
    }

    function openOverlay() {
        width = cardWidth;
        height = cardHeight;
        x = Screen.virtualX + Math.round((Screen.width - width) / 2);
        y = Screen.virtualY + Math.round((Screen.height - height) / 2);
        visible = true;
        // The compositor places the window itself, so the position is
        // applied again a tick later - it then wins over the placement
        // the compositor chose.
        Qt.callLater(function() {
            x = Screen.virtualX + Math.round((Screen.width - width) / 2);
            y = Screen.virtualY + Math.round((Screen.height - height) / 2);
        });
    }

    function closeOverlay() {
        visible = false;
    }

    // The dialog's default property is mainItem, so the card is a bare
    // child (see the session search overlay).
    mainItem: Rectangle {
        id: card

        implicitWidth: waitingOverlay.cardWidth
        implicitHeight: waitingOverlay.cardHeight
        radius: Kirigami.Units.largeSpacing

        // The themed backdrop, painted here because the dialog surface
        // itself is bare: dark and near-black in the dark scheme, the
        // theme's own light one otherwise.
        color: PhoenixTheme.backgroundColor
        border.width: 1
        border.color: PhoenixTheme.backdropBorderColor

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Kirigami.Units.gridUnit
            spacing: Kirigami.Units.largeSpacing

            // The wheel: it turns for as long as the wait lasts.
            PC3.BusyIndicator {
                Layout.alignment: Qt.AlignHCenter

                running: waitingOverlay.waiting
                visible: running
            }

            // The line of text: what is happening, in as few words as
            // it takes.
            Text {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter

                text: waitingOverlay.message
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                font.pixelSize: Math.max(
                    9, Math.round(Qt.application.font.pixelSize * 0.9))
                color: PhoenixTheme.textColor
            }
        }
    }
}

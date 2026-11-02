// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.plasmoid
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami

import plasma.applet.org.phoenixdna.agent as Phoenix

/*
 * The panel widget's compact representation.
 *
 * Layout, left to right:
 *   * the three mode buttons      (root / user / web, active one checked on)
 *   * the ring-buffered waveform  (the last five seconds of audio)
 *   * the record button, which becomes a stop button while recording
 */
Item {
    id: root

    // The current privilege mode. Owned by the C++ controller.
    property int mode: Phoenix.PhoenixController.mode

    // While recording, the record button turns into a stop button.
    property bool recording: Phoenix.PhoenixController.recording

    // The waveform model: one row per bucket, each with a minimum and maximum.
    property var waveformModel: Phoenix.PhoenixController.waveform

    signal recordClicked()

    // Around 3 cm of physical panel width, so the waveform stays legible on
    // any display. Screen reports physical dots per millimetre; dividing out
    // the device pixel ratio yields the logical pixels QML geometry uses.
    //
    // These are attached Layout properties because that is what plasmashell's
    // applet container negotiates against: a plain implicitWidth alone lets a
    // crowded panel shrink the widget below its content size, which clipped
    // the waveform and record button at the panel edge. The minimum is the
    // layout's own minimum (the buttons, waveform and record button cannot
    // shrink further), never below the 3 cm floor — a smaller allocation
    // made the row overflow its background and spill the record button onto
    // the neighbouring widgets.
    readonly property real minimumWidthCm: 3
    readonly property real minimumWidthPx:
        minimumWidthCm * 10 * Screen.pixelDensity / Screen.devicePixelRatio

    Layout.minimumWidth: Math.max(layout.implicitWidth, minimumWidthPx)
    Layout.preferredWidth: Math.max(layout.implicitWidth, minimumWidthPx)

    implicitWidth: Layout.preferredWidth
    implicitHeight: layout.implicitHeight

    RowLayout {
        id: layout
        anchors.fill: parent
        spacing: Kirigami.Units.smallSpacing

        // ---- Left: the three mode buttons ----------------------------------
        // One button per mode, the active one shown checked, so every mode is
        // reachable with a single click — no cycling, no popup. Selecting the
        // system (root) mode makes the daemon pop the KDE password dialog.
        Flow {
            id: modeButtons

            spacing: Kirigami.Units.smallSpacing

            Repeater {
                model: [
                    { mode: Phoenix.AgentMode.Root, label: i18n("Root") },
                    { mode: Phoenix.AgentMode.User, label: i18n("User") },
                    { mode: Phoenix.AgentMode.Web,  label: i18n("Web") }
                ]

                delegate: PC3.ToolButton {
                    id: modeButton

                    required property var modelData

                    // Whether this button's mode is the one the controller
                    // currently runs in.
                    readonly property bool active: root.mode === modelData.mode

                    width: Kirigami.Units.iconSizes.medium
                    height: Kirigami.Units.iconSizes.medium

                    icon.name: Phoenix.PhoenixController.modeIconName(modelData.mode)
                    icon.width: Kirigami.Units.iconSizes.small
                    icon.height: Kirigami.Units.iconSizes.small
                    icon.color: active ? Kirigami.Theme.highlightColor : "white"

                    // The controller owns the state; the buttons only display
                    // it. `checked` paints the active mode as pressed/on.
                    checked: active

                    onClicked: Phoenix.PhoenixController.mode = modelData.mode

                    Accessible.name: modelData.label

                    PC3.ToolTip {
                        text: modelData.label
                    }
                }
            }
        }

        // ---- Middle: the waveform ------------------------------------------
        Phoenix.WaveformView {
            id: waveformView
            objectName: "waveformView"

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: Kirigami.Units.gridUnit * 4

            model: root.waveformModel
            active: root.recording
        }

        // ---- Right: the record button --------------------------------------
        PC3.ToolButton {
            id: recordButton

            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
            Layout.preferredHeight: Kirigami.Units.iconSizes.medium

            // The record button becomes a stop button while recording. No
            // colour tint: Breeze's media-record is already the red circle
            // the convention calls for.
            icon.name: root.recording ? "media-playback-stop" : "media-record"
            icon.width: Kirigami.Units.iconSizes.small
            icon.height: Kirigami.Units.iconSizes.small

            onClicked: Phoenix.PhoenixController.toggleRecording()

            PC3.ToolTip {
                // Mentions the global hotkey when one is bound, so the
                // pairing of button and shortcut is discoverable without a
                // detour through System Settings.
                text: {
                    const action = root.recording ? i18n("Stop") : i18n("Record");
                    const hotkey = Phoenix.PhoenixController.recordShortcut;
                    return hotkey.length > 0 ? i18n("%1 (%2)", action, hotkey) : action;
                }
            }
        }
    }
}

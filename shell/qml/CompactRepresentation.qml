// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.kde.ksvg as KSvg

import plasma.applet.org.phoenixdna.agent as Phoenix

/*
 * The panel widget's compact representation.
 *
 * Layout, left to right:
 *   * the keyboard button, which opens the prompt editor for a typed query
 *   * the ring-buffered waveform  (the last five seconds of audio)
 *   * the record button, which becomes a stop button while recording
 *   * the camera button, which photographs a window of the desktop
 *   * the search button, which opens the session search
 *   * the settings button, which toggles the settings window
 *
 * The waveform and the record button next to it are one target: the
 * waveform is the button's hover and press area, so hovering it lights
 * the button up and clicking it starts or stops the recording.
 *
 * Every button sits in a PlasmaCore.ToolTipArea rather than carrying a
 * PC3.ToolTip. The widget-level tooltip Plasma shows for this applet
 * ("Phoenix DNA") is rendered in one shared tooltip window, and a
 * ToolTipArea hands that window over to whatever is hovered — so a
 * button's tooltip replaces the widget's instead of stacking on top of
 * it, which is what a PC3.ToolTip's separate window would do.
 */
Item {
    id: root

    // The PlasmoidItem (the applet interface declared in main.qml) this
    // representation belongs to. plasmashell injects it when it creates the
    // compact representation; the full representation — the same component,
    // shown expanded in the popup — receives no injection, hence the null
    // default instead of a required property (which would abort creation).
    property PlasmoidItem plasmoidItem: null

    // The buttons' tooltips stand down while the applet is expanded: the
    // popup owns the cursor then, exactly like the widget-level tooltip
    // Plasma shows for the rest of the widget (CompactApplet.qml in the
    // desktop package does `active: !plasmoidItem.expanded`).
    readonly property bool tooltipsEnabled:
        plasmoidItem ? !plasmoidItem.expanded : true

    // While recording, the record button turns into a stop button.
    property bool recording: Phoenix.PhoenixController.recording

    // While the finished clip is with the transcription service.
    property bool transcribing: Phoenix.PhoenixController.transcribing

    // The waveform model: one row per bucket, each with a minimum and maximum.
    property var waveformModel: Phoenix.PhoenixController.waveform

    // The record target: the microphone button and the waveform next to it
    // act as one button. The pointer over the waveform lights the record
    // button up, and a click on the waveform presses it.
    readonly property bool waveformHovered: waveformPointer.containsMouse

    signal recordClicked()

    // Emitted when the settings window should open (the gear button).
    signal settingsClicked()

    // Whether the settings window is up, so the gear button can act as
    // a toggle: one press opens the pane, the next puts it away. main.qml
    // owns the truth here — it is the window's own visibility.
    property bool settingsOpen: false

    // Around 1.5 cm of physical panel width, so the waveform stays legible on
    // any display. Screen reports physical dots per millimetre; dividing out
    // the device pixel ratio yields the logical pixels QML geometry uses.
    //
    // These are attached Layout properties because that is what plasmashell's
    // applet container negotiates against: a plain implicitWidth alone lets a
    // crowded panel shrink the widget below its content size, which clipped
    // the waveform and record button at the panel edge. The minimum is the
    // layout's own minimum (the buttons, waveform and record button cannot
    // shrink further), never below the 1.5 cm floor — a smaller allocation
    // made the row overflow its background and spill the record button onto
    // the neighbouring widgets.
    readonly property real minimumWidthCm: 1.5
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

        // ---- Far left: the keyboard button --------------------------------
        // Opens the prompt editor: the query is typed rather than spoken,
        // and nothing is recorded at all. The global hotkey does the same
        // from any application.
        PlasmaCore.ToolTipArea {
            id: keyboardArea

            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
            Layout.preferredHeight: Kirigami.Units.iconSizes.medium

            location: Plasmoid.location
            active: root.tooltipsEnabled

            // Mentions the global hotkey when one is bound, like the
            // record button's tooltip does.
            mainText: {
                const action = i18n("Type a query");
                const hotkey = Phoenix.PhoenixController.promptShortcut;
                return hotkey.length > 0 ? i18n("%1 (%2)", action, hotkey) : action;
            }

            PC3.ToolButton {
                id: keyboardButton
                objectName: "keyboardButton"

                anchors.fill: parent

                icon.name: "input-keyboard"
                icon.width: Kirigami.Units.iconSizes.small
                icon.height: Kirigami.Units.iconSizes.small

                // The editor is open while the button is pressed: a toggle,
                // like the search button - pressing it again puts the editor
                // away again.
                checked: Phoenix.PhoenixController.promptEditorOpen
                icon.color: checked ? Kirigami.Theme.highlightColor
                                    : Kirigami.Theme.textColor

                // While the microphone is open the dictation owns the
                // prompt: the button waits for it to end rather than
                // fighting it for the overlay.
                enabled: !root.recording && !root.transcribing

                onClicked: Phoenix.PhoenixController.togglePromptEditor()

                Accessible.name: keyboardArea.mainText
            }
        }

        // ---- Left: the waveform --------------------------------------------
        Phoenix.WaveformView {
            id: waveformView
            objectName: "waveformView"

            Layout.fillWidth: true
            Layout.fillHeight: true
            // How far the waveform may be squeezed when the panel is
            // crowded. Note that this is not what makes the applet wide:
            // the row's implicit width is, and that is the sum of what the
            // children ask for - the waveform's own implicitWidth among
            // them.
            Layout.minimumWidth: Kirigami.Units.gridUnit * 3

            model: root.waveformModel
            active: root.recording

            // The waveform is part of the record button's target: the two
            // act as one button. A click on the waveform presses the record
            // button - starting or stopping the recording - and hovering it
            // lights the button up.
            //
            // The mouse area also carries the pointing hand: it says that
            // the waveform is something to click. (A plain item cannot: only
            // the mouse area and the pointer handlers know a cursor shape.)
            MouseArea {
                id: waveformPointer

                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor

                // animateClick() rather than clicked(): the button shows its
                // pressed state for a moment, exactly as it does when it is
                // clicked itself.
                onClicked: recordButton.animateClick()
            }
        }

        // ---- Right: the record button --------------------------------------
        PlasmaCore.ToolTipArea {
            id: recordArea

            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
            Layout.preferredHeight: Kirigami.Units.iconSizes.medium

            location: Plasmoid.location
            active: root.tooltipsEnabled

            // Mentions the global hotkey when one is bound, so the
            // pairing of button and shortcut is discoverable without a
            // detour through System Settings.
            mainText: {
                const action = root.recording ? i18n("Stop") : i18n("Record");
                const hotkey = Phoenix.PhoenixController.recordShortcut;
                return hotkey.length > 0 ? i18n("%1 (%2)", action, hotkey) : action;
            }

            // The hover surface of the record button, drawn while the
            // pointer is on the waveform: the waveform is part of the
            // button's target, so the button lights up there as well.
            //
            // The button cannot be told about it (AbstractButton.hovered is
            // read-only), so the theme's own hover surface is drawn here
            // instead - the very one the button draws for itself. It sits
            // below the button, so the icon stays on top of it.
            KSvg.FrameSvgItem {
                anchors.fill: parent
                imagePath: "widgets/button"
                prefix: ["toolbutton-hover", "normal"]
                visible: root.waveformHovered
            }

            PC3.ToolButton {
                id: recordButton

                anchors.fill: parent

                // The record button becomes a stop button while recording. A
                // microphone rather than a red circle signals speech input;
                // no colour tint needed either way.
                icon.name: root.recording ? "media-playback-stop" : "audio-input-microphone"
                icon.width: Kirigami.Units.iconSizes.small
                icon.height: Kirigami.Units.iconSizes.small

                onClicked: Phoenix.PhoenixController.toggleRecording()

                Accessible.name: recordArea.mainText
            }
        }

        // ---- Right: the camera ----------------------------------------------
        // The camera photographs a window of the desktop and attaches the
        // picture to the prompt that is being composed; the global
        // shortcut does the same from anywhere.
        PlasmaCore.ToolTipArea {
            id: screenshotArea

            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
            Layout.preferredHeight: Kirigami.Units.iconSizes.medium

            location: Plasmoid.location
            active: root.tooltipsEnabled

            mainText: {
                const picker = Phoenix.PhoenixController.screenshotPicker;
                // While the picker is up, the tooltip names the window
                // a click would photograph.
                if (picker.picking && picker.targetTitle.length > 0) {
                    return i18n("Screenshot: %1", picker.targetTitle);
                }
                const shortcut = picker.shortcut;
                return shortcut.length > 0
                    ? i18n("Screenshot a window (%1)", shortcut)
                    : i18n("Screenshot a window");
            }

            PC3.ToolButton {
                id: screenshotButton
                objectName: "screenshotButton"

                anchors.fill: parent

                icon.name: "camera-photo"
                icon.width: Kirigami.Units.iconSizes.small
                icon.height: Kirigami.Units.iconSizes.small
                // While the picker waits for a click, the button says so.
                icon.color: Phoenix.PhoenixController.screenshotPicker.picking
                    ? Kirigami.Theme.highlightColor
                    : Kirigami.Theme.textColor

                onClicked: Phoenix.PhoenixController.startScreenshot()

                Accessible.name: screenshotArea.mainText
            }
        }

        // ---- Far right: the search button -----------------------------------
        // Opens the session search: the user's previous sessions on the
        // left, the selected one's queries and responses on the right.
        // A toggle: pressing it while the search is up closes it again.
        PlasmaCore.ToolTipArea {
            id: searchArea

            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
            Layout.preferredHeight: Kirigami.Units.iconSizes.medium

            location: Plasmoid.location
            active: root.tooltipsEnabled

            // As with the record button, the global hotkey is named
            // when one is bound.
            mainText: {
                const action = i18n("Search sessions");
                const hotkey = Phoenix.PhoenixController.searchShortcut;
                return hotkey.length > 0 ? i18n("%1 (%2)", action, hotkey) : action;
            }

            PC3.ToolButton {
                id: searchButton
                objectName: "searchButton"

                anchors.fill: parent

                icon.name: "edit-find"
                icon.width: Kirigami.Units.iconSizes.small
                icon.height: Kirigami.Units.iconSizes.small

                // The search is open while the button is pressed.
                checked: Phoenix.PhoenixController.sessionSearchOpen
                icon.color: checked ? Kirigami.Theme.highlightColor
                                    : Kirigami.Theme.textColor

                onClicked: Phoenix.PhoenixController.toggleSessionSearch()

                Accessible.name: searchArea.mainText
            }
        }

        // ---- Far right: the settings button ---------------------------------
        // Toggles the settings window: the API keys and the global hotkeys.
        PlasmaCore.ToolTipArea {
            id: settingsArea

            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
            Layout.preferredHeight: Kirigami.Units.iconSizes.medium

            location: Plasmoid.location
            active: root.tooltipsEnabled

            mainText: i18n("Settings — API keys and global shortcuts")

            PC3.ToolButton {
                id: settingsButton
                objectName: "settingsButton"

                anchors.fill: parent

                icon.name: "settings-configure"
                icon.width: Kirigami.Units.iconSizes.small
                icon.height: Kirigami.Units.iconSizes.small

                // The pane is up while the button is checked; as with the
                // search button, the next press puts it away.
                checked: root.settingsOpen
                icon.color: checked ? Kirigami.Theme.highlightColor
                                    : Kirigami.Theme.textColor

                onClicked: root.settingsClicked()

                Accessible.name: settingsArea.mainText
            }
        }
    }
}

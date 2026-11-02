// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami

/*
 * The settings window — the widget's configuration dialog.
 *
 * Two groups:
 *
 *   * API keys — the LLM key (the agent daemon's MiniMax key) and the
 *     speech-to-text key, with the speech-to-text provider chosen here
 *     too. They are stored in ~/.config/phoenix-dna/keys.json; the LLM key
 *     applies after a daemon restart, the speech-to-text key with the
 *     next dictation.
 *
 *   * Global shortcuts — the three system-wide hotkeys (recording,
 *     screenshot, session search), captured with the keyboard and bound
 *     through KGlobalAccel, so the bindings show up in System Settings →
 *     Shortcuts and survive restarts.
 *
 * Like the results window, this is a fully-fledged decorated toplevel
 * (compositor-owned geometry, size persisted in the XDG state directory)
 * on the app's dark backdrop, and it is purely presentational: the shell
 * binds the state (current keys, current bindings, the store's status
 * line) and routes the interaction signals to the controller's settings
 * store, which owns the files and the KGlobalAccel actions.
 */
Window {
    id: window

    // ---- Bound state (the shell owns all of it) --------------------------
    // The live global bindings, in native notation ("" when unbound).
    property string recordShortcut: ""
    property string searchShortcut: ""
    property string screenshotShortcut: ""

    // The stored API keys and the speech-to-text provider ("minimax" /
    // "openai"). Password-echoed below, revealed by the eye button.
    property string llmApiKey: ""
    property string sttApiKey: ""
    property string sttProvider: "minimax"

    // Which keys.json field each key is stored as, shown next to it —
    // the speech-to-text field follows the provider.
    property string llmKeyField: "minimax_api_key"
    property string sttKeyField: "minimax_api_key"

    property string keysFilePath: ""

    // Set when the environment overrides the stored keys (env beats the
    // key file); the fields warn then, because a stored key would be
    // ignored.
    property bool llmKeyFromEnvironment: false
    property bool sttKeyFromEnvironment: false

    // The settings store's answer to the last action.
    property string statusMessage: ""
    property bool statusIsError: false

    // ---- Interaction (the shell routes these to the store) ---------------
    signal llmKeySaveRequested(string key)
    signal sttKeySaveRequested(string key)
    signal sttProviderChangeRequested(string provider)
    signal shortcutCaptureRequested(string role, int key, int modifiers)
    signal shortcutClearRequested(string role)

    title: i18n("Settings — Phoenix DNA")
    flags: Qt.Dialog

    // The compositor resizes the window; these are the floors.
    minimumWidth: Math.round(Kirigami.Units.gridUnit * 28)
    minimumHeight: Math.round(Kirigami.Units.gridUnit * 20)

    // The remembered size, or a default wide enough for the form.
    width: geometryStore.windowWidth > 0
        ? geometryStore.windowWidth
        : Math.round(Kirigami.Units.gridUnit * 34)
    height: geometryStore.windowHeight > 0
        ? geometryStore.windowHeight
        : Math.round(Kirigami.Units.gridUnit * 26)

    visible: false

    // The remembered window size, persisted across restarts in the XDG
    // state directory — the same arrangement as the results window.
    onVisibleChanged: {
        if (!visible && width > 0 && height > 0) {
            geometryStore.windowWidth = Math.round(width)
            geometryStore.windowHeight = Math.round(height)
        } else if (visible) {
            // Re-opened, the editor fields drop unsaved edits: what is
            // shown is what is stored. (The text cannot be bound — these
            // fields are editable, so a binding would be broken by the
            // first edit, like the transcript fields elsewhere in the
            // app; values are pushed imperatively.)
            llmKeyFieldEdit.text = window.llmApiKey
            sttKeyFieldEdit.text = window.sttApiKey
            sttProviderCombo.currentIndex = window.sttProvider === "openai" ? 1 : 0
        }
    }

    onLlmApiKeyChanged: llmKeyFieldEdit.text = window.llmApiKey
    onSttApiKeyChanged: sttKeyFieldEdit.text = window.sttApiKey
    onSttProviderChanged: sttProviderCombo.currentIndex =
        window.sttProvider === "openai" ? 1 : 0

    // Closing from the title bar hides instead of destroying: the shell
    // owns the window's lifetime.
    onClosing: (close) => {
        close.accepted = false
        hide()
    }

    function openSettings() {
        show()
        raise()
        requestActivate()
    }

    Settings {
        id: geometryStore

        category: "Geometry"
        location: StandardPaths.writableLocation(StandardPaths.GenericStateLocation)
            + "/phoenix-dna/settings-window.ini"
        property real windowWidth: 0
        property real windowHeight: 0
    }

    Item {
        id: content

        anchors.fill: parent
        focus: true

        // Esc closes the window — unless a capture button is listening,
        // in which case the Esc belongs to it (it cancels the capture;
        // the button consumes the event before it can arrive here).
        Keys.onEscapePressed: window.close()

        // The dark, semi-transparent backdrop, as the results window:
        // fixed rather than theme-derived, so the light text on it is
        // fixed too.
        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0.07, 0.07, 0.09, 0.93)
        }

        ColumnLayout {
            id: body

            anchors.fill: parent
            anchors.margins: Kirigami.Units.gridUnit
            spacing: Kirigami.Units.smallSpacing

            Flickable {
                id: scroller

                Layout.fillWidth: true
                Layout.fillHeight: true

                contentWidth: width
                contentHeight: settingsColumn.implicitHeight
                clip: true

                PC3.ScrollBar.vertical: PC3.ScrollBar {}

                ColumnLayout {
                    id: settingsColumn

                    width: scroller.width
                    spacing: Kirigami.Units.smallSpacing

                    // ---- API keys ---------------------------------------
                    Kirigami.Heading {
                        level: 2
                        text: i18n("API keys")
                        color: "#f2f2f2"
                    }

                    Kirigami.Heading {
                        level: 5
                        text: i18n("LLM (MiniMax)")
                        color: "#cfcfcf"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.TextField {
                            id: llmKeyFieldEdit
                            objectName: "llmKeyFieldEdit"

                            Layout.fillWidth: true
                            echoMode: llmRevealButton.checked ? TextInput.Normal
                                                              : TextInput.Password
                            placeholderText: i18n("No key set")
                            onAccepted: if (text !== window.llmApiKey) {
                                window.llmKeySaveRequested(text)
                            }
                        }

                        PC3.ToolButton {
                            id: llmRevealButton

                            icon.name: checked ? "password-show-off" : "password-show-on"
                            checkable: true
                            checked: false

                            PC3.ToolTip {
                                text: llmRevealButton.checked ? i18n("Hide the key")
                                                              : i18n("Show the key")
                            }
                        }

                        PC3.Button {
                            text: i18n("Save")
                            enabled: llmKeyFieldEdit.text !== window.llmApiKey
                            onClicked: window.llmKeySaveRequested(llmKeyFieldEdit.text)
                        }
                    }

                    // Stored-as note, the environment warning and the
                    // restart hint: what a saved key means for the daemon.
                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("Stored as %1 in %2. The agent daemon reads it when "
                            + "it starts — restart it to apply: systemctl --user "
                            + "restart phoenix.service",
                            window.llmKeyField, window.keysFilePath)
                    }
                    HintLabel {
                        Layout.fillWidth: true
                        visible: window.llmKeyFromEnvironment
                        color: "#f67400"
                        text: i18n("An API key environment variable is set and takes "
                            + "precedence over the stored key.")
                    }

                    Kirigami.Separator {
                        Layout.fillWidth: true
                        Layout.topMargin: Kirigami.Units.smallSpacing
                    }

                    // ---- Speech-to-text ----------------------------------
                    Kirigami.Heading {
                        level: 5
                        text: i18n("Speech-to-text")
                        color: "#cfcfcf"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Provider")
                            color: "#e8e8e8"
                        }

                        PC3.ComboBox {
                            id: sttProviderCombo

                            model: [i18n("MiniMax"), i18n("OpenAI")]

                            // Choosing a provider is a change like any
                            // other: the window reports it, the store
                            // writes it, the refreshed state comes back
                            // through the bound properties above.
                            onActivated: (index) =>
                                window.sttProviderChangeRequested(index === 1 ? "openai"
                                                                              : "minimax")
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.TextField {
                            id: sttKeyFieldEdit
                            objectName: "sttKeyFieldEdit"

                            Layout.fillWidth: true
                            echoMode: sttRevealButton.checked ? TextInput.Normal
                                                              : TextInput.Password
                            placeholderText: i18n("No key set")
                            onAccepted: if (text !== window.sttApiKey) {
                                window.sttKeySaveRequested(text)
                            }
                        }

                        PC3.ToolButton {
                            id: sttRevealButton

                            icon.name: checked ? "password-show-off" : "password-show-on"
                            checkable: true
                            checked: false

                            PC3.ToolTip {
                                text: sttRevealButton.checked ? i18n("Hide the key")
                                                              : i18n("Show the key")
                            }
                        }

                        PC3.Button {
                            text: i18n("Save")
                            enabled: sttKeyFieldEdit.text !== window.sttApiKey
                            onClicked: window.sttKeySaveRequested(sttKeyFieldEdit.text)
                        }
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        text: window.sttProvider === "openai"
                              ? i18n("Stored as %1 in %2; the next dictation uses it.",
                                     window.sttKeyField, window.keysFilePath)
                              : i18n("The MiniMax key shared with the agent (%1); the "
                                + "next dictation uses it. Stored in %2.",
                                window.sttKeyField, window.keysFilePath)
                    }
                    HintLabel {
                        Layout.fillWidth: true
                        visible: window.sttKeyFromEnvironment
                        color: "#f67400"
                        text: i18n("An API key environment variable is set and takes "
                            + "precedence over the stored key.")
                    }

                    Kirigami.Separator {
                        Layout.fillWidth: true
                        Layout.topMargin: Kirigami.Units.smallSpacing
                    }

                    // ---- Global shortcuts --------------------------------
                    Kirigami.Heading {
                        level: 2
                        text: i18n("Global shortcuts")
                        color: "#f2f2f2"
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("Click a shortcut, then press the new key combination. "
                            + "Escape cancels, Backspace clears. Combinations already "
                            + "taken by the system do not arrive.")
                    }

                    // One row per hotkey. The binding is resolved in the
                    // delegate (not baked into the model) so it stays
                    // live: re-binding one row updates exactly that row.
                    Repeater {
                        model: [
                            { role: "record", label: i18n("Toggle recording") },
                            { role: "screenshot", label: i18n("Screenshot a window") },
                            { role: "search", label: i18n("Search sessions") }
                        ]

                        delegate: RowLayout {
                            id: shortcutRow

                            required property var modelData

                            readonly property string currentShortcut:
                                modelData.role === "record" ? window.recordShortcut
                              : modelData.role === "screenshot" ? window.screenshotShortcut
                              : window.searchShortcut

                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            PC3.Label {
                                text: shortcutRow.modelData.label
                                color: "#e8e8e8"
                                Layout.fillWidth: true
                            }

                            KeySequenceButton {
                                sequenceText: shortcutRow.currentShortcut
                                onSequenceCaptured: (key, modifiers) =>
                                    window.shortcutCaptureRequested(
                                        shortcutRow.modelData.role, key, modifiers)
                                onSequenceCleared:
                                    window.shortcutClearRequested(shortcutRow.modelData.role)
                            }

                            // The discoverable way to unbind, next to the
                            // Backspace-the-capture one.
                            PC3.ToolButton {
                                icon.name: "edit-clear-remove"
                                enabled: shortcutRow.currentShortcut.length > 0
                                onClicked:
                                    window.shortcutClearRequested(shortcutRow.modelData.role)

                                PC3.ToolTip {
                                    text: i18n("Remove the shortcut")
                                }
                            }
                        }
                    }
                }
            }

            // ---- The store's answer to the last action -----------------
            Kirigami.SelectableLabel {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing

                visible: window.statusMessage.length > 0
                wrapMode: Text.WordWrap
                text: window.statusMessage
                // Fixed rather than theme-derived: it sits on the
                // window's own dark backdrop. Complaints red, successes
                // green.
                color: window.statusIsError ? "#f66151" : "#57e389"
            }
        }
    }

    // A small grey footnote, like the attributions under the chat bubbles
    // of the results window.
    component HintLabel: PC3.Label {
        wrapMode: Text.WordWrap
        color: "#9a9a9a"
        font.pixelSize: Math.max(9, Math.round(Qt.application.font.pixelSize * 0.8))
    }
}

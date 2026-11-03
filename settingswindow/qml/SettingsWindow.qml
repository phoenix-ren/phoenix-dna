// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

/*
 * The settings window — the widget's configuration dialog.
 *
 * Three groups:
 *
 *   * Appearance — the colour scheme of the app's own windows: follow
 *     the system, or force light or dark, and the font the agent's
 *     answers are set in, one family per mode. The choice applies to
 *     every window the moment it is made, through the shared theme
 *     singleton.
 *
 *   * API keys — the LLM key (the agent daemon's MiniMax key) and the
 *     speech-to-text key, with the speech-to-text provider chosen here
 *     too. They are stored in ~/.config/phoenix-dna/keys.json; the LLM key
 *     applies after a daemon restart, the speech-to-text key with the
 *     next dictation.
 *
 *   * Global shortcuts — the four system-wide hotkeys (recording,
 *     screenshot, session search, typed prompt), captured with the
 *     keyboard and bound through KGlobalAccel, so the bindings show up in
 *     System Settings → Shortcuts and survive restarts.
 *
 * Like the results window, this is a fully-fledged decorated toplevel
 * (compositor-owned geometry, size persisted in the XDG state directory)
 * on the app's own backdrop, and it is purely presentational: the shell
 * binds the state (current keys, current bindings, the store's status
 * line) and routes the interaction signals to the controller's settings
 * store, which owns the files and the KGlobalAccel actions.
 */
Window {
    id: window

    // ---- Bound state (the shell owns all of it) --------------------------
    // The colour scheme of the app's windows: "system" (the default),
    // "light" or "dark".
    property string theme: "system"

    // The families the agent's answers are set in, one per mode ("" —
    // the system's own font — when nothing is chosen).
    property string lightFont: ""
    property string darkFont: ""

    // The sizes the agent's answers are set in, in pixels, one per mode
    // (0 — the size the window would give the answer anyway — when
    // nothing is chosen).
    property int lightFontSize: 0
    property int darkFontSize: 0

    // The families on offer, as they are listed: every one this
    // system has, with the system's own font first. The list is built
    // when the window opens, so a family installed in the meantime
    // shows up.
    property var fontChoices: []

    // The live global bindings, in native notation ("" when unbound).
    property string recordShortcut: ""
    property string searchShortcut: ""
    property string screenshotShortcut: ""
    property string promptShortcut: ""

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
    signal themeChangeRequested(string theme)
    signal lightFontChangeRequested(string family)
    signal darkFontChangeRequested(string family)
    signal lightFontSizeChangeRequested(int size)
    signal darkFontSizeChangeRequested(int size)
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
            themeCombo.currentIndex = window.themeIndex(window.theme)
            // The font list is rebuilt on every opening: what this
            // system has may have changed since the last look.
            window.fontChoices = window.buildFontChoices()
            lightFontCombo.currentIndex =
                window.fontIndex(window.fontChoices, window.lightFont)
            darkFontCombo.currentIndex =
                window.fontIndex(window.fontChoices, window.darkFont)
            // The sizes: the chosen one, or the size the window would
            // give the answer anyway.
            lightFontSizeBox.value = window.lightFontSize > 0
                ? window.lightFontSize
                : PhoenixTheme.lightAnswerFontSize
            darkFontSizeBox.value = window.darkFontSize > 0
                ? window.darkFontSize
                : PhoenixTheme.darkAnswerFontSize
        }
    }

    onLlmApiKeyChanged: llmKeyFieldEdit.text = window.llmApiKey
    onSttApiKeyChanged: sttKeyFieldEdit.text = window.sttApiKey
    onSttProviderChanged: sttProviderCombo.currentIndex =
        window.sttProvider === "openai" ? 1 : 0
    onThemeChanged: themeCombo.currentIndex = window.themeIndex(window.theme)
    onLightFontChanged: lightFontCombo.currentIndex =
        window.fontIndex(window.fontChoices, window.lightFont)
    onDarkFontChanged: darkFontCombo.currentIndex =
        window.fontIndex(window.fontChoices, window.darkFont)
    onLightFontSizeChanged: lightFontSizeBox.value = window.lightFontSize > 0
        ? window.lightFontSize
        : PhoenixTheme.lightAnswerFontSize
    onDarkFontSizeChanged: darkFontSizeBox.value = window.darkFontSize > 0
        ? window.darkFontSize
        : PhoenixTheme.darkAnswerFontSize

    // The combo's row for a scheme value ("system" → 0, "light" → 1,
    // "dark" → 2), kept in one place for the two pushes above.
    function themeIndex(theme) {
        return theme === "light" ? 1 : theme === "dark" ? 2 : 0
    }

    // The families on offer: every one this system has, sorted by name,
    // with the system's own font first — it needs nothing installed,
    // and it is where most people want to be anyway.
    function buildFontChoices() {
        const families = Qt.fontFamilies()
        families.sort(function (a, b) {
            const first = a.toLowerCase()
            const second = b.toLowerCase()
            return first < second ? -1 : (first > second ? 1 : 0)
        })
        const choices = [{
            family: "",
            label: i18n("System default"),
            note: i18n("The system's own font.")
        }]
        for (let i = 0; i < families.length; ++i) {
            choices.push({
                family: families[i],
                label: families[i],
                note: ""
            })
        }
        return choices
    }

    // The face a mode's answers are set in — what the preview below
    // the list is shown in.
    function previewFamily(dark) {
        const chosen = dark ? darkFont : lightFont
        return chosen.length > 0 ? chosen : PhoenixTheme.systemFontFamily
    }

    // The size the preview is shown in: the one chosen for the mode,
    // kept within what the window can show without growing.
    function previewSize(dark) {
        const size = dark ? PhoenixTheme.darkAnswerFontSize
                          : PhoenixTheme.lightAnswerFontSize
        return Math.max(9, Math.min(size, 28))
    }

    // The row of a family in the list. A family that is not on this
    // system — or one that is no longer offered — has no row of its
    // own and falls back to the system's font.
    function fontIndex(choices, family) {
        for (let i = 0; i < choices.length; ++i) {
            if (choices[i].family === family) {
                return i
            }
        }
        return 0
    }

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

        // The semi-transparent backdrop, as the results window: painted
        // by the window itself after the chosen colour scheme, so the
        // text on it is fixed with it. The PC3 controls keep following
        // the Plasma theme, which "system" (the default) agrees with.
        Rectangle {
            anchors.fill: parent
            color: PhoenixTheme.backgroundColor
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

                    // ---- Appearance ---------------------------------------
                    Kirigami.Heading {
                        level: 2
                        text: i18n("Appearance")
                        color: PhoenixTheme.headingTextColor
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Color scheme")
                            color: PhoenixTheme.textColor
                        }

                        PC3.ComboBox {
                            id: themeCombo

                            model: [i18n("Follow system"), i18n("Light"), i18n("Dark")]

                            // Choosing a scheme is a change like any
                            // other: the window reports it, the store
                            // writes it, and the shell pushes the
                            // refreshed value into the theme singleton
                            // every window reads — so this very window
                            // (and the others, open or not) recolour
                            // the moment the choice is made.
                            onActivated: (index) =>
                                window.themeChangeRequested(
                                    ["system", "light", "dark"][index])
                        }
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("Applies to this window, the conversation "
                            + "window and the session search. The panel widget "
                            + "itself keeps matching the panel it lives in.")
                    }

                    // ---- The fonts of the answers -------------------------
                    // One family and one size per mode: the light mode
                    // is meant to be read like a news article, the dark
                    // one like a screen that never sleeps. The two are
                    // chosen independently of each other and of the
                    // colour scheme — the scheme decides which of the
                    // two applies.
                    //
                    // A grid, so the two rows line up: the same
                    // columns, the same widths.
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 4
                        columnSpacing: Kirigami.Units.smallSpacing
                        rowSpacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Light mode font")
                            color: PhoenixTheme.textColor
                        }

                        PC3.ComboBox {
                            id: lightFontCombo
                            objectName: "lightFontCombo"

                            // The two lists share a column, so they
                            // are as wide as each other.
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 14

                            model: window.fontChoices
                            textRole: "label"

                            // Every entry is shown in its own face: a
                            // name says little about a font, the
                            // letterforms say everything.
                            delegate: PC3.ItemDelegate {
                                required property var model
                                required property int index

                                width: ListView.view.width
                                text: model.label
                                highlighted: lightFontCombo.highlightedIndex
                                    === index
                                font.family: model.family.length > 0
                                    ? model.family
                                    : PhoenixTheme.systemFontFamily
                            }

                            onActivated: (index) =>
                                window.lightFontChangeRequested(
                                    window.fontChoices[index].family)
                        }

                        // The Plasma popup grows to the height of the
                        // whole list — and the list is every font on
                        // the system, taller than the screen, so it
                        // cannot be scrolled. Its height is capped
                        // here, and its width is half as much wider
                        // again, so the longer family names are
                        // readable.
                        Binding {
                            target: lightFontCombo.popup
                            property: "width"
                            value: lightFontCombo.width * 1.5
                        }
                        Binding {
                            target: lightFontCombo.popup
                            property: "height"
                            value: Math.min(
                                lightFontCombo.popup.contentItem.contentHeight,
                                Screen.desktopAvailableHeight * 0.6)
                        }

                        PC3.Label {
                            text: i18n("px")
                            color: PhoenixTheme.textColor
                        }

                        PC3.SpinBox {
                            id: lightFontSizeBox
                            objectName: "lightFontSizeBox"

                            Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                            from: 8
                            to: 48
                            // Only the user's own changes are stored
                            // (valueModified); a value set from here
                            // is the store's, not the user's.
                            onValueModified:
                                window.lightFontSizeChangeRequested(value)
                        }

                        // The face itself, in a line of text: the
                        // choice is seen, not only named.
                        PC3.Label {
                            Layout.columnSpan: 4
                            Layout.fillWidth: true
                            // Inset from the controls above it, so the
                            // sample is read as a sample and not as
                            // another label of the form.
                            Layout.leftMargin: Kirigami.Units.gridUnit * 2

                            text: i18n("The quick brown fox jumps over the lazy dog")
                            color: PhoenixTheme.primaryTextColor
                            font.family: window.previewFamily(false)
                            font.pixelSize: window.previewSize(false)
                            elide: Text.ElideRight
                        }

                        PC3.Label {
                            text: i18n("Dark mode font")
                            color: PhoenixTheme.textColor
                        }

                        PC3.ComboBox {
                            id: darkFontCombo
                            objectName: "darkFontCombo"

                            // The two lists share a column, so they
                            // are as wide as each other.
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 14

                            model: window.fontChoices
                            textRole: "label"

                            // The same preview as the light mode's
                            // list: the face, shown in its own
                            // letterforms.
                            delegate: PC3.ItemDelegate {
                                required property var model
                                required property int index

                                width: ListView.view.width
                                text: model.label
                                highlighted: darkFontCombo.highlightedIndex
                                    === index
                                font.family: model.family.length > 0
                                    ? model.family
                                    : PhoenixTheme.systemFontFamily
                            }

                            onActivated: (index) =>
                                window.darkFontChangeRequested(
                                    window.fontChoices[index].family)
                        }

                        // The same two corrections as the light mode's
                        // list: a height that can be scrolled and a
                        // width that fits the names.
                        Binding {
                            target: darkFontCombo.popup
                            property: "width"
                            value: darkFontCombo.width * 1.5
                        }
                        Binding {
                            target: darkFontCombo.popup
                            property: "height"
                            value: Math.min(
                                darkFontCombo.popup.contentItem.contentHeight,
                                Screen.desktopAvailableHeight * 0.6)
                        }

                        PC3.Label {
                            text: i18n("px")
                            color: PhoenixTheme.textColor
                        }

                        PC3.SpinBox {
                            id: darkFontSizeBox
                            objectName: "darkFontSizeBox"

                            Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                            from: 8
                            to: 48
                            onValueModified:
                                window.darkFontSizeChangeRequested(value)
                        }

                        PC3.Label {
                            Layout.columnSpan: 4
                            Layout.fillWidth: true
                            Layout.leftMargin: Kirigami.Units.gridUnit * 2

                            text: i18n("The quick brown fox jumps over the lazy dog")
                            color: PhoenixTheme.primaryTextColor
                            font.family: window.previewFamily(true)
                            font.pixelSize: window.previewSize(true)
                            elide: Text.ElideRight
                        }
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("These set the font of the agent's answers "
                            + "alone; the rest of the window keeps the system's "
                            + "font. Whichever mode the window is in decides "
                            + "which of the two applies. In the light mode a "
                            + "serif belongs there — Newsreader was drawn for "
                            + "news articles, and Noto Serif is on every "
                            + "system; on a dark ground a serif is tiring, so a "
                            + "sans does.")
                    }

                    Kirigami.Separator {
                        Layout.fillWidth: true
                        Layout.topMargin: Kirigami.Units.smallSpacing
                    }

                    // ---- API keys ---------------------------------------
                    Kirigami.Heading {
                        level: 2
                        text: i18n("API keys")
                        color: PhoenixTheme.headingTextColor
                    }

                    Kirigami.Heading {
                        level: 5
                        text: i18n("LLM (MiniMax)")
                        color: PhoenixTheme.subheadingTextColor
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
                        color: PhoenixTheme.warningColor
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
                        color: PhoenixTheme.subheadingTextColor
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Provider")
                            color: PhoenixTheme.textColor
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
                        color: PhoenixTheme.warningColor
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
                        color: PhoenixTheme.headingTextColor
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
                            { role: "search", label: i18n("Search sessions") },
                            { role: "prompt", label: i18n("Type a query") }
                        ]

                        delegate: RowLayout {
                            id: shortcutRow

                            required property var modelData

                            readonly property string currentShortcut:
                                modelData.role === "record" ? window.recordShortcut
                              : modelData.role === "screenshot" ? window.screenshotShortcut
                              : modelData.role === "prompt" ? window.promptShortcut
                              : window.searchShortcut

                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            PC3.Label {
                                text: shortcutRow.modelData.label
                                color: PhoenixTheme.textColor
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
                // The palette's own semantics: complaints red, successes
                // green.
                color: window.statusIsError ? PhoenixTheme.errorColor
                                            : PhoenixTheme.successColor
            }
        }
    }

    // A small grey footnote, like the attributions under the chat bubbles
    // of the results window.
    component HintLabel: PC3.Label {
        wrapMode: Text.WordWrap
        color: PhoenixTheme.secondaryTextColor
        font.pixelSize: Math.max(9, Math.round(Qt.application.font.pixelSize * 0.8))
    }
}

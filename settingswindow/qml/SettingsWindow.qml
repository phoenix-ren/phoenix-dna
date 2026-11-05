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
 *     too. They are stored in ~/.config/phoenix-dna/keys.json; saving the
 *     LLM key restarts the daemon to put it in force, the speech-to-text
 *     key with the next dictation.
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
    // the app's own default for the mode — when nothing is chosen;
    // "system" for the system's own font).
    property string lightFont: ""
    property string darkFont: ""

    // The sizes the agent's answers are set in, in pixels, one per mode
    // (0 — the app's own size for the mode — when nothing is chosen).
    property int lightFontSize: 0
    property int darkFontSize: 0

    // The families on offer, as they are listed: every one this
    // system has, with the two special rows first — the app's own
    // default for the mode, and the system's own font. One list per
    // mode, since the app's default differs between them. The lists
    // are built when the window opens, so a family installed in the
    // meantime shows up.
    property var lightFontChoices: []
    property var darkFontChoices: []

    // The live global bindings, in native notation ("" when unbound).
    property string recordShortcut: ""
    property string searchShortcut: ""
    property string screenshotShortcut: ""
    property string promptShortcut: ""

    // The stored API keys and the endpoints that go with them. The
    // endpoint is the whole of the configuration: an OpenAI-compatible
    // base URL names the service, and the daemon works out the
    // provider from the host. Password-echoed below, revealed by the
    // eye button.
    property string llmApiKey: ""
    property string llmProvider: "minimax"
    property string llmBaseUrl: ""
    property string llmModel: ""
    // How many tokens the model holds at once: its context window.
    // Zero when nothing is named — the provider's own default is in
    // force then, and a server of the user's own has none until the
    // user says how much its model holds.
    property int llmContextWindow: 0
    property string sttApiKey: ""
    property string sttProvider: "minimax"
    property string sttEndpoint: ""
    property string sttModel: ""
    property string ttsEndpoint: ""
    property string ttsModel: ""

    // The store's changes, pushed in when they happen: the endpoint
    // and the model of each function come as a pair, so they arrive
    // together. The names differ from the store's own ("...Changed")
    // so that binding the properties does not clash with them.
    signal llmEndpointUpdated()
    signal sttEndpointUpdated()
    signal ttsEndpointUpdated()

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

    // Read-aloud: whether the daemon speaks the answers, when it starts
    // speaking them ("final"/"stream"), the voice id it synthesizes
    // with, which engine speaks it ("minimax" / "openai" / "local"), and
    // whether the environment overrides any of it.
    property bool speechEnabled: false
    property string speechReadMode: "final"
    property string speechVoice: ""
    property string speechProvider: "minimax"
    readonly property string speechVoiceDefault: "English_expressive_narrator"

    // How fast the spoken answers play; 1.0 is the speed the voice was
    // spoken at.
    property real speechPlaybackSpeed: 1.0
    property bool speechFromEnvironment: false

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
    signal llmProviderChangeRequested(string provider)
    signal llmEndpointSaveRequested(string endpoint, string model)
    signal llmContextWindowSaveRequested(int tokens)
    signal sttKeySaveRequested(string key)
    signal sttProviderChangeRequested(string provider)
    signal sttEndpointSaveRequested(string endpoint, string model)
    signal ttsEndpointSaveRequested(string endpoint, string model)
    signal shortcutCaptureRequested(string role, int key, int modifiers)
    signal shortcutClearRequested(string role)
    signal speechEnabledChangeRequested(bool enabled)
    signal speechReadModeChangeRequested(string mode)
    signal speechVoiceChangeRequested(string voice)
    signal speechProviderChangeRequested(string provider)
    signal speechPlaybackSpeedChangeRequested(real speed)
    signal speechTestRequested()

    title: i18n("Settings — Phoenix DNA")
    flags: Qt.Dialog

    // The window's own icon: the app's avatar, as the taskbar entry of
    // the settings window shows it - the same mark every other window of
    // the app carries.
    //
    // QML has no window-icon API (QIcon is not a QML type), so the icon
    // is put on the window from C++ (see AppIcon).
    Component.onCompleted: AppIcon.setWindowIcon(window)

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
            llmEndpointEdit.text = window.llmBaseUrl
            llmModelEdit.text = window.llmModel
            llmContextWindowEdit.text = window.llmContextWindow > 0
                ? String(window.llmContextWindow) : ""
            sttEndpointEdit.text = window.sttEndpoint
            sttModelEdit.text = window.sttModel
            ttsEndpointEdit.text = window.ttsEndpoint
            ttsModelEdit.text = window.ttsModel
            speechProviderCombo.currentIndex = window.speechProviderIndex(window.speechProvider)
            themeCombo.currentIndex = window.themeIndex(window.theme)
            speechEnabledBox.checked = window.speechEnabled
            speechModeCombo.currentIndex =
                window.speechReadMode === "stream" ? 1 : 0
            speechVoiceCombo.editText = window.speechVoice !== ""
                ? window.speechVoice
                : window.speechVoiceDefault
            speechProviderCombo.currentIndex = window.speechProviderIndex(window.speechProvider)
            speechSpeedCombo.currentIndex =
                speechSpeedCombo.speeds.indexOf(window.speechPlaybackSpeed)
            // The font lists are rebuilt on every opening: what this
            // system has may have changed since the last look.
            window.lightFontChoices = window.buildFontChoices(false)
            window.darkFontChoices = window.buildFontChoices(true)
            lightFontCombo.currentIndex =
                window.fontIndex(window.lightFontChoices, window.lightFont)
            darkFontCombo.currentIndex =
                window.fontIndex(window.darkFontChoices, window.darkFont)
            // The sizes: the chosen one, or the app's own size for the
            // mode.
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
    onLlmEndpointUpdated: {
        llmEndpointEdit.text = window.llmBaseUrl
        llmModelEdit.text = window.llmModel
    }
    onSttEndpointUpdated: {
        sttEndpointEdit.text = window.sttEndpoint
        sttModelEdit.text = window.sttModel
    }
    onTtsEndpointUpdated: {
        ttsEndpointEdit.text = window.ttsEndpoint
        ttsModelEdit.text = window.ttsModel
    }
    onSpeechEnabledChanged: speechEnabledBox.checked = window.speechEnabled
    onSpeechReadModeChanged: speechModeCombo.currentIndex =
        window.speechReadMode === "stream" ? 1 : 0
    onSpeechVoiceChanged: speechVoiceCombo.editText = window.speechVoice !== ""
        ? window.speechVoice
        : window.speechVoiceDefault
    onSpeechProviderChanged: speechProviderCombo.currentIndex =
        window.speechProviderIndex(window.speechProvider)
    onSpeechPlaybackSpeedChanged: speechSpeedCombo.currentIndex =
        speechSpeedCombo.speeds.indexOf(window.speechPlaybackSpeed)
    onThemeChanged: themeCombo.currentIndex = window.themeIndex(window.theme)
    onLightFontChanged: lightFontCombo.currentIndex =
        window.fontIndex(window.lightFontChoices, window.lightFont)
    onDarkFontChanged: darkFontCombo.currentIndex =
        window.fontIndex(window.darkFontChoices, window.darkFont)
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

    // Whether the context window in the field is one the store does not
    // hold yet: the field's number and the stored one differ. An empty
    // field means "the provider's own default", which is the same as
    // a stored zero.
    function contextWindowChanged(text) {
        const entered = Number(text) || 0
        return entered !== window.llmContextWindow
    }

    // The speech engine's row ("minimax" → 0, "local" → 1). The
    // OpenAI-shaped engines are named by their endpoint, not from the
    // list; only MiniMax (a different protocol) and the on-device one
    // are chosen here.
    function speechProviderIndex(provider) {
        return provider === "local" ? 1 : 0
    }

    // The families on offer for a mode: every one this system has,
    // sorted by name, with the two special rows first — the app's own
    // default for the mode, and the system's own font.
    //
    // Each row carries the family it is *set* as well as the family it
    // *names*: the row of the app's default names a face the answers
    // would be in, but stores the empty string, and the row of the
    // system's own font stores the word that stands for it.
    function buildFontChoices(dark) {
        const families = Qt.fontFamilies()
        families.sort(function (a, b) {
            const first = a.toLowerCase()
            const second = b.toLowerCase()
            return first < second ? -1 : (first > second ? 1 : 0)
        })
        const defaultFamily = dark ? PhoenixTheme.defaultDarkFont
                                   : PhoenixTheme.defaultLightFont
        // The app's default, if this system has it; the system's own
        // font, which every system has, if it does not.
        const defaultFace = PhoenixTheme.installedFamilies
            .indexOf(defaultFamily) >= 0
            ? defaultFamily
            : PhoenixTheme.systemFontFamily
        const choices = [{
            family: "",
            face: defaultFace,
            label: i18n("App default (%1)", defaultFace),
            note: ""
        }, {
            family: PhoenixTheme.systemFontChoice,
            face: PhoenixTheme.systemFontFamily,
            label: i18n("System default"),
            note: i18n("The system's own font.")
        }]
        for (let i = 0; i < families.length; ++i) {
            choices.push({
                family: families[i],
                face: families[i],
                label: families[i],
                note: ""
            })
        }
        return choices
    }

    // The face a mode's answers are set in — what the preview below
    // the list is shown in. A family that is not on this system falls
    // back to the system's own font, as the answers themselves do.
    //
    // The window's own choice is what is shown, not the theme's: the
    // window is given the stored values, and the theme is given the
    // same ones by the shell.
    function previewFamily(dark) {
        const chosen = dark ? darkFont : lightFont
        const fallback = dark ? PhoenixTheme.defaultDarkFont
                              : PhoenixTheme.defaultLightFont
        const family = PhoenixTheme.resolveAnswerFont(chosen, fallback)
        return PhoenixTheme.installedFamilies.indexOf(family) >= 0
            ? family
            : PhoenixTheme.systemFontFamily
    }

    // The size the preview is shown in: the one chosen for the mode,
    // kept within what the window can show without growing.
    function previewSize(dark) {
        const size = dark ? PhoenixTheme.darkAnswerFontSize
                          : PhoenixTheme.lightAnswerFontSize
        return Math.max(9, Math.min(size, 28))
    }

    // The row a family is on in the list. A family that is not on this
    // system — or one that is no longer offered — has no row of its
    // own and falls back to the system's font.
    function fontIndex(choices, family) {
        for (let i = 0; i < choices.length; ++i) {
            if (choices[i].family === family) {
                return i
            }
        }
        for (let i = 0; i < choices.length; ++i) {
            if (choices[i].family === PhoenixTheme.systemFontChoice) {
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
        AppIcon.showWindow(window)
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

                            model: window.lightFontChoices
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
                                font.family: model.face
                            }

                            onActivated: (index) =>
                                window.lightFontChangeRequested(
                                    window.lightFontChoices[index].family)
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

                            model: window.darkFontChoices
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
                                font.family: model.face
                            }

                            onActivated: (index) =>
                                window.darkFontChangeRequested(
                                    window.darkFontChoices[index].family)
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
                            + "which of the two applies. The light mode is set "
                            + "in Literata by default, a face drawn for long "
                            + "reading on a screen, and the dark one in Inter, "
                            + "a screen face; a system that has neither keeps "
                            + "its own font. In the light mode a serif belongs "
                            + "there — Noto Serif is on every system; on a dark "
                            + "ground a serif is tiring, so a sans does.")
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
                        text: i18n("LLM")
                        color: PhoenixTheme.subheadingTextColor
                    }

                    // The endpoint is the whole of the configuration:
                    // an OpenAI-compatible base URL is enough to name a
                    // service. The daemon works out the provider from
                    // the host, so a known service brings its own model
                    // and key with it, and any other server is the
                    // user's own.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Endpoint")
                            color: PhoenixTheme.textColor
                        }

                        PC3.TextField {
                            id: llmEndpointEdit
                            objectName: "llmEndpointEdit"

                            Layout.fillWidth: true
                            placeholderText: i18n("https://api.openai.com/v1")
                            onAccepted: window.llmEndpointSaveRequested(
                                text, llmModelEdit.text)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Model")
                            color: PhoenixTheme.textColor
                        }

                        PC3.TextField {
                            id: llmModelEdit
                            objectName: "llmModelEdit"

                            Layout.fillWidth: true
                            placeholderText: i18n("the provider's default")
                            onAccepted: window.llmEndpointSaveRequested(
                                llmEndpointEdit.text, text)
                        }

                        PC3.Button {
                            text: i18n("Save")
                            enabled: llmEndpointEdit.text !== window.llmBaseUrl
                                     || llmModelEdit.text !== window.llmModel
                            onClicked: window.llmEndpointSaveRequested(
                                llmEndpointEdit.text, llmModelEdit.text)
                        }
                    }

                    // How much the model holds at once: the context
                    // window the conversation is measured against. The
                    // provider knows the size of the models it serves;
                    // a server of the user's own knows nothing, so the
                    // number is the user's to give.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Context window")
                            color: PhoenixTheme.textColor
                        }

                        PC3.TextField {
                            id: llmContextWindowEdit
                            objectName: "llmContextWindowEdit"

                            Layout.fillWidth: true
                            placeholderText: i18n("the provider's default")
                            inputMethodHints: Qt.ImhDigitsOnly
                            validator: IntValidator { bottom: 0; top: 100000000 }
                            onAccepted: window.llmContextWindowSaveRequested(
                                Number(text))
                        }

                        PC3.Button {
                            text: i18n("Save")
                            enabled: window.contextWindowChanged(
                                llmContextWindowEdit.text)
                            onClicked: window.llmContextWindowSaveRequested(
                                Number(llmContextWindowEdit.text))
                        }
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("Any OpenAI-compatible endpoint works — OpenAI, "
                            + "OpenRouter, Groq, MiniMax or your own server. "
                            + "Leave the model empty to use the service's default, "
                            + "and the context window empty to use the service's own. "
                            + "The daemon reads the endpoint and the model when it "
                            + "starts, so saving them needs a restart: systemctl "
                            + "--user restart phoenix.service")
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

                            icon.source: LucideIcons.url(checked ? "eye-off" : "eye",
                                PhoenixTheme.textColor)
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
                    // restart note: what a saved key means for the daemon.
                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("Stored as %1 in %2. The agent daemon reads it when "
                            + "it starts — saving this restarts the daemon for you.",
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
                            text: i18n("Endpoint")
                            color: PhoenixTheme.textColor
                        }

                        PC3.TextField {
                            id: sttEndpointEdit
                            objectName: "sttEndpointEdit"

                            Layout.fillWidth: true
                            placeholderText: i18n("https://api.openai.com/v1/audio/transcriptions")
                            onAccepted: window.sttEndpointSaveRequested(
                                text, sttModelEdit.text)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Model")
                            color: PhoenixTheme.textColor
                        }

                        PC3.TextField {
                            id: sttModelEdit
                            objectName: "sttModelEdit"

                            Layout.fillWidth: true
                            placeholderText: i18n("the provider's default")
                            onAccepted: window.sttEndpointSaveRequested(
                                sttEndpointEdit.text, text)
                        }

                        PC3.Button {
                            text: i18n("Save")
                            enabled: sttEndpointEdit.text !== window.sttEndpoint
                                     || sttModelEdit.text !== window.sttModel
                            onClicked: window.sttEndpointSaveRequested(
                                sttEndpointEdit.text, sttModelEdit.text)
                        }
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("Any OpenAI-compatible transcription endpoint "
                            + "works — OpenAI, Groq, OpenRouter, MiniMax or your "
                            + "own server.")
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

                            icon.source: LucideIcons.url(checked ? "eye-off" : "eye",
                                PhoenixTheme.textColor)
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
                        text: window.sttProvider === "minimax"
                              ? i18n("The MiniMax key shared with the agent (%1); the "
                                + "next dictation uses it. Stored in %2.",
                                window.sttKeyField, window.keysFilePath)
                              : window.sttProvider === "custom"
                                ? i18n("Stored as %1 in %2; the next dictation uses it. "
                                    + "Name its endpoint and model in agent.toml.",
                                    window.sttKeyField, window.keysFilePath)
                                : i18n("Stored as %1 in %2; the next dictation uses it.",
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

                    // ---- Read aloud ---------------------------------------
                    // The daemon speaks the agent's answers; this is its
                    // on/off switch and the two choices that shape it.
                    // Every change applies at once (the shell asks the
                    // daemon to rebuild its speech pipeline after
                    // saving) — no daemon restart, unlike the keys above.
                    Kirigami.Heading {
                        level: 2
                        text: i18n("Read aloud")
                        color: PhoenixTheme.headingTextColor
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.CheckBox {
                            id: speechEnabledBox
                            objectName: "speechEnabledBox"

                            // Instant apply, like the combos above; the
                            // store's value is pushed imperatively so an
                            // edit cannot fight a binding.
                            onToggled: window.speechEnabledChangeRequested(checked)
                        }

                        PC3.Label {
                            Layout.fillWidth: true
                            text: i18n("The agent reads its answers aloud")
                            color: PhoenixTheme.textColor

                            // The label is the checkbox's own label, not a
                            // caption beside it: clicking it toggles the
                            // box, as the text of any checkbox does. The
                            // text cannot be the box's own — its colour is
                            // fixed with the backdrop, the box's label
                            // would follow the Plasma theme — so it stands
                            // apart and clicks itself through.
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor

                                // The full click, flash and all: the same
                                // path a click on the box itself takes.
                                onClicked: speechEnabledBox.animateClick()
                            }
                        }

                        PC3.Button {
                            objectName: "speechTestButton"

                            // Hearing the voice is the honest way to
                            // check the setup: the key, the voice and
                            // the playback, one short round-trip.
                            text: i18n("Test")
                            enabled: speechEnabledBox.checked
                            onClicked: window.speechTestRequested()
                        }
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("The answer's math is spoken as mathematics, "
                            + "code is not read out, and speaking stops the moment "
                            + "you close the results window or press its record "
                            + "button.")
                    }

                    // The voice's endpoint, like the others: an
                    // OpenAI-compatible speech endpoint is enough to name
                    // a service. MiniMax's speaks a different protocol,
                    // so it is the one that needs choosing from the
                    // list; the others are named by their URL.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Endpoint")
                            color: PhoenixTheme.textColor
                        }

                        PC3.TextField {
                            id: ttsEndpointEdit
                            objectName: "ttsEndpointEdit"

                            Layout.fillWidth: true
                            placeholderText: i18n("https://api.openai.com/v1/audio/speech")
                            onAccepted: window.ttsEndpointSaveRequested(
                                text, ttsModelEdit.text)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Model")
                            color: PhoenixTheme.textColor
                        }

                        PC3.TextField {
                            id: ttsModelEdit
                            objectName: "ttsModelEdit"

                            Layout.fillWidth: true
                            placeholderText: i18n("the provider's default")
                            onAccepted: window.ttsEndpointSaveRequested(
                                ttsEndpointEdit.text, text)
                        }

                        PC3.Button {
                            text: i18n("Save")
                            enabled: ttsEndpointEdit.text !== window.ttsEndpoint
                                     || ttsModelEdit.text !== window.ttsModel
                            onClicked: window.ttsEndpointSaveRequested(
                                ttsEndpointEdit.text, ttsModelEdit.text)
                        }
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("Any OpenAI-compatible speech endpoint works. "
                            + "MiniMax's own speaks a different protocol, so it "
                            + "is chosen from the list below.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Voice provider:")
                            color: PhoenixTheme.textColor
                        }

                        PC3.ComboBox {
                            id: speechProviderCombo

                            // MiniMax speaks a different protocol from
                            // the OpenAI-shaped ones, so it is named here
                            // rather than by its URL. "Local" is the
                            // on-device engine, which needs no key.
                            model: [i18n("MiniMax"), i18n("Local")]

                            onActivated: (index) =>
                                window.speechProviderChangeRequested(
                                    index === 1 ? "local" : "minimax")
                        }
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("The provider decides which voice ids exist and "
                            + "where the audio is made. MiniMax and OpenAI send the "
                            + "text to their service; Local speaks it on this "
                            + "computer and needs no key.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("When:")
                            color: PhoenixTheme.textColor
                        }

                        PC3.ComboBox {
                            id: speechModeCombo
                            objectName: "speechModeCombo"

                            model: [i18n("After the answer is complete"),
                                    i18n("While it is generated")]

                            onActivated: (index) =>
                                window.speechReadModeChangeRequested(
                                    index === 1 ? "stream" : "final")
                        }
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        // The trade-off in one breath, so the choice is
                        // an informed one: only "complete" can promise
                        // that a working note is never spoken aloud.
                        text: i18n("“After the answer is complete” never reads "
                            + "the agent's working notes aloud. “While it is "
                            + "generated” starts speaking sooner, but a note "
                            + "that precedes a tool call may already have been "
                            + "said when it is retracted — it cannot be unsaid.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Voice:")
                            color: PhoenixTheme.textColor
                        }

                        PC3.ComboBox {
                            id: speechVoiceCombo
                            objectName: "speechVoiceCombo"

                            editable: true
                            model: [window.speechVoiceDefault]

                            onActivated: (index) =>
                                window.speechVoiceChangeRequested(editText)
                            onAccepted: window.speechVoiceChangeRequested(editText)
                        }
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("A MiniMax voice id; the entry is editable for "
                            + "the voices the service offers.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        PC3.Label {
                            text: i18n("Speed:")
                            color: PhoenixTheme.textColor
                        }

                        PC3.ComboBox {
                            id: speechSpeedCombo
                            objectName: "speechSpeedCombo"

                            // The playback speeds on offer. The audio is
                            // stretched without touching the pitch, so
                            // every entry is the same voice; 1.0 is the
                            // speed the voice was spoken at. The player's
                            // own range reaches further by chaining its
                            // filters — these are the speeds worth
                            // hearing.
                            readonly property var speeds:
                                [0.75, 1.0, 1.25, 1.5, 2.0]

                            model: speeds
                            // A bare number reads as a setting, not a
                            // speed; the × says faster or slower.
                            displayText:
                                (currentIndex >= 0
                                 ? speeds[currentIndex]
                                 : window.speechPlaybackSpeed) + "×"

                            onActivated: (index) =>
                                window.speechPlaybackSpeedChangeRequested(
                                    speeds[index])
                        }
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        text: i18n("How fast the answers play back. The audio "
                            + "is stretched with the pitch kept, so the voice "
                            + "stays the voice at every speed.")
                    }

                    HintLabel {
                        Layout.fillWidth: true
                        visible: window.speechFromEnvironment
                        color: PhoenixTheme.warningColor
                        text: i18n("A PHOENIX_TTS_* environment variable is set "
                            + "and takes precedence over these settings.")
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
                                icon.source: LucideIcons.url("circle-x", PhoenixTheme.textColor)
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

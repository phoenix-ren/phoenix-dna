// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.plasmoid

// The applet's own QML module: it exposes the C++ controller singleton, the
// mode enum, and the QML types defined alongside them.
import plasma.applet.org.phoenixdna.agent as Phoenix

// Kirigami's theme, read inside the applet, is the Plasma theme — the
// judge of what "follow the system" resolves to (see the Binding below).
import org.kde.kirigami as Kirigami

// Components 2, 3, 4 and 5 are separate QML modules, installed independently.
import org.phoenixdna.overlay as PhoenixOverlay
import org.phoenixdna.resultswindow as PhoenixResults
import org.phoenixdna.search as PhoenixSearch
import org.phoenixdna.settings as PhoenixSettings
import org.phoenixdna.theme

/*
 * Component 1 — the panel widget.
 *
 * The compact representation is what the user sees in the taskbar: the mode
 * button on the left, the waveform in the middle and the record button on the
 * right.
 *
 * The overlay (component 2), the results pane (component 3) and the session
 * search (component 4) are separate QML modules; they are instantiated
 * lazily and only when needed.
 */
PlasmoidItem {
    id: root

    // The overlay pops up by itself the moment dictation starts, and the
    // transcript streams into it as the speech is decoded. When the
    // results pane is up, its own input bar is the transcript surface
    // instead: the dictation belongs to the conversation on screen there,
    // and a second window stacked over the pane would only cover it.
    Connections {
        target: Phoenix.PhoenixController

        function onDictationOverlayRequested() {
            if (resultsWindow.visible) {
                // The pane's bar mirrors listening/transcribing via its
                // bindings below; nothing else to do here.
                return;
            }
            // followUp is deliberately not touched here: it is false for a
            // fresh panel-record dictation (new session) and stays true
            // only while a pane-started clip is handing over (below), whose
            // submission must remain a follow-on. It resets on submit or
            // close.
            dictationOverlay.typing = false;
            dictationOverlay.transcript = Phoenix.PhoenixController.partialTranscript;
            dictationOverlay.listening = Phoenix.PhoenixController.recording;
            dictationOverlay.transcribing = Phoenix.PhoenixController.transcribing;
            dictationOverlay.statusMessage = Phoenix.PhoenixController.statusMessage;
            dictationOverlay.inputAnchor = root;
            dictationOverlay.visible = true;
        }

        // The keyboard button (or its global hotkey) asks for the prompt
        // editor: the query is typed rather than dictated, and nothing is
        // recorded.
        function onPromptEditorRequested() {
            root.openTypedPrompt();
        }

        function onConversationPaneRequested() {
            resultsWindow.toggle();
        }

        // The search button (or its global hotkey) asks for the session
        // search: the user's previous sessions on the left, the selected
        // one's queries and responses on the right. The button is a
        // toggle: asking again while the search is up closes it.
        function onSessionSearchRequested() {
            sessionSearch.open();
        }

        function onSessionSearchCloseRequested() {
            sessionSearch.dismiss();
        }
    }

    // The answer to the generated query arrives asynchronously from the
    // agent daemon: activityStarted opens the pane the moment the run
    // produces its first output, and the bridge streams the answer and
    // tool log into the pane's conversation as they happen; resultReady —
    // the finished result — re-pins the scroll to the newest exchange.
    Connections {
        target: Phoenix.PhoenixController.agent

        function onActivityStarted() {
            resultsWindow.showLive();
        }

        function onResultReady() {
            resultsWindow.showResult();
        }

        // A session the user opened from the session search: its history
        // is in the conversation model now, so the results window opens
        // on it.
        function onStoredSessionOpened() {
            resultsWindow.showResult();
        }
    }

    // The transcript updates asynchronously while the agent is listening,
    // and once more when the finished clip returns from transcription.
    // Pushed imperatively into both transcript surfaces: each editor is
    // editable, so a declarative binding there would be broken by the
    // first user edit.
    Connections {
        target: Phoenix.PhoenixController

        function onPartialTranscriptChanged() {
            dictationOverlay.transcript = Phoenix.PhoenixController.partialTranscript;
            resultsWindow.transcript = Phoenix.PhoenixController.partialTranscript;
        }

        function onTranscribingChanged() {
            dictationOverlay.transcribing = Phoenix.PhoenixController.transcribing;
        }

        function onStatusMessageChanged() {
            dictationOverlay.statusMessage = Phoenix.PhoenixController.statusMessage;
        }
    }

    // The colour scheme of the app's windows reaches every component
    // through the shared theme singleton: the store below owns the
    // persisted value, and these bindings push the live inputs into the
    // singleton — so a change made in the settings window, or the
    // desktop switching its own scheme, recolours the results window,
    // the session search and the dictation overlay at once.
    Binding {
        target: PhoenixTheme
        property: "preference"
        value: Phoenix.PhoenixController.settings.theme
    }
    Binding {
        target: PhoenixTheme
        property: "systemPrefersDark"
        // The desktop's mode, read off the applet's own Kirigami theme:
        // inside plasmashell that is the Plasma theme (PlasmaCore no
        // longer exposes a Theme object to QML), and the attached
        // property updates live when the desktop switches schemes. The
        // perceived-brightness test draws the light/dark line.
        value: {
            const background = Kirigami.Theme.backgroundColor;
            return 0.299 * background.r
                + 0.587 * background.g
                + 0.114 * background.b < 0.5;
        }
    }

    // Opens the prompt editor for a query the user types rather than
    // dictates: the same overlay the microphone opens, but empty, with
    // the keyboard in it and nothing being recorded.
    function openTypedPrompt() {
        if (resultsWindow.visible) {
            // The conversation window is on screen: its own follow-up
            // field is the prompt surface there (the same rule the
            // dictation overlay follows), so the typed query goes there
            // and continues the session on screen.
            resultsWindow.focusPrompt();
            return;
        }
        dictationOverlay.typing = true;
        dictationOverlay.transcript = "";
        dictationOverlay.listening = false;
        dictationOverlay.transcribing = false;
        dictationOverlay.statusMessage = "";
        dictationOverlay.inputAnchor = root;
        dictationOverlay.visible = true;
        // The window has to be up before the editor can take the focus.
        Qt.callLater(dictationOverlay.focusEditor);
    }

    // Component 2 — the dictation overlay.
    PhoenixOverlay.DictationOverlay {
        id: dictationOverlay

        // Whether a dictation submitted here belongs to the conversation on
        // screen (follow-on) or starts a new session. Normally the overlay
        // is opened by the panel's record button — a new session. The one
        // exception: a dictation started from the pane's input bar that is
        // still recording when the pane is dismissed hands over to the
        // overlay (see the pane's onDismissed below); that clip remains a
        // follow-on of the session it interrupted.
        property bool followUp: false

        // The transcript becomes a prompt only when the user says so; the
        // results pane is not opened here — it appears by itself when the
        // agent's answer comes back.
        onSubmitted: (text) => {
            dictationOverlay.visible = false;
            dictationOverlay.typing = false;
            Phoenix.PhoenixController.submitText(text, !dictationOverlay.followUp);
            dictationOverlay.followUp = false;
        }

        // The files attached to the prompt: the overlay and the results
        // pane edit the same prompt, so they show the same list.
        attachments: Phoenix.PhoenixController.attachments.items

        onAttachmentAdded: (file) => Phoenix.PhoenixController.addAttachment(file)
        onAttachmentRemoved: (index) => Phoenix.PhoenixController.removeAttachment(index)

        // The window picker: it photographs a window of the desktop and
        // attaches the picture to this prompt.
        screenshotPicking: Phoenix.PhoenixController.screenshotPicker.picking
        screenshotShortcut: Phoenix.PhoenixController.screenshotPicker.shortcut
        onScreenshotRequested: Phoenix.PhoenixController.startScreenshot()

        // Closing abandons the dictation; nothing is sent.
        onClosed: {
            dictationOverlay.visible = false;
            dictationOverlay.typing = false;
            dictationOverlay.followUp = false;
        }
    }

    // Component 3 — the results window.
    //
    // A fully-fledged, decorated toplevel: the compositor moves, resizes,
    // maximizes and closes it like any other window (the frameless Plasma
    // Dialog predecessor was a window class the compositor refuses to move
    // or resize interactively). It shows itself the moment the agent's
    // result arrives, stays above other windows so the result announces
    // itself, and persists until closed by hand; closing only hides it, so
    // the compositor keeps it where the user left it.
    //
    // Its input bar continues the conversation: record and send route
    // through the controller exactly like the panel's record button and
    // the overlay's "Send to agent", so a follow-on rides on the session
    // history the initial voice prompt created (AgentBridge tracks the
    // session id).
    PhoenixResults.ResultsWindow {
        id: resultsWindow

        // The bridge appends one row per run to the conversation model and
        // streams the answer (text deltas) and tool-call narration (fenced
        // Markdown) into it live; the pane renders the history and follows
        // the newest exchange. Follow-on questions add rows — previous
        // results stay, ChatGPT style.
        conversation: Phoenix.PhoenixController.agent.conversation

        // Dictation state for the input bar, mirroring the overlay. The
        // transcript itself is pushed imperatively (see the Connections
        // above) because the pane's editor is editable.
        listening: Phoenix.PhoenixController.recording
        transcribing: Phoenix.PhoenixController.transcribing
        statusMessage: Phoenix.PhoenixController.statusMessage

        // The files attached to the prompt being composed - the same list
        // the dictation overlay shows.
        attachments: Phoenix.PhoenixController.attachments.items

        onAttachmentAdded: (file) => Phoenix.PhoenixController.addAttachment(file)
        onAttachmentRemoved: (index) => Phoenix.PhoenixController.removeAttachment(index)

        // The window picker: it photographs a window of the desktop and
        // attaches the picture to this follow-up.
        screenshotPicking: Phoenix.PhoenixController.screenshotPicker.picking
        screenshotShortcut: Phoenix.PhoenixController.screenshotPicker.shortcut
        onScreenshotRequested: Phoenix.PhoenixController.startScreenshot()

        // The daemon names the session after every completed run (and
        // renames it when follow-ons reshape the conversation); the
        // header carries that title.
        sessionTitle: Phoenix.PhoenixController.agent.sessionTitle

        // The same toggle the panel widget's record button performs.
        onRecordClicked: Phoenix.PhoenixController.toggleRecording()

        onSubmitted: (text) => {
            // Follow-ons keep the pane as the conversation surface: the
            // controller clears both transcript surfaces when the text is
            // submitted (see submitText()), so the follow-up field is left
            // empty — placeholder reading "Ask a follow-up…" — for the next
            // query; here we only follow the newest exchange while the
            // answer streams in below the previous ones. A follow-on keeps
            // the session: the daemon continues the conversation on screen.
            resultsWindow.showLive();
            Phoenix.PhoenixController.submitText(text, false);
        }

        // The pane persists until closed by hand (button, Esc); a dismissal
        // mid-dictation must not strand the recording: the dictation
        // overlay takes over as the transcript surface so the follow-on can
        // still be verified and sent — and its submission must keep the
        // pane's session intent (follow-up), not restart the session.
        onDismissed: if (resultsWindow.listening) {
            dictationOverlay.followUp = true;
            dictationOverlay.transcript = Phoenix.PhoenixController.partialTranscript;
            dictationOverlay.listening = Phoenix.PhoenixController.recording;
            dictationOverlay.transcribing = Phoenix.PhoenixController.transcribing;
            dictationOverlay.statusMessage = Phoenix.PhoenixController.statusMessage;
            dictationOverlay.inputAnchor = root;
            dictationOverlay.visible = true;
        }
    }

    // Component 4 — the session search.
    //
    // A transparent overlay window with two sub-panes: the sessions of
    // the current user on the left, the selected session's queries and
    // responses on the right. The list is filtered by the daemon: it
    // matches the query fuzzily against the sessions' summary names, so
    // every further character drops the sessions that no longer match.
    PhoenixSearch.SessionSearchOverlay {
        id: sessionSearch

        // No anchor: the search is a workspace of its own, centred on
        // the display - not a popup next to the widget.

        // The user whose history is searched: root mode browses root's
        // sessions, every other mode the local user's.
        user: Phoenix.PhoenixController.searchUser

        // The daemon's answers: the matching sessions, and the messages
        // of the selected one.
        summaries: Phoenix.PhoenixController.agent.sessionSummaries
        messages: Phoenix.PhoenixController.agent.sessionMessages

        // The query goes to the daemon, which owns the sessions.
        onQueryChanged: (query) => {
            Phoenix.PhoenixController.searchSessions(query);
        }

        // The selected session's transcript is fetched on demand.
        onSessionSelected: (sessionId) => {
            Phoenix.PhoenixController.openSession(sessionId);
        }

        // A double-clicked session opens the results window on it, so
        // the conversation can be continued there. The search gives way
        // to the window: it is where the follow-on is typed.
        onSessionActivated: (sessionId, mode, title) => {
            sessionSearch.dismiss();
            Phoenix.PhoenixController.openSessionResults(
                sessionId, mode, title);
        }

        // The panel's search button is a toggle: it shows the search
        // while it is pressed.
        onOpened: Phoenix.PhoenixController.setSessionSearchOpen(true);
        onDismissed: Phoenix.PhoenixController.setSessionSearchOpen(false);
    }

    // Component 5 — the settings window.
    //
    // The gear button opens it. The window itself is presentational
    // (like the other components): the state below is bound from the
    // controller's settings store and the live hotkey bindings, and the
    // interaction signals are routed back to the store, which owns
    // keys.json and the KGlobalAccel actions.
    PhoenixSettings.SettingsWindow {
        id: settingsWindow

        // The colour scheme of the app's windows ("system"/"light"/
        // "dark"); the store persists it and the Binding above fans it
        // out to every window through the theme singleton.
        theme: Phoenix.PhoenixController.settings.theme

        // The live global bindings, as the panel buttons' tooltips show
        // them. Re-binds in System Settings update these too.
        recordShortcut: Phoenix.PhoenixController.recordShortcut
        searchShortcut: Phoenix.PhoenixController.searchShortcut
        screenshotShortcut: Phoenix.PhoenixController.screenshotPicker.shortcut
        promptShortcut: Phoenix.PhoenixController.promptShortcut

        // The stored keys and everything about them the fields explain.
        llmApiKey: Phoenix.PhoenixController.settings.llmApiKey
        llmKeyField: Phoenix.PhoenixController.settings.llmKeyField
        llmKeyFromEnvironment: Phoenix.PhoenixController.settings.llmKeyFromEnvironment
        sttApiKey: Phoenix.PhoenixController.settings.sttApiKey
        sttKeyField: Phoenix.PhoenixController.settings.sttKeyField
        sttProvider: Phoenix.PhoenixController.settings.sttProvider
        sttKeyFromEnvironment: Phoenix.PhoenixController.settings.sttKeyFromEnvironment
        keysFilePath: Phoenix.PhoenixController.settings.keysFilePath
        statusMessage: Phoenix.PhoenixController.settings.statusMessage
        statusIsError: Phoenix.PhoenixController.settings.statusIsError

        onLlmKeySaveRequested: (key) =>
            Phoenix.PhoenixController.settings.saveLlmApiKey(key)
        onSttKeySaveRequested: (key) =>
            Phoenix.PhoenixController.settings.saveSttApiKey(key)
        onSttProviderChangeRequested: (provider) =>
            Phoenix.PhoenixController.settings.saveSttProvider(provider)
        onThemeChangeRequested: (theme) =>
            Phoenix.PhoenixController.settings.saveTheme(theme)

        // role is "record", "screenshot", "search" or "prompt"; the store
        // dispatches to the matching KGlobalAccel action.
        onShortcutCaptureRequested: (role, key, modifiers) =>
            Phoenix.PhoenixController.settings.applyShortcut(role, key, modifiers)
        onShortcutClearRequested: (role) =>
            Phoenix.PhoenixController.settings.clearShortcut(role)
    }

    compactRepresentation: Phoenix.CompactRepresentation {
        onRecordClicked: Phoenix.PhoenixController.toggleRecording()

        // The window re-reads keys.json when it opens, so hand-edits and
        // the installer's writes are reflected without a restart.
        onSettingsClicked: {
            Phoenix.PhoenixController.settings.refresh()
            settingsWindow.openSettings()
        }
    }

    fullRepresentation: Phoenix.CompactRepresentation {
        onRecordClicked: Phoenix.PhoenixController.toggleRecording()

        onSettingsClicked: {
            Phoenix.PhoenixController.settings.refresh()
            settingsWindow.openSettings()
        }
    }
}

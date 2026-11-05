// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PC3
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

    // The widget-level tooltip — the applet's name and the plugin's
    // description — stands down.
    //
    // Plasma draws that tooltip in the very window the buttons' tooltips
    // are drawn in: the panel's own tooltip area (CompactApplet.qml in the
    // desktop package) covers the whole applet and hands that window over
    // to whichever area is hovered. Hover events reach the innermost area
    // first, so the button's area arms its timer first — and the widget's
    // area, the outermost one, fires last and overwrites the button's text
    // with "Phoenix DNA" and the plugin description. Only one of the two
    // can be seen.
    //
    // Emptying both texts leaves the widget's area invalid, and an invalid
    // area never shows a tooltip (ToolTipArea::isValid()). The buttons keep
    // their own: they are the widget's only tooltips now.
    toolTipMainText: ""
    toolTipSubText: ""

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
            //
            // A dictation takes the editor over from a typed query: the
            // typed editor is put away (its draft is kept), and the
            // microphone owns the prompt.
            Phoenix.PhoenixController.setPromptEditorOpen(false);
            dictationOverlay.typing = false;
            dictationOverlay.transcript = Phoenix.PhoenixController.partialTranscript;
            dictationOverlay.listening = Phoenix.PhoenixController.recording;
            dictationOverlay.transcribing = Phoenix.PhoenixController.transcribing;
            dictationOverlay.statusMessage = Phoenix.PhoenixController.statusMessage;
            dictationOverlay.inputAnchor = root;
            dictationOverlay.showOverlay();
        }

        // The keyboard button (or its global hotkey) asks for the prompt
        // editor: the query is typed rather than dictated, and nothing is
        // recorded. Asking again puts it away again - it is a toggle.
        function onPromptEditorRequested() {
            root.openTypedPrompt();
        }

        function onPromptEditorCloseRequested() {
            root.closeTypedPrompt();
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
    // agent daemon: runStarted opens the pane the moment the daemon picks
    // the prompt up — the model has not said anything yet, and it will
    // not for a while — and the bridge streams the answer and tool log
    // into the pane's conversation as they happen; resultReady — the
    // finished result — re-pins the scroll to the newest exchange.
    Connections {
        target: Phoenix.PhoenixController.agent

        function onRunStarted() {
            resultsWindow.showLive();
        }

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
            const text = Phoenix.PhoenixController.partialTranscript;
            dictationOverlay.transcript = text;
            resultsWindow.transcript = text;
            // The voice input has ended and the transcript is the prompt
            // the user is about to check: it goes to the clipboard, so it
            // can be pasted elsewhere without being picked out of the
            // editor by hand. A transcript that is still being recorded or
            // transcribed is not a prompt yet, and an empty one is nothing
            // to copy.
            if (text.length > 0
                    && !Phoenix.PhoenixController.recording
                    && !Phoenix.PhoenixController.transcribing) {
                Phoenix.PhoenixController.copyToClipboard(text);
            }
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

    // The fonts of the agent's answers: one family per mode, both
    // chosen independently of the colour scheme above. The theme
    // singleton resolves which of the two the windows currently use.
    Binding {
        target: PhoenixTheme
        property: "lightFont"
        value: Phoenix.PhoenixController.settings.lightFont
    }
    Binding {
        target: PhoenixTheme
        property: "darkFont"
        value: Phoenix.PhoenixController.settings.darkFont
    }
    Binding {
        target: PhoenixTheme
        property: "lightFontSize"
        value: Phoenix.PhoenixController.settings.lightFontSize
    }
    Binding {
        target: PhoenixTheme
        property: "darkFontSize"
        value: Phoenix.PhoenixController.settings.darkFontSize
    }

    // Opens the prompt editor for a query the user types rather than
    // dictates: the same overlay the microphone opens, but with the
    // keyboard in it and nothing being recorded.
    function openTypedPrompt() {
        // The editor shows the draft it kept, not whatever a dictation
        // left behind in it.
        dictationOverlay.typing = true;
        dictationOverlay.listening = false;
        dictationOverlay.transcribing = false;
        dictationOverlay.statusMessage = "";
        dictationOverlay.inputAnchor = root;
        dictationOverlay.showOverlay();
        Phoenix.PhoenixController.setPromptEditorOpen(true);
        // The window has to be up before the editor can take the focus.
        Qt.callLater(dictationOverlay.focusEditor);
    }

    // Puts the typed editor away again. The query stays: it is a draft
    // the toggle can bring back (Esc is what discards it).
    function closeTypedPrompt() {
        dictationOverlay.visible = false;
        dictationOverlay.typing = false;
        Phoenix.PhoenixController.setPromptEditorOpen(false);
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
            // A dictated prompt carries the intent of the button that
            // started the recording: the panel widget's record button
            // starts a new session, the results pane's own record button
            // continues the conversation on screen. The controller
            // remembers it (it is the only place that can tell the two
            // apart — a dictation started from the panel widget lands in
            // the pane's input bar when the pane happens to be open, and
            // a clip handed over from a dismissed pane was started by the
            // pane's own button).
            //
            // A typed query continues the conversation that is on screen
            // (the window owns the session while it is up).
            const dictated = !dictationOverlay.typing;
            const newSession = dictated
                ? Phoenix.PhoenixController.newSessionIntent(true)
                    && !dictationOverlay.followUp
                : !resultsWindow.visible;
            dictationOverlay.visible = false;
            dictationOverlay.typing = false;
            // The query is on its way: it must not come back with the
            // next toggle.
            dictationOverlay.draft = "";
            Phoenix.PhoenixController.setPromptEditorOpen(false);
            Phoenix.PhoenixController.submitText(text, newSession);
            dictationOverlay.followUp = false;
        }

        // Esc discards the typed query: the text is thrown away and the
        // editor goes away with it.
        onDiscarded: {
            dictationOverlay.typing = false;
            dictationOverlay.visible = false;
            Phoenix.PhoenixController.setPromptEditorOpen(false);
        }

        // The user emptied the editor: the dictated transcript is gone, so
        // what is typed in its place is a fresh query — a follow-on of the
        // conversation on screen, not the new session the recording's
        // button asked for.
        onDictationDiscarded: Phoenix.PhoenixController.clearDictatedPrompt()

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

        // Closing abandons the dictation; nothing is sent. A typed draft
        // is kept (the toggle can bring it back).
        onClosed: {
            dictationOverlay.visible = false;
            dictationOverlay.typing = false;
            dictationOverlay.followUp = false;
            Phoenix.PhoenixController.setPromptEditorOpen(false);
            // The abandoned transcript is not a prompt that is waiting to
            // be sent: the next one is a fresh beginning.
            Phoenix.PhoenixController.clearDictatedPrompt();
        }
    }

    // Component 2b — the waiting overlay.
    //
    // The daemon runs one prompt at a time, so a prompt sent while
    // another run is busy waits its turn in the daemon's queue. The
    // results pane opens when the daemon *starts* a run — which, for a
    // queued prompt, is only once the run before it is done. Without
    // this, the screen does not react at all until then.
    //
    // The overlay is that reaction: a spinning wheel and a line of text,
    // centred on the display. It is up for exactly as long as the
    // prompt waits — the daemon's queue length says when it is over.
    PhoenixOverlay.WaitingOverlay {
        id: waitingOverlay

        // How many prompts wait in the daemon's queue: the overlay is up
        // while there is one, and gone as soon as the daemon picks the
        // prompt up (which is when the results pane opens on it).
        queuedPrompts: Phoenix.PhoenixController.agent.queuedPrompts
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
    // Its input bar continues the conversation: record, send and stop
    // route through the controller exactly like the panel's record button
    // and the overlay's send arrow, so a follow-on rides on the session
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

        // The CPU meter of the command the running bash tool call
        // executes: the daemon reports the command's process id over
        // D-Bus, the bridge's meter follows it through /proc, and the
        // pane draws it as an audio level meter under the running step.
        cpuMeter: Phoenix.PhoenixController.agent.cpuMeter

        // The rate of the conversation with the model: the bytes that go
        // out and come in, as a pair of level meters under the running
        // thought step.
        wireMeter: Phoenix.PhoenixController.agent.wireMeter

        // Dictation state for the input bar, mirroring the overlay. The
        // transcript itself is pushed imperatively (see the Connections
        // above) because the pane's editor is editable.
        listening: Phoenix.PhoenixController.recording
        transcribing: Phoenix.PhoenixController.transcribing
        statusMessage: Phoenix.PhoenixController.statusMessage

        // Whether the prompt in the input bar came from a recording: the
        // controller knows, because it is the one that produced the
        // transcript. A dictated prompt carries the intent of the button
        // that started the recording (see newSessionIntent()).
        //
        // The binding follows the controller, which clears the flag when
        // the user empties the editor (see onDictationDiscarded below) —
        // the pane itself never writes to it.
        dictated: Phoenix.PhoenixController.dictatedPromptPending

        // The user emptied the input bar: the dictated transcript is
        // gone, so what is typed in its place is a fresh query — a
        // follow-on of the conversation on screen, not the new session
        // the recording's button asked for.
        onDictationDiscarded: Phoenix.PhoenixController.clearDictatedPrompt()

        // Whether an agent run is live: the pane's send button is a stop
        // button while it is, and stopping it goes back the same way —
        // through the bridge, which speaks the daemon's Cancel(run_id).
        runActive: Phoenix.PhoenixController.agent.runActive
        onCancelRequested: Phoenix.PhoenixController.agent.cancelRun()

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

        // Read-aloud: the pane's per-message "Read aloud" and its stop
        // control route through the agent bridge to the daemon
        // (SpeakText / StopSpeaking). The daemon owns synthesis — the
        // shell only asks and mirrors the state; the affordance hides
        // itself while speech is not configured.
        speechAvailable: Phoenix.PhoenixController.agent.speechAvailable
        agentSpeaking: Phoenix.PhoenixController.agent.agentSpeaking
        onReadAloudRequested: (text) =>
            Phoenix.PhoenixController.agent.speakText(text)
        onStopSpeakingRequested:
            Phoenix.PhoenixController.agent.stopSpeaking()

        // The form the running prompt is blocked on (ask_user): the
        // bridge fills it from the daemon's QuestionAsked and empties it
        // when the form is answered, times out or the run ends. One
        // question renders as the card above the input bar; several open
        // the wizard overlay.
        pendingForm: Phoenix.PhoenixController.agent.pendingForm

        // Whether the wizard overlay was dismissed for this form: the
        // slim card then offers to bring it back.
        wizardOverlayDismissed: wizardOverlay.dismissed

        // The card's answers go straight back to the daemon as one
        // AnswerQuestion call per form.
        onFormSubmitted: (answers) =>
            Phoenix.PhoenixController.agent.submitAnswers(answers)

        // The card's "Show questions" brings the dismissed overlay back.
        onShowQuestionsRequested: wizardOverlay.reopen()

        // The same toggle the panel widget's record button performs, but
        // with the opposite intent: the pane's own record button
        // continues the conversation on screen, so the dictation is a
        // follow-on rather than the start of a new session.
        onRecordClicked: Phoenix.PhoenixController.toggleFollowUpRecording()

        onSubmitted: (text, dictated) => {
            // While the run is blocked on a form of one question, the
            // input bar answers it — typed or dictated, in the user's
            // own words — instead of queuing another prompt behind the
            // blocked run. The model interprets free text ("the second
            // one", "actually cancel"), so the spoken path needs no
            // parsing. A wizard (several questions) is answered in its
            // own overlay: its answers leave as one submission, so a
            // single text answer cannot serve it and submitAnswers
            // declines it — the text goes on as the prompt it is.
            if (Phoenix.PhoenixController.agent.submitAnswers([{ "text": text }])) {
                // The transcript is consumed: it became an answer, not a
                // prompt (submitText's clearing, done here because the
                // answer path bypasses it).
                resultsWindow.transcript = "";
                return;
            }
            // No question pending (or one a single answer cannot serve):
            // a follow-up. Follow-ons keep the pane as the conversation
            // surface: the controller clears both transcript surfaces
            // when the text is submitted (see submitText()), so the
            // follow-up field is left empty — placeholder reading "Ask
            // a follow-up…" — for the next query; here we only follow
            // the newest exchange while the answer streams in below the
            // previous ones. A follow-on keeps the session: the daemon
            // continues the conversation on screen.
            //
            // The one exception is a dictation the user started from the
            // panel widget's record button while this pane was open: its
            // transcript lands in this very input bar, so the surface
            // cannot tell it from one dictated with the pane's own
            // record button — only the button that started the recording
            // can, and the controller remembers its intent.
            Phoenix.PhoenixController.submitText(text,
                Phoenix.PhoenixController.newSessionIntent(dictated));
            resultsWindow.showLive();
        }

        // The pane persists until closed by hand (button, Esc); a dismissal
        // mid-dictation must not strand the recording: the dictation
        // overlay takes over as the transcript surface so the follow-on can
        // still be verified and sent — and its submission must keep the
        // pane's session intent (follow-up), not restart the session.
        //
        // The dismissal also stops any read-aloud in flight: the audio
        // is the daemon's (not the window's), so it would keep speaking
        // into an empty screen otherwise — putting the window away is
        // the clearest "enough" a user can give.
        onDismissed: {
            resultsWindow.stopSpeakingRequested()
            if (resultsWindow.listening) {
                dictationOverlay.followUp = true;
                dictationOverlay.transcript = Phoenix.PhoenixController.partialTranscript;
                dictationOverlay.listening = Phoenix.PhoenixController.recording;
                dictationOverlay.transcribing = Phoenix.PhoenixController.transcribing;
                dictationOverlay.statusMessage = Phoenix.PhoenixController.statusMessage;
                dictationOverlay.inputAnchor = root;
                dictationOverlay.showOverlay();
            }
        }
    }

    // Component 3b — the wizard overlay.
    //
    // A form of several questions (one ask_user call) takes the form of
    // a large, transient, centred window of its own — like the session
    // search, a Plasma Dialog so it can sit above the shell's surfaces
    // on Wayland, painting its own themed backdrop (dark in the dark
    // scheme, light in the light one). The questions are walked entirely
    // client-side; the answers leave as one submission, the bridge
    // clears the form the instant they do, and the overlay follows it
    // out — the conversation underneath carries the summary in its tool
    // log. Esc dismisses the overlay (the run keeps waiting; the results
    // window shows a card that brings it back).
    PlasmaCore.Dialog {
        id: wizardOverlay

        // Whether the user put this form's overlay away without
        // answering; cleared when another form arrives or this one ends.
        property bool dismissed: false
        property string dismissedAskId: ""

        readonly property real minimumWidth: 700
        readonly property real minimumHeight: 560
        readonly property real overlayWidth: Math.max(
            minimumWidth, Math.round(Screen.desktopAvailableWidth * 0.55))
        readonly property real overlayHeight: Math.max(
            minimumHeight, Math.round(Screen.desktopAvailableHeight * 0.7))

        location: PlasmaCore.Types.Floating
        type: PlasmaCore.Dialog.WindowType.DialogWindow
        backgroundHints: PlasmaCore.Dialog.NoBackground
        flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
        hideOnWindowDeactivate: false
        visible: false

        // The window's own icon: the app's avatar, the same mark every
        // other window of the app carries.
        //
        // QML has no window-icon API (QIcon is not a QML type), so the
        // icon is put on the window from C++ (see AppIcon).
        Component.onCompleted: AppIcon.setWindowIcon(wizardOverlay)

        // The form being walked; {} once it is answered or gone.
        property var form: Phoenix.PhoenixController.agent.pendingForm

        // A wizard form pending and not dismissed: the overlay is up.
        // The optimistic clear on submit empties the form, so the
        // overlay leaves the moment the answers are handed over.
        onFormChanged: sync()

        function reopen() {
            dismissed = false;
            dismissedAskId = "";
            sync();
        }

        function sync() {
            const askId = (form && form.askId) || "";
            if (askId !== dismissedAskId) {
                // Another form arrived (or this one ended): the
                // dismissal belonged to the old one.
                dismissed = false;
                dismissedAskId = "";
            }
            const questions = (form && form.questions) || [];
            if (questions.length >= 2 && !dismissed) {
                open();
            } else {
                close();
            }
        }

        function open() {
            width = overlayWidth;
            height = overlayHeight;
            x = Screen.virtualX + Math.round((Screen.width - width) / 2);
            y = Screen.virtualY + Math.round((Screen.height - height) / 2);
            visible = true;
            // The compositor places the window itself, so the position
            // is applied again a tick later — it then wins over the
            // placement the compositor chose. The same tick asks for
            // the keyboard: the overlay must hold the focus for Esc to
            // reach it (it is dismissed with the keyboard, see below),
            // and a window that comes back after a refused submission
            // does not get the keyboard back by itself.
            Qt.callLater(function() {
                x = Screen.virtualX + Math.round((Screen.width - width) / 2);
                y = Screen.virtualY + Math.round((Screen.height - height) / 2);
                wizardOverlay.requestActivate();
                wizardBackdrop.forceActiveFocus();
            });
        }

        // Esc puts the overlay away; the questions stay pending, and the
        // results window's card offers to bring them back.
        function dismiss() {
            dismissed = true;
            dismissedAskId = form && form.askId ? form.askId : "";
            close();
        }

        // The dialog's default property is mainItem, so the backdrop is
        // a bare child (see the session search overlay).
        mainItem: Rectangle {
            id: wizardBackdrop

            implicitWidth: wizardOverlay.overlayWidth
            implicitHeight: wizardOverlay.overlayHeight
            radius: Kirigami.Units.smallSpacing

            // The themed backdrop, painted here because the dialog
            // surface itself is bare: dark and near-black in the dark
            // scheme, the theme's own light one otherwise.
            color: PhoenixTheme.backgroundColor
            border.width: 1
            border.color: PhoenixTheme.backdropBorderColor

            // Esc's home is the backdrop itself: key events travel
            // from whatever holds the focus up through its ancestors,
            // and the backdrop is an ancestor of everything the wizard
            // shows — an option row, the own-answer field, the
            // buttons. A handler on a sibling of the content (as this
            // once was) sees Esc only until the user clicks anything
            // inside the wizard, and a frameless always-on-top window
            // without a working Esc has no way down at all. Only
            // Escape is handled, so the walker's keys (Enter in the
            // own-answer field) are not stolen.
            focus: true
            Keys.onEscapePressed: wizardOverlay.dismiss()

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Kirigami.Units.gridUnit * 2

                // The close button: the one way down that needs no
                // keyboard. It stays enabled while the answers are on
                // their way (every walker control is disabled then,
                // but hiding the window costs nothing — the questions
                // stay pending and the pane's card brings them back,
                // exactly as with Esc).
                RowLayout {
                    Layout.fillWidth: true

                    Item {
                        Layout.fillWidth: true
                    }

                    PC3.ToolButton {
                        objectName: "wizardCloseButton"

                        icon.source: LucideIcons.url("x", PhoenixTheme.textColor)
                        icon.width: Kirigami.Units.iconSizes.small
                        icon.height: Kirigami.Units.iconSizes.small

                        onClicked: wizardOverlay.dismiss()

                        PC3.ToolTip {
                            text: i18n("Close")
                        }
                    }
                }

                PhoenixResults.WizardView {
                    id: wizardWalker

                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    form: wizardOverlay.form

                    onSubmitted: (answers) =>
                        Phoenix.PhoenixController.agent.submitAnswers(answers)
                }

                // Esc dismisses; the caption says where the questions
                // went, so dismissing never reads as losing them.
                Text {
                    Layout.fillWidth: true

                    text: i18n("Esc hides this window; the agent keeps waiting for your answers.")
                    font.pixelSize: Math.max(
                        9, Math.round(Qt.application.font.pixelSize * 0.8))
                    color: PhoenixTheme.secondaryTextColor
                }
            }
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

        // The user whose history is searched: the local login name -
        // the agent works as the desktop user.
        user: Phoenix.PhoenixController.searchUser

        // The daemon's answers: the matching sessions, and the
        // transcript of the selected one.
        summaries: Phoenix.PhoenixController.agent.sessionSummaries
        transcript: Phoenix.PhoenixController.agent.sessionTranscript

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

        // A session the user threw away: the daemon removes it and
        // everything that belongs to it, and the list follows.
        onSessionDeleteRequested: (sessionId) => {
            Phoenix.PhoenixController.deleteSession(sessionId);
        }

        // A session the user named: the daemon keeps that name against
        // the model's own, and the list follows its answer.
        onSessionRenameRequested: (sessionId, title) => {
            Phoenix.PhoenixController.renameSession(sessionId, title);
        }

        // The panel's search button is a toggle: it shows the search
        // while it is pressed.
        onOpened: Phoenix.PhoenixController.setSessionSearchOpen(true);
        onDismissed: Phoenix.PhoenixController.setSessionSearchOpen(false);
    }

    // Component 5 — the settings window.
    //
    // The gear button toggles it. The window itself is presentational
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

        // The fonts of the agent's answers, one family and one size per
        // mode; the store persists them and the Bindings above fan
        // them out the same way.
        lightFont: Phoenix.PhoenixController.settings.lightFont
        darkFont: Phoenix.PhoenixController.settings.darkFont
        lightFontSize: Phoenix.PhoenixController.settings.lightFontSize
        darkFontSize: Phoenix.PhoenixController.settings.darkFontSize

        // The live global bindings, as the panel buttons' tooltips show
        // them. Re-binds in System Settings update these too.
        recordShortcut: Phoenix.PhoenixController.recordShortcut
        searchShortcut: Phoenix.PhoenixController.searchShortcut
        screenshotShortcut: Phoenix.PhoenixController.screenshotPicker.shortcut
        promptShortcut: Phoenix.PhoenixController.promptShortcut

        // The store's changes, pushed in: the endpoint and the model of
        // each function come as a pair, so the fields are filled in
        // together. The property bindings above carry the values; these
        // carry the moment they changed.
        Connections {
            target: Phoenix.PhoenixController.settings

            function onLlmEndpointChanged() {
                settingsWindow.llmEndpointUpdated();
            }
            function onSttEndpointChanged() {
                settingsWindow.sttEndpointUpdated();
            }
            function onTtsEndpointChanged() {
                settingsWindow.ttsEndpointUpdated();
            }
        }

        // The stored keys and everything about them the fields explain.
        llmApiKey: Phoenix.PhoenixController.settings.llmApiKey
        llmProvider: Phoenix.PhoenixController.settings.llmProvider
        llmBaseUrl: Phoenix.PhoenixController.settings.llmBaseUrl
        llmModel: Phoenix.PhoenixController.settings.llmModel
        llmKeyField: Phoenix.PhoenixController.settings.llmKeyField
        llmKeyFromEnvironment: Phoenix.PhoenixController.settings.llmKeyFromEnvironment
        sttApiKey: Phoenix.PhoenixController.settings.sttApiKey
        sttKeyField: Phoenix.PhoenixController.settings.sttKeyField
        sttProvider: Phoenix.PhoenixController.settings.sttProvider
        sttEndpoint: Phoenix.PhoenixController.settings.sttEndpoint
        sttModel: Phoenix.PhoenixController.settings.sttModel
        sttKeyFromEnvironment: Phoenix.PhoenixController.settings.sttKeyFromEnvironment
        ttsEndpoint: Phoenix.PhoenixController.settings.ttsEndpoint
        ttsModel: Phoenix.PhoenixController.settings.ttsModel
        keysFilePath: Phoenix.PhoenixController.settings.keysFilePath
        statusMessage: Phoenix.PhoenixController.settings.statusMessage
        statusIsError: Phoenix.PhoenixController.settings.statusIsError

        // Read-aloud: what the store holds, for the window to show.
        // Applying is two steps below — the store writes keys.json, and
        // the agent bridge asks the daemon to rebuild its speech
        // pipeline, so a flipped toggle speaks (or falls silent) at
        // once.
        speechEnabled: Phoenix.PhoenixController.settings.speechEnabled
        speechReadMode: Phoenix.PhoenixController.settings.speechReadMode
        speechVoice: Phoenix.PhoenixController.settings.speechVoice
        speechProvider: Phoenix.PhoenixController.settings.speechProvider
        speechPlaybackSpeed: Phoenix.PhoenixController.settings.speechPlaybackSpeed
        speechFromEnvironment: Phoenix.PhoenixController.settings.speechFromEnvironment

        onLlmKeySaveRequested: (key) =>
            Phoenix.PhoenixController.settings.saveLlmApiKey(key)
        onLlmProviderChangeRequested: (provider) =>
            Phoenix.PhoenixController.settings.saveLlmProvider(provider)
        onLlmEndpointSaveRequested: (endpoint, model) =>
            Phoenix.PhoenixController.settings.saveLlmEndpoint(endpoint, model)
        onSttEndpointSaveRequested: (endpoint, model) =>
            Phoenix.PhoenixController.settings.saveSttEndpoint(endpoint, model)
        onTtsEndpointSaveRequested: (endpoint, model) =>
            Phoenix.PhoenixController.settings.saveTtsEndpoint(endpoint, model)
        onSttKeySaveRequested: (key) =>
            Phoenix.PhoenixController.settings.saveSttApiKey(key)
        onSttProviderChangeRequested: (provider) =>
            Phoenix.PhoenixController.settings.saveSttProvider(provider)
        onThemeChangeRequested: (theme) =>
            Phoenix.PhoenixController.settings.saveTheme(theme)
        onLightFontChangeRequested: (family) =>
            Phoenix.PhoenixController.settings.saveLightFont(family)
        onDarkFontChangeRequested: (family) =>
            Phoenix.PhoenixController.settings.saveDarkFont(family)
        onLightFontSizeChangeRequested: (size) =>
            Phoenix.PhoenixController.settings.saveLightFontSize(size)
        onDarkFontSizeChangeRequested: (size) =>
            Phoenix.PhoenixController.settings.saveDarkFontSize(size)

        // role is "record", "screenshot", "search" or "prompt"; the store
        // dispatches to the matching KGlobalAccel action.
        onShortcutCaptureRequested: (role, key, modifiers) =>
            Phoenix.PhoenixController.settings.applyShortcut(role, key, modifiers)
        onShortcutClearRequested: (role) =>
            Phoenix.PhoenixController.settings.clearShortcut(role)

        // Each speech setting: persist it, then make the daemon re-read
        // it. The reload also re-probes the daemon's state, so the
        // results pane's "Read aloud" affordance appears (or goes)
        // immediately.
        onSpeechEnabledChangeRequested: (enabled) => {
            Phoenix.PhoenixController.settings.saveSpeechEnabled(enabled)
            Phoenix.PhoenixController.agent.reloadSpeech()
        }
        onSpeechReadModeChangeRequested: (mode) => {
            Phoenix.PhoenixController.settings.saveSpeechReadMode(mode)
            Phoenix.PhoenixController.agent.reloadSpeech()
        }
        onSpeechVoiceChangeRequested: (voice) => {
            Phoenix.PhoenixController.settings.saveSpeechVoice(voice)
            Phoenix.PhoenixController.agent.reloadSpeech()
        }
        onSpeechProviderChangeRequested: (provider) => {
            Phoenix.PhoenixController.settings.saveSpeechProvider(provider)
            Phoenix.PhoenixController.agent.reloadSpeech()
        }
        onSpeechPlaybackSpeedChangeRequested: (speed) => {
            Phoenix.PhoenixController.settings.saveSpeechPlaybackSpeed(speed)
            Phoenix.PhoenixController.agent.reloadSpeech()
        }
        onSpeechTestRequested:
            Phoenix.PhoenixController.agent.speakText(
                i18n("This is how the agent's answers will sound."))
    }

    compactRepresentation: Phoenix.CompactRepresentation {
        onRecordClicked: Phoenix.PhoenixController.toggleRecording()

        // The gear button is a toggle: the pane's own visibility decides
        // which way a press goes, and the button's checked state follows.
        settingsOpen: settingsWindow.visible

        onSettingsClicked: {
            // A press while the pane is up puts it away, exactly as
            // closing it by hand does (the window hides, not closes).
            if (settingsWindow.visible) {
                settingsWindow.hide()
                return
            }

            // The window re-reads keys.json when it opens, so hand-edits
            // and the installer's writes are reflected without a restart.
            Phoenix.PhoenixController.settings.refresh()
            settingsWindow.openSettings()
        }
    }

    fullRepresentation: Phoenix.CompactRepresentation {
        onRecordClicked: Phoenix.PhoenixController.toggleRecording()

        settingsOpen: settingsWindow.visible

        onSettingsClicked: {
            if (settingsWindow.visible) {
                settingsWindow.hide()
                return
            }

            Phoenix.PhoenixController.settings.refresh()
            settingsWindow.openSettings()
        }
    }
}

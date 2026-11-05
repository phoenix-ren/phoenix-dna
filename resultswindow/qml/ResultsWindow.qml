// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Window
import QtQuick.Dialogs
import QtQuick.Layouts
import QtCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.context as PhoenixContext
import org.phoenixdna.theme

/*
 * The results window — the widget's conversation window.
 *
 * A fully-fledged, decorated toplevel: the compositor owns its geometry,
 * so it is moved, resized, maximized and closed with the ordinary window
 * decorations, like any other window on the desktop. That is deliberate:
 * the previous incarnation was a frameless Plasma Dialog, a window class
 * the compositor refuses to move or resize interactively (startSystemMove
 * and startSystemResize are accepted and then ignored for it), so three
 * rounds of custom geometry code fought the shell and lost.
 *
 * The window opens as soon as a run produces its first output
 * (showLive()) and then streams the answer and the tool log as they
 * happen — pinned to the bottom, like a terminal. It is persistent: it
 * stays on screen until the user closes it (title bar or Esc), however
 * many results come and go in between; closing only hides it, so it
 * reappears where the compositor last had it — on Wayland the one
 * reliable way to "remember" a position, since a client can neither set
 * nor learn a toplevel's position there. The window *size* the client
 * does know, so it is persisted across restarts in the XDG state
 * directory and restored on creation.
 *
 * Its history comes from the conversation model (one row per prompt/
 * answer pair, fed by the agent bridge): follow-on questions append a new
 * row and the view flows downwards, ChatGPT style, instead of replacing
 * the previous result.
 *
 * The bottom bar continues the conversation: it replicates the two input
 * affordances of the panel widget — the record button from the compact
 * representation (a follow-on can be dictated) and the editable transcript
 * field plus "Send" button from the dictation overlay (a dictated follow-on
 * can be corrected, or one simply typed). Both emit signals that the shell
 * routes through the controller, so a follow-on rides on the same session
 * history as the initial voice prompt.
 *
 * The bar's own record button is a follow-on: it continues the session
 * the window shows. A dictation started from the panel widget's record
 * button lands in this very bar when the window happens to be open, and
 * that one starts a new session — the button that started the recording
 * decides, and the shell asks the controller about it.
 *
 * The shell binds the conversation model and the dictation state
 * (transcript/listening/transcribing/statusMessage); the window itself is
 * purely presentational.
 *
 * What the window shows is ordered by time and weighted by size: the
 * trail streams in first — what the agent did, in the order it did it:
 * the notes it thinks in, the words it says, the tools it calls — and
 * every step of it is folded away by default, one click each away; the
 * answer lands last, the one part rendered at full size, in a card of
 * its own. An answer the user has to dig for is an answer they miss,
 * and a trail that claims the answer's place buries it.
 *
 * The trail is one stream, not three piles: the agent's work is shown
 * as it happens, in the order it happens, so a run can be followed
 * while it is going on. A step that is still running says so, and says
 * for how long — the difference between an agent at work and an agent
 * that hung.
 *
 * The addresses in everything the window shows — the answer, the
 * notes, the tool log, the user's own prompts — are links: a file or
 * folder opens in the file manager (a file selected in its folder), a
 * page in the browser, a mailbox in the mail client, a phone number in
 * the dialer. The detection is the window's own
 * (MarkdownDocument.linkify, on the rendered document, after the
 * Markdown import), so the daemon is untouched by it.
 *
 * The answer's math takes a two-stage path: while the run streams, the
 * Markdown view shows the LaTeX verbatim; when the run finishes and the
 * answer carries math, the math spans are compiled by LaTeX and swapped
 * in as tinted images inside the very same Markdown document — the text
 * around them stays native, with the app's font, size, margins, lists
 * and this one scrollbar. A failed compile — no engine installed, a
 * formula LaTeX rejects — keeps the verbatim view and says why.
 */
Window {
    id: resultsWindow

    // The conversation history: one row per prompt/answer pair, with the
    // roles prompt / result / running / steps (see ConversationModel).
    // Bound by the shell to the agent bridge's model.
    property var conversation: null

    // The CPU meter of the command the agent is running (see AgentBridge):
    // while a bash tool call runs, its step shows the meter as an audio
    // level meter — the level, its held peak, the share of the machine.
    // Bound by the shell to the agent bridge's meter; null leaves the
    // meter out (no meter to follow).
    property var cpuMeter: null

    // The rate of the conversation with the model (see AgentBridge): the
    // bytes that go out and come in, as a pair of level meters under the
    // running thought step. Bound by the shell to the agent bridge's
    // meter; null leaves the meters out.
    property var wireMeter: null

    // The progress of the transfer the agent is running (see
    // AgentBridge): the pace of the bytes and the share of the thing
    // that is here, drawn under the running transfer step — a level
    // meter and a progress bar, so a download that takes an hour is an
    // hour of something to watch. Bound by the shell to the agent
    // bridge's meter; null leaves the meter out.
    property var transferMeter: null

    // What the session on screen spent of the model's context window: the
    // map the daemon answered with (see AgentBridge::sessionContext).
    // Bound by the shell; the ring in the corner of the window reads it.
    property var context: null

    // Emitted when the context ring is clicked: the details are wanted.
    // The shell opens the context overlay, which has them.
    signal contextRequested()

    // ---- Follow-on input, mirroring the dictation overlay ---------------
    // The transcript of a follow-on being dictated into the window, and the
    // dictation state. Bound by the shell exactly like the overlay's; the
    // transcript is pushed imperatively (not bound), because the editor
    // below is editable — a binding would be broken by the first edit.
    property string transcript: ""
    property bool listening: false
    property bool transcribing: false
    property string statusMessage: ""

    // Whether the prompt in the editor came from a recording rather than
    // from the keyboard. Set by the shell when a dictated transcript
    // lands, and cleared when the user empties the editor: a dictated
    // prompt carries the intent of the button that started the recording
    // — a new session for the panel widget's record button, a follow-on
    // for this window's own record button — while a typed one continues
    // the conversation on screen.
    property bool dictated: false

    // Emitted when the user empties the input bar: the dictated
    // transcript is gone, so what is typed in its place is a fresh
    // query rather than the recording's prompt.
    signal dictationDiscarded()

    // The files attached to the prompt being composed: a list of maps with
    // "name", "path", "mimeType" and "size" keys. Bound by the shell; the
    // same list the dictation overlay shows, because both edit the same
    // prompt.
    property var attachments: []

    // The form the running prompt is blocked on (ask_user): a map with
    // askId, questions ([{question, header, options, multiple}]) and
    // optionally goal / harness. Bound by the shell to the agent
    // bridge's pendingForm; {} while no form is waiting. One question
    // renders as the inline card; several open the wizard view.
    property var pendingForm: ({})

    // Whether a form is waiting — a card or the wizard overlay is up.
    readonly property bool formPending: pendingForm
        && pendingForm.askId !== undefined

    // Whether the pending form is a wizard (several questions) whose
    // overlay the user dismissed: the slim card below the conversation
    // then offers to bring it back. Set by the shell, which owns the
    // overlay.
    property bool wizardOverlayDismissed: false

    // Emitted when the user submits a form's answers: one
    // {selected, text} map per question, in order (the card's single
    // question is a form of one). The shell routes it to the agent
    // bridge, which sends it as AnswerQuestion over D-Bus.
    signal formSubmitted(var answers)

    // Emitted when the user wants the dismissed wizard overlay back.
    signal showQuestionsRequested()

    // Emitted when the user picks a file to attach to the follow-up.
    signal attachmentAdded(url file)

    // Emitted when the user takes an attached file off the follow-up.
    signal attachmentRemoved(int index)

    // Whether the window picker is waiting for the user to click a window
    // of the desktop; bound by the shell.
    property bool screenshotPicking: false

    // The global shortcut that starts the window picker; bound by the
    // shell (empty when nothing is bound).
    property string screenshotShortcut: ""

    // Emitted when the user wants to photograph a window of the desktop.
    signal screenshotRequested()

    // Emitted when the window goes away (title bar close, Esc, toggle()).
    signal dismissed()

    // Emitted when the input bar's record button is clicked — the same
    // toggle the panel widget's record button performs.
    signal recordClicked()

    // Emitted when the user sends a follow-on (typed or dictated); the
    // shell submits it over the existing session history. The second
    // argument says whether the prompt came from a recording, so the
    // shell can tell a dictated prompt from a typed one.
    signal submitted(string text, bool dictated)

    // Emitted when the user forks the conversation at one of its
    // queries: the session is branched into a new one at that point,
    // and the follow-ups continue the fork. The shell routes it to the
    // agent bridge, which asks the daemon to do the branching.
    //
    // The argument counts the user's queries: 1 is the first thing the
    // user asked in the session, 2 the next one.
    signal forkRequested(int exchange)

    // A word about what just happened to the conversation - a fork, for
    // instance. Shown above the input bar for a few seconds and then
    // gone again; the shell fills it in.
    property string notice: ""

    // How long the notice stays: long enough to be read, short enough
    // not to become furniture.
    property int noticeDuration: 5000

    // Takes the notice away again.
    Timer {
        id: noticeTimer

        interval: resultsWindow.noticeDuration
        repeat: false
        onTriggered: resultsWindow.notice = ""
    }

    // Puts a notice on screen.
    function showNotice(text) {
        resultsWindow.notice = text;
        noticeTimer.restart();
    }

    // Whether an agent run is live right now: the send button is a stop
    // button while it is. Bound by the shell to the agent bridge, which
    // tracks the run from RunStarted to RunFinished — runs begun anywhere
    // (the panel's record button as much as a follow-on) count.
    property bool runActive: false

    // Emitted when the user stops the live run; the shell routes it to
    // the bridge, which speaks the daemon's Cancel(run_id).
    signal cancelRequested()

    // Whether a follow-on can be submitted right now: there is text, and
    // no dictation is in flight. The send button and the Return shortcut
    // both consult it, so the two paths can never disagree.
    readonly property bool canSubmit: transcript.length > 0
        && !listening && !transcribing

    // The current session's LLM-generated title, as announced by the
    // daemon (and re-generated on every follow-on). It rides the window
    // title, so the taskbar entry names the conversation too.
    property string sessionTitle: ""

    // Read-aloud state, bound by the shell to the agent bridge: whether
    // the daemon's speech is configured at all (the per-message
    // affordance is hidden when it is not), and whether the daemon is
    // currently reading something aloud (the stop control shows then).
    property bool speechAvailable: false
    property bool agentSpeaking: false

    // Emitted when a message's "Read aloud" is clicked. The shell routes
    // the text to the daemon's SpeakText; the daemon normalizes and
    // segments it exactly as it does an answer (math is spoken as
    // mathematics, code fences are skipped) — the shell never
    // synthesizes.
    signal readAloudRequested(string text)

    // Emitted when the stop control interrupts the read-aloud.
    signal stopSpeakingRequested()

    // The gap between two paragraphs of the agent's Markdown, which Qt
    // renders with none at all (see MarkdownDocument.spaceParagraphs);
    // a line of the answer's own size, so paragraphs read as
    // paragraphs and the text breathes with the size it is set in.
    readonly property real paragraphSpacing:
        PhoenixTheme.answerParagraphSpacing

    // Tokens for the formula renders of finished answers; the delegates
    // match their own token in LatexRenderer's signals.
    property int formulaTokens: 0

    function nextFormulaToken() {
        return ++formulaTokens
    }

    // The clock the running steps read their elapsed time from. It
    // ticks once a second, so a step that takes long says how long it
    // has been running — the difference between an agent at work and an
    // agent that hung.
    property var now: Date.now()

    Timer {
        interval: 1000
        repeat: true
        running: resultsWindow.visible
        onTriggered: resultsWindow.now = Date.now()
    }

    // A duration, the way a stopwatch reads it: seconds, then minutes
    // and seconds, then hours and minutes.
    function formatElapsed(milliseconds) {
        const total = Math.max(0, Math.round(milliseconds / 1000))
        if (total < 60) {
            return i18n("%1s", total)
        }
        const minutes = Math.floor(total / 60)
        if (minutes < 60) {
            return i18n("%1m %2s", minutes, total % 60)
        }
        return i18n("%1h %2m", Math.floor(minutes / 60), minutes % 60)
    }

    // What a transfer call is called in the trail's titles: the doing
    // while it runs, the done once it is over — "Downloading…" reads
    // better than "Used 1 transfer", and the step is labelled as the
    // specific thing it is.
    function transferLabel(direction, running) {
        if (direction === "upload") {
            return running ? i18n("Uploading…") : i18n("Uploaded")
        }
        return running ? i18n("Downloading…") : i18n("Downloaded")
    }

    // The header of a folded-away step: the notes the agent thinks in
    // and the tools it calls are context, not the answer, and they
    // start folded — the title, the arrow that opens the step and the
    // time it took are always there, the content only when the user
    // asks for it.
    component SectionHeader: Item {
        id: sectionHeader

        // The step's name.
        property string title: ""

        // Whether the step's content is shown.
        property bool expanded: false

        // Whether the step is still running: a spinner joins the title,
        // so a run that hangs is visible as such.
        property bool busy: false

        // Whether the step failed: its title carries the error tint.
        property bool failed: false

        // How long the step took — or, while it runs, how long it has
        // been running.
        property string detail: ""

        // Whether the pointer is on the header: the title brightens
        // then, to show that the section opens on a click.
        readonly property bool hovered: hoverArea.containsMouse

        // Emitted when the header is clicked.
        signal toggled()

        implicitHeight: headerRow.implicitHeight

        RowLayout {
            id: headerRow

            anchors.fill: parent
            spacing: Kirigami.Units.smallSpacing

            // The arrow: down while the section is open, to the right
            // while it is folded. Brighter while the pointer is on the
            // header: the section is one click away.
            Kirigami.Icon {
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                source: LucideIcons.url(sectionHeader.expanded ? "chevron-down" : "chevron-right",
                    sectionHeader.hovered ? PhoenixTheme.hoveredTextColor
                                          : PhoenixTheme.secondaryTextColor)
            }

            Kirigami.Heading {
                Layout.fillWidth: true
                // KF6 6.31 has no Kirigami.Heading.Level enum; levels are plain ints.
                level: 6
                text: sectionHeader.title
                color: sectionHeader.failed ? PhoenixTheme.errorColor
                     : (sectionHeader.hovered ? PhoenixTheme.hoveredTextColor
                                             : PhoenixTheme.secondaryTextColor)
            }

            // The spinner of a step that is still running.
            PC3.BusyIndicator {
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                visible: sectionHeader.busy
                running: sectionHeader.busy
            }

            // How long the step took.
            PC3.Label {
                visible: sectionHeader.detail.length > 0

                text: sectionHeader.detail
                color: PhoenixTheme.secondaryTextColor
                font.pixelSize: Math.max(
                    9, Math.round(Qt.application.font.pixelSize * 0.8))
            }
        }

        // The whole header is the button.
        MouseArea {
            id: hoverArea

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: sectionHeader.toggled()
        }
    }

    title: sessionTitle.length > 0
        ? i18n("%1 — Phoenix DNA", sessionTitle)
        : i18n("Conversation — Phoenix DNA")

    // The window's own icon: the app's avatar, as the taskbar entry of
    // the conversation window shows it - the same mark every other window
    // of the app carries.
    //
    // QML has no window-icon API (QIcon is not a QML type), so the icon
    // is put on the window from C++ (see AppIcon).
    Component.onCompleted: AppIcon.setWindowIcon(resultsWindow)

    // A dialog-style toplevel: no minimise/maximise noise in the taskbar
    // entry beyond the title, still fully decorated, movable and
    // resizable by the compositor. Keep-above preserves the pane's old
    // "the result announces itself" behaviour: nothing may stack on it.
    flags: Qt.Dialog | Qt.WindowStaysOnTopHint

    // The compositor resizes the window; these are the floors it may not
    // resize past.
    minimumWidth: Math.round(Kirigami.Units.gridUnit * 12)
    minimumHeight: Math.round(Kirigami.Units.gridUnit * 8)

    // The remembered size, or a sensible default: wide enough for a
    // paragraph, about two thirds of the available height. After the
    // first drag this is whatever the user left the window at.
    width: geometryStore.windowWidth > 0
        ? geometryStore.windowWidth
        : Math.round(Kirigami.Units.gridUnit * 30)
    height: geometryStore.windowHeight > 0
        ? geometryStore.windowHeight
        : Math.round(Math.max(
            Kirigami.Units.gridUnit * 12,
            Math.min(Screen.desktopAvailableHeight * 0.66,
                     Screen.desktopAvailableHeight - 2 * Kirigami.Units.gridUnit)))

    visible: false

    // The window size is the one piece of geometry the client reliably
    // knows on Wayland; persist it when the window goes away, so restarts
    // find it at the user's size. Position needs no bookkeeping: the
    // compositor keeps a hidden-again window where the user had it.
    onVisibleChanged: if (!visible && width > 0 && height > 0) {
        geometryStore.windowWidth = Math.round(width)
        geometryStore.windowHeight = Math.round(height)
    }

    // Closing from the title bar hides instead of destroying: the shell
    // owns the window's lifetime, and the next result reopens it right
    // where it was.
    onClosing: (close) => {
        close.accepted = false
        hide()
        dismissed()
    }

    // Engine results are pushed into the editor imperatively (not via a
    // text binding): a binding would be destroyed by the first user edit,
    // and a later transcript would then never appear — same guard as the
    // dictation overlay, whose editor this bar replicates.
    onTranscriptChanged: followUpEdit.text = transcript

    // Puts the keyboard in the follow-up editor: the window's reason to
    // be open is a conversation, so a shown window is ready to be typed
    // into (and Return submitted) without a click into the field
    // first.
    function focusInput() {
        if (!followUpEdit.readOnly) {
            followUpEdit.forceActiveFocus()
        }
    }

    // Shows a result: brings the window up and pins the scroll to the
    // newest exchange — answers flow downwards, ChatGPT style, so the
    // newest content is what the user wants to see.
    function showResult() {
        scroller.pinnedToBottom = true
        AppIcon.showWindow(resultsWindow)
        raise()
        requestActivate()
        // The pin alone does not move the view: a window that was
        // reopened (or one whose content was already there) sits
        // wherever the last read left it, and the newest exchange must
        // be the thing in view.
        scroller.followToBottom()
        Qt.callLater(focusInput)
    }

    // Opens the window for a streaming run: the run's text and tool log
    // arrive as they happen, following the newest content.
    function showLive() {
        scroller.pinnedToBottom = true
        AppIcon.showWindow(resultsWindow)
        raise()
        requestActivate()
        scroller.followToBottom()
        Qt.callLater(focusInput)
    }

    // Manual toggle, e.g. from the applet: bring the conversation back up,
    // or hide the window if it is already showing.
    function toggle() {
        if (visible) {
            dismiss()
        } else if (conversation && conversation.count > 0) {
            showResult()
        }
    }

    function dismiss() {
        hide()
        dismissed()
    }

    // The remembered window size, persisted across plasmashell restarts in
    // an INI file under the XDG state directory.
    Settings {
        id: geometryStore

        category: "Geometry"
        location: StandardPaths.writableLocation(StandardPaths.GenericStateLocation)
            + "/phoenix-dna/results-window.ini"
        property real windowWidth: 0
        property real windowHeight: 0
    }

    // Wide enough for a paragraph, like the dictation overlay; the user's
    // dragged size (persisted above) overrides the defaults.
    Item {
        id: content

        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: resultsWindow.dismiss()

        // Return submits a follow-on from anywhere in the window: the
        // shortcut lives on the window rather than on the editor, because
        // keyboard focus often rests elsewhere (a just-shown window, the
        // send button) and a key handler on the editor would simply never
        // fire — previously the only keyboard way to send was pressing
        // Space on the focused button. "Return" covers the main Enter
        // key, "Enter" the numpad's. Shift+Return is *not* a shortcut:
        // it stays the line break it is in the editor, so a long query
        // can be written without the first Return sending it away.
        Shortcut {
            sequences: ["Return", "Enter"]
            context: Qt.WindowShortcut
            enabled: resultsWindow.canSubmit
            onActivated: resultsWindow.submitted(resultsWindow.transcript,
                                                  resultsWindow.dictated)
        }

        // The semi-transparent backdrop, painted after the chosen colour
        // scheme (see PhoenixTheme): the text on it is fixed with it.
        // Mostly opaque: readable over busy desktops, with just a hint of
        // the desktop through it.
        Rectangle {
            anchors.fill: parent
            color: PhoenixTheme.backgroundColor
        }

        ColumnLayout {
            id: body

            anchors.fill: parent
            // A full gridUnit of padding on every side: largeSpacing (~half
            // that) made the text hug the window's edges and read cramped.
            anchors.margins: Kirigami.Units.gridUnit
            spacing: Kirigami.Units.smallSpacing

            // ---- Top: the session's name and its context ring ------------
            // The ring says how much of the model's context window this
            // session has used; a click on it opens the context overlay,
            // which has the numbers behind that share. The context of a
            // session belongs to the session, so it is here and not in
            // the panel widget: the widget is the app's presence, not
            // any one conversation's.
            RowLayout {
                id: headerBar

                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    Layout.fillWidth: true
                    // KF6 6.31 has no Kirigami.Heading.Level enum; levels are plain ints.
                    level: 6
                    elide: Text.ElideRight

                    text: resultsWindow.sessionTitle.length > 0
                        ? resultsWindow.sessionTitle
                        : i18n("Conversation")
                    color: PhoenixTheme.subheadingTextColor
                }

                PhoenixContext.ContextRing {
                    id: contextRing
                    objectName: "contextRing"

                    Layout.preferredWidth: Kirigami.Units.iconSizes.large
                    Layout.preferredHeight: Kirigami.Units.iconSizes.large

                    context: resultsWindow.context

                    onClicked: resultsWindow.contextRequested()
                }
            }

            Flickable {
                id: scroller
                objectName: "scroller"

                Layout.fillWidth: true
                Layout.fillHeight: true


                contentWidth: width
                // The content's height is the column's *laid-out* height,
                // not its implicit one: a Repeater's delegates are not
                // counted in the implicit height of the layout they
                // live in, so the implicit height reported what the
                // conversation had been a layout pass ago — a value
                // that lagged behind every arrival, and that the
                // follow then chased. The laid-out height is the real
                // one.
                contentHeight: column.height
                clip: true

                // The scrollbar for a conversation longer than the pane.
                PC3.ScrollBar.vertical: PC3.ScrollBar {
                    id: verticalBar
                    objectName: "verticalBar"
                }

                // The scrollbar lies over the content, so the text must
                // not run under it. Its width is reserved *always*, not
                // only while it is visible: a column that changes width
                // with the scrollbar re-wraps the whole conversation,
                // which changes the content's height and jumps the
                // scroll — the "jagged" descent, seen afresh every time
                // the scrollbar appeared or vanished.
                readonly property real scrollbarInset:
                    verticalBar.width

                // Terminal behaviour: keep the newest exchange in view —
                // the conversation flows downwards. Scrolling up to read
                // older results unpins; showLive()/showResult() re-pin.
                property bool pinnedToBottom: false

                // How close to the bottom the user has to be for the
                // conversation to keep following itself. Generous on
                // purpose: the content is polished in several passes
                // (the Markdown import, the paragraph spacing, the
                // link pass, the formula swap), each of which changes
                // the height a little, so a tight threshold would drop
                // the pin on the first of them and leave the answer
                // stranded mid-window. A line of slack is enough to
                // absorb the passes and still short enough that a
                // deliberate scroll upward unpins.
                readonly property real followTolerance:
                    Math.max(Kirigami.Units.smallSpacing * 2,
                             Kirigami.Units.gridUnit)

                // The scroll position that shows the newest content.
                //
                // A function, not a binding: the binding's value is
                // stale at the very moment it is needed — the height
                // changes, and the handler that follows it reads the
                // bound value *before* the dependency has been
                // re-evaluated, so the follow aimed at the bottom as it
                // was a moment ago. That is how the view came to rest a
                // screenful short of the newest content.
                function bottomPosition() {
                    return Math.max(0, contentHeight - height)
                }

                // Whether the user is at the bottom right now.
                readonly property bool userAtEnd:
                    contentY >= bottomPosition() - followTolerance

                // Follows the newest content, if the conversation is
                // following itself. Called whenever the content's height
                // changes — which happens several times per arrival, as
                // the document is polished — and after the formula swap
                // rewrites the document wholesale.
                function followToBottom() {
                    if (!pinnedToBottom) {
                        return
                    }
                    // The height may still be settling (the polish
                    // passes are queued), so the scroll is applied
                    // again on the next turn of the loop rather than
                    // only once — otherwise the last pass wins and the
                    // view rests a few pixels short of the bottom.
                    scrollToBottom()
                    followPending = true
                    // A new change: the quiet period starts afresh.
                    settlePending = false
                    settleCount = 0
                }

                // Moves the view to the newest content.
                function scrollToBottom() {
                    contentY = bottomPosition()
                }

                // Whether another scroll-to-bottom is owed: the polish
                // passes change the height after the fact, so the
                // position is re-applied until the height stops
                // moving.
                property bool followPending: false

                // The scroll position of the newest content, captured
                // across a document rewrite. The formula swap replaces
                // the whole document (setMarkdown), and the view's
                // position does not survive that — it snaps back to the
                // top, and the answer the user was reading is gone from
                // the window. The exchange asks for this before its
                // rewrite and restores the position after it.
                property real savedBottomY: 0
                property bool hasSavedBottom: false

                // Saves the position across a document rewrite that will
                // invalidate it.
                function saveBottom() {
                    savedBottomY = bottomPosition()
                    hasSavedBottom = true
                }

                // Restores the position after such a rewrite, if the
                // conversation was following itself when it was saved.
                function restoreBottom() {
                    if (!hasSavedBottom) {
                        return
                    }
                    hasSavedBottom = false
                    if (!pinnedToBottom) {
                        return
                    }
                    contentY = Math.max(0, savedBottomY)
                    followPending = true
                }

                // The user took hold of the content: the conversation
                // stops following itself — unless they stopped within
                // the tolerance of the bottom, which is the follow
                // resuming rather than the user reading further up.
                //
                // The Flickable's own movement signals are the honest
                // signal here: they begin when the *user* takes hold of
                // the content, never when the window moves it. Inferring
                // the user's scroll from the position alone could not
                // work — the window's own move to the bottom looks
                // exactly like the user scrolling away, and the two
                // raced with the content's height changing underneath
                // (which is how the view came to rest a screenful
                // short of the newest content).
                function onUserScroll() {
                    if (userAtEnd) {
                        pinnedToBottom = true
                    } else {
                        pinnedToBottom = false
                    }
                }

                // The user dragging the content.
                onMovementStarted: onUserScroll()
                // The user flicking it.
                onFlickStarted: onUserScroll()

                onContentHeightChanged: followToBottom()

                // The content's height settles over a few frames: the
                // Markdown import, the paragraph spacing and the link
                // pass are queued, and each of them measures the
                // document again. The scroll is re-applied until the
                // height stops moving, so the conversation descends in
                // one smooth run instead of lurching once per pass.
                //
                // The follow stays armed for a few frames after the last
                // change: the passes land in *later* frames, not within
                // the one the height change was noticed in, so a follow
                // that gave up at the first quiet moment would rest a
                // whole screenful short of the bottom.
                Timer {
                    id: followTimer

                    interval: 0
                    repeat: true
                    running: scroller.followPending

                    onTriggered: {
                        // The height the last scroll was made for.
                        const settled = scroller.contentHeight

                        scroller.scrollToBottom()

                        // The height moved while we scrolled: another
                        // pass has landed, and the scroll is owed again.
                        if (scroller.contentHeight !== settled) {
                            scroller.followPending = true
                            return
                        }

                        // The height held still. The passes are over —
                        // or the next one is a frame away. Either way,
                        // the follow rests for a moment and is re-armed
                        // by the next height change, which starts it
                        // again.
                        scroller.followPending = false
                        scroller.settlePending = true
                    }
                }

                // Whether the content's height is still settling. The
                // window keeps following while it is.
                property bool settlePending: false

                // The grace period: how long the follow rests after the
                // height held still, waiting for the next pass.
                readonly property int settleFrames: 3

                // Counts the quiet frames.
                property int settleCount: 0

                Timer {
                    id: settleTimer

                    interval: 0
                    repeat: true
                    running: scroller.settlePending

                    onTriggered: {
                        scroller.settleCount += 1
                        if (scroller.settleCount >= scroller.settleFrames) {
                            scroller.settlePending = false
                            scroller.settleCount = 0
                            return
                        }
                        // Still settling: keep the view at the bottom.
                        scroller.scrollToBottom()
                    }
                }

                ColumnLayout {
                    id: column
                    width: scroller.width - scroller.scrollbarInset
                    spacing: Kirigami.Units.largeSpacing

                    // One delegate per prompt/answer pair, oldest first:
                    // follow-on questions append below, ChatGPT style, and
                    // the scroller follows the newest.
                    //
                    // NOTE: preferredWidth, not fillWidth — Qt ignores
                    // fillWidth on layout-typed Repeater delegates (the row
                    // would shrink to its implicit width, capping the
                    // prompt bubbles at a fraction of the window).
                    Repeater {
                        model: resultsWindow.conversation

                        delegate: ColumnLayout {
                            id: exchange

                            required property int index
                            required property string prompt
                            required property string sender
                            required property string result
                            required property bool running
                            required property var steps

                            // The exchange's number: the user's queries
                            // are counted, so the first thing they asked
                            // in the session is 1. It is what the fork is
                            // taken at.
                            required property int exchangeNumber

                            // Whether the pointer is on the exchange: the
                            // fork button appears then, and only then.
                            readonly property bool hovered: exchangeHover.hovered

                            // Which steps of the trail are open, keyed by
                            // the step's own id: the trail grows while
                            // the run goes on, and the steps that were
                            // opened stay open.
                            property var openSteps: ({})

                            // The pointer on the exchange. A passive
                            // handler: it watches, it does not swallow
                            // anything the prompt's own text does with
                            // the mouse.
                            HoverHandler {
                                id: exchangeHover
                            }

                            // ---- The answer's math, rendered -------------
                            // While the run streams, the answer shows as
                            // native Markdown with its LaTeX verbatim.
                            // When the run finishes and the answer carries
                            // LaTeX, the math spans are compiled and
                            // swapped in as tinted images — the text
                            // around them stays native: the app's font,
                            // size, margins, lists and scrolling. A
                            // failed compile (no engine installed, a
                            // formula LaTeX rejects) keeps the verbatim
                            // view — no retry storm.
                            property int formulaToken: -1
                            property string formulaFailedReason: ""

                            // -1: idle; 0..100 while the formulas render.
                            property real renderProgress: -1

                            readonly property bool wantsFormulas: !running
                                && result.length > 0
                                // -1: never requested. -2: requested and
                                // failed — the verbatim view stays, and
                                // the answer is not asked again (a retry
                                // every second would be a storm).
                                && formulaToken === -1
                                && LatexRenderer.available
                                && LatexRenderer.containsLatex(result)

                            // Every settling path leads to the request:
                            // the final text and the finished flag may
                            // arrive in either order (and a reopened
                            // window starts with both already set), so
                            // the guard above — not the caller — decides
                            // whether a request goes out.
                            onRunningChanged: maybeEmbedFormulas()
                            onResultChanged: {
                                // A result that changes again after the
                                // formulas went in (a late correction)
                                // resets the round: the verbatim binding
                                // re-applies, the guard re-arms.
                                if (formulaToken > 0) {
                                    formulaToken = -1
                                    formulaFailedReason = ""
                                }
                                maybeEmbedFormulas()
                            }
                            Component.onCompleted: maybeEmbedFormulas()

                            function maybeEmbedFormulas() {
                                if (!wantsFormulas) {
                                    return
                                }
                                formulaToken = resultsWindow.nextFormulaToken()
                                renderProgress = 0
                                // The embed replaces the whole document
                                // (setMarkdown), which leaves the view's
                                // scroll position behind: the position is
                                // saved now and restored when the embed
                                // reports back.
                                scroller.saveBottom()
                                LatexRenderer.embedFormulas(
                                    formulaToken, resultLabel.textDocument,
                                    result, PhoenixTheme.primaryTextColor,
                                    resultLabel.font.pixelSize,
                                    resultsWindow.paragraphSpacing)
                            }

                            // A theme switch re-tints the formulas: the
                            // compiled pages are cached, so this is an
                            // instant re-render in the other ink, never a
                            // recompile. The token moves with it: the
                            // render that is on its way must not be
                            // mistaken for this one's result.
                            function reembedFormulas() {
                                // Only a rendered answer is re-tinted: an
                                // answer that was never rendered gets its
                                // first render from maybeEmbedFormulas(),
                                // and one that failed is not asked again.
                                if (formulaToken <= 0 || !resultLabel) {
                                    return
                                }
                                formulaToken = resultsWindow.nextFormulaToken()
                                renderProgress = 0
                                // The re-embed replaces the document too:
                                // the position is saved and restored the
                                // same way.
                                scroller.saveBottom()
                                LatexRenderer.embedFormulas(
                                    formulaToken, resultLabel.textDocument,
                                    result, PhoenixTheme.primaryTextColor,
                                    resultLabel.font.pixelSize,
                                    resultsWindow.paragraphSpacing)
                            }

                            // The links of this exchange, in the link ink
                            // of the mode the window now runs in. A scheme
                            // switch changes nothing in the documents —
                            // only the ink the anchors wear — so this is
                            // the one walk linkify() makes again, and its
                            // idempotence keeps it to exactly that.
                            function relinkify() {
                                const ink = PhoenixTheme.linkColor
                                Qt.callLater(MarkdownDocument.linkify,
                                             promptLabel.textDocument, ink)
                                Qt.callLater(MarkdownDocument.linkify,
                                             trailText.textDocument, ink)
                                Qt.callLater(MarkdownDocument.linkify,
                                             trailThought.textDocument, ink)
                                Qt.callLater(MarkdownDocument.linkify,
                                             trailTool.textDocument, ink)
                                Qt.callLater(MarkdownDocument.linkify,
                                             resultLabel.textDocument, ink)
                            }

                            Connections {
                                target: LatexRenderer

                                function onEmbedded(token) {
                                    if (token === exchange.formulaToken) {
                                        exchange.renderProgress = -1
                                        // The embed rebuilt the document
                                        // from the raw Markdown, so the
                                        // answer's bare addresses are
                                        // anchors again — and the
                                        // Markdown's own links are back
                                        // in the palette's ink.
                                        Qt.callLater(
                                            MarkdownDocument.linkify,
                                            resultLabel.textDocument,
                                            PhoenixTheme.linkColor)
                                        // The rebuild left the scroll
                                        // position behind; it is put
                                        // back, so the answer stays where
                                        // the user was reading it —
                                        // and where the following
                                        // content will continue from.
                                        scroller.restoreBottom()
                                    }
                                }

                                function onProgress(token, done, total) {
                                    if (token === exchange.formulaToken
                                        && total > 0) {
                                        exchange.renderProgress =
                                            Math.round(100 * done / total)
                                    }
                                }

                                function onFailed(token, reason) {
                                    // -2: requested, failed, never again
                                    // for this result.
                                    if (token === exchange.formulaToken) {
                                        exchange.formulaToken = -2
                                        exchange.formulaFailedReason = reason
                                        exchange.renderProgress = -1
                                        // No rebuild happened, so the
                                        // position was never lost — but
                                        // the save must not be left
                                        // dangling.
                                        scroller.restoreBottom()
                                    }
                                }
                            }

                            Connections {
                                target: PhoenixTheme

                                function onIsDarkChanged() {
                                    exchange.reembedFormulas()
                                    exchange.relinkify()
                                }
                            }

                            // Safety net for the change-triggered paths:
                            // they can die mid-flight (they did, in the
                            // field — a streaming delta's stack overflow
                            // took the finished-flag handler with it),
                            // so while the exchange runs, or wanted
                            // formulas have not been requested yet,
                            // re-check every second. Once the request is
                            // out the net stops: wantsFormulas turns
                            // false on its own.
                            Timer {
                                interval: 1000
                                repeat: true
                                running: exchange.running
                                         || exchange.wantsFormulas
                                onTriggered: exchange.maybeEmbedFormulas()
                            }

                            Layout.preferredWidth: column.width
                            spacing: Kirigami.Units.smallSpacing

                            // The user's prompt, as a chat bubble: pinned
                            // to the right, hugging its content up to four
                            // fifths of the window, with a tail pointing
                            // down-right and the sender's name underneath
                            // it — the bubble points at whoever issued it
                            // ("root" or the login name, per the
                            // session the prompt belongs to). Plain
                            // text — it is dictated, not authored
                            // Markdown. Hidden for runs not started from
                            // this widget.
                            ColumnLayout {
                                id: promptGroup

                                visible: exchange.prompt.length > 0
                                Layout.alignment: Qt.AlignRight
                                spacing: 0

                                Rectangle {
                                    id: promptBubble

                                    objectName: "promptBubble"

                                    Layout.alignment: Qt.AlignRight
                                    // Hug the text, but wrap (and so stop
                                    // growing) at four fifths of the row.
                                    // NOTE: implicitWidth, not width — a
                                    // ColumnLayout sizes its children from
                                    // their implicit size, and a width
                                    // binding here would be clobbered on
                                    // the first layout pass, collapsing
                                    // the bubble to nothing.
                                    implicitWidth: Math.min(
                                        exchange.width * 0.8,
                                        promptLabel.implicitWidth
                                            + 2 * Kirigami.Units.smallSpacing)
                                    implicitHeight: promptLabel.implicitHeight
                                                    + 2 * Kirigami.Units.smallSpacing

                                    radius: Kirigami.Units.smallSpacing * 2
                                    color: PhoenixTheme.bubbleFill
                                    border.width: 1
                                    border.color: PhoenixTheme.bubbleBorderColor

                                    Kirigami.SelectableLabel {
                                        id: promptLabel

                                        anchors.fill: parent
                                        anchors.margins: Kirigami.Units.smallSpacing

                                        text: exchange.prompt
                                        textFormat: TextEdit.PlainText
                                        wrapMode: Text.WordWrap
                                        color: PhoenixTheme.textColor
                                        // A dictated address is a link
                                        // like any other; the click is
                                        // handed to the desktop.
                                        onLinkActivated: (link) =>
                                            MarkdownDocument.openLink(link)
                                        // Breathing room between the
                                        // paragraphs of a long dictation.
                                        onTextChanged: {
                                            Qt.callLater(
                                                MarkdownDocument.spaceParagraphs,
                                                textDocument,
                                                resultsWindow.paragraphSpacing)
                                            Qt.callLater(
                                                MarkdownDocument.linkify,
                                                textDocument,
                                                PhoenixTheme.linkColor)
                                        }
                                        // The spacing follows the size
                                        // of the text: when the user
                                        // changes it, the paragraphs are
                                        // spaced again.
                                        onFontChanged: Qt.callLater(
                                            MarkdownDocument.spaceParagraphs,
                                            textDocument,
                                            resultsWindow.paragraphSpacing)
                                    }
                                }

                                // The pointy edge: a small tail dropping
                                // from the bubble's bottom-right corner,
                                // overlapping the bubble's border by a
                                // pixel so the two read as one shape.
                                Canvas {
                                    objectName: "promptTail"

                                    Layout.alignment: Qt.AlignRight
                                    Layout.rightMargin: Kirigami.Units.smallSpacing * 2
                                    Layout.topMargin: -1
                                    implicitWidth: Kirigami.Units.smallSpacing * 2
                                    implicitHeight: Kirigami.Units.smallSpacing
                                    onPaint: {
                                        const ctx = getContext("2d")
                                        ctx.fillStyle = promptBubble.color
                                        ctx.strokeStyle = promptBubble.border.color
                                        ctx.lineWidth = 1
                                        // Fill: top edge sits on the
                                        // bubble's border row, apex at the
                                        // bottom right.
                                        ctx.beginPath()
                                        ctx.moveTo(0, 0)
                                        ctx.lineTo(width, 0)
                                        ctx.lineTo(width - 1, height - 0.5)
                                        ctx.closePath()
                                        ctx.fill()
                                        // Stroke only the two outer edges.
                                        ctx.beginPath()
                                        ctx.moveTo(width, 0)
                                        ctx.lineTo(width - 1, height - 0.5)
                                        ctx.lineTo(0, 0)
                                        ctx.stroke()
                                    }
                                }

                                // The sender the tail points at, and the
                                // fork button beside it.
                                RowLayout {
                                    id: promptMeta

                                    Layout.alignment: Qt.AlignRight
                                    Layout.topMargin: 2
                                    spacing: Kirigami.Units.smallSpacing

                                    Kirigami.SelectableLabel {
                                        objectName: "senderLabel"

                                        visible: exchange.sender.length > 0

                                        text: exchange.sender
                                        textFormat: TextEdit.PlainText
                                        font.pixelSize: Math.max(
                                            9, Math.round(
                                                Qt.application.font.pixelSize * 0.8))
                                        color: PhoenixTheme.secondaryTextColor
                                    }

                                    // The fork: the conversation on screen,
                                    // branched into a new session at this
                                    // very query.
                                    //
                                    // The fork is the other way round: the
                                    // session it was taken from keeps what
                                    // came after, and the follow-ups typed
                                    // after the fork continue the fork — a
                                    // second line of thought, with the
                                    // first one left alone.
                                    //
                                    // It stands quietly until the pointer
                                    // comes near it, and says what it does
                                    // in the accent colour then. While a
                                    // run is live it is disabled: a
                                    // conversation that is still being
                                    // written cannot be forked at a query
                                    // that has not been answered yet.
                                    PC3.ToolButton {
                                        id: forkButton
                                        objectName: "forkButton"

                                        visible: exchange.hovered
                                        enabled: !resultsWindow.runActive
                                            && exchange.exchangeNumber > 0

                                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                        Layout.preferredHeight: Kirigami.Units.iconSizes.small

                                        icon.source: LucideIcons.url("git-branch",
                                            hovered ? PhoenixTheme.linkColor
                                                    : PhoenixTheme.secondaryTextColor)
                                        icon.width: Kirigami.Units.iconSizes.small
                                        icon.height: Kirigami.Units.iconSizes.small

                                        onClicked: resultsWindow.forkRequested(exchange.exchangeNumber)

                                        PC3.ToolTip {
                                            text: resultsWindow.runActive
                                                ? i18n("The conversation cannot be forked while the agent is working")
                                                : i18n("Fork the conversation from here")
                                        }
                                    }
                                }
                            }

                            // The trail of the run: what the agent
                            // did, in the order it did it. The notes it
                            // thinks in, the words it says and the
                            // tools it calls are one stream of steps,
                            // not three piles — so the run can be
                            // followed while it happens, and a step
                            // that hangs says so (and says for how
                            // long).
                            ColumnLayout {
                                id: trailSection
                                objectName: "trailSection"

                                visible: exchange.steps.length > 0
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing

                                Repeater {
                                    id: trailRepeater
                                    objectName: "trailRepeater"

                                    model: exchange.steps

                                    delegate: ColumnLayout {
                                        id: trailItem
                                        objectName: "trailItem"

                                        required property var modelData
                                        required property int index

                                        Layout.fillWidth: true
                                        spacing: 0

                                        // Whether this step is open. The
                                        // state is kept by the exchange
                                        // and keyed by the step's own
                                        // id, so it survives the trail
                                        // growing under it.
                                        readonly property bool open:
                                            exchange.openSteps[trailItem.modelData.id] === true

                                        function toggle() {
                                            const steps = Object.assign({}, exchange.openSteps)
                                            const id = trailItem.modelData.id
                                            if (steps[id]) {
                                                delete steps[id]
                                            } else {
                                                steps[id] = true
                                            }
                                            exchange.openSteps = steps
                                        }

                                        // How long the step took — and,
                                        // while it runs, how long it has
                                        // been running. A step that took
                                        // less than a second says
                                        // nothing: it is over, and it
                                        // was quick.
                                        readonly property string elapsed: {
                                            const step = trailItem.modelData
                                            if (step.running) {
                                                return resultsWindow.formatElapsed(
                                                    resultsWindow.now - step.startedAt)
                                            }
                                            if (!step.startedAt || !step.endedAt) {
                                                return ""
                                            }
                                            const spent = step.endedAt - step.startedAt
                                            return spent < 1000
                                                ? ""
                                                : resultsWindow.formatElapsed(spent)
                                        }

                                        // ---- The words the agent says --
                                        // The preamble of a stretch that
                                        // turned out to be work: shown
                                        // as it was said, in the place it
                                        // was said — and in the same
                                        // style as the answer below it.
                                        // The words the agent says are
                                        // the agent's words, whether they
                                        // are the answer or the words
                                        // before the work; only the
                                        // step titles around them are
                                        // dimmed.
                                        Kirigami.SelectableLabel {
                                            id: trailText
                                            objectName: "trailText"

                                            Layout.fillWidth: true
                                            visible: trailItem.modelData.kind === "text"

                                            text: trailItem.modelData.body
                                            textFormat: TextEdit.MarkdownText
                                            wrapMode: Text.WordWrap
                                            // The same ink as the answer:
                                            // these are the agent's words
                                            // too.
                                            color: PhoenixTheme.primaryTextColor
                                            font.family: PhoenixTheme.answerFontFamily
                                            font.pixelSize: PhoenixTheme.answerFontSize
                                            onTextChanged: {
                                                Qt.callLater(
                                                    MarkdownDocument.spaceParagraphs,
                                                    textDocument, resultsWindow.paragraphSpacing)
                                                Qt.callLater(
                                                    MarkdownDocument.linkify,
                                                    textDocument,
                                                    PhoenixTheme.linkColor)
                                            }
                                            // What these words name — a
                                            // file, a page, an address —
                                            // the desktop opens.
                                            onLinkActivated: (link) =>
                                                MarkdownDocument.openLink(link)
                                        }

                                        // ---- The notes it thinks in ----
                                        // The model's reasoning: not
                                        // words it says to the user, but
                                        // the thoughts it thinks in — so
                                        // it keeps the context style (as
                                        // OpenCode greys its reasoning
                                        // out), behind a title.
                                        SectionHeader {
                                            Layout.fillWidth: true
                                            visible: trailItem.modelData.kind === "thought"

                                            title: i18n("Used %1 Thought",
                                                       trailItem.modelData.count)
                                            expanded: trailItem.open
                                            busy: trailItem.modelData.running
                                            detail: trailItem.elapsed
                                            onToggled: trailItem.toggle()
                                        }

                                        // ---- The rate of the talk ----
                                        // While the model is thinking, the
                                        // pace of the conversation with it:
                                        // what goes out on the wire and what
                                        // comes back, as a pair of level
                                        // meters (the request on top, the
                                        // answer below). The rates are
                                        // moving averages of the traffic, and
                                        // the bars are scaled against the
                                        // busiest each direction has been.
                                        WireMeterView {
                                            Layout.fillWidth: true
                                            Layout.topMargin: Kirigami.Units.smallSpacing
                                            // The whole meter keeps to a third
                                            // of the window, as the CPU meter
                                            // does.
                                            Layout.maximumWidth: Math.min(
                                                resultsWindow.width * 0.33, 300)

                                            readonly property var meter:
                                                resultsWindow.wireMeter

                                            showing: Boolean(meter && meter.active
                                                && trailItem.modelData.running)
                                            sendRate: meter ? meter.sendRate : 0.0
                                            receiveRate: meter ? meter.receiveRate : 0.0
                                            sendPeak: meter ? meter.sendPeak : 0.0
                                            receivePeak: meter ? meter.receivePeak : 0.0
                                            sendFraction: meter ? meter.sendFraction : 0.0
                                            receiveFraction:
                                                meter ? meter.receiveFraction : 0.0
                                            turn: meter ? meter.turn : 0
                                        }

                                        Kirigami.SelectableLabel {
                                            id: trailThought
                                            objectName: "trailThought"

                                            Layout.fillWidth: true
                                            visible: trailItem.open
                                                    && trailItem.modelData.kind === "thought"

                                            // The text follows the fold: a
                                            // folded step carries no
                                            // document. A TextEdit imports
                                            // its Markdown into a
                                            // QTextDocument on every change
                                            // of its text, and the notes
                                            // grow with every 100 ms delta
                                            // flush — on the GUI thread, in
                                            // plasmashell, for text nobody
                                            // could see. The import happens
                                            // when the step is opened
                                            // instead: once, on demand, and
                                            // live while it is watched.
                                            text: trailItem.open
                                                ? trailItem.modelData.body : ""
                                            textFormat: TextEdit.MarkdownText
                                            wrapMode: Text.WordWrap
                                            color: PhoenixTheme.secondaryTextColor
                                            // The agent's own words, in the
                                            // family's voice as well — the
                                            // notes are prose, not chrome.
                                            font.family: PhoenixTheme.answerFontFamily
                                            font.pixelSize: PhoenixTheme.notesFontSize
                                            onTextChanged: {
                                                Qt.callLater(
                                                    MarkdownDocument.spaceParagraphs,
                                                    textDocument, resultsWindow.paragraphSpacing)
                                                Qt.callLater(
                                                    MarkdownDocument.linkify,
                                                    textDocument,
                                                    PhoenixTheme.linkColor)
                                            }
                                            // Even the private notes name
                                            // files and pages; opened on a
                                            // click like the answer's own.
                                            onLinkActivated: (link) =>
                                                MarkdownDocument.openLink(link)
                                        }

                                        // ---- The tools it calls -------
                                        // What it actually did: every
                                        // call with its arguments and
                                        // its output. The evidence for
                                        // the answer, not the answer
                                        // itself — so it is dimmed and
                                        // folded away, one click away.
                                        SectionHeader {
                                            id: toolStepHeader
                                            objectName: "toolStepHeader"

                                            Layout.fillWidth: true
                                            visible: trailItem.modelData.kind === "tool"

                                            // A step of transfer calls is
                                            // titled by what it is doing —
                                            // "Downloading…" while it runs,
                                            // "Downloaded" once it is over —
                                            // rather than by the tool's
                                            // name: the doing is the title,
                                            // the tool is the detail. A step
                                            // that mixes transfers with
                                            // other tools keeps the "Used n
                                            // …" shape, the transfer as its
                                            // verb in the list. Tool calls
                                            // run one at a time, so of a
                                            // step's calls only the last can
                                            // be the one still running.
                                            title: {
                                                const step = trailItem.modelData
                                                let allTransfers = step.tools.length > 0
                                                const parts = []
                                                for (let i = 0; i < step.tools.length; ++i) {
                                                    const tool = step.tools[i]
                                                    if (tool === "transfer") {
                                                        const direction = i < step.directions.length
                                                            && step.directions[i] !== ""
                                                            ? step.directions[i] : "download"
                                                        parts.push(resultsWindow.transferLabel(
                                                            direction,
                                                            step.running && i === step.tools.length - 1))
                                                    } else {
                                                        allTransfers = false
                                                        parts.push(tool)
                                                    }
                                                }
                                                if (allTransfers) {
                                                    return parts.join(", ")
                                                }
                                                return i18n("Used %1 %2",
                                                            step.count, parts.join(", "))
                                            }
                                            expanded: trailItem.open
                                            busy: trailItem.modelData.running
                                            failed: trailItem.modelData.status === "error"
                                            detail: trailItem.elapsed
                                            onToggled: trailItem.toggle()
                                        }

                                        // ---- The meter under the work ----
                                        // While this step's command runs, its
                                        // CPU usage as an audio level meter:
                                        // the agent at work, visible from
                                        // across the room. The meter is the
                                        // bridge's one live meter (the run's
                                        // tools execute one at a time), and it
                                        // shows exactly while a command of
                                        // this step is in flight.
                                        CpuMeterView {
                                            Layout.fillWidth: true
                                            Layout.topMargin: Kirigami.Units.smallSpacing
                                            // The whole meter — caption, bar and
                                            // readouts together — never grows past
                                            // a third of the window, nor 300 px:
                                            // a level meter of this shape reads
                                            // best compact, and the trail's text
                                            // keeps the room it needs.
                                            Layout.maximumWidth: Math.min(
                                                resultsWindow.width * 0.33, 300)

                                            readonly property var meter:
                                                resultsWindow.cpuMeter

                                            showing: Boolean(meter && meter.active
                                                && trailItem.modelData.running)
                                            fraction: meter ? meter.fraction : 0.0
                                            peak: meter ? meter.peak : 0.0
                                            cores: meter ? meter.cores : 0.0
                                            // The readouts' columns are reserved
                                            // for the machine's widest values, so
                                            // the bar cannot breathe with the
                                            // digits.
                                            maxCores: meter ? meter.coreCount : 16
                                        }

                                        // ---- The meter under the transfer ----
                                        // While this step's transfer runs, the
                                        // pace of its bytes and the share of
                                        // the thing that is here: the level
                                        // meter for the pace (the same row
                                        // the wire meter draws, "DL" or "UL"),
                                        // the progress bar for the distance
                                        // still to go. The run's tools execute
                                        // one at a time, so the bridge's one
                                        // transfer meter serves whoever is
                                        // running, and it shows exactly while
                                        // a transfer of this step is in
                                        // flight.
                                        TransferMeterView {
                                            Layout.fillWidth: true
                                            Layout.topMargin: Kirigami.Units.smallSpacing
                                            // As the CPU meter beside it: a
                                            // transfer meter of this shape
                                            // reads best compact.
                                            Layout.maximumWidth: Math.min(
                                                resultsWindow.width * 0.33, 300)

                                            readonly property var meter:
                                                resultsWindow.transferMeter

                                            showing: Boolean(meter && meter.active
                                                && trailItem.modelData.running)
                                            uploading: meter ? meter.uploading : false
                                            done: meter ? meter.done : 0.0
                                            total: meter ? meter.total : 0.0
                                            rate: meter ? meter.rate : 0.0
                                            rateFraction: meter
                                                ? meter.rateFraction : 0.0
                                        }

                                        Kirigami.SelectableLabel {
                                            id: trailTool
                                            objectName: "trailTool"

                                            Layout.fillWidth: true
                                            visible: trailItem.open
                                                    && trailItem.modelData.kind === "tool"

                                            // Like the notes above: a folded
                                            // step imports nothing. The log
                                            // of a stretch of work is long,
                                            // and it grew twice per tool
                                            // call of the run — every growth
                                            // re-imported the entire log
                                            // into the document, on the GUI
                                            // thread, for a step that
                                            // starts folded.
                                            text: trailItem.open
                                                ? trailItem.modelData.body : ""
                                            textFormat: TextEdit.MarkdownText
                                            wrapMode: Text.WordWrap
                                            color: PhoenixTheme.secondaryTextColor
                                            // Smaller than the answer, and
                                            // in the plain type: a log to
                                            // read when wanted, not text
                                            // to read past.
                                            font.pixelSize: Math.round(
                                                Qt.application.font.pixelSize * 0.85)
                                            onTextChanged: {
                                                Qt.callLater(
                                                    MarkdownDocument.spaceParagraphs,
                                                    textDocument, resultsWindow.paragraphSpacing)
                                                Qt.callLater(
                                                    MarkdownDocument.linkify,
                                                    textDocument,
                                                    PhoenixTheme.linkColor)
                                            }
                                            // The commands and their
                                            // output name files most of
                                            // all — a click shows them in
                                            // the file manager.
                                            onLinkActivated: (link) =>
                                                MarkdownDocument.openLink(link)
                                        }
                                    }
                                }
                            }

                            // The agent's answer, last in the exchange:
                            // the one thing the window exists to show,
                            // so it is the only part rendered at full
                            // size — with the attribution below it.
                            // Rendered as Markdown: the agent mostly
                            // answers in plain sentences, but code and
                            // lists do occur.
                            ColumnLayout {
                                id: answerGroup

                                Layout.fillWidth: true
                                spacing: 0

                                // The answer itself: bigger than the
                                // rest of the window, brighter, and set
                                // off from the notes and the log that
                                // precede it — but on the window's own
                                // background, without a card around it:
                                // the answer is the window's text, and
                                // the formulas in it are drawn on the
                                // same background, the way the text
                                // beside them is.
                                Kirigami.SelectableLabel {
                                    id: resultLabel
                                    objectName: "resultLabel"

                                    Layout.fillWidth: true
                                    visible: exchange.result.length > 0

                                    // The answer as Markdown, its
                                    // LaTeX verbatim — until the run
                                    // finishes and the formulas are
                                    // compiled into images inside this
                                    // very document (the text around
                                    // them stays this native view).
                                    text: exchange.result
                                    textFormat: TextEdit.MarkdownText
                                    wrapMode: Text.WordWrap
                                    color: PhoenixTheme.primaryTextColor
                                    // The agent's own voice, in the
                                    // family the user chose for this
                                    // mode (see PhoenixTheme): the
                                    // answer is the one text set
                                    // apart from the chrome.
                                    font.family: PhoenixTheme.answerFontFamily
                                    // Larger than the rest of the
                                    // window: this is the text the
                                    // user came for.
                                    font.pixelSize: PhoenixTheme.answerFontSize
                                    // Re-space after every change: the
                                    // answer streams in as deltas,
                                    // each rebuilding the document.
                                    // Deferred out of the change
                                    // cascade (and coalesced): the
                                    // document edits of one polish
                                    // must not synchronously begin
                                    // the next.
                                    onTextChanged: {
                                        Qt.callLater(
                                            MarkdownDocument.spaceParagraphs,
                                            textDocument,
                                            resultsWindow.paragraphSpacing)
                                        // The answer's addresses become
                                        // links with the same deferred
                                        // walk — after the Markdown
                                        // import, so the sentence's
                                        // punctuation is known to be
                                        // decoration, not part of the
                                        // address.
                                        Qt.callLater(
                                            MarkdownDocument.linkify,
                                            textDocument,
                                            PhoenixTheme.linkColor)
                                    }
                                    // A click goes to the desktop:
                                    // the file manager, the browser,
                                    // the mail client, the dialer.
                                    onLinkActivated: (link) =>
                                        MarkdownDocument.openLink(link)
                                }

                                // While the formulas render, a quiet
                                // progress line under the card — the
                                // pool delivers one tick per formula.
                                RowLayout {
                                    Layout.fillWidth: true
                                    visible: exchange.renderProgress >= 0

                                    PC3.ProgressBar {
                                        Layout.preferredWidth:
                                            Kirigami.Units.gridUnit * 8
                                        from: 0
                                        to: 100
                                        value: exchange.renderProgress
                                    }

                                    PC3.Label {
                                        text: i18n("Rendering math… %1%",
                                                   exchange.renderProgress)
                                        color: PhoenixTheme.secondaryTextColor
                                        font.pixelSize: Math.max(
                                            9, Math.round(
                                                Qt.application.font.pixelSize
                                                * 0.8))
                                    }
                                }

                                // When a compile fails, the verbatim view
                                // stays — and says why, so a broken
                                // formula or a missing TeX engine is
                                // diagnosable instead of mysterious.
                                Kirigami.SelectableLabel {
                                    Layout.fillWidth: true
                                    visible: exchange.formulaFailedReason.length > 0
                                    wrapMode: Text.WordWrap
                                    text: i18n("Math not rendered: %1",
                                               exchange.formulaFailedReason)
                                    color: PhoenixTheme.secondaryTextColor
                                    font.pixelSize: Math.max(
                                        9, Math.round(
                                            Qt.application.font.pixelSize * 0.8))
                                }

                                // Who answered — and, while the agent is
                                // still working, a spinner next to its
                                // name.
                                RowLayout {
                                    Layout.alignment: Qt.AlignLeft
                                    Layout.topMargin: 2
                                    spacing: Kirigami.Units.smallSpacing

                                    PC3.BusyIndicator {
                                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                        visible: exchange.running
                                        running: exchange.running
                                    }

                                    Kirigami.SelectableLabel {
                                        id: agentLabel
                                        objectName: "agentLabel"

                                        text: i18n("Agent")
                                        textFormat: TextEdit.PlainText
                                        font.pixelSize: Math.max(
                                            9, Math.round(
                                                Qt.application.font.pixelSize * 0.8))
                                        color: PhoenixTheme.secondaryTextColor
                                    }

                                    // Read the answer of this exchange
                                    // aloud, on demand: the daemon does
                                    // the speaking (and the math, and
                                    // the interrupting); hidden unless
                                    // the daemon's speech is configured.
                                    //
                                    // It reads the answer and nothing
                                    // else: the trail's words are the
                                    // agent's working notes, and they
                                    // are never spoken. The button only
                                    // exists once the run is over, and
                                    // by then the model's role holds the
                                    // answer alone.
                                    PC3.ToolButton {
                                        id: readAloudButton
                                        objectName: "readAloudButton"

                                        visible: !exchange.running
                                            && exchange.result.length > 0
                                            && resultsWindow.speechAvailable
                                        enabled: visible

                                        icon.source: LucideIcons.url("megaphone", PhoenixTheme.textColor)
                                        icon.width: Kirigami.Units.iconSizes.small
                                        icon.height: Kirigami.Units.iconSizes.small

                                        onClicked:
                                            resultsWindow.readAloudRequested(exchange.result)

                                        PC3.ToolTip {
                                            text: i18n("Read aloud")
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ---- The agent's lone question ---------------------------------
            // ask_user blocks the run on this form; when it is a form of
            // one question (or a harness confirmation), the card under
            // the conversation is how it is answered (the options,
            // own-words field and send live in QuestionInputs, shared
            // with the wizard view). Answering, a timeout or the run's
            // end takes it away.
            Rectangle {
                id: questionCard

                Layout.fillWidth: true
                implicitHeight: questionBody.implicitHeight
                                + 2 * Kirigami.Units.largeSpacing
                // A form of one question - or a wizard whose overlay the
                // user dismissed (the card offers to bring it back).
                visible: resultsWindow.formPending
                         && (resultsWindow.wizardOverlayDismissed
                             || (resultsWindow.pendingForm.questions ?? []).length < 2)

                // Themed like the conversation's bubbles (see
                // PhoenixTheme): the card reads correctly in both the
                // light and the dark scheme. A harness confirmation
                // carries the error tint.
                radius: Kirigami.Units.smallSpacing * 2
                color: PhoenixTheme.cardFill
                border.width: 1
                border.color: resultsWindow.pendingForm.harness === true
                    ? PhoenixTheme.errorColor
                    : PhoenixTheme.cardBorderColor

                ColumnLayout {
                    id: questionBody

                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.largeSpacing
                    spacing: Kirigami.Units.smallSpacing

                    // A harness-authored confirmation (destructive root
                    // command): not a question the model made up. The
                    // warning tint and the fixed Allow/Deny options below
                    // make that visible — this is the system asking.
                    Kirigami.Heading {
                        level: 5
                        visible: resultsWindow.pendingForm.harness === true
                        color: PhoenixTheme.errorColor

                        text: i18n("System confirmation — required before the command runs")
                    }

                    // The form's goal, when the model gave one, names
                    // what the lone question collects.
                    Kirigami.Heading {
                        level: 5
                        visible: text.length > 0
                        color: PhoenixTheme.secondaryTextColor

                        text: resultsWindow.pendingForm.goal ?? ""
                    }

                    Kirigami.Heading {
                        level: 4
                        visible: text.length > 0
                        color: PhoenixTheme.subheadingTextColor

                        text: {
                            const questions = resultsWindow.pendingForm.questions ?? [];
                            return questions.length > 0
                                ? (questions[0].header ?? "") : "";
                        }
                    }

                    Kirigami.SelectableLabel {
                        objectName: "questionLabel"

                        Layout.fillWidth: true

                        text: {
                            const questions = resultsWindow.pendingForm.questions ?? [];
                            return questions.length > 0
                                ? (questions[0].question ?? "") : "";
                        }
                        textFormat: TextEdit.PlainText
                        wrapMode: Text.WordWrap
                        font.weight: Font.DemiBold
                        color: PhoenixTheme.headingTextColor
                    }

                    // The options, the own-words field and the send:
                    // shared with the wizard view.
                    QuestionInputs {
                        id: cardInputs

                        Layout.fillWidth: true
                        Layout.topMargin: Kirigami.Units.smallSpacing

                        question: {
                            const questions = resultsWindow.pendingForm.questions ?? [];
                            return questions.length > 0 ? questions[0] : {};
                        }

                        onSubmitted: (selected, text) =>
                            resultsWindow.formSubmitted([{ selected, text }])
                    }

                    Text {
                        Layout.fillWidth: true

                        text: i18n("The agent is waiting for this answer before it continues.")
                        font.pixelSize: Math.max(
                            9, Math.round(
                                Qt.application.font.pixelSize * 0.8))
                        color: PhoenixTheme.secondaryTextColor
                    }

                    // The dismissed wizard: its questions are still
                    // pending, and the run is still blocked - bring the
                    // overlay back.
                    PC3.Button {
                        visible: resultsWindow.wizardOverlayDismissed
                        icon.source: LucideIcons.url("clipboard-list", PhoenixTheme.textColor)
                        text: i18n("Show questions")
                        onClicked: resultsWindow.showQuestionsRequested()
                    }
                }
            }

            // ---- Bottom: the follow-on input bar ------------------------
            // Replicates the two input surfaces of the panel widget so the
            // conversation can continue right where its result is showing:
            // the compact representation's record button (dictate a
            // follow-on; it becomes a stop button while recording) and the
            // dictation overlay's editable transcript (correct a
            // recognition, or simply type). All of it lives inside the one
            // rounded prompt field below, and Return — not a click — is
            // what sends. The window stays presentational: recordClicked,
            // submitted and cancelRequested go to the shell, which routes
            // them through the controller and thus the same session
            // history the initial voice prompt created.
            Kirigami.Separator {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing
            }

            Item {
                id: followUpBar

                Layout.fillWidth: true
                implicitHeight: followUpColumn.implicitHeight

                ColumnLayout {
                    id: followUpColumn

                    anchors.fill: parent
                    spacing: Kirigami.Units.smallSpacing

                    // The files attached to the follow-up: one chip per
                    // file, each with a button that takes it off the list
                    // again.
                    Flow {
                        id: attachmentFlow

                        Layout.fillWidth: true
                        visible: resultsWindow.attachments.length > 0
                        spacing: Kirigami.Units.smallSpacing

                        Repeater {
                            id: attachmentRepeater
                            objectName: "attachmentRepeater"

                            model: resultsWindow.attachments

                            delegate: Rectangle {
                                id: attachmentChip

                                required property var modelData
                                required property int index

                                implicitWidth: chipRow.implicitWidth + 2 * Kirigami.Units.smallSpacing
                                implicitHeight: chipRow.implicitHeight + 2 * Kirigami.Units.smallSpacing

                                radius: Kirigami.Units.smallSpacing
                                color: PhoenixTheme.chipFill
                                border.width: 1
                                border.color: PhoenixTheme.chipBorderColor

                                RowLayout {
                                    id: chipRow

                                    anchors.fill: parent
                                    anchors.margins: Kirigami.Units.smallSpacing
                                    spacing: Kirigami.Units.smallSpacing

                                    Kirigami.Icon {
                                        source: LucideIcons.url("paperclip", PhoenixTheme.textColor)
                                        width: 16
                                        height: 16
                                    }

                                    Text {
                                        text: attachmentChip.modelData.name
                                        elide: Text.ElideMiddle
                                        color: PhoenixTheme.textColor
                                        Layout.maximumWidth: Kirigami.Units.gridUnit * 8
                                    }

                                    PC3.ToolButton {
                                        icon.source: LucideIcons.url("x", PhoenixTheme.textColor)
                                        icon.width: 12
                                        icon.height: 12

                                        onClicked: resultsWindow.attachmentRemoved(attachmentChip.index)
                                    }
                                }
                            }
                        }
                    }

                    // What just happened to the conversation: a fork
                    // taken here, the pane following the new session
                    // from now on. The notice says so, and goes away
                    // again by itself.
                    PC3.Label {
                        id: noticeLabel
                        objectName: "noticeLabel"

                        Layout.fillWidth: true
                        visible: resultsWindow.notice.length > 0
                        text: resultsWindow.notice
                        elide: Text.ElideRight
                        color: PhoenixTheme.secondaryTextColor
                        font.pixelSize: Math.max(
                            9, Math.round(Qt.application.font.pixelSize * 0.8))
                    }

                    // ---- The prompt field -----------------------------------
                    // One rounded box that holds the whole prompt: the editor
                    // on top, and inside it — on the row below the text — the
                    // controls that act on the prompt, with the send (or
                    // stop) button at the far right. Nothing dangles outside
                    // the field, so nothing can be mistaken for belonging to
                    // another part of the window, and the prompt reads as
                    // the one thing the bar is about.
                    Rectangle {
                        id: promptField

                        Layout.fillWidth: true
                        implicitHeight: fieldColumn.implicitHeight
                                        + 2 * Kirigami.Units.smallSpacing

                        // A large radius: the field is a container, not a
                        // text box — the controls sit inside it, and a
                        // shallow corner would read as a border of a row,
                        // not as the shell of a thing.
                        radius: Kirigami.Units.largeSpacing * 1.5
                        color: PhoenixTheme.paneFill
                        border.width: 1
                        // The keyboard's place is shown by the outline: the
                        // accent colour while the editor has it, the quiet
                        // chip colour otherwise.
                        border.color: followUpEdit.activeFocus
                            ? PhoenixTheme.focusBorderColor
                            : PhoenixTheme.chipBorderColor

                        ColumnLayout {
                            id: fieldColumn

                            anchors.fill: parent
                            anchors.margins: Kirigami.Units.smallSpacing
                            spacing: Kirigami.Units.smallSpacing

                            // The transcript, editable so a dictated follow-on
                            // can be corrected before sending — and typeable
                            // from scratch. The editor is the single source of
                            // truth for what "Send to agent" submits: engine
                            // results flow in via onTranscriptChanged above,
                            // edits flow back into resultsWindow.transcript here
                            // (guarded, so the two never ping-pong).
                            PC3.TextArea {
                                id: followUpEdit
                                objectName: "followUpEdit"

                                Layout.fillWidth: true
                                // A visibly multi-line field: two text lines
                                // tall even when empty (one line reads as a
                                // single-line edit box). Grows with the
                                // dictated or typed text — follow-ons are
                                // often longer and need clarifications — but
                                // never eats the conversation above it: past
                                // roughly six text lines the editor stops
                                // growing and its content scrolls inside.
                                // Measures are content-based, so the shape
                                // survives font and scale changes.
                                Layout.preferredHeight: Math.max(
                                    2 * contentHeight + topPadding + bottomPadding,
                                    Math.min(
                                        implicitHeight,
                                        6 * contentHeight + topPadding + bottomPadding))

                                // The field paints its own frame (see
                                // promptField above), so the editor inside it
                                // draws none of its own. Its text takes the
                                // app's palette for the same reason: the
                                // window paints its own backdrop (see
                                // PhoenixTheme), so the text must follow the
                                // scheme of everything around it, not the
                                // desktop's.
                                background: null
                                color: PhoenixTheme.textColor
                                placeholderTextColor: PhoenixTheme.secondaryTextColor

                                wrapMode: Text.Wrap
                                // Locked while the engine owns the text;
                                // editable the moment a result has landed.
                                readOnly: resultsWindow.listening || resultsWindow.transcribing
                                placeholderText: resultsWindow.transcribing ? i18n("Transcribing…")
                                                : resultsWindow.listening ? i18n("Speak now…")
                                                : resultsWindow.formPending ? i18n("Answer the question…")
                                                : i18n("Ask a follow-up…")

                                onTextChanged: if (text !== resultsWindow.transcript) {
                                    resultsWindow.transcript = text
                                    // The user emptied the editor: the
                                    // transcript of the recording is gone,
                                    // so what is typed in its place is a
                                    // fresh query — a follow-on of the
                                    // conversation on screen, not the new
                                    // session the recording's button asked
                                    // for. A correction is not an emptying:
                                    // it keeps the recording's intent.
                                    if (text.length === 0) {
                                        resultsWindow.dictationDiscarded()
                                    }
                                }

                                // Return submits, even with the keyboard in
                                // the editor: a text edit swallows the key
                                // before the window's shortcut ever gets to
                                // see it (it would insert a line break
                                // instead). The window shortcut above covers
                                // the case where the focus rests elsewhere;
                                // this one covers the editor itself, so the
                                // two can never both fire. Shift is the line
                                // break: it is simply not handled here, and
                                // the editor inserts it the way it always has.
                                Keys.onPressed: (event) => {
                                    if ((event.key === Qt.Key_Return
                                         || event.key === Qt.Key_Enter)
                                        && !(event.modifiers & Qt.ShiftModifier)) {
                                        event.accepted = true;
                                        if (resultsWindow.canSubmit) {
                                            resultsWindow.submitted(
                                                resultsWindow.transcript,
                                                resultsWindow.dictated);
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing

                                // The record button, as in the compact
                                // representation: microphone while idle, stop
                                // while recording. No colour tint.
                                PC3.ToolButton {
                                    id: recordButton
                                    objectName: "recordButton"

                                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium

                                    icon.source: LucideIcons.url(resultsWindow.listening ? "circle-stop" : "mic-audio-lines",
                                        PhoenixTheme.textColor)
                                    icon.width: Kirigami.Units.iconSizes.small
                                    icon.height: Kirigami.Units.iconSizes.small

                                    onClicked: resultsWindow.recordClicked()

                                    PC3.ToolTip {
                                        text: resultsWindow.listening ? i18n("Stop") : i18n("Record a follow-up")
                                    }
                                }

                                // The read-aloud stop control: the daemon is
                                // reading an answer (this exchange's or another
                                // one's, or an ad-hoc SpeakText) and this cuts
                                // it off. Shown only while it speaks — pressing
                                // record also stops the speech (barge-in), so
                                // the microphone never transcribes the agent's
                                // own voice.
                                PC3.ToolButton {
                                    id: stopSpeakingButton
                                    objectName: "stopSpeakingButton"

                                    visible: resultsWindow.agentSpeaking
                                    enabled: visible

                                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium

                                    icon.source: LucideIcons.url("circle-stop", PhoenixTheme.textColor)
                                    icon.width: Kirigami.Units.iconSizes.small
                                    icon.height: Kirigami.Units.iconSizes.small
                                    display: PC3.AbstractButton.IconOnly

                                    onClicked: resultsWindow.stopSpeakingRequested()

                                    PC3.ToolTip {
                                        text: i18n("Stop reading")
                                    }
                                }

                                // Runs while the dictated clip is being transcribed,
                                // as in the dictation overlay's header.
                                PC3.BusyIndicator {
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                                    visible: resultsWindow.transcribing
                                    running: resultsWindow.transcribing
                                }

                                // The button that attaches files to the follow-up.
                                PC3.ToolButton {
                                    id: addAttachmentButton
                                    objectName: "addAttachmentButton"

                                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium

                                    icon.source: LucideIcons.url("plus", PhoenixTheme.textColor)
                                    icon.width: Kirigami.Units.iconSizes.small
                                    icon.height: Kirigami.Units.iconSizes.small

                                    onClicked: attachmentDialog.open()

                                    PC3.ToolTip {
                                        text: i18n("Attach a file to the prompt")
                                    }
                                }

                                // The camera: photograph a window of the desktop
                                // and attach the picture to this follow-up.
                                PC3.ToolButton {
                                    id: screenshotButton
                                    objectName: "screenshotButton"

                                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium

                                    // While the picker waits for a click, the
                                    // button says so.
                                    icon.source: LucideIcons.url("camera", resultsWindow.screenshotPicking
                                        ? PhoenixTheme.strongTextColor
                                        : PhoenixTheme.textColor)
                                    icon.width: Kirigami.Units.iconSizes.small
                                    icon.height: Kirigami.Units.iconSizes.small

                                    onClicked: resultsWindow.screenshotRequested()

                                    PC3.ToolTip {
                                        text: {
                                            const shortcut = resultsWindow.screenshotShortcut;
                                            return shortcut.length > 0
                                                ? i18n("Screenshot a window (%1)", shortcut)
                                                : i18n("Screenshot a window");
                                        }
                                    }
                                }

                                // Everything before the send button hugs the
                                // field's left edge; this spacer carries the
                                // rest of the width to the send button, so the
                                // two ends read independently.
                                Item {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                }

                                // The send — a plain up arrow, the one glyph
                                // chat inputs have agreed on. While an agent
                                // run is live the very same button stops it:
                                // one square, one place to look, and the
                                // arrow's meaning (more words to the agent)
                                // is the wrong one mid-run anyway. The shape
                                // is fixed, so the two icons land in the same
                                // place and a click means one thing at a
                                // time.
                                PC3.Button {
                                    id: sendButton
                                    objectName: "sendButton"

                                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                                    display: PC3.AbstractButton.IconOnly

                                    icon.source: LucideIcons.url(resultsWindow.runActive
                                        ? "circle-stop"
                                        : "arrow-up", PhoenixTheme.textColor)
                                    icon.width: Kirigami.Units.iconSizes.small
                                    icon.height: Kirigami.Units.iconSizes.small
                                    // A run can always be stopped; the arrow
                                    // only fires when there is something to
                                    // send and no dictation is in flight.
                                    enabled: resultsWindow.runActive
                                        || resultsWindow.canSubmit

                                    onClicked: {
                                        if (resultsWindow.runActive) {
                                            resultsWindow.cancelRequested();
                                        } else {
                                            resultsWindow.submitted(
                                                resultsWindow.transcript,
                                                resultsWindow.dictated);
                                        }
                                    }

                                    PC3.ToolTip {
                                        text: resultsWindow.runActive
                                            ? i18n("Stop the running query")
                                            : i18n("Send to agent (Return)")
                                    }
                                }
                            }
                        }
                    }

                    // Why a clip could not be transcribed (missing key,
                    // network, quota); shown only when there is something
                    // to complain about.
                    Kirigami.SelectableLabel {
                        Layout.fillWidth: true
                        visible: resultsWindow.statusMessage.length > 0
                        wrapMode: Text.WordWrap
                        text: resultsWindow.statusMessage
                        // The palette's error colour: it sits on the
                        // window's own backdrop, whichever scheme runs.
                        color: PhoenixTheme.errorColor
                    }

                    // The file picker for the "+" button. Every picked file is
                    // handed to the shell, which owns the attachment list.
                    FileDialog {
                        id: attachmentDialog

                        title: i18n("Attach files to the prompt")
                        fileMode: FileDialog.OpenFiles
                        nameFilters: [
                            i18n("Images and documents"),
                            "*.png *.jpg *.jpeg *.gif *.webp *.bmp *.tif *.tiff *.pdf *.txt *.md *.csv *.log *.json *.xml *.yaml *.yml",
                            i18n("All files"),
                            "*"
                        ]

                        onAccepted: {
                            for (let i = 0; i < selectedFiles.length; ++i) {
                                resultsWindow.attachmentAdded(selectedFiles[i])
                            }
                        }
                    }
                }
            }
        }
    }
}

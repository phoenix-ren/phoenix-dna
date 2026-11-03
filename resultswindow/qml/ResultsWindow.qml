// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Window
import QtQuick.Dialogs
import QtQuick.Layouts
import QtCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
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
 * The shell binds the conversation model and the dictation state
 * (transcript/listening/transcribing/statusMessage); the window itself is
 * purely presentational.
 *
 * What the window shows is ordered by time and weighted by size: the
 * trail streams in first — the notes the agent wrote while it worked,
 * then the log of the tools it called — dimmed, set in a smaller type
 * and folded away by default, one click each away; the answer lands
 * last, the one part rendered at full size, in a card of its own. An
 * answer the user has to dig for is an answer they miss, and a trail
 * that claims the answer's place buries it.
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
    // roles prompt / result / toolLog / running (see ConversationModel).
    // Bound by the shell to the agent bridge's model.
    property var conversation: null

    // ---- Follow-on input, mirroring the dictation overlay ---------------
    // The transcript of a follow-on being dictated into the window, and the
    // dictation state. Bound by the shell exactly like the overlay's; the
    // transcript is pushed imperatively (not bound), because the editor
    // below is editable — a binding would be broken by the first edit.
    property string transcript: ""
    property bool listening: false
    property bool transcribing: false
    property string statusMessage: ""

    // The files attached to the prompt being composed: a list of maps with
    // "name", "path", "mimeType" and "size" keys. Bound by the shell; the
    // same list the dictation overlay shows, because both edit the same
    // prompt.
    property var attachments: []

    // The question the running prompt is blocked on (ask_user): a map
    // with askId, question, options ([{label, description}]), and header,
    // multiple, goal, step, estSteps when the model gave them. Bound by
    // the shell to the agent bridge's pendingQuestion; {} while no
    // question is waiting.
    property var pendingQuestion: ({})

    // The wizard state (a series of questions): {active, goal, step,
    // estSteps, waiting, answers}. While active, the conversation gives
    // way to the wizard view — one question at a time, with progress.
    // Bound by the shell to the agent bridge's wizardState; {} while no
    // wizard is (or was) in flight.
    property var wizard: ({})

    // Whether a question is waiting — the question card is on screen.
    readonly property bool questionPending: pendingQuestion
        && pendingQuestion.askId !== undefined

    // Whether the wizard view owns the window's centre: the bridge keeps
    // it up across the gaps between a series' questions.
    readonly property bool wizardActive: wizard.active === true

    // Emitted when the user answers the pending question: the 0-based
    // indices of the chosen options (may be empty) and their own words
    // (may be empty). The shell routes both to the agent bridge, which
    // sends them as AnswerQuestion over D-Bus.
    signal answerSubmitted(var selected, string text)

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
    // shell submits it over the existing session history.
    signal submitted(string text)

    // Whether a follow-on can be submitted right now: there is text, and
    // no dictation is in flight. The send button and the Shift+Enter
    // shortcut both consult it, so the two paths can never disagree.
    readonly property bool canSubmit: transcript.length > 0
        && !listening && !transcribing

    // The current session's LLM-generated title, as announced by the
    // daemon (and re-generated on every follow-on). It rides the window
    // title, so the taskbar entry names the conversation too.
    property string sessionTitle: ""

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

    // The header of a folded-away section: the thinking notes and the
    // tool log are context, not the answer, and they start folded —
    // the title and the arrow that opens the section are always there,
    // the content only when the user asks for it.
    component SectionHeader: Item {
        id: sectionHeader

        // The section's name.
        property string title: ""

        // Whether the section's content is shown.
        property bool expanded: false

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
            // while it is folded.
            Kirigami.Icon {
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                source: sectionHeader.expanded ? "arrow-down" : "arrow-right"
                // Brighter while the pointer is on the header: the
                // section is one click away.
                color: sectionHeader.hovered ? PhoenixTheme.hoveredTextColor
                                             : PhoenixTheme.secondaryTextColor
            }

            Kirigami.Heading {
                Layout.fillWidth: true
                // KF6 6.31 has no Kirigami.Heading.Level enum; levels are plain ints.
                level: 6
                text: sectionHeader.title
                color: sectionHeader.hovered ? PhoenixTheme.hoveredTextColor
                                             : PhoenixTheme.secondaryTextColor
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
    // into (and Shift+Enter submitted) without a click into the field
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
        show()
        raise()
        requestActivate()
        Qt.callLater(focusInput)
    }

    // Opens the window for a streaming run: the run's text and tool log
    // arrive as they happen, following the newest content.
    function showLive() {
        scroller.pinnedToBottom = true
        show()
        raise()
        requestActivate()
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

        // Shift+Enter submits a follow-on from anywhere in the window: the
        // shortcut lives on the window rather than on the editor, because
        // keyboard focus often rests elsewhere (a just-shown window, the
        // send button) and a key handler on the editor would simply never
        // fire. "Return" covers the main Enter key, "Enter" the numpad's.
        // Plain Enter keeps inserting the line break — no shortcut for it.
        Shortcut {
            sequences: ["Shift+Return", "Shift+Enter"]
            context: Qt.WindowShortcut
            enabled: resultsWindow.canSubmit
            onActivated: resultsWindow.submitted(resultsWindow.transcript)
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

            Flickable {
                id: scroller
                objectName: "scroller"

                Layout.fillWidth: true
                Layout.fillHeight: true

                // The wizard view replaces the conversation while the
                // agent is asking its series; a hidden item does not
                // take layout space, so the two cleanly alternate.
                visible: !resultsWindow.wizardActive

                contentWidth: width
                contentHeight: column.implicitHeight
                clip: true

                // The scrollbar for a conversation longer than the pane.
                PC3.ScrollBar.vertical: PC3.ScrollBar {
                    id: verticalBar
                    objectName: "verticalBar"
                }

                // The scrollbar lies over the content, so the text must
                // not run under it: the conversation is inset by its
                // width while it is there.
                readonly property real scrollbarInset:
                    verticalBar.visible ? verticalBar.width : 0

                // Terminal behaviour: keep the newest exchange in view —
                // the conversation flows downwards. Scrolling up to read
                // older results unpins; showLive()/showResult() re-pin.
                property bool pinnedToBottom: false

                readonly property bool atEnd:
                    contentY >= contentHeight - height - Kirigami.Units.smallSpacing

                onContentYChanged: pinnedToBottom = atEnd
                onContentHeightChanged: {
                    if (pinnedToBottom) {
                        Qt.callLater(function() {
                            contentY = Math.max(0, contentHeight - height)
                        })
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
                            required property string toolLog
                            required property bool running
                            required property string thinking
                            required property int toolCount

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
                                LatexRenderer.embedFormulas(
                                    formulaToken, resultLabel.textDocument,
                                    result, PhoenixTheme.primaryTextColor,
                                    resultLabel.font.pixelSize,
                                    resultsWindow.paragraphSpacing)
                            }

                            Connections {
                                target: LatexRenderer

                                function onEmbedded(token) {
                                    if (token === exchange.formulaToken) {
                                        exchange.renderProgress = -1
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
                                    }
                                }
                            }

                            Connections {
                                target: PhoenixTheme

                                function onIsDarkChanged() {
                                    exchange.reembedFormulas()
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
                            // mode the prompt was submitted in). Plain
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
                                        // Breathing room between the
                                        // paragraphs of a long dictation.
                                        onTextChanged: Qt.callLater(
                                            MarkdownDocument.spaceParagraphs,
                                            textDocument,
                                            resultsWindow.paragraphSpacing)
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

                                // The sender the tail points at.
                                Kirigami.SelectableLabel {
                                    objectName: "senderLabel"

                                    Layout.alignment: Qt.AlignRight
                                    Layout.topMargin: 2
                                    visible: exchange.sender.length > 0

                                    text: exchange.sender
                                    textFormat: TextEdit.PlainText
                                    font.pixelSize: Math.max(
                                        9, Math.round(
                                            Qt.application.font.pixelSize * 0.8))
                                    color: PhoenixTheme.secondaryTextColor
                                }
                            }

                            // What the agent said while it worked: the
                            // notes it wrote before it reached for a
                            // tool. The trail of the run, not its
                            // outcome — dimmed, in a smaller type and
                            // folded away, one click from view. It
                            // streams in above the answer it produced.
                            ColumnLayout {
                                id: thinkingSection
                                objectName: "thinkingSection"

                                // Whether the notes are shown: folded by
                                // default, for the answer's sake.
                                property bool expanded: false

                                visible: exchange.thinking.length > 0
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing

                                SectionHeader {
                                    Layout.fillWidth: true
                                    title: i18n("Thinking")
                                    expanded: thinkingSection.expanded
                                    onToggled: thinkingSection.expanded = !thinkingSection.expanded
                                }

                                Kirigami.SelectableLabel {
                                    id: thinkingLabel
                                    objectName: "thinkingLabel"

                                    Layout.fillWidth: true
                                    visible: thinkingSection.expanded

                                    text: exchange.thinking
                                    textFormat: TextEdit.MarkdownText
                                    wrapMode: Text.WordWrap
                                    color: PhoenixTheme.secondaryTextColor
                                    // The agent's own words, in the
                                    // family's voice as well — the
                                    // notes are prose, not chrome.
                                    font.family: PhoenixTheme.answerFontFamily
                                    // Smaller than the answer: context,
                                    // not the result.
                                    font.pixelSize: PhoenixTheme.notesFontSize
                                    onTextChanged: Qt.callLater(
                                        MarkdownDocument.spaceParagraphs,
                                        textDocument, resultsWindow.paragraphSpacing)
                                }
                            }

                            // What the agent actually did in this run:
                            // every tool call with its command and output.
                            // The evidence for the answer, not the answer
                            // itself — so it is dimmed and folded away
                            // like the notes above, and streams in above
                            // the answer it produced. Only present when
                            // something ran (plain answers often have no
                            // tool calls at all).
                            ColumnLayout {
                                id: toolSection
                                objectName: "toolSection"

                                // Whether the log is shown: folded by
                                // default, for the answer's sake.
                                property bool expanded: false

                                visible: exchange.toolLog.length > 0
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing

                                SectionHeader {
                                    Layout.fillWidth: true
                                    title: i18n("Tool activity (%1)", exchange.toolCount)
                                    expanded: toolSection.expanded
                                    onToggled: toolSection.expanded = !toolSection.expanded
                                }

                                Kirigami.SelectableLabel {
                                    id: toolLabel
                                    objectName: "toolLabel"

                                    Layout.fillWidth: true
                                    visible: toolSection.expanded

                                    text: exchange.toolLog
                                    textFormat: TextEdit.MarkdownText
                                    wrapMode: Text.WordWrap
                                    color: PhoenixTheme.secondaryTextColor
                                    // Smaller than the answer, and in the
                                    // plain type: a log to read when
                                    // wanted, not text to read past.
                                    font.pixelSize: Math.round(
                                        Qt.application.font.pixelSize * 0.85)
                                    onTextChanged: Qt.callLater(
                                        MarkdownDocument.spaceParagraphs,
                                        textDocument, resultsWindow.paragraphSpacing)
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
                                    onTextChanged: Qt.callLater(
                                        MarkdownDocument.spaceParagraphs,
                                        textDocument,
                                        resultsWindow.paragraphSpacing)
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
                                }
                            }
                        }
                    }
                }
            }

            // ---- The wizard view -------------------------------------------
            // A series of questions (ask_user with goal/step/est_steps)
            // takes over the window's centre: one question at a time, a
            // progress bar towards "the agent has what it needs", the
            // answers collected so far, and a skip that tells the agent
            // to proceed on its judgment. The bridge keeps the wizard up
            // across the gaps between the series' questions; when the
            // agent stops asking and goes to work, the conversation
            // returns.
            WizardView {
                id: wizardView

                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: resultsWindow.wizardActive

                wizard: resultsWindow.wizard
                question: resultsWindow.pendingQuestion

                onSubmitted: (selected, text) =>
                    resultsWindow.answerSubmitted(selected, text)
            }

            // ---- The agent's lone question ---------------------------------
            // ask_user blocks the run on this question; when it is not
            // part of a wizard series, the card under the conversation is
            // how it is answered (the options, own-words field and send
            // live in QuestionInputs, shared with the wizard view).
            // Answering, a timeout or the run's end takes it away.
            Rectangle {
                id: questionCard

                Layout.fillWidth: true
                implicitHeight: questionBody.implicitHeight
                                + 2 * Kirigami.Units.largeSpacing
                visible: resultsWindow.questionPending
                         && !resultsWindow.wizardActive

                // Themed like the conversation's bubbles (see
                // PhoenixTheme): the card reads correctly in both the
                // light and the dark scheme. A harness confirmation
                // carries the error tint.
                radius: Kirigami.Units.smallSpacing * 2
                color: PhoenixTheme.cardFill
                border.width: 1
                border.color: resultsWindow.pendingQuestion.harness === true
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
                        visible: resultsWindow.pendingQuestion.harness === true
                        color: PhoenixTheme.errorColor

                        text: i18n("System confirmation — required before the command runs")
                    }

                    // The wizard context, when the model gave the
                    // metadata on a lone question: "Question 2 of ~4 ·
                    // what is collected". (A real series shows the
                    // wizard view instead of this card.)
                    Kirigami.Heading {
                        level: 5
                        visible: text.length > 0
                        color: PhoenixTheme.secondaryTextColor

                        text: {
                            const question = resultsWindow.pendingQuestion;
                            if (question.step === undefined) {
                                return "";
                            }
                            let context = i18n("Question %1 of ~%2",
                                               question.step,
                                               question.estSteps ?? question.step);
                            if (question.goal) {
                                context += " · " + question.goal;
                            }
                            return context;
                        }
                    }

                    Kirigami.Heading {
                        level: 4
                        visible: text.length > 0
                        color: PhoenixTheme.subheadingTextColor
                        text: resultsWindow.pendingQuestion.header ?? ""
                    }

                    Kirigami.SelectableLabel {
                        objectName: "questionLabel"

                        Layout.fillWidth: true

                        text: resultsWindow.pendingQuestion.question ?? ""
                        textFormat: TextEdit.PlainText
                        wrapMode: Text.WordWrap
                        font.weight: Font.DemiBold
                        color: PhoenixTheme.headingTextColor
                    }

                    // The options, the own-words field and the send:
                    // shared with the wizard view.
                    QuestionInputs {
                        Layout.fillWidth: true
                        Layout.topMargin: Kirigami.Units.smallSpacing

                        question: resultsWindow.pendingQuestion

                        onSubmitted: (selected, text) =>
                            resultsWindow.answerSubmitted(selected, text)
                    }

                    Text {
                        Layout.fillWidth: true

                        text: i18n("The agent is waiting for this answer before it continues.")
                        font.pixelSize: Math.max(
                            9, Math.round(
                                Qt.application.font.pixelSize * 0.8))
                        color: PhoenixTheme.secondaryTextColor
                    }
                }
            }

            // ---- Bottom: the follow-on input bar ------------------------
            // Replicates the two input surfaces of the panel widget so the
            // conversation can continue right where its result is showing:
            // the compact representation's record button (dictate a
            // follow-on; it becomes a stop button while recording) and the
            // dictation overlay's editable transcript plus "Send" button
            // (correct a recognition, or simply type). The window stays
            // presentational: recordClicked and submitted go to the shell,
            // which routes both through the controller and thus the same
            // session history the initial voice prompt created.
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
                                        source: "mail-attachment"
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
                                        icon.name: "window-close"
                                        icon.width: 12
                                        icon.height: 12

                                        onClicked: resultsWindow.attachmentRemoved(attachmentChip.index)
                                    }
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

                            icon.name: resultsWindow.listening ? "media-playback-stop" : "audio-input-microphone"
                            icon.width: Kirigami.Units.iconSizes.small
                            icon.height: Kirigami.Units.iconSizes.small

                            onClicked: resultsWindow.recordClicked()

                            PC3.ToolTip {
                                text: resultsWindow.listening ? i18n("Stop") : i18n("Record a follow-up")
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

                        // The button that attaches files to the follow-up,
                        // on the left of the prompt field.
                        PC3.ToolButton {
                            id: addAttachmentButton
                            objectName: "addAttachmentButton"

                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium

                            icon.name: "list-add"
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

                            icon.name: "camera-photo"
                            icon.width: Kirigami.Units.iconSizes.small
                            icon.height: Kirigami.Units.iconSizes.small
                            // While the picker waits for a click, the
                            // button says so.
                            icon.color: resultsWindow.screenshotPicking
                                ? PhoenixTheme.strongTextColor
                                : PhoenixTheme.textColor

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

                            wrapMode: Text.Wrap
                            // Locked while the engine owns the text;
                            // editable the moment a result has landed.
                            readOnly: resultsWindow.listening || resultsWindow.transcribing
                            placeholderText: resultsWindow.transcribing ? i18n("Transcribing…")
                                            : resultsWindow.listening ? i18n("Speak now…")
                                            : resultsWindow.questionPending ? i18n("Answer the question…")
                                            : i18n("Ask a follow-up…")

                            onTextChanged: if (text !== resultsWindow.transcript) {
                                resultsWindow.transcript = text
                            }
                        }

                        // One explicit send, no accidental submissions —
                        // same contract as the overlay's button. Stays at
                        // its natural size so the label is never squeezed.
                        PC3.Button {
                            objectName: "sendButton"
                            Layout.alignment: Qt.AlignRight
                            icon.name: "document-send"
                            text: i18n("Send to agent")
                            enabled: resultsWindow.canSubmit
                            onClicked: resultsWindow.submitted(resultsWindow.transcript)

                            PC3.ToolTip {
                                text: i18n("Send to agent (Shift+Enter)")
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

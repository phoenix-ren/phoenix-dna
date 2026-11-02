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
 * What the window shows is weighted: the agent's answer is the point of
 * the run, so it is the only part rendered at full size, in a card of
 * its own. The two things that surround it — the notes the agent wrote
 * while it worked, and the log of the tools it called — are context:
 * they are dimmed, set in a smaller type and folded away by default,
 * one click each away. An answer the user has to dig for is an answer
 * they miss.
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

                Layout.fillWidth: true
                Layout.fillHeight: true

                contentWidth: width
                contentHeight: column.implicitHeight
                clip: true

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
                    width: scroller.width
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

                            // The agent's answer, on the left: the one
                            // thing the window exists to show, so it is
                            // the only part rendered at full size — in a
                            // card of its own, with the mirrored tail and
                            // the attribution below it.
                            // Rendered as Markdown: the agent mostly
                            // answers in plain sentences, but code and
                            // lists do occur.
                            ColumnLayout {
                                id: answerGroup

                                Layout.fillWidth: true
                                spacing: 0

                                // The answer itself, in a card: bigger
                                // than the rest of the window, brighter,
                                // and set off from the notes and the log
                                // that follow it.
                                Rectangle {
                                    id: answerCard
                                    objectName: "answerCard"

                                    Layout.alignment: Qt.AlignLeft
                                    visible: exchange.result.length > 0

                                    // The margin between the answer and
                                    // the card's edge.
                                    readonly property int padding:
                                        Kirigami.Units.smallSpacing * 2

                                    // Hug the text, but wrap (and so stop
                                    // growing) at nearly the whole row:
                                    // the answer is the main text of the
                                    // window and may use the width.
                                    // NOTE: implicitWidth, not width — a
                                    // ColumnLayout sizes its children
                                    // from their implicit size, and a
                                    // width binding here would be clobbered
                                    // on the first layout pass.
                                    implicitWidth: Math.min(
                                        exchange.width * 0.96,
                                        resultLabel.implicitWidth
                                            + 2 * answerCard.padding)
                                    implicitHeight: resultLabel.implicitHeight
                                        + 2 * answerCard.padding

                                    radius: Kirigami.Units.smallSpacing * 2
                                    color: PhoenixTheme.cardFill
                                    border.width: 1
                                    border.color: PhoenixTheme.cardBorderColor

                                    Kirigami.SelectableLabel {
                                        id: resultLabel
                                        objectName: "resultLabel"

                                        anchors.fill: parent
                                        anchors.margins: answerCard.padding

                                        text: exchange.result
                                        textFormat: TextEdit.MarkdownText
                                        wrapMode: Text.WordWrap
                                        color: PhoenixTheme.primaryTextColor
                                        // Larger than the rest of the
                                        // window: this is the text the
                                        // user came for.
                                        font.pixelSize: Math.round(
                                            Qt.application.font.pixelSize * 1.12)
                                    }
                                }

                                // The mirrored pointy edge, at the
                                // answer's bottom-left corner: it takes
                                // the card's colors, so the two read as
                                // one shape.
                                Canvas {
                                    id: answerTail
                                    objectName: "answerTail"

                                    Layout.alignment: Qt.AlignLeft
                                    Layout.leftMargin: Kirigami.Units.smallSpacing * 2
                                    Layout.topMargin: -1
                                    visible: exchange.result.length > 0
                                    implicitWidth: Kirigami.Units.smallSpacing * 2
                                    implicitHeight: Kirigami.Units.smallSpacing
                                    onPaint: {
                                        const ctx = getContext("2d")
                                        ctx.fillStyle = answerCard.color
                                        ctx.strokeStyle = answerCard.border.color
                                        ctx.lineWidth = 1
                                        ctx.beginPath()
                                        ctx.moveTo(0, 0)
                                        ctx.lineTo(width, 0)
                                        ctx.lineTo(1, height - 0.5)
                                        ctx.closePath()
                                        ctx.fill()
                                        ctx.beginPath()
                                        ctx.moveTo(0, 0)
                                        ctx.lineTo(1, height - 0.5)
                                        ctx.lineTo(width, 0)
                                        ctx.stroke()
                                    }
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

                            // What the agent said while it worked: the
                            // notes it wrote before it reached for a
                            // tool. The trail of the run, not its
                            // outcome — dimmed, in a smaller type and
                            // folded away, one click from view.
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
                                    // Smaller than the answer: context,
                                    // not the result.
                                    font.pixelSize: Math.round(
                                        Qt.application.font.pixelSize * 0.85)
                                }
                            }

                            // What the agent actually did in this run:
                            // every tool call with its command and output.
                            // The evidence for the answer, not the answer
                            // itself — so it is dimmed and folded away
                            // like the notes above. Only present when
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
                                }
                            }
                        }
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
                        // representation: record while idle, stop while
                        // recording. No colour tint — Breeze's
                        // media-record is already the red circle.
                        PC3.ToolButton {
                            id: recordButton
                            objectName: "recordButton"

                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium

                            icon.name: resultsWindow.listening ? "media-playback-stop" : "media-record"
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

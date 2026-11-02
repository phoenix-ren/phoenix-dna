// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Window
import QtQuick.Layouts
import QtCore
import Qt.labs.settings as LabsSettings
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami

/*
 * The results pane — the widget's conversation window.
 *
 * The pane opens as soon as a run produces its first output (showLive())
 * and then streams the answer and the tool log as they happen — pinned to
 * the bottom, like a terminal. It is persistent: it stays on screen until
 * the user closes it explicitly (the close button or Esc), however many
 * results come and go in between.
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
 * The pane is user-sizable: the right and bottom edges (and the bottom-
 * right corner) drag-resize it, and the header has a maximize/restore
 * button next to the close button. The header itself drags the window:
 * once moved, the pane pops up at that position from then on — persisted
 * (with the dragged size) across plasmashell restarts in the XDG state
 * directory, so the user arranges it once and finds it where they left it.
 * While never moved, the pane keeps floating next to the widget that
 * triggered it.
 *
 * The shell binds the conversation model and the dictation state
 * (transcript/listening/transcribing/statusMessage); the pane itself is
 * purely presentational.
 *
 * It is still a Plasma Dialog, for the same reason as the dictation
 * overlay: on Wayland an ordinary application window cannot put itself
 * above the shell's surfaces, and the Dialog is the compositor-managed
 * route.
 */
PlasmaCore.Dialog {
    id: conversationPane

    // The conversation history: one row per prompt/answer pair, with the
    // roles prompt / result / toolLog / running (see ConversationModel).
    // Bound by the shell to the agent bridge's model.
    property var conversation: null

    // ---- Follow-on input, mirroring the dictation overlay ---------------
    // The transcript of a follow-on being dictated into the pane, and the
    // dictation state. Bound by the shell exactly like the overlay's; the
    // transcript is pushed imperatively (not bound), because the editor
    // below is editable — a binding would be broken by the first edit.
    property string transcript: ""
    property bool listening: false
    property bool transcribing: false
    property string statusMessage: ""

    // Emitted when the pane goes away (close button, Esc, toggle()).
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

    // The item the pane floats next to — the shell passes its own item so
    // the result appears where the user triggered the query.
    property Item anchor: null

    // Short answers still get a pane of substance; taller content grows
    // the pane up to the screen's available height (see the content item).
    property int minHeight: 600

    // ---- User sizing -----------------------------------------------------
    // A size the user dragged the pane to (0 = automatic: width follows the
    // default, height grows with the content up to the screen's cap).
    property real userWidth: 0
    property real userHeight: 0

    // ---- User position ---------------------------------------------------
    // Where the user dragged the pane to (-1 = never moved: the Dialog
    // floats next to the anchor item). Once set, this position wins for
    // every future show and is persisted across restarts (see the
    // geometry store inside the content item).
    property real userX: -1
    property real userY: -1

    // Whether the pane currently fills the screen (the header's maximize
    // button toggles this; the pre-maximize size is kept for restore).
    property bool maximized: false
    property real normalWidth: 0
    property real normalHeight: 0
    property bool normalAuto: true

    readonly property real minPaneWidth: Kirigami.Units.gridUnit * 12
    readonly property real minPaneHeight: Kirigami.Units.gridUnit * 8

    // Restores the remembered geometry once, when the pane is created; the
    // dragged size feeds the same userWidth/userHeight the resize handles
    // use, the dragged position the same userX/userY the header drag does.
    Component.onCompleted: {
        userX = geometryStore.paneX
        userY = geometryStore.paneY
        if (geometryStore.paneWidth > 0) {
            userWidth = geometryStore.paneWidth
        }
        if (geometryStore.paneHeight > 0) {
            userHeight = geometryStore.paneHeight
        }
    }

    // Keeps part of the pane on screen however far the user drags it.
    function clampX(x) {
        return Math.max(
            -(conversationPane.width - Kirigami.Units.gridUnit * 4),
            Math.min(Screen.width - Kirigami.Units.gridUnit * 4, x))
    }

    function clampY(y) {
        return Math.max(
            0,
            Math.min(Screen.desktopAvailableHeight - minPaneHeight, y))
    }

    // Puts the window at the user's remembered position; a no-op while the
    // pane has never been moved (the Dialog then keeps its own placement
    // next to the anchor item). Called right after the window becomes
    // visible — also via callLater, so it wins over the Dialog's own
    // placement pass, which may run a frame later.
    function applyPosition() {
        if (userX < 0 || userY < 0) {
            return
        }
        conversationPane.x = clampX(userX)
        conversationPane.y = clampY(userY)
    }

    // Remembers the current position for this and every future show.
    function savePosition() {
        userX = conversationPane.x
        userY = conversationPane.y
        geometryStore.paneX = Math.round(userX)
        geometryStore.paneY = Math.round(userY)
    }

    // Remembers the current size for future sessions (sizes persist within
    // a session anyway, through userWidth/userHeight).
    function saveSize() {
        geometryStore.paneWidth = Math.round(conversationPane.width)
        geometryStore.paneHeight = Math.round(conversationPane.height)
    }

    location: PlasmaCore.Types.Floating
    type: PlasmaCore.Dialog.WindowType.DialogWindow

    // The theme's background would be opaque; the pane paints its own dark,
    // semi-transparent one, so the dialog surface itself must be bare.
    backgroundHints: PlasmaCore.Dialog.NoBackground

    // Renders above any view: the pane is how a finished run announces
    // itself, so nothing may be stacked on top of it.
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    hideOnWindowDeactivate: false

    visible: false

    // Fades in when a result appears, out again when it is dismissed; the
    // window itself only goes away once the fade has completed.
    opacity: 0

    Behavior on opacity {
        NumberAnimation {
            duration: Kirigami.Units.shortDuration
            easing.type: Easing.InOutQuad
        }
    }

    // Parameter-free on purpose: signal handlers that use the injected
    // "opacity" parameter are deprecated (and warn on every load).
    onOpacityChanged: conversationPane.visible = conversationPane.opacity > 0

    // Engine results are pushed into the editor imperatively (not via a
    // text binding): a binding would be destroyed by the first user edit,
    // and a later transcript would then never appear — same guard as the
    // dictation overlay, whose editor this bar replicates.
    onTranscriptChanged: followUpEdit.text = transcript

    onAnchorChanged: {
        if (anchor) {
            visualParent = anchor;
        }
    }

    // Shows a result: fades the pane in (or keeps it up) and pins the
    // scroll to the newest exchange — answers flow downwards, ChatGPT
    // style, so the newest content is what the user wants to see.
    function showResult() {
        applySize()
        scroller.pinnedToBottom = true
        visible = true
        opacity = 1
        // The user's remembered position (if any) wins over the Dialog's
        // own placement next to the anchor item.
        applyPosition()
        Qt.callLater(applyPosition)
    }

    // Opens the pane for a streaming run: the run's text and tool log
    // arrive as they happen, following the newest content.
    function showLive() {
        applySize()
        scroller.pinnedToBottom = true
        visible = true
        opacity = 1
        applyPosition()
        Qt.callLater(applyPosition)
    }

    // Manual toggle, e.g. from the applet: bring the conversation back up,
    // or hide the pane if it is already showing.
    function toggle() {
        if (visible) {
            dismiss()
        } else if (conversation && conversation.count > 0) {
            showResult()
        }
    }

    function dismiss() {
        opacity = 0
        dismissed()
    }

    // The single source of truth for the window's geometry. Plasma's Dialog
    // only half-heartedly follows the main item's implicit size (it grows
    // but stops syncing after the first shrink), so the window is sized
    // here, explicitly: the user's dragged size if any, the screen if
    // maximized, otherwise the default width and a content-driven height
    // capped by the screen.
    function applySize() {
        if (maximized) {
            conversationPane.width = Screen.width - 2 * Kirigami.Units.gridUnit
            conversationPane.height = Screen.desktopAvailableHeight - 2 * Kirigami.Units.gridUnit
            return
        }
        conversationPane.width = userWidth > 0
            ? Math.max(minPaneWidth, userWidth)
            : Kirigami.Units.gridUnit * 30
        conversationPane.height = userHeight > 0
            ? Math.max(minPaneHeight, userHeight)
            : Math.max(
                minHeight,
                Math.min(
                    body.implicitHeight + 2 * Kirigami.Units.gridUnit,
                    Screen.desktopAvailableHeight - 2 * Kirigami.Units.gridUnit
                ))
    }

    // Maximize fills the screen's available area; restore returns to the
    // size the pane had before (the dragged size if the user had set one,
    // otherwise the automatic content-driven sizing) — and, if the pane had
    // been moved, back to the remembered position.
    function toggleMaximized() {
        if (!maximized) {
            normalWidth = conversationPane.width
            normalHeight = conversationPane.height
            normalAuto = userWidth === 0 && userHeight === 0
            maximized = true
        } else {
            maximized = false
            if (normalAuto) {
                userWidth = 0
                userHeight = 0
            } else {
                userWidth = normalWidth
                userHeight = normalHeight
            }
            applySize()
            applyPosition()
            return
        }
        applySize()
    }

    // Wide enough for a paragraph, like the dictation overlay; the user's
    // dragged size (or maximize) overrides the defaults.
    //
    // Note: everything that is not the content must live *inside* this item
    // — a Dialog's default property is mainItem, so a bare child object
    // (e.g. a Timer) would clobber this binding.
    mainItem: Item {
        id: content

        focus: true
        Keys.onEscapePressed: conversationPane.dismiss()

        // The dragged geometry, persisted across plasmashell restarts in an
        // INI file under the XDG state directory. Property writes sync to
        // disk lazily, so the writes on drag release are cheap. Declared
        // inside the content item on purpose: the Dialog's default property
        // is mainItem, so a bare child object would clobber it.
        LabsSettings.Settings {
            id: geometryStore

            category: "Geometry"
            fileName: StandardPaths.writableLocation(StandardPaths.GenericStateLocation)
                + "/phoenix-dna/pane-geometry.ini"
            property int paneX: -1
            property int paneY: -1
            property real paneWidth: 0
            property real paneHeight: 0
        }

        implicitWidth: conversationPane.maximized
            ? Screen.width - 2 * Kirigami.Units.gridUnit
            : (conversationPane.userWidth > 0
                ? Math.max(conversationPane.minPaneWidth, conversationPane.userWidth)
                : Kirigami.Units.gridUnit * 30)

        // Never less than minHeight, so even a one-line answer reads like a
        // proper window — normally up to the screen's available height
        // (taskbar/panels excluded), past which the scroll area scrolls
        // instead; a dragged or maximized height overrides.
        implicitHeight: conversationPane.maximized
            ? Screen.desktopAvailableHeight - 2 * Kirigami.Units.gridUnit
            : (conversationPane.userHeight > 0
                ? Math.max(conversationPane.minPaneHeight, conversationPane.userHeight)
                : Math.max(
                    conversationPane.minHeight,
                    Math.min(
                        body.implicitHeight + 2 * Kirigami.Units.gridUnit,
                        Screen.desktopAvailableHeight - 2 * Kirigami.Units.gridUnit
                    )
                ))

        // The dark, semi-transparent backdrop. Deliberately fixed rather
        // than theme-derived: it must be dark whichever theme runs, and so
        // the text on it can be fixed light. Mostly opaque: readable over
        // busy desktops, with just a hint of the desktop through it.
        Rectangle {
            anchors.fill: parent
            radius: Kirigami.Units.smallSpacing
            color: Qt.rgba(0.07, 0.07, 0.09, 0.93)
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.18)
        }

        ColumnLayout {
            id: body
            anchors.fill: parent
            // A full gridUnit of padding on every side: largeSpacing (~half
            // that) made the text hug the pane's edges and read cramped.
            anchors.margins: Kirigami.Units.gridUnit
            spacing: Kirigami.Units.smallSpacing

            // In automatic sizing the pane grows with its content (new
            // results, a taller editor) up to the screen's cap: re-apply
            // the size whenever the content's ideal height changes. The
            // Dialog's own implicit-size sync is unreliable (it stops
            // after the first shrink), so applySize() owns the window.
            onImplicitHeightChanged: {
                if (!conversationPane.maximized && conversationPane.userHeight === 0 && conversationPane.visible) {
                    conversationPane.applySize()
                }
            }

            RowLayout {
                id: headerRow
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    source: "dialog-information"
                }

                // The title area is the move handle: dragging it moves the
                // whole window. The delta is computed in global screen
                // coordinates from the press-time anchor — the same scheme
                // as the resize handles — because the window's own origin
                // moves under the cursor as the Dialog applies the move
                // asynchronously on Wayland.
                Item {
                    Layout.fillWidth: true
                    implicitHeight: paneHeading.implicitHeight

                    Kirigami.Heading {
                        id: paneHeading
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        // KF6 6.31 has no Kirigami.Heading.Level enum; levels are plain ints.
                        level: 3
                        text: i18n("Conversation")
                        color: "#f2f2f2"
                    }

                    MouseArea {
                        objectName: "moveHeader"

                        anchors.fill: parent
                        enabled: !conversationPane.maximized
                        cursorShape: Qt.SizeAllCursor

                        property point pressGlobal: Qt.point(0, 0)
                        property point startPos: Qt.point(0, 0)
                        property bool moved: false

                        onPressed: (mouse) => {
                            pressGlobal = mapToGlobal(mouse.x, mouse.y)
                            startPos = Qt.point(conversationPane.x, conversationPane.y)
                            moved = false
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed) {
                                return
                            }
                            const global = mapToGlobal(mouse.x, mouse.y)
                            conversationPane.x = conversationPane.clampX(startPos.x + global.x - pressGlobal.x)
                            conversationPane.y = conversationPane.clampY(startPos.y + global.y - pressGlobal.y)
                            moved = true
                        }
                        onReleased: if (moved) {
                            conversationPane.savePosition()
                        }
                    }
                }

                // The pane persists until closed by hand; maximize makes
                // room for long answers and histories.
                PC3.ToolButton {
                    objectName: "maximizeButton"

                    icon.name: conversationPane.maximized ? "view-restore" : "view-fullscreen"
                    icon.color: "white"

                    PC3.ToolTip {
                        text: conversationPane.maximized ? i18n("Restore size") : i18n("Maximize")
                    }

                    onClicked: conversationPane.toggleMaximized()
                }

                PC3.ToolButton {
                    objectName: "closeButton"

                    icon.name: "window-close"
                    icon.color: "white"

                    PC3.ToolTip {
                        text: i18n("Close")
                    }

                    onClicked: conversationPane.dismiss()
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            Flickable {
                id: scroller

                Layout.fillWidth: true
                Layout.fillHeight: true
                // A touch of air between the header separator and the text.
                Layout.topMargin: Kirigami.Units.smallSpacing

                // A Flickable has no content-driven implicit size: without
                // this it collapses to zero height and the pane shrinks to
                // a bare header. With it, the pane grows with the content
                // until the cap on the content item, then this area is
                // squeezed and scrolls instead.
                implicitHeight: column.implicitHeight

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
                    // prompt bubbles at a fraction of the pane).
                    Repeater {
                        model: conversationPane.conversation

                        delegate: ColumnLayout {
                            id: exchange

                            required property int index
                            required property string prompt
                            required property string sender
                            required property string result
                            required property string toolLog
                            required property bool running

                            Layout.preferredWidth: column.width
                            spacing: Kirigami.Units.smallSpacing

                            // The user's prompt, as a chat bubble: pinned
                            // to the right, hugging its content up to four
                            // fifths of the pane, with a tail pointing
                            // down-right and the sender's name underneath
                            // it — the bubble points at whoever issued it
                            // ("root", the login name, or "anon", per the
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
                                    color: Qt.rgba(1, 1, 1, 0.08)
                                    border.width: 1
                                    border.color: Qt.rgba(1, 1, 1, 0.14)

                                    Kirigami.SelectableLabel {
                                        id: promptLabel

                                        anchors.fill: parent
                                        anchors.margins: Kirigami.Units.smallSpacing

                                        text: exchange.prompt
                                        textFormat: TextEdit.PlainText
                                        wrapMode: Text.WordWrap
                                        color: "#e8e8e8"
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
                                    color: "#9a9a9a"
                                }
                            }

                            // The agent's answer, on the left: full width,
                            // with the mirrored tail and attribution.
                            // Rendered as Markdown: the agent mostly
                            // answers in plain sentences, but code and
                            // lists do occur — and the tool log below is
                            // fenced code by construction.
                            ColumnLayout {
                                id: answerGroup

                                Layout.fillWidth: true
                                spacing: 0

                                Kirigami.SelectableLabel {
                                    objectName: "resultLabel"

                                    Layout.fillWidth: true

                                    text: exchange.result
                                    textFormat: TextEdit.MarkdownText
                                    wrapMode: Text.WordWrap
                                    color: "#f2f2f2"
                                }

                                // The mirrored pointy edge, at the
                                // answer's bottom-left.
                                Canvas {
                                    objectName: "answerTail"

                                    Layout.alignment: Qt.AlignLeft
                                    Layout.leftMargin: Kirigami.Units.smallSpacing * 2
                                    implicitWidth: Kirigami.Units.smallSpacing * 2
                                    implicitHeight: Kirigami.Units.smallSpacing
                                    onPaint: {
                                        const ctx = getContext("2d")
                                        ctx.fillStyle = Qt.rgba(1, 1, 1, 0.05)
                                        ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.12)
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

                                Kirigami.SelectableLabel {
                                    objectName: "agentLabel"

                                    Layout.alignment: Qt.AlignLeft
                                    Layout.topMargin: 2

                                    text: i18n("Agent")
                                    textFormat: TextEdit.PlainText
                                    font.pixelSize: Math.max(
                                        9, Math.round(
                                            Qt.application.font.pixelSize * 0.8))
                                    color: "#9a9a9a"
                                }
                            }

                            // What the agent actually did in this run:
                            // every tool call with its command and output.
                            // Only present when something ran (web answers
                            // often have no tool calls beyond a search).
                            Kirigami.Separator {
                                Layout.fillWidth: true
                                visible: exchange.toolLog.length > 0
                            }

                            Kirigami.Heading {
                                level: 5
                                text: i18n("Tool activity")
                                color: "#cfcfcf"
                                visible: exchange.toolLog.length > 0
                            }

                            Kirigami.SelectableLabel {
                                objectName: "toolLabel"

                                Layout.fillWidth: true

                                text: exchange.toolLog
                                textFormat: TextEdit.MarkdownText
                                wrapMode: Text.WordWrap
                                color: "#e8e8e8"
                                visible: exchange.toolLog.length > 0
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
            // (correct a recognition, or simply type). The pane stays
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

                            icon.name: conversationPane.listening ? "media-playback-stop" : "media-record"
                            icon.width: Kirigami.Units.iconSizes.small
                            icon.height: Kirigami.Units.iconSizes.small

                            onClicked: conversationPane.recordClicked()

                            PC3.ToolTip {
                                text: conversationPane.listening ? i18n("Stop") : i18n("Record a follow-up")
                            }
                        }

                        // Runs while the dictated clip is being transcribed,
                        // as in the dictation overlay's header.
                        PC3.BusyIndicator {
                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                            visible: conversationPane.transcribing
                            running: conversationPane.transcribing
                        }

                        // The transcript, editable so a dictated follow-on
                        // can be corrected before sending — and typeable
                        // from scratch. The editor is the single source of
                        // truth for what "Send to agent" submits: engine
                        // results flow in via onTranscriptChanged above,
                        // edits flow back into conversationPane.transcript here
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
                            readOnly: conversationPane.listening || conversationPane.transcribing
                            placeholderText: conversationPane.transcribing ? i18n("Transcribing…")
                                            : conversationPane.listening ? i18n("Speak now…")
                                            : i18n("Ask a follow-up…")

                            onTextChanged: if (text !== conversationPane.transcript) {
                                conversationPane.transcript = text
                            }

                            // Same contract as the send button, from the
                            // keyboard: Shift+Enter submits the follow-on,
                            // Enter breaks the line (the editor is
                            // multi-line). Any other key keeps the editor's
                            // own behaviour — hence no blanket accept.
                            Keys.onPressed: (event) => {
                                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                    && (event.modifiers & Qt.ShiftModifier)) {
                                    if (conversationPane.canSubmit) {
                                        conversationPane.submitted(conversationPane.transcript)
                                    }
                                    event.accepted = true
                                }
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
                            enabled: conversationPane.canSubmit
                            onClicked: conversationPane.submitted(conversationPane.transcript)

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
                        visible: conversationPane.statusMessage.length > 0
                        wrapMode: Text.WordWrap
                        text: conversationPane.statusMessage
                        // Fixed rather than theme-derived: it sits on the
                        // pane's own dark backdrop, which is dark whichever
                        // theme runs.
                        color: "#f66151"
                    }
                }
            }
        }

        // ---- Resize handles -------------------------------------------------
        // Frameless window, so the edges carry invisible drag strips: right
        // and bottom resize, the corner does both. They are only needed in
        // the normal state (maximized fills the screen already).
        //
        // All three compute the drag delta in *global* screen coordinates,
        // captured once at press time. Item-relative deltas cannot work
        // here: the handle moves with the window between the pointer event
        // and the Dialog applying the resize (window geometry lands
        // asynchronously on Wayland), so a bottom-right drag would measure
        // the window's own motion against itself — the drag cancels itself
        // (no height gain) and resizes jaggedly, bouncing back as the
        // geometry catches up. Global deltas are anchored to the screen and
        // immune to all of it.
        MouseArea {
            objectName: "resizeRight"

            visible: !conversationPane.maximized
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Kirigami.Units.largeSpacing
            cursorShape: Qt.SizeHorCursor

            property point pressGlobal: Qt.point(0, 0)
            property real startWidth: 0

            onPressed: (mouse) => {
                pressGlobal = mapToGlobal(mouse.x, mouse.y)
                startWidth = conversationPane.width
            }
            onPositionChanged: (mouse) => {
                if (!pressed) {
                    return
                }
                const global = mapToGlobal(mouse.x, mouse.y)
                conversationPane.userWidth = Math.max(conversationPane.minPaneWidth,
                    Math.min(Screen.width - 2 * Kirigami.Units.gridUnit,
                             startWidth + global.x - pressGlobal.x))
                conversationPane.applySize()
            }
            onReleased: conversationPane.saveSize()
        }

        MouseArea {
            objectName: "resizeBottom"

            visible: !conversationPane.maximized
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: Kirigami.Units.largeSpacing
            cursorShape: Qt.SizeVerCursor

            property point pressGlobal: Qt.point(0, 0)
            property real startHeight: 0

            onPressed: (mouse) => {
                pressGlobal = mapToGlobal(mouse.x, mouse.y)
                startHeight = conversationPane.height
            }
            onPositionChanged: (mouse) => {
                if (!pressed) {
                    return
                }
                const global = mapToGlobal(mouse.x, mouse.y)
                conversationPane.userHeight = Math.max(conversationPane.minPaneHeight,
                    Math.min(Screen.desktopAvailableHeight - 2 * Kirigami.Units.gridUnit,
                             startHeight + global.y - pressGlobal.y))
                conversationPane.applySize()
            }
            onReleased: conversationPane.saveSize()
        }

        MouseArea {
            objectName: "resizeCorner"

            visible: !conversationPane.maximized
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: Kirigami.Units.gridUnit
            height: Kirigami.Units.gridUnit
            cursorShape: Qt.SizeFDiagCursor

            property point pressGlobal: Qt.point(0, 0)
            property real startWidth: 0
            property real startHeight: 0

            onPressed: (mouse) => {
                pressGlobal = mapToGlobal(mouse.x, mouse.y)
                startWidth = conversationPane.width
                startHeight = conversationPane.height
            }
            onPositionChanged: (mouse) => {
                if (!pressed) {
                    return
                }
                const global = mapToGlobal(mouse.x, mouse.y)
                conversationPane.userWidth = Math.max(conversationPane.minPaneWidth,
                    Math.min(Screen.width - 2 * Kirigami.Units.gridUnit,
                             startWidth + global.x - pressGlobal.x))
                conversationPane.userHeight = Math.max(conversationPane.minPaneHeight,
                    Math.min(Screen.desktopAvailableHeight - 2 * Kirigami.Units.gridUnit,
                             startHeight + global.y - pressGlobal.y))
                conversationPane.applySize()
            }
            onReleased: conversationPane.saveSize()
        }
    }
}

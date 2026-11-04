// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

/*
 * The session search overlay.
 *
 * It lists the user's previous sessions on the left, under the summary
 * name the daemon gave them (its LLM-generated title, or the session's
 * first query when it was never named), and shows the queries and
 * responses of the selected session on the right.
 *
 * Typing narrows the list down: the text is matched fuzzily against the
 * names, so every further character drops the sessions that no longer
 * match. The selection moves with the arrow keys and with the mouse;
 * a double click - or the Return key - opens the session in the results
 * window, where the conversation carries on.
 *
 * The right pane shows the stored answers as native Markdown, with
 * their math compiled: a stored answer is always finished, so its LaTeX
 * spans can be swapped for tinted images inside the very same document
 * as soon as the answer is shown. The formulas come from the same
 * cache the results window uses, so a session that was already read is
 * shown instantly.
 *
 * The addresses in the stored words — the user's queries as much as
 * the agent's answers — are links, exactly as the results window makes
 * them (the same MarkdownDocument.linkify walk), so a session read
 * from the history opens its files, pages and mailboxes the same way.
 *
 * The window is a transparent overlay: the dark backdrop is painted by
 * the content itself, the window around it stays bare. Like the
 * dictation overlay it is a Plasma Dialog, because on Wayland an
 * ordinary application window cannot put itself above the shell's
 * surfaces.
 */
PlasmaCore.Dialog {
    id: searchOverlay

    // The sessions to choose from: one map per session, with the keys
    // "id", "mode", "title", "summary", "preview", "createdAt",
    // "updatedAt" and "messageCount". Bound by the shell to the daemon's
    // answer for the current query - the daemon does the matching.
    property var summaries: []

    // The transcript of the selected session, as the shell's list
    // model: one row per message, with the roles "role", "content" and
    // "ts". The rows are handed over a chunk at a time, so the pane
    // fills while the window stays responsive.
    property var transcript: null

    // The user whose history is searched (the local login name).
    // Bound by the shell.
    property string user: ""

    // Emitted when the query changed: the shell asks the daemon for the
    // sessions that match it.
    signal queryChanged(string query)

    // Emitted when another session was selected: the shell asks the
    // daemon for its queries and responses.
    signal sessionSelected(string sessionId)

    // Emitted when the overlay goes away (close button, Esc).
    signal dismissed()

    // Emitted when the overlay opens: the content resets the query field
    // and asks for the full list of sessions.
    signal opened()

    // Emitted when the user double-clicks a session in the list: the
    // shell opens the results window on that session, so the
    // conversation can be continued there.
    signal sessionActivated(string sessionId, string mode, string title)

    // The session the right pane shows, and its place in the list.
    property string currentSessionId: ""
    property int currentIndex: -1

    // The gap between two paragraphs of the agent's Markdown, which Qt
    // renders with none at all (see MarkdownDocument.spaceParagraphs);
    // a line of the answer's own size, so paragraphs read as
    // paragraphs and the text breathes with the size it is set in.
    readonly property real paragraphSpacing:
        PhoenixTheme.answerParagraphSpacing

    // Tokens for the formula renders of the stored answers; the
    // delegates match their own token in LatexRenderer's signals.
    property int formulaTokens: 0

    function nextFormulaToken() {
        return ++formulaTokens
    }

    // ---- The transcript on its way ------------------------------------
    // The messages of the selected session: how many there are, and
    // whether the handover is still going on.
    readonly property int transcriptTotal: transcript
        ? transcript.totalCount : 0
    readonly property bool transcriptLoading: transcript
        ? transcript.loading : false

    // While the query field is cleared on opening, its text change must
    // not trigger a second search.
    property bool clearingQuery: false

    // ---- The window's size and place ------------------------------------
    // The search is a workspace, not a tooltip: it takes 60% of the
    // display's width and 80% of its height - the area the plasma
    // panels leave free - and never less than 800x600.
    readonly property real minimumWidth: 800
    readonly property real minimumHeight: 600
    readonly property real searchWidth: Math.max(
        minimumWidth, Math.round(Screen.desktopAvailableWidth * 0.6))
    readonly property real searchHeight: Math.max(
        minimumHeight, Math.round(Screen.desktopAvailableHeight * 0.8))

    location: PlasmaCore.Types.Floating
    type: PlasmaCore.Dialog.WindowType.DialogWindow

    // The theme's background would be opaque; the overlay paints its own
    // dark, semi-transparent one, so the dialog surface itself must be
    // bare.
    backgroundHints: PlasmaCore.Dialog.NoBackground

    // Renders above any view: the search is how the user reaches the
    // history, so nothing may be stacked on top of it.
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    hideOnWindowDeactivate: false

    // The window's own icon: the app's avatar, the same mark every other
    // window of the app carries (see PhoenixTheme.appIcon).
    icon: PhoenixTheme.appIcon

    visible: false

    // Fades in when the search opens, out again when it is dismissed; the
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
    onOpacityChanged: searchOverlay.visible = searchOverlay.opacity > 0

    // Opens the search: an empty query lists every session of the user,
    // newest first, and the newest one is selected. The content below
    // resets the query field and takes the focus - it is the only place
    // that can reach them.
    function open() {
        visible = true
        opacity = 1
        placeOnDisplay()
        opened()
        // The compositor places the window itself, so the position is
        // applied again a tick later - it then wins over the placement
        // the compositor chose.
        Qt.callLater(placeOnDisplay)
    }

    // Puts the window in the middle of the display, at its full size.
    //
    // The window has no visual parent, so Plasma places it in the
    // middle of the screen; the position set here is what the X11
    // window manager uses (on Wayland a client cannot position its own
    // toplevel - the compositor does it).
    //
    // The size follows the area the plasma panels leave free, so the
    // window never hides under them.
    function placeOnDisplay() {
        width = searchWidth
        height = searchHeight
        x = Screen.virtualX + Math.round((Screen.width - width) / 2)
        y = Screen.virtualY + Math.round((Screen.height - height) / 2)
    }

    function dismiss() {
        opacity = 0
        dismissed()
    }

    // Moves the selection by \a delta entries, staying inside the list.
    function moveSelection(delta) {
        if (summaries.length === 0) {
            return
        }
        let index = currentIndex + delta
        if (index < 0) {
            index = 0
        }
        if (index > summaries.length - 1) {
            index = summaries.length - 1
        }
        selectSession(index)
    }

    // Selects the session at \a index and asks for its conversation.
    function selectSession(index) {
        if (index < 0 || index > summaries.length - 1) {
            return
        }
        currentIndex = index
        const id = summaries[index].id
        if (id !== currentSessionId) {
            currentSessionId = id
            sessionSelected(id)
        }
    }

    // Opens the session at \a index in the results window - what a double
    // click on it in the list does.
    function activateSession(index) {
        if (index < 0 || index > summaries.length - 1) {
            return
        }
        const session = summaries[index]
        sessionActivated(session.id, session.mode || "",
                         session.title || session.summary || "")
    }

    // Opens the selected session in the results window - what the Return
    // key does.
    function activateCurrentSession() {
        activateSession(currentIndex)
    }

    // The place of the session with the given id in the list, or -1.
    function indexOfSession(id) {
        for (let i = 0; i < summaries.length; ++i) {
            if (summaries[i].id === id) {
                return i
            }
        }
        return -1
    }

    // A new set of results: keep the selected session when it is still
    // in the list, otherwise start at the top.
    onSummariesChanged: {
        const index = indexOfSession(currentSessionId)
        if (index >= 0) {
            currentIndex = index
        } else if (summaries.length > 0) {
            selectSession(0)
        } else {
            currentIndex = -1
            currentSessionId = ""
        }
    }

    // Note: everything that is not the content must live *inside* this
    // item - a Dialog's default property is mainItem, so a bare child
    // object (e.g. a Timer) would clobber this binding.
    mainItem: Item {
        id: content

        focus: true
        Keys.onEscapePressed: searchOverlay.dismiss()

        // The arrow keys and Return belong to the list, not to the text
        // field: shortcuts rather than key handlers, because the text
        // input swallows those keys before they reach the item.
        Shortcut {
            sequences: ["Up"]
            context: Qt.WindowShortcut
            onActivated: searchOverlay.moveSelection(-1)
        }

        Shortcut {
            sequences: ["Down"]
            context: Qt.WindowShortcut
            onActivated: searchOverlay.moveSelection(1)
        }

        // Return opens the selected session in the results window, like
        // a double click on it does.
        Shortcut {
            sequences: ["Return", "Enter"]
            context: Qt.WindowShortcut
            onActivated: searchOverlay.activateCurrentSession()
        }

        // The search opens with an empty query - every session of the
        // user - and the keyboard in the query field: the search is a
        // typing affair.
        Connections {
            target: searchOverlay

            function onOpened() {
                searchOverlay.clearingQuery = true
                queryField.text = ""
                searchOverlay.clearingQuery = false
                // An empty field emits no text change, so the search for
                // the full list is asked for here.
                searchOverlay.queryChanged("")
                // The focus is taken a tick later, once the window is up
                // and can take it.
                Qt.callLater(function() {
                    queryField.forceActiveFocus()
                })
            }
        }

        // Follow the selection: the list scrolls to the selected session,
        // wherever the selection came from - the keyboard or the mouse.
        Connections {
            target: searchOverlay

            function onCurrentIndexChanged() {
                if (searchOverlay.currentIndex >= 0) {
                    sessionList.positionViewAtIndex(
                        searchOverlay.currentIndex, ListView.Contain)
                }
            }
        }

        // The searched user is fixed (the local login name), so the list
        // is fetched once, when the overlay opens.
        Connections {
            target: searchOverlay

            function onUserChanged() {
                if (searchOverlay.visible) {
                    searchOverlay.queryChanged(queryField.text)
                }
            }
        }

        implicitWidth: searchOverlay.searchWidth
        implicitHeight: searchOverlay.searchHeight

        // The backdrop, painted after the chosen colour scheme (see
        // PhoenixTheme) — the theme's own background would be opaque; the
        // dialog surface itself stays bare.
        //
        // The search's own backdrop colour: black and nearly opaque in
        // the dark mode, so the window reads as a hole cut into the
        // desktop rather than as a grey haze over it.
        Rectangle {
            anchors.fill: parent
            radius: Kirigami.Units.smallSpacing
            color: PhoenixTheme.searchBackdropColor
            border.width: 1
            border.color: PhoenixTheme.backdropBorderColor
        }

        ColumnLayout {
            id: body

            anchors.fill: parent
            anchors.margins: Kirigami.Units.gridUnit
            spacing: Kirigami.Units.smallSpacing

            // ---- Header: the query ------------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    source: LucideIcons.url("search", PhoenixTheme.headingTextColor)
                }

                // The query: matched fuzzily against the session names.
                // Every character narrows the list down.
                PC3.TextField {
                    id: queryField
                    objectName: "queryField"

                    Layout.fillWidth: true
                    placeholderText: i18n("Search sessions…")
                    selectByMouse: true

                    onTextChanged: {
                        if (!searchOverlay.clearingQuery) {
                            searchOverlay.queryChanged(text)
                        }
                    }
                }

                PC3.ToolButton {
                    id: closeButton
                    objectName: "closeButton"

                    icon.source: LucideIcons.url("x", PhoenixTheme.textColor)

                    onClicked: searchOverlay.dismiss()

                    PC3.ToolTip {
                        text: i18n("Close")
                    }
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            // ---- The two sub-panes -------------------------------------
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Kirigami.Units.smallSpacing

                // ---- Left: the sessions --------------------------------
                Rectangle {
                    // A quarter of the window, but never narrower than
                    // the names need to be readable.
                    Layout.preferredWidth: Math.max(
                        Kirigami.Units.gridUnit * 15, searchOverlay.width * 0.25)
                    Layout.minimumWidth: Kirigami.Units.gridUnit * 8
                    Layout.fillHeight: true
                    radius: Kirigami.Units.smallSpacing
                    color: PhoenixTheme.paneFill

                    ListView {
                        id: sessionList
                        objectName: "sessionList"

                        anchors.fill: parent
                        anchors.margins: Kirigami.Units.smallSpacing
                        clip: true
                        model: searchOverlay.summaries
                        boundsBehavior: Flickable.StopAtBounds

                        // The scrollbar for a long session list.
                        PC3.ScrollBar.vertical: PC3.ScrollBar {
                            id: sessionListBar
                            objectName: "sessionListBar"
                        }

                        // The scrollbar lies over the list, so the names
                        // must not run under it.
                        readonly property real scrollbarInset:
                            sessionListBar.visible ? sessionListBar.width : 0

                        // The selection is the overlay's: the list only
                        // shows it (and scrolls to it).
                        delegate: Rectangle {
                            id: sessionItem

                            required property var modelData
                            required property int index

                            // The daemon sends every field, but a missing
                            // one must not take the whole list down.
                            readonly property string summaryText:
                                modelData.summary || ""
                            readonly property string previewText:
                                modelData.preview || ""

                            width: sessionList.width - sessionList.scrollbarInset
                            height: sessionColumn.implicitHeight
                                + 2 * Kirigami.Units.smallSpacing
                            radius: Kirigami.Units.smallSpacing
                            color: searchOverlay.currentIndex === index
                                ? PhoenixTheme.selectionFill
                                : "transparent"

                            ColumnLayout {
                                id: sessionColumn

                                anchors.fill: parent
                                anchors.margins: Kirigami.Units.smallSpacing
                                spacing: 0

                                // The summary name: the session's title,
                                // or its first query when the daemon never
                                // named it.
                                Text {
                                    Layout.fillWidth: true
                                    text: sessionItem.summaryText.length > 0
                                        ? sessionItem.summaryText
                                        : i18n("(untitled session)")
                                    elide: Text.ElideRight
                                    color: PhoenixTheme.headingTextColor
                                }

                                // The query the session started with - the
                                // detail that tells two similarly named
                                // sessions apart.
                                Text {
                                    Layout.fillWidth: true
                                    visible: sessionItem.previewText.length > 0
                                        && sessionItem.previewText
                                            !== sessionItem.summaryText
                                    text: sessionItem.previewText
                                    elide: Text.ElideRight
                                    font.pixelSize: Math.max(
                                        9, Math.round(
                                            Qt.application.font.pixelSize * 0.8))
                                    color: PhoenixTheme.secondaryTextColor
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor

                                onClicked: {
                                    searchOverlay.selectSession(sessionItem.index)
                                    // The keyboard belongs to the query
                                    // field: put it back there, so typing
                                    // goes on narrowing the list down.
                                    queryField.forceActiveFocus()
                                }

                                // A double click opens the session in the
                                // results window: the conversation carries
                                // on there, with a follow-up.
                                onDoubleClicked: {
                                    searchOverlay.activateSession(sessionItem.index)
                                }
                            }
                        }
                    }

                    // Nothing to search through yet.
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 2 * Kirigami.Units.gridUnit
                        visible: sessionList.count === 0
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: queryField.text.length > 0
                            ? i18n("No session matches the query.")
                            : i18n("No sessions yet.")
                        color: PhoenixTheme.secondaryTextColor
                    }
                }

                // ---- Right: the selected session's conversation ----------
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Kirigami.Units.smallSpacing
                    color: PhoenixTheme.paneFill

                    // A ListView, not a Flickable over a Repeater: a
                    // transcript holds every query and answer the session
                    // ever produced, and each answer is a Markdown
                    // document to parse and lay out. A Repeater builds all
                    // of them in the one frame the messages arrive in —
                    // a long session froze the window for hundreds of
                    // milliseconds on every click, the selection
                    // highlight included, since the reply of the local
                    // daemon beats the next paint. A ListView builds only
                    // the messages on screen (plus the cache band just
                    // outside it) and lets the rest wait until scrolling
                    // asks for them.
                    ListView {
                        id: transcriptList
                        objectName: "transcript"

                        anchors.fill: parent
                        anchors.margins: Kirigami.Units.smallSpacing
                        clip: true
                        model: searchOverlay.transcript
                        spacing: Kirigami.Units.largeSpacing
                        boundsBehavior: Flickable.StopAtBounds

                        // The scrollbar for a long conversation.
                        PC3.ScrollBar.vertical: PC3.ScrollBar {
                            id: transcriptBar
                            objectName: "transcriptBar"
                        }

                        // The scrollbar lies over the content, so the
                        // text must not run under it.
                        readonly property real scrollbarInset:
                            transcriptBar.visible ? transcriptBar.width : 0

                        delegate: ColumnLayout {
                            id: message

                            // The model's roles: who spoke, what was
                            // said, and when.
                            required property string role
                            required property string content

                            width: transcriptList.width
                                - transcriptList.scrollbarInset
                            spacing: 0

                            // The queries the user asked, as chat
                            // bubbles on the right; the agent's
                            // answers flow below them on the left.
                            readonly property bool fromUser:
                                role === "user"
                            readonly property string contentText:
                                content || ""

                            // ---- The answer's math, rendered -------------
                            // A stored answer is always finished, so
                            // its math can be compiled right away:
                            // the spans are swapped for tinted
                            // images inside the very same Markdown
                            // document — the text around them stays
                            // this native view. A failed compile (no
                            // engine installed, a formula LaTeX
                            // rejects) keeps the verbatim view and
                            // says why.
                            property int formulaToken: -1
                            property string formulaFailedReason: ""

                            readonly property bool wantsFormulas:
                                !message.fromUser
                                && message.contentText.length > 0
                                // -1: never requested. -2: requested
                                // and failed — the verbatim view
                                // stays, and the answer is not asked
                                // again (a retry every second would
                                // be a storm).
                                && message.formulaToken === -1
                                && LatexRenderer.available
                                && LatexRenderer.containsLatex(
                                    message.contentText)

                            // The delegate is created when the
                            // session is shown, so this is where
                            // the answer's math goes in.
                            Component.onCompleted: message.maybeEmbedFormulas()

                            function maybeEmbedFormulas() {
                                if (!message.wantsFormulas) {
                                    return
                                }
                                message.formulaToken =
                                    searchOverlay.nextFormulaToken()
                                // The raw source, never
                                // answerLabel.text: a MarkdownText
                                // label re-exports its text with
                                // escapes, and a doubled backslash
                                // is a LaTeX line break.
                                LatexRenderer.embedFormulas(
                                    message.formulaToken,
                                    answerLabel.textDocument,
                                    message.contentText,
                                    PhoenixTheme.primaryTextColor,
                                    answerLabel.font.pixelSize,
                                    searchOverlay.paragraphSpacing)
                            }

                            // A theme switch re-tints the formulas:
                            // the compiled pages are cached, so this
                            // is an instant re-render in the other
                            // ink, never a recompile.
                            function reembedFormulas() {
                                // Only a rendered answer is
                                // re-tinted: an answer that was
                                // never rendered gets its first
                                // render from maybeEmbedFormulas(),
                                // and one that failed is not asked
                                // again.
                                if (message.formulaToken <= 0
                                    || !answerLabel) {
                                    return
                                }
                                // The same formulas, asked for again: the
                                // theme changed, so they are typeset in
                                // the other ink.
                                message.formulaToken =
                                    searchOverlay.nextFormulaToken()
                                LatexRenderer.embedFormulas(
                                    message.formulaToken,
                                    answerLabel.textDocument,
                                    message.contentText,
                                    PhoenixTheme.primaryTextColor,
                                    answerLabel.font.pixelSize,
                                    searchOverlay.paragraphSpacing)
                            }

                            Connections {
                                target: LatexRenderer

                                function onEmbedded(token) {
                                    // The embed rebuilt the document
                                    // from the raw Markdown: the
                                    // answer's bare addresses are
                                    // anchors again, and the links of
                                    // the Markdown itself are back in
                                    // the palette's ink.
                                    if (token === message.formulaToken) {
                                        Qt.callLater(
                                            MarkdownDocument.linkify,
                                            answerLabel.textDocument,
                                            PhoenixTheme.linkColor)
                                    }
                                }

                                function onFailed(token, reason) {
                                    // -2: requested, failed, never
                                    // again for this answer.
                                    if (token === message.formulaToken) {
                                        message.formulaToken = -2
                                        message.formulaFailedReason = reason
                                    }
                                }
                            }

                            Connections {
                                target: PhoenixTheme

                                function onIsDarkChanged() {
                                    message.reembedFormulas()
                                    message.relinkify()
                                }
                            }

                            // Safety net for the change-triggered
                            // paths: they can die mid-flight, so
                            // while a wanted set of formulas has not
                            // been requested yet, re-check every
                            // second. Once the request is out the
                            // net stops: wantsFormulas turns false
                            // on its own.
                            Timer {
                                interval: 1000
                                repeat: true
                                running: message.wantsFormulas
                                onTriggered: message.maybeEmbedFormulas()
                            }

                            // The links of this message, in the link ink
                            // of the mode the overlay now runs in: a
                            // scheme switch changes nothing in the
                            // documents, only the ink their anchors
                            // wear.
                            function relinkify() {
                                const ink = PhoenixTheme.linkColor
                                Qt.callLater(MarkdownDocument.linkify,
                                             queryLabel.textDocument, ink)
                                Qt.callLater(MarkdownDocument.linkify,
                                             answerLabel.textDocument, ink)
                            }

                            Rectangle {
                                id: queryBubble
                                objectName: "queryBubble"

                                Layout.alignment: Qt.AlignRight
                                Layout.maximumWidth:
                                    message.width * 0.85
                                visible: message.fromUser
                                implicitWidth: Math.min(
                                    message.width * 0.85,
                                    queryLabel.implicitWidth
                                        + 2 * Kirigami.Units.smallSpacing)
                                implicitHeight: queryLabel.implicitHeight
                                    + 2 * Kirigami.Units.smallSpacing
                                radius: Kirigami.Units.smallSpacing * 2
                                color: PhoenixTheme.bubbleFill
                                border.width: 1
                                border.color: PhoenixTheme.bubbleBorderColor

                                Kirigami.SelectableLabel {
                                    id: queryLabel

                                    anchors.fill: parent
                                    anchors.margins: Kirigami.Units.smallSpacing

                                    text: message.contentText
                                    textFormat: TextEdit.PlainText
                                    wrapMode: Text.WordWrap
                                    color: PhoenixTheme.textColor
                                    // An address in the user's own
                                    // words opens like any other.
                                    onLinkActivated: (link) =>
                                        MarkdownDocument.openLink(link)
                                    onTextChanged: Qt.callLater(
                                        MarkdownDocument.linkify,
                                        textDocument,
                                        PhoenixTheme.linkColor)
                                }
                            }

                            Kirigami.Heading {
                                Layout.fillWidth: true
                                visible: message.fromUser
                                level: 6
                                text: i18n("You")
                                horizontalAlignment: Text.AlignRight
                                color: PhoenixTheme.secondaryTextColor
                            }

                            Kirigami.Heading {
                                Layout.fillWidth: true
                                visible: !message.fromUser
                                // KF6 6.31 has no Kirigami.Heading.Level
                                // enum; levels are plain ints.
                                level: 6
                                text: i18n("Agent")
                                horizontalAlignment: Text.AlignLeft
                                color: PhoenixTheme.secondaryTextColor
                            }

                            Kirigami.SelectableLabel {
                                id: answerLabel
                                objectName: "answerLabel"

                                Layout.fillWidth: true
                                visible: !message.fromUser

                                text: message.contentText
                                // The agent answers in Markdown;
                                // the query was plain speech.
                                textFormat: TextEdit.MarkdownText
                                wrapMode: Text.WordWrap
                                color: PhoenixTheme.primaryTextColor
                                // The agent's own voice, in the
                                // family and the size the user
                                // chose for this mode (see
                                // PhoenixTheme).
                                font.family: PhoenixTheme.answerFontFamily
                                font.pixelSize: PhoenixTheme.answerFontSize
                                onTextChanged: {
                                    Qt.callLater(
                                        MarkdownDocument.spaceParagraphs,
                                        textDocument,
                                        searchOverlay.paragraphSpacing)
                                    // The stored answer's addresses
                                    // are links, as they are in the
                                    // results window.
                                    Qt.callLater(
                                        MarkdownDocument.linkify,
                                        textDocument,
                                        PhoenixTheme.linkColor)
                                }
                                // A click goes to the desktop: the
                                // file manager, the browser, the mail
                                // client, the dialer.
                                onLinkActivated: (link) =>
                                    MarkdownDocument.openLink(link)
                                // The spacing follows the size
                                // of the text: when the user
                                // changes it, the paragraphs are
                                // spaced again.
                                onFontChanged: Qt.callLater(
                                    MarkdownDocument.spaceParagraphs,
                                    textDocument,
                                    searchOverlay.paragraphSpacing)
                            }

                            // The math of this answer is rendered with
                            // the rest of the window's math: the bar at
                            // the top of the window counts them all,
                            // so a row under every answer would say the
                            // same thing over and over.

                            // When a compile fails, the verbatim
                            // view stays — and says why, so a broken
                            // formula or a missing TeX engine is
                            // diagnosable instead of mysterious.
                            Kirigami.SelectableLabel {
                                Layout.fillWidth: true
                                visible:
                                    message.formulaFailedReason.length > 0
                                wrapMode: Text.WordWrap
                                text: i18n("Math not rendered: %1",
                                           message.formulaFailedReason)
                                color: PhoenixTheme.secondaryTextColor
                                font.pixelSize: Math.max(
                                    9, Math.round(
                                        Qt.application.font.pixelSize * 0.8))
                            }
                        }
                    }

                    // Nothing selected (or a session without a
                    // transcript). While the transcript is on its way
                    // the text stays away — the bar at the top says
                    // what is happening.
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 2 * Kirigami.Units.gridUnit
                        visible: searchOverlay.transcriptTotal === 0
                            && !searchOverlay.transcriptLoading
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: searchOverlay.currentSessionId.length > 0
                            ? i18n("This session has no messages yet.")
                            : i18n("Select a session to see its conversation.")
                        color: PhoenixTheme.secondaryTextColor
                    }
                }
            }
        }
    }
}

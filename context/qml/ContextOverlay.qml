// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

/*
 * The context overlay: what the session on screen spent of the model's
 * context window.
 *
 * The ring in the corner of the results window says how much of the
 * window the conversation has used; this is where the numbers behind
 * that share are: which model is in force, how much it holds at once,
 * the tokens the session spent (the fresh input, the output, the part
 * of the prompt the server answered from its cache and the reasoning),
 * how many queries and answers the session holds, what it all cost,
 * and when the session began.
 *
 * The window is a Plasma Dialog, like the session search and the
 * wizard: on Wayland an ordinary application window cannot put itself
 * above the shell's surfaces. It is centred on the display, and Esc
 * puts it away — as does the Close button, for the pointer that never
 * reaches a key.
 *
 * The numbers are the daemon's (see AgentBridge::sessionContext): the
 * session's own audit trail, and the model's own configuration. What
 * the daemon does not know is said so: a window whose size is not
 * known, a model nobody priced.
 */
PlasmaCore.Dialog {
    id: contextOverlay

    // The context view's picture of the session: the map the daemon
    // answered with. Bound by the shell.
    property var context: null

    // Emitted when the overlay goes away (close button, Esc).
    signal dismissed()

    // ---- The window's size and place ------------------------------------
    // Big enough for every row the view has, and never less than that:
    // the details are the point of the window.
    readonly property real minimumWidth: 520
    readonly property real minimumHeight: 580
    readonly property real overlayWidth: Math.max(
        minimumWidth, Math.round(Screen.desktopAvailableWidth * 0.45))
    readonly property real overlayHeight: Math.max(
        minimumHeight, Math.round(Screen.desktopAvailableHeight * 0.68))

    location: PlasmaCore.Types.Floating
    type: PlasmaCore.Dialog.WindowType.DialogWindow

    // The theme's background would be opaque; the overlay paints its
    // own backdrop, so the dialog surface itself must be bare.
    backgroundHints: PlasmaCore.Dialog.NoBackground

    // Renders above any view: the details are what the user asked for,
    // so nothing may be stacked on top of them.
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    hideOnWindowDeactivate: false

    visible: false

    // The window's own icon: the app's avatar, the same mark every
    // other window of the app carries.
    //
    // QML has no window-icon API (QIcon is not a QML type), so the
    // icon is put on the window from C++ (see AppIcon).
    Component.onCompleted: AppIcon.setWindowIcon(contextOverlay)

    // ---- The numbers -----------------------------------------------------

    // The session's title, or the empty string when it has none yet.
    readonly property string sessionTitle: context
        ? String(context.title || "") : ""

    // The size of the model's context window, in tokens; zero when it
    // is not known.
    readonly property int contextLimit: context
        ? Number(context.contextLimit || 0) : 0

    // How much of that window the session takes up: the size of the
    // biggest prompt it sent.
    readonly property int contextUsed: context
        ? Number(context.contextUsed || 0) : 0

    // Whether that number is measured (the provider counted it) or
    // estimated from the size of the conversation.
    readonly property bool contextUsedEstimated: context
        ? context.contextUsedEstimated === true : false

    // The share of the window the session has used, in per cent; the
    // daemon works it out, and it is zero when the window's size is
    // not known.
    readonly property real usage: context
        ? Number(context.usage || 0) : 0

    // Whether the model is priced: without a price the cost of the
    // session is not a number the view may quote.
    readonly property bool priced: context
        ? context.priced === true : false

    // The rows of the view: what is being told, and what it says.
    readonly property var rows: [
        { label: i18n("Session"), value: sessionTitle.length > 0
            ? sessionTitle : i18n("(not named yet)") },
        { label: i18n("Messages"), value: formatCount(context
            ? context.messageCount : 0) },
        { label: i18n("Provider"), value: formatText(context
            ? context.provider : "") },
        { label: i18n("Model"), value: formatText(context
            ? context.model : "") },
        { label: i18n("Context limit"), value: contextLimit > 0
            ? formatCount(contextLimit) : i18n("not known") },
        { label: i18n("Context used"), value: contextLimit > 0
            ? (formatCount(contextUsed)
               + (contextUsedEstimated ? i18n(" (estimated)") : "")
               + " — " + formatPercent(usage))
            : i18n("not known") },
        { label: i18n("Tokens spent"), value: formatCount(context
            ? context.totalTokens : 0) },
        { label: i18n("Input tokens"), value: formatCount(context
            ? context.tokensIn : 0) },
        { label: i18n("Output tokens"), value: formatCount(context
            ? context.tokensOut : 0) },
        { label: i18n("Reasoning tokens"), value: formatCount(context
            ? context.reasoningTokens : 0) },
        { label: i18n("Cache tokens (read)"), value: formatCount(context
            ? context.cacheTokens : 0) },
        { label: i18n("User messages"), value: formatCount(context
            ? context.userMessages : 0) },
        { label: i18n("Assistant messages"), value: formatCount(context
            ? context.assistantMessages : 0) },
        { label: i18n("Total cost"), value: priced
            ? formatCost(context ? context.cost : 0)
            : i18n("the model is not priced") },
        { label: i18n("Session created"), value: formatDate(context
            ? context.createdAt : "") },
    ]

    // A number of tokens, with the separators of the locale.
    function formatCount(value) {
        const number = Number(value || 0)
        return number.toLocaleString(Qt.locale(), "f", 0)
    }

    // A share of the window, in per cent: with a decimal when it is
    // small enough for one to matter.
    function formatPercent(value) {
        const number = Number(value || 0)
        return number.toLocaleString(Qt.locale(), "f", number < 10 ? 1 : 0) + "%"
    }

    // What the session cost, in dollars.
    function formatCost(value) {
        const number = Number(value || 0)
        return "$" + number.toLocaleString(Qt.locale(), "f", 2)
    }

    // A text the daemon sent, or the dash that says there is none.
    function formatText(value) {
        const text = String(value || "")
        return text.length > 0 ? text : "—"
    }

    // The moment the session began, in the local time.
    function formatDate(value) {
        const text = String(value || "")
        if (text.length === 0) {
            return "—"
        }
        const date = new Date(text)
        if (isNaN(date.getTime())) {
            return text
        }
        // The locale's own short format: the date and the time of the
        // day, as this system's own conventions write them. The UI
        // locale, not the default one — the desktop's is what the
        // user reads.
        return Qt.formatDateTime(date, Qt.locale().ShortFormat)
    }

    // ---- Opening and closing ---------------------------------------------

    // Opens the overlay, centred on the display.
    function open() {
        // The window comes up under the app's own name, so it is the
        // app's window in the windowing system's eyes too.
        AppIcon.showWindow(contextOverlay)
        width = overlayWidth
        height = overlayHeight
        x = Screen.virtualX + Math.round((Screen.width - width) / 2)
        y = Screen.virtualY + Math.round((Screen.height - height) / 2)
        visible = true
        // The compositor places the window itself, so the position is
        // applied again a tick later — it then wins over the placement
        // the compositor chose. The same tick asks for the keyboard:
        // the overlay must hold the focus for Esc to reach it.
        Qt.callLater(function() {
            x = Screen.virtualX + Math.round((Screen.width - width) / 2)
            y = Screen.virtualY + Math.round((Screen.height - height) / 2)
            contextOverlay.requestActivate()
            backdrop.forceActiveFocus()
        })
    }

    // Puts the overlay away.
    function dismiss() {
        close()
        dismissed()
    }

    // The dialog's default property is mainItem, so the backdrop is a
    // bare child (see the session search overlay).
    mainItem: Rectangle {
        id: backdrop

        implicitWidth: contextOverlay.overlayWidth
        implicitHeight: contextOverlay.overlayHeight
        radius: Kirigami.Units.smallSpacing

        // The themed backdrop, painted here because the dialog surface
        // itself is bare: dark and near-black in the dark scheme, the
        // theme's own light one otherwise.
        color: PhoenixTheme.backgroundColor
        border.width: 1
        border.color: PhoenixTheme.backdropBorderColor

        // Esc's home is the backdrop itself: key events travel from
        // whatever holds the focus up through its ancestors, and the
        // backdrop is an ancestor of everything the overlay shows. Only
        // Escape is handled, so no other key is stolen.
        focus: true
        Keys.onEscapePressed: contextOverlay.dismiss()

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Kirigami.Units.gridUnit * 2
            spacing: Kirigami.Units.smallSpacing

            // What the window is about.
            Kirigami.Heading {
                level: 4
                Layout.fillWidth: true

                text: i18n("Context")
                color: PhoenixTheme.headingTextColor
            }

            Text {
                Layout.fillWidth: true

                text: i18n("What this session has used of the model's context window.")
                color: PhoenixTheme.secondaryTextColor
                wrapMode: Text.WordWrap
            }

            Kirigami.Separator {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing
            }

            // The facts: a name on the left, what it says on the right.
            //
            // One row per fact, each a row of its own: a Repeater makes
            // one delegate per entry, so the two halves of a fact have
            // to live in a single delegate — a row of its own.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Kirigami.Units.smallSpacing

                Repeater {
                    id: factRepeater

                    model: contextOverlay.rows

                    delegate: RowLayout {
                        id: factRow

                        required property var modelData

                        Layout.fillWidth: true
                        spacing: Kirigami.Units.gridUnit

                        // The name of the fact...
                        PC3.Label {
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 9
                            Layout.alignment: Qt.AlignTop | Qt.AlignLeft

                            text: factRow.modelData.label
                            color: PhoenixTheme.secondaryTextColor
                        }

                        // ...and what it says.
                        Kirigami.SelectableLabel {
                            Layout.fillWidth: true

                            text: factRow.modelData.value
                            textFormat: TextEdit.PlainText
                            wrapMode: Text.WordWrap
                            color: PhoenixTheme.textColor
                        }
                    }
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            // The foot: the way out that needs no keyboard, and the
            // one that does.
            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Text {
                    Layout.fillWidth: true

                    text: i18n("Esc closes this window.")
                    font.pixelSize: Math.max(
                        9, Math.round(Qt.application.font.pixelSize * 0.8))
                    color: PhoenixTheme.secondaryTextColor
                }

                PC3.Button {
                    objectName: "contextCloseButton"

                    text: i18n("Close")
                    onClicked: contextOverlay.dismiss()
                }
            }
        }
    }
}

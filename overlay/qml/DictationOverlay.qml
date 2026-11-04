// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

/*
 * The dictation overlay.
 *
 * It appears automatically the moment dictation starts, centred and aligned
 * horizontally with the input widget in the panel. While the agent is still
 * transcribing, the text streams in asynchronously — and the result lands in
 * an editable field, so the user can correct the recognition before sending.
 *
 * The Dialog type comes from org.kde.plasma.core so the window is managed by
 * the shell rather than being an ordinary application window — which matters on
 * Wayland, where an app cannot position its own windows.
 */
PlasmaCore.Dialog {
    id: dictationOverlay

    // Positioned by the shell; the horizontal alignment with the panel widget
    // comes from `visualParent` being the widget's item.
    location: PlasmaCore.Types.Floating
    type: PlasmaCore.Dialog.WindowType.DialogWindow
    backgroundHints: PlasmaCore.Dialog.StandardBackground
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    hideOnWindowDeactivate: false

    // The window's own icon: the app's avatar, the same mark every other
    // window of the app carries.
    //
    // QML has no window-icon API (QIcon is not a QML type), so the icon
    // is put on the window from C++ (see AppIcon).
    Component.onCompleted: AppIcon.setWindowIcon(dictationOverlay)

    // The transcript arrives progressively from the speech-to-text engine
    // and stays editable: the user can correct the recognition before it
    // becomes a prompt.
    property string transcript: ""
    property bool listening: false

    // Whether the prompt is being typed rather than dictated: the panel's
    // keyboard button (and its global hotkey) open the very same editor
    // for a query the user writes, with the microphone closed and nothing
    // transcribed. The editor behaves the same, it only starts empty.
    property bool typing: false

    // The typed query, kept while the editor is hidden: the toggle can
    // bring it back, so a mis-click costs nothing. Esc throws it away.
    property string draft: ""

    // Set once recording stops and the clip is with the transcription
    // service; the transcript appears (or the error below) when it returns.
    property bool transcribing: false
    property string statusMessage: ""

    // The files attached to this prompt: a list of maps with "name",
    // "path", "mimeType" and "size" keys. Bound by the shell; the same
    // list the results pane shows, because both edit the same prompt.
    property var attachments: []

    // Emitted when the user picks a file to attach to the prompt.
    signal attachmentAdded(url file)

    // Emitted when the user takes an attached file off the prompt.
    signal attachmentRemoved(int index)

    // Whether the window picker is waiting for the user to click a window
    // of the desktop; bound by the shell.
    property bool screenshotPicking: false

    // The global shortcut that starts the window picker; bound by the
    // shell (empty when nothing is bound).
    property string screenshotShortcut: ""

    // Emitted when the user wants to photograph a window of the desktop.
    signal screenshotRequested()

    // Emitted when the user accepts the transcript and wants it sent to the
    // agent as a prompt. The shell wires this to the controller.
    signal submitted(string text)

    // Emitted when the user empties the editor: the dictated transcript is
    // gone, so what is typed in its place is a fresh query rather than the
    // recording's prompt.
    signal dictationDiscarded()

    // Whether the transcript can be submitted right now: something was
    // dictated, the clip is not mid-recording or mid-transcription. The
    // send button and the Return shortcut both consult it, so the two
    // paths can never disagree.
    readonly property bool canSubmit: transcript.length > 0
        && !listening && !transcribing

    // Emitted when the user dismisses the overlay without sending.
    signal closed()

    // Emitted when the user discards the query (Esc): the text is thrown
    // away and the overlay goes away with it.
    signal discarded()

    // Engine results are pushed into the editor imperatively (not via a
    // text binding): a binding would be destroyed by the first user edit
    // and a later transcript would then never appear.
    onTranscriptChanged: transcriptEdit.text = transcript

    // Starting to type shows the draft again - and nothing else: what a
    // dictation left in the editor must not turn into a typed query.
    onTypingChanged: if (typing) {
        transcript = draft;
    }

    // Anchoring the overlay to the panel widget keeps it horizontally aligned
    // with wherever the user is actually interacting.
    property Item inputAnchor: null

    visible: false

    // Puts the keyboard in the editor: a typed prompt is written right
    // away, so the editor takes the focus as soon as the overlay opens.
    // The shell calls this after showing the overlay (the window has to
    // be up before the item can be given the focus).
    function focusEditor() {
        if (!transcriptEdit.readOnly) {
            transcriptEdit.forceActiveFocus();
        }
    }

    // The overlay is a window of its own, so it can only be given the
    // focus once it is up and active: every activation hands the
    // keyboard to the editor, which is where a typed prompt is written.
    onActiveChanged: if (active) {
        focusEditor();
    }

    onInputAnchorChanged: {
        if (inputAnchor) {
            visualParent = inputAnchor;
        }
    }

    mainItem: Kirigami.ScrollablePage {
        id: page

        title: i18n("Phoenix DNA")
        visible: dictationOverlay.visible

        // A stable dialog size: wide enough for a sentence, tall enough for
        // a paragraph. Longer transcripts scroll in place — without a fixed
        // implicit size the dialog would stretch to the transcript's
        // single-line width or clip it.
        implicitWidth: Kirigami.Units.gridUnit * 30
        implicitHeight: Kirigami.Units.gridUnit * 12

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.smallSpacing

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    id: statusIcon
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    source: LucideIcons.url(
                        dictationOverlay.typing ? "keyboard"
                            : dictationOverlay.listening ? "mic-audio-lines"
                            : "info",
                        Kirigami.Theme.textColor)
                }

                Kirigami.Heading {
                    Layout.fillWidth: true
                    // KF6 6.31 has no Kirigami.Heading.Level enum; levels are plain ints.
                    level: 3
                    text: dictationOverlay.typing ? i18n("Type a query")
                        : dictationOverlay.listening ? i18n("Listening…")
                        : dictationOverlay.transcribing ? i18n("Transcribing…")
                        : i18n("Phoenix DNA")
                }

                // Runs while the finished clip is being transcribed.
                PC3.BusyIndicator {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                    visible: dictationOverlay.transcribing
                    running: dictationOverlay.transcribing
                }

                // A dictation can be abandoned: close without sending.
                PC3.ToolButton {
                    icon.source: LucideIcons.url("x", Kirigami.Theme.textColor)
                    visible: !dictationOverlay.listening
                    opacity: visible ? 1 : 0
                    enabled: visible

                    PC3.ToolTip {
                        text: i18n("Close")
                    }

                    onClicked: dictationOverlay.closed()
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            // The files attached to this prompt: one chip per file, each
            // with a button that takes it off the list again. The chips
            // sit above the editor, so the text field keeps its full
            // height when there is nothing attached.
            Flow {
                id: attachmentFlow

                Layout.fillWidth: true
                visible: dictationOverlay.attachments.length > 0
                spacing: Kirigami.Units.smallSpacing

                Repeater {
                    id: attachmentRepeater
                    objectName: "attachmentRepeater"

                    model: dictationOverlay.attachments

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
                                // The chip is painted by the app palette
                                // (it must stay readable on it), so its
                                // text is too.
                                color: PhoenixTheme.textColor
                                Layout.maximumWidth: Kirigami.Units.gridUnit * 8
                            }

                            PC3.ToolButton {
                                icon.source: LucideIcons.url("x", PhoenixTheme.textColor)
                                icon.width: 12
                                icon.height: 12

                                onClicked: dictationOverlay.attachmentRemoved(attachmentChip.index)
                            }
                        }
                    }
                }
            }

            // The prompt itself: one rounded field with the editor on top
            // and — inside the field, on the row below the text — the
            // buttons that act on the prompt, the send at the far right.
            // Nothing dangles outside the field, so nothing can be
            // mistaken for belonging to another part of the overlay.
            Rectangle {
                id: promptField

                Layout.fillWidth: true
                Layout.fillHeight: true

                // A large radius: the field is a container, not a text box
                // — the buttons sit inside it, and a shallow corner would
                // read as the border of a row, not as the shell of a thing.
                radius: Kirigami.Units.largeSpacing * 1.5
                color: PhoenixTheme.paneFill
                border.width: 1
                // The keyboard's place is shown by the outline: the accent
                // colour while the editor has it, the quiet chip colour
                // otherwise.
                border.color: transcriptEdit.activeFocus
                    ? PhoenixTheme.focusBorderColor
                    : PhoenixTheme.chipBorderColor

                ColumnLayout {
                    id: fieldColumn

                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing
                    spacing: Kirigami.Units.smallSpacing

                    // The live transcript, editable so the user can correct the
                    // recognition before sending. The editor is the single source
                    // of truth for what "Send to agent" submits: engine results
                    // flow in via onTranscriptChanged above, edits flow back into
                    // dictationOverlay.transcript here (guarded, so the two never ping-pong).
                    PC3.TextArea {
                        id: transcriptEdit
                        objectName: "transcriptEdit"

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        // "TextArea" is not addressable unqualified here: the
                        // plasma-components import is namespaced ("as PC3"), and
                        // an unqualified reference is a ReferenceError at load.
                        // The field paints its own frame (see promptField
                        // above), so the editor inside it draws none of its own.
                        // Its text keeps the plasma palette: the dialog's
                        // ground is the plasma theme's own, and text and
                        // ground have to come from the same theme.
                        background: null
                        wrapMode: Text.Wrap
                        // Locked while the engine owns the text; editable the
                        // moment a result has landed — and editable right
                        // away when the prompt is typed rather than dictated.
                        readOnly: dictationOverlay.listening || dictationOverlay.transcribing
                        placeholderText: dictationOverlay.typing ? i18n("Type your query…")
                                        : dictationOverlay.transcribing ? i18n("Transcribing…")
                                        : dictationOverlay.listening ? i18n("Speak now…")
                                        : i18n("No speech was recognized.")

                        onTextChanged: {
                            if (text !== dictationOverlay.transcript) {
                                dictationOverlay.transcript = text
                                // The user emptied the editor: the
                                // transcript of the recording is gone, so
                                // what is typed in its place is a fresh
                                // query rather than the recording's
                                // prompt. A correction is not an emptying:
                                // it keeps the recording's intent.
                                if (text.length === 0) {
                                    dictationOverlay.dictationDiscarded()
                                }
                            }
                            // What the user writes is the draft: it is kept
                            // while the editor is hidden, so the toggle can
                            // bring the query back.
                            if (dictationOverlay.typing) {
                                dictationOverlay.draft = text
                            }
                        }

                        // Return submits, even with the keyboard in the
                        // editor: a text edit swallows the key before the
                        // window's shortcut ever gets to see it (it would
                        // insert a line break instead). The window shortcut
                        // below covers the case where the focus rests
                        // elsewhere; this one covers the editor itself, so
                        // the two can never both fire. Shift is the line
                        // break: it is simply not handled here, and the
                        // editor inserts it the way it always has.
                        Keys.onPressed: (event) => {
                            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                && !(event.modifiers & Qt.ShiftModifier)) {
                                event.accepted = true;
                                if (dictationOverlay.canSubmit) {
                                    dictationOverlay.submitted(dictationOverlay.transcript);
                                }
                            }
                        }
                    }

                    // The row of controls that act on the prompt, inside the
                    // field: the attachments on the left, the send at the
                    // far right.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        // The button that attaches files to the prompt.
                        PC3.ToolButton {
                            id: addAttachmentButton
                            objectName: "addAttachmentButton"

                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium

                            icon.source: LucideIcons.url("plus", Kirigami.Theme.textColor)
                            icon.width: Kirigami.Units.iconSizes.small
                            icon.height: Kirigami.Units.iconSizes.small

                            onClicked: attachmentDialog.open()

                            PC3.ToolTip {
                                text: i18n("Attach a file to the prompt")
                            }
                        }

                        // The camera: photograph a window of the desktop and
                        // attach the picture to this prompt.
                        PC3.ToolButton {
                            id: screenshotButton
                            objectName: "screenshotButton"

                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium

                            // While the picker waits for a click, the button
                            // says so.
                            icon.source: LucideIcons.url("camera", dictationOverlay.screenshotPicking
                                ? Kirigami.Theme.highlightColor
                                : Kirigami.Theme.textColor)
                            icon.width: Kirigami.Units.iconSizes.small
                            icon.height: Kirigami.Units.iconSizes.small

                            onClicked: dictationOverlay.screenshotRequested()

                            PC3.ToolTip {
                                text: {
                                    const shortcut = dictationOverlay.screenshotShortcut;
                                    return shortcut.length > 0
                                        ? i18n("Screenshot a window (%1)", shortcut)
                                        : i18n("Screenshot a window");
                                }
                            }
                        }

                        // Everything before the send button hugs the field's
                        // left edge; this spacer carries the rest of the
                        // width to the send button, so the two ends read
                        // independently.
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                        }

                        // The send: a plain up arrow, the one glyph chat
                        // inputs have agreed on. One explicit send, no
                        // accidental submissions — Return does the same from
                        // the keyboard. The square shape is fixed, so the
                        // button never changes size with its state.
                        PC3.Button {
                            objectName: "sendButton"

                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                            display: PC3.AbstractButton.IconOnly

                            icon.source: LucideIcons.url("arrow-up", Kirigami.Theme.textColor)
                            icon.width: Kirigami.Units.iconSizes.small
                            icon.height: Kirigami.Units.iconSizes.small
                            enabled: dictationOverlay.canSubmit
                            onClicked: dictationOverlay.submitted(dictationOverlay.transcript)

                            PC3.ToolTip {
                                text: i18n("Send to agent (Return)")
                            }
                        }
                    }
                }
            }

            // Return submits the transcript from anywhere in the overlay
            // window. The shortcut lives on the window rather than on the
            // editor because keyboard focus often rests elsewhere (a fresh
            // dictation, the send button after a mouse click) and a key
            // handler on the editor would simply never fire — previously
            // the only keyboard way to send was pressing Space on the
            // focused button. "Return" covers the main Enter key, "Enter"
            // the numpad's. Shift+Return is *not* a shortcut: it stays the
            // line break it is in the editor, so a long query can be
            // written without the first Return sending it away. The
            // canSubmit guard is shared with the button, so the two paths
            // can never disagree.
            Shortcut {
                sequences: ["Return", "Enter"]
                context: Qt.WindowShortcut
                enabled: dictationOverlay.canSubmit
                onActivated: dictationOverlay.submitted(dictationOverlay.transcript)
            }

            // Esc throws a typed query away: the editor goes down and
            // the text with it. It is the one way to lose the draft -
            // hiding the editor with the toggle keeps it.
            //
            // The shortcut lives on the window rather than on the editor
            // for the same reason as the one above: the keyboard focus
            // often rests elsewhere, and a key handler on the editor
            // would simply never fire.
            Shortcut {
                sequences: ["Escape"]
                context: Qt.WindowShortcut
                enabled: dictationOverlay.typing
                onActivated: {
                    dictationOverlay.draft = "";
                    dictationOverlay.transcript = "";
                    dictationOverlay.discarded();
                }
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
                        dictationOverlay.attachmentAdded(selectedFiles[i])
                    }
                }
            }

            // Why a clip could not be transcribed (missing key, network,
            // quota); shown only when there is nothing to show for it.
            Kirigami.SelectableLabel {
                visible: dictationOverlay.statusMessage.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: dictationOverlay.statusMessage
                color: Kirigami.Theme.negativeTextColor
            }
        }
    }
}

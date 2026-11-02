// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami

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

    // The transcript arrives progressively from the speech-to-text engine
    // and stays editable: the user can correct the recognition before it
    // becomes a prompt.
    property string transcript: ""
    property bool listening: false

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

    // Whether the transcript can be submitted right now: something was
    // dictated, the clip is not mid-recording or mid-transcription. The
    // send button and the Shift+Enter shortcut both consult it, so the two
    // paths can never disagree.
    readonly property bool canSubmit: transcript.length > 0
        && !listening && !transcribing

    // Emitted when the user dismisses the overlay without sending.
    signal closed()

    // Engine results are pushed into the editor imperatively (not via a
    // text binding): a binding would be destroyed by the first user edit
    // and a later transcript would then never appear.
    onTranscriptChanged: transcriptEdit.text = transcript

    // Anchoring the overlay to the panel widget keeps it horizontally aligned
    // with wherever the user is actually interacting.
    property Item inputAnchor: null

    visible: false

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
                    source: dictationOverlay.listening ? "audio-input-microphone" : "dialog-information"
                }

                Kirigami.Heading {
                    Layout.fillWidth: true
                    // KF6 6.31 has no Kirigami.Heading.Level enum; levels are plain ints.
                    level: 3
                    text: dictationOverlay.listening ? i18n("Listening…")
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
                    icon.name: "window-close"
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
                        color: Qt.rgba(1, 1, 1, 0.08)
                        border.width: 1
                        border.color: Qt.rgba(1, 1, 1, 0.16)

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
                                color: Kirigami.Theme.textColor
                                Layout.maximumWidth: Kirigami.Units.gridUnit * 8
                            }

                            PC3.ToolButton {
                                icon.name: "window-close"
                                icon.width: 12
                                icon.height: 12

                                onClicked: dictationOverlay.attachmentRemoved(attachmentChip.index)
                            }
                        }
                    }
                }
            }

            // The prompt itself, with the button that attaches files to
            // it on its left.
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Kirigami.Units.smallSpacing

                PC3.ToolButton {
                    id: addAttachmentButton
                    objectName: "addAttachmentButton"

                    Layout.alignment: Qt.AlignTop
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

                // The camera: photograph a window of the desktop and
                // attach the picture to this prompt.
                PC3.ToolButton {
                    id: screenshotButton
                    objectName: "screenshotButton"

                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium

                    icon.name: "camera-photo"
                    icon.width: Kirigami.Units.iconSizes.small
                    icon.height: Kirigami.Units.iconSizes.small
                    // While the picker waits for a click, the button says
                    // so.
                    icon.color: dictationOverlay.screenshotPicking
                        ? Kirigami.Theme.highlightColor
                        : Kirigami.Theme.textColor

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

                // The live transcript, editable so the user can correct the
                // recognition before sending. The editor is the single source
                // of truth for what "Send to agent" submits: engine results
                // flow in via onTranscriptChanged above, edits flow back into
                // dictationOverlay.transcript here (guarded, so the two never ping-pong).
                PC3.TextArea {
                    id: transcriptEdit

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    // "TextArea" is not addressable unqualified here: the
                    // plasma-components import is namespaced ("as PC3"), and
                    // an unqualified reference is a ReferenceError at load.
                    wrapMode: Text.Wrap
                    // Locked while the engine owns the text; editable the
                    // moment a result has landed.
                    readOnly: dictationOverlay.listening || dictationOverlay.transcribing
                    placeholderText: dictationOverlay.transcribing ? i18n("Transcribing…")
                                    : dictationOverlay.listening ? i18n("Speak now…")
                                    : i18n("No speech was recognized.")

                    onTextChanged: if (text !== dictationOverlay.transcript) {
                        dictationOverlay.transcript = text
                    }
                }
            }

            // Shift+Enter submits the transcript from anywhere in the
            // overlay window. The shortcut lives on the window rather than
            // on the editor because keyboard focus often rests elsewhere
            // (a fresh dictation, the send button after a mouse click) and
            // a key handler on the editor would simply never fire —
            // previously the only keyboard way to send was pressing Space
            // on the focused button. "Return" covers the main Enter key,
            // "Enter" the numpad's. Plain Enter keeps inserting the line
            // break — there is no shortcut for it. The canSubmit guard is
            // shared with the button, so the two paths can never disagree.
            Shortcut {
                sequences: ["Shift+Return", "Shift+Enter"]
                context: Qt.WindowShortcut
                enabled: dictationOverlay.canSubmit
                onActivated: dictationOverlay.submitted(dictationOverlay.transcript)
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

            // The user verifies the transcript before it becomes a prompt;
            // one explicit send, no accidental submissions. The button stays
            // at its natural size so the label is never squeezed away.
            RowLayout {
                Layout.fillWidth: true

                PC3.Button {
                    Layout.alignment: Qt.AlignRight
                    icon.name: "document-send"
                    text: i18n("Send to agent")
                    enabled: dictationOverlay.canSubmit
                    onClicked: dictationOverlay.submitted(dictationOverlay.transcript)

                    PC3.ToolTip {
                        text: i18n("Send to agent (Shift+Enter)")
                    }
                }
            }
        }
    }
}

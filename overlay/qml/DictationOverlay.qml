// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
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

                // Same contract as the send button, from the keyboard:
                // Shift+Enter submits, Enter breaks the line (the editor is
                // multi-line). Any other key keeps the editor's own
                // behaviour — hence no blanket accept.
                Keys.onPressed: (event) => {
                    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                        && (event.modifiers & Qt.ShiftModifier)) {
                        if (dictationOverlay.canSubmit) {
                            dictationOverlay.submitted(dictationOverlay.transcript)
                        }
                        event.accepted = true
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

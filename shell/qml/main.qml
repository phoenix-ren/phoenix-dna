// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.plasmoid

// The applet's own QML module: it exposes the C++ controller singleton, the
// mode enum, and the QML types defined alongside them.
import plasma.applet.org.phoenixai.agent as Phoenix

// Components 2 and 3 are separate QML modules, installed independently.
import org.phoenixai.overlay as PhoenixOverlay
import org.phoenixai.pane as PhoenixPane

/*
 * Component 1 — the panel widget.
 *
 * The compact representation is what the user sees in the taskbar: the mode
 * button on the left, the waveform in the middle and the record button on the
 * right.
 *
 * The overlay (component 2) and the results pane (component 3) are separate
 * QML modules; they are instantiated lazily and only when needed.
 */
PlasmoidItem {
    id: root

    // The overlay pops up by itself the moment dictation starts, and the
    // transcript streams into it as the speech is decoded. When the
    // results pane is up, its own input bar is the transcript surface
    // instead: the dictation belongs to the conversation on screen there,
    // and a second window stacked over the pane would only cover it.
    Connections {
        target: Phoenix.PhoenixController

        function onOverlayRequested() {
            if (resultsPane.visible) {
                // The pane's bar mirrors listening/transcribing via its
                // bindings below; nothing else to do here.
                return;
            }
            overlay.transcript = Phoenix.PhoenixController.partialTranscript;
            overlay.listening = Phoenix.PhoenixController.recording;
            overlay.transcribing = Phoenix.PhoenixController.transcribing;
            overlay.statusMessage = Phoenix.PhoenixController.statusMessage;
            overlay.inputAnchor = root;
            overlay.visible = true;
        }

        function onPaneRequested() {
            resultsPane.toggle();
        }
    }

    // The answer to the generated query arrives asynchronously from the
    // agent daemon: activityStarted opens the pane the moment the run
    // produces its first output, and the bridge streams the answer and
    // tool log into the pane's conversation as they happen; resultReady —
    // the finished result — re-pins the scroll to the newest exchange.
    Connections {
        target: Phoenix.PhoenixController.agent

        function onActivityStarted() {
            resultsPane.showLive();
        }

        function onResultReady() {
            resultsPane.showResult();
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
            overlay.transcript = Phoenix.PhoenixController.partialTranscript;
            resultsPane.transcript = Phoenix.PhoenixController.partialTranscript;
        }

        function onTranscribingChanged() {
            overlay.transcribing = Phoenix.PhoenixController.transcribing;
        }

        function onStatusMessageChanged() {
            overlay.statusMessage = Phoenix.PhoenixController.statusMessage;
        }
    }

    // Component 2 — the dictation overlay.
    PhoenixOverlay.Overlay {
        id: overlay

        // The transcript becomes a prompt only when the user says so; the
        // results pane is not opened here — it appears by itself when the
        // agent's answer comes back.
        onSubmitted: (text) => {
            overlay.visible = false;
            Phoenix.PhoenixController.submitText(text);
        }

        // Closing abandons the dictation; nothing is sent.
        onClosed: overlay.visible = false
    }

    // Component 3 — the results pane.
    //
    // Transient: it shows itself the moment the agent's result arrives,
    // renders above any view, and dismisses itself again once the result
    // has had its time on screen. Anchored to the widget like the overlay
    // so it appears where the query was triggered from.
    //
    // Its input bar continues the conversation: record and send route
    // through the controller exactly like the panel's record button and
    // the overlay's "Send to agent", so a follow-on rides on the session
    // history the initial voice prompt created (AgentBridge tracks the
    // session id).
    PhoenixPane.Pane {
        id: resultsPane

        anchor: root

        // The bridge appends one row per run to the conversation model and
        // streams the answer (text deltas) and tool-call narration (fenced
        // Markdown) into it live; the pane renders the history and follows
        // the newest exchange. Follow-on questions add rows — previous
        // results stay, ChatGPT style.
        conversation: Phoenix.PhoenixController.agent.conversation

        // Dictation state for the input bar, mirroring the overlay. The
        // transcript itself is pushed imperatively (see the Connections
        // above) because the pane's editor is editable.
        listening: Phoenix.PhoenixController.recording
        transcribing: Phoenix.PhoenixController.transcribing
        statusMessage: Phoenix.PhoenixController.statusMessage

        // The same toggle the panel widget's record button performs.
        onRecordClicked: Phoenix.PhoenixController.toggleRecording()

        onSubmitted: (text) => {
            // Follow-ons keep the pane as the conversation surface: clear
            // the input and follow the newest exchange while the answer
            // streams in below the previous ones.
            resultsPane.transcript = "";
            resultsPane.showLive();
            Phoenix.PhoenixController.submitText(text);
        }

        // The pane persists until closed by hand (button, Esc); a dismissal
        // mid-dictation must not strand the recording: the dictation
        // overlay takes over as the transcript surface so the follow-on can
        // still be verified and sent.
        onDismissed: if (resultsPane.listening) {
            overlay.transcript = Phoenix.PhoenixController.partialTranscript;
            overlay.listening = Phoenix.PhoenixController.recording;
            overlay.transcribing = Phoenix.PhoenixController.transcribing;
            overlay.statusMessage = Phoenix.PhoenixController.statusMessage;
            overlay.inputAnchor = root;
            overlay.visible = true;
        }
    }

    compactRepresentation: Phoenix.CompactRepresentation {
        onRecordClicked: Phoenix.PhoenixController.toggleRecording()
    }

    fullRepresentation: Phoenix.CompactRepresentation {
        onRecordClicked: Phoenix.PhoenixController.toggleRecording()
    }
}

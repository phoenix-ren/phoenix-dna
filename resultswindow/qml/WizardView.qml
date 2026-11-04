// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

/*
 * The wizard: a form of several questions (one ask_user call) walked
 * one question at a time, entirely client-side — the whole form arrived
 * at once, so back is instant and the progress is exact. Every answer
 * is kept as it is given (a question cannot be left behind: Next stays
 * disabled until it is answered) and all of them are submitted together
 * by the final Submit.
 *
 * Fed by `form` ({askId, questions, goal}); the answers leave as one
 * submitted(answers) signal, routed to AnswerQuestion. Embedded in the
 * wizard overlay (the large, transient, centered window).
 */
ColumnLayout {
    id: wizardView

    property var form: ({})

    // Emitted when the user finishes the form: one {selected, text}
    // map per question, in order.
    signal submitted(var answers)

    readonly property var questions: form.questions ?? []
    readonly property int count: questions.length
    readonly property int cursor: __cursor

    // The walker's state: which question is on screen, and one answer
    // entry per question. All local — back and forward are instant, and
    // nothing reaches the agent until the final submit.
    property int __cursor: 0
    property var __answers: []
    // Set with the final Submit; the chrome waits with the agent until
    // the form is answered or replaced (the overlay usually closes long
    // before that - the bridge clears the form optimistically).
    property bool __sent: false

    onFormChanged: {
        __cursor = 0;
        __sent = false;
        __answers = questions.map(question => ({ "selected": [], "text": "" }));
    }

    function saveCurrent() {
        if (__answers.length > __cursor) {
            __answers[__cursor] = inputs.currentState();
        }
    }

    function goBack() {
        if (__cursor > 0 && !__sent) {
            saveCurrent();
            __cursor--;
        }
    }

    function goNext() {
        if (!__sent && inputs.canSend && __cursor < count - 1) {
            saveCurrent();
            __cursor++;
        }
    }

    function submitAll() {
        if (__sent || !inputs.canSend) {
            return;
        }
        saveCurrent();
        __sent = true;
        submitted(__answers);
    }

    // Restoring a page's answer is bound to the question change itself
    // (not a callLater): the inputs reset first - their own handler -
    // and this connection puts the snapshot back right after it, in a
    // deterministic order. Choices survive navigation.
    Connections {
        target: inputs

        function onQuestionChanged() {
            inputs.setState(wizardView.__answers[wizardView.cursor] || {});
        }
    }

    // The goal names what the questions collect ("backup setup"); the
    // fallback keeps the pane titled when the model omitted it.
    Kirigami.Heading {
        level: 2
        color: PhoenixTheme.headingTextColor
        wrapMode: Text.WordWrap

        text: wizardView.form.goal
              ? wizardView.form.goal
              : i18n("A few questions first")
    }

    // Exact progress: the whole form is known, so the count is real -
    // no "~" estimates - and the bar stands full on the last question.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing

        PC3.ProgressBar {
            Layout.fillWidth: true
            // PC3, not Kirigami: Kirigami has no ProgressBar (c300500).
            from: 1
            to: Math.max(wizardView.count, 1)
            value: wizardView.cursor + 1
        }

        Text {
            color: PhoenixTheme.secondaryTextColor
            font.pixelSize: Math.max(
                9, Math.round(Qt.application.font.pixelSize * 0.9))

            text: i18n("Question %1 of %2",
                       wizardView.cursor + 1, wizardView.count)
        }
    }

    Kirigami.Separator {
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.smallSpacing
    }

    // What has been answered so far — dimmed context, revisable with
    // the Back button (nothing is sent until the final submit).
    Repeater {
        model: wizardView.cursor  // the questions behind the current one

        delegate: ColumnLayout {
            id: answeredEntry

            required property int index

            Layout.fillWidth: true
            spacing: 0

            readonly property var question: wizardView.questions[answeredEntry.index]
            readonly property var answer: wizardView.__answers[answeredEntry.index]

            Text {
                Layout.fillWidth: true

                text: answeredEntry.question ? answeredEntry.question.question : ""
                wrapMode: Text.WordWrap
                elide: Text.ElideRight
                color: PhoenixTheme.secondaryTextColor
                font.pixelSize: Math.max(
                    9, Math.round(Qt.application.font.pixelSize * 0.85))
            }

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.smallSpacing

                text: {
                    const answer = answeredEntry.answer ?? {};
                    const own = answer.text ?? "";
                    if (answer.ownSelected && own) {
                        return "✓ " + own;
                    }
                    const labels = (answer.selected ?? []).map(
                        index => answeredEntry.question.options[index].label);
                    if (labels && own) {
                        return "✓ " + labels.join(", ") + " — " + own;
                    }
                    if (labels) {
                        return "✓ " + labels.join(", ");
                    }
                    return own ? "✓ " + own : "· " + i18n("left blank");
                }
                wrapMode: Text.WordWrap
                color: PhoenixTheme.secondaryTextColor
                font.pixelSize: Math.max(
                    9, Math.round(Qt.application.font.pixelSize * 0.85))
            }
        }
    }

    // The current question.
    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            level: 5
            visible: text.length > 0
            color: PhoenixTheme.secondaryTextColor

            text: wizardView.questions[wizardView.cursor]
                  ? (wizardView.questions[wizardView.cursor].header ?? "")
                  : ""
        }

        Kirigami.SelectableLabel {
            objectName: "wizardQuestionLabel"

            Layout.fillWidth: true

            text: wizardView.questions[wizardView.cursor]
                  ? (wizardView.questions[wizardView.cursor].question ?? "")
                  : ""
            textFormat: TextEdit.PlainText
            wrapMode: Text.WordWrap
            font.weight: Font.DemiBold
            color: PhoenixTheme.headingTextColor
        }

        QuestionInputs {
            id: inputs

            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing

            question: wizardView.questions[wizardView.cursor] ?? {}
            showSubmit: false
        }
    }

    // The walker's chrome: back through the answered questions (instant
    // - it is all local state), forward - only once this question is
    // answered - and the submit that finally sends every answer at once.
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        PC3.Button {
            objectName: "backButton"

            enabled: wizardView.cursor > 0 && !wizardView.__sent
            icon.name: "go-previous"
            text: i18n("Back")
            onClicked: wizardView.goBack()
        }

        Item { Layout.fillWidth: true }

        PC3.BusyIndicator {
            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
            visible: wizardView.__sent
            running: wizardView.__sent
        }

        PC3.Button {
            objectName: "nextButton"

            visible: wizardView.cursor < wizardView.count - 1
            enabled: inputs.canSend && !wizardView.__sent
            icon.name: "go-next"
            text: i18n("Next")
            onClicked: wizardView.goNext()
        }

        PC3.Button {
            objectName: "submitButton"

            visible: wizardView.cursor >= wizardView.count - 1
            enabled: inputs.canSend && !wizardView.__sent
            icon.name: "document-send"
            text: i18n("Submit answers")
            onClicked: wizardView.submitAll()
        }
    }

    Text {
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.smallSpacing

        text: wizardView.__sent
              ? i18n("Answers sent — the agent is working with them.")
              : (wizardView.cursor >= wizardView.count - 1
                 ? i18n("Everything is sent together when you submit; the agent is waiting.")
                 : i18n("Nothing is sent until you submit; Back revisits your answers."))
        font.pixelSize: Math.max(
            9, Math.round(Qt.application.font.pixelSize * 0.8))
        color: PhoenixTheme.secondaryTextColor
    }
}

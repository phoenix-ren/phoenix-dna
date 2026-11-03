// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

/*
 * The wizard view: while the run is blocked on a form of several
 * questions (one ask_user call), the results window's conversation gives
 * way to this pane. It walks the questions entirely client-side — one
 * question at a time, instant back, exact progress (the count is known:
 * the whole form arrived at once) — and submits every answer together
 * only when the user finishes. A blank answer is a deliberate skip; the
 * model is told there was no answer, not asked again.
 *
 * Fed by the agent bridge: `form` is its pendingForm ({askId, questions,
 * goal}) and the answers leave as one submit signal, routed to
 * AnswerQuestion.
 */
ColumnLayout {
    id: wizardView

    property var form: ({})
    property var wizard: ({})   // the bridge's summary: {active, goal, count}

    // Emitted when the user finishes the form: one {selected, text} map
    // per question, in order.
    signal submitted(var answers)

    readonly property var questions: form.questions ?? []
    readonly property int count: questions.length
    readonly property int cursor: __cursor

    // The walker's state: which question is on screen, and one answer
    // entry per question. All local — back and forward are instant, and
    // nothing reaches the agent until the final submit.
    property int __cursor: 0
    property var __answers: []

    onFormChanged: {
        __cursor = 0;
        __answers = questions.map(question => ({ "selected": [], "text": "" }));
    }

    function saveCurrent() {
        if (__answers.length > __cursor) {
            __answers[__cursor] = inputs.currentState();
        }
    }

    function goBack() {
        if (__cursor > 0) {
            saveCurrent();
            __cursor--;
        }
    }

    function goNext() {
        if (__cursor < count - 1) {
            saveCurrent();
            __cursor++;
        }
    }

    function submitAll() {
        saveCurrent();
        submitted(__answers);
    }

    on__CursorChanged: Qt.callLater(() => inputs.setState(__answers[__cursor] || {}))

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
    // no "~" estimates. The bar's filled part is the answered part; the
    // question on screen is the one being answered.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing

        PC3.ProgressBar {
            Layout.fillWidth: true
            // PC3, not Kirigami: Kirigami has no ProgressBar (c300500).
            from: 0
            to: wizardView.count
            value: wizardView.cursor
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
                    const labels = (answer.selected ?? []).map(
                        index => answeredEntry.question.options[index].label);
                    const own = answer.text ?? "";
                    if (labels && own) {
                        return "✓ " + labels.join(", ") + " — " + own;
                    }
                    if (labels) {
                        return "✓ " + labels.join(", ");
                    }
                    if (own) {
                        return "✓ " + own;
                    }
                    return "· " + i18n("left blank");
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
    // - it is all local state), forward (a blank is a deliberate skip),
    // and the submit that finally sends every answer at once.
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        PC3.Button {
            objectName: "backButton"

            enabled: wizardView.cursor > 0
            icon.name: "go-previous"
            text: i18n("Back")
            onClicked: wizardView.goBack()
        }

        Item { Layout.fillWidth: true }

        PC3.Button {
            objectName: "nextButton"

            visible: wizardView.cursor < wizardView.count - 1
            icon.name: "go-next"
            text: i18n("Next")
            onClicked: wizardView.goNext()
        }

        PC3.Button {
            objectName: "submitButton"

            visible: wizardView.cursor >= wizardView.count - 1
            icon.name: "document-send"
            text: i18n("Submit answers")
            onClicked: wizardView.submitAll()
        }
    }

    Text {
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.smallSpacing

        text: wizardView.cursor >= wizardView.count - 1
              ? i18n("Everything is sent together when you submit; the agent is waiting.")
              : i18n("Nothing is sent until you submit; Back revisits your answers.")
        font.pixelSize: Math.max(
            9, Math.round(Qt.application.font.pixelSize * 0.8))
        color: PhoenixTheme.secondaryTextColor
    }
}

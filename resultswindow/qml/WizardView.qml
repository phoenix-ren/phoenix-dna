// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

import QtQuick
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.phoenixdna.theme

/*
 * The wizard view: while the agent is asking a series of questions
 * (ask_user carrying goal/step/est_steps), the results window's
 * conversation gives way to this pane — one question at a time, a
 * progress bar towards "the agent has what it needs", and the answers
 * collected so far. When the agent stops asking and goes to work, the
 * conversation returns.
 *
 * The pane is fed by the agent bridge: `wizard` is its wizard state
 * ({active, goal, step, estSteps, waiting, answers}) and `question` the
 * pending question ({} in the waiting gap between two questions, while
 * the model thinks about the answer it just got). The estimates are the
 * model's own and may be revised as it learns — hence the "~".
 */
ColumnLayout {
    id: wizardView

    property var wizard: ({})
    property var question: ({})

    // Emitted when the user answers the current question; routed to the
    // agent bridge like the inline card's answers.
    signal submitted(var selected, string text)

    // The gap between two questions: the last answer was accepted and
    // the next question has not arrived yet.
    readonly property bool waiting: wizard.active === true
        && question.askId === undefined

    readonly property int step: wizard.step ?? 1
    readonly property int estSteps: wizard.estSteps ?? step

    // The goal names what the questions collect ("backup setup"); the
    // fallback keeps the pane titled when the model omitted it.
    Kirigami.Heading {
        level: 2
        color: PhoenixTheme.headingTextColor
        wrapMode: Text.WordWrap

        text: wizardView.wizard.goal
              ? wizardView.wizard.goal
              : i18n("A few questions first")
    }

    // The progress towards enough information. `to` follows the model's
    // current estimate, so a revised count moves the bar's scale.
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing

        // PC3, not Kirigami: Kirigami has no ProgressBar — the trap this
        // applet already fell into once (c7916fe). Instantiating the
        // window (results_harness.qml) catches this at build time now.
        PC3.ProgressBar {
            Layout.fillWidth: true
            from: 1
            to: Math.max(wizardView.estSteps, wizardView.step)
            value: wizardView.step
        }

        Text {
            color: PhoenixTheme.secondaryTextColor
            font.pixelSize: Math.max(
                9, Math.round(Qt.application.font.pixelSize * 0.9))

            text: wizardView.waiting
                  ? i18n("Question %1 of ~%2 answered — preparing the next…",
                         wizardView.step, wizardView.estSteps)
                  : i18n("Question %1 of ~%2",
                         wizardView.step, wizardView.estSteps)
        }
    }

    Kirigami.Separator {
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.smallSpacing
    }

    // What has been collected so far — dimmed, because it is context,
    // not the task at hand.
    Repeater {
        model: wizardView.wizard.answers ?? []

        delegate: ColumnLayout {
            id: answeredEntry

            required property var modelData

            Layout.fillWidth: true
            spacing: 0

            Text {
                Layout.fillWidth: true

                text: answeredEntry.modelData.question ?? ""
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
                    const answer = answeredEntry.modelData.answer ?? {};
                    const labels = (answer.selected ?? []).join(", ");
                    const own = answer.text ?? "";
                    if (labels && own) {
                        return "✓ " + labels + " — " + own;
                    }
                    return "✓ " + (labels || own);
                }
                wrapMode: Text.WordWrap
                color: PhoenixTheme.secondaryTextColor
                font.pixelSize: Math.max(
                    9, Math.round(Qt.application.font.pixelSize * 0.85))
            }
        }
    }

    // The current question, or the waiting gap.
    PC3.BusyIndicator {
        Layout.alignment: Qt.AlignHCenter
        Layout.topMargin: Kirigami.Units.largeSpacing
        visible: wizardView.waiting
        running: wizardView.waiting
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.largeSpacing
        visible: !wizardView.waiting

        Kirigami.Heading {
            level: 5
            visible: text.length > 0
            color: PhoenixTheme.secondaryTextColor

            text: wizardView.question.header ?? ""
        }

        Kirigami.SelectableLabel {
            objectName: "wizardQuestionLabel"

            Layout.fillWidth: true

            text: wizardView.question.question ?? ""
            textFormat: TextEdit.PlainText
            wrapMode: Text.WordWrap
            font.weight: Font.DemiBold
            color: PhoenixTheme.headingTextColor
        }

        QuestionInputs {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing

            question: wizardView.question
            showSkip: true

            onSubmitted: (selected, text) =>
                wizardView.submitted(selected, text)
        }

        Text {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing

            text: i18n("The agent is waiting for this answer before it continues.")
            font.pixelSize: Math.max(
                9, Math.round(Qt.application.font.pixelSize * 0.8))
            color: PhoenixTheme.secondaryTextColor
        }
    }
}

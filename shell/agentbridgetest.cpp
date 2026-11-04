// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

//
// The answer-submission half of the agent bridge, checked without a
// daemon: what submitAnswers() accepts outright, what it refuses before
// anything is sent, and what each refusal leaves behind.
//
// The form is put in place exactly the way the daemon's QuestionAsked
// signal would put it: onQuestionAsked is the bridge's only setter of
// the pending form, and being a slot the meta-object reaches it.
//
// The wrong-count refusal is the wizard's usability bug in miniature:
// a single text answer can never satisfy a form of several questions
// (the daemon refuses the payload — "N questions were asked, M answers
// arrived"), and a bridge that sends it anyway consumes the caller's
// text, clears the form the wizard is walking and brings it back a
// round-trip later — the overlay closes and reopens, the answers are
// gone, and the run stays blocked. The refusal must come before any of
// that, with the form left exactly as it was.
//
//   agentbridgetest
//
// Exits non-zero when a check fails. (Plain printf, not qInfo: the
// categorized logging prints nothing in this environment.)

#include "agentbridge.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

// A wizard form of two questions, the shape QuestionAsked carries.
const char *const kWizardForm =
    R"({"goal": "backup setup", "questions": [)"
    R"({"question": "Which disk?", "options": [{"label": "/dev/sda"}, {"label": "/dev/sdb"}]},)"
    R"({"question": "Encrypt it?", "options": [{"label": "Yes"}, {"label": "No"}]}]})";

// A form of one question, the inline card's shape.
const char *const kCardForm =
    R"({"questions": [{"question": "Which disk?", "options": [{"label": "/dev/sda"},)"
    R"({"label": "/dev/sdb"}]}]})";

// The input bar's single text answer, in the user's own words.
QVariantList ownWords(const char *text)
{
    return QVariantList{QVariantMap{{QStringLiteral("text"), QString::fromLatin1(text)}}};
}

// One answer per question of the wizard form: the first question's
// first option, the second question's second option.
QVariantList wizardAnswers()
{
    QVariantList answers;
    answers << QVariantMap{{QStringLiteral("selected"), QVariantList{0}}};
    answers << QVariantMap{{QStringLiteral("selected"), QVariantList{1}}};
    return answers;
}

// Puts \a json in place as the pending form, as the daemon would
// (QuestionAsked names it by \a askId).
void askForm(AgentBridge &bridge, const char *askId, const char *json)
{
    // The arguments are hoisted: QStringLiteral does not survive being
    // nested inside Q_ARG's macro expansion, and the ids and payload
    // are runtime strings, so they take the runtime constructor.
    const QString runId = QStringLiteral("run_test");
    const QString id = QString::fromLatin1(askId);
    const QString payload = QString::fromLatin1(json);
    const bool invoked = QMetaObject::invokeMethod(&bridge, "onQuestionAsked", Qt::DirectConnection,
                                                   Q_ARG(QString, runId), Q_ARG(QString, id),
                                                   Q_ARG(QString, payload));
    check(invoked, "the form reached the bridge");
    check(bridge.pendingForm().value(QStringLiteral("askId")).toString() == id,
          "the form is pending under its ask id");
}

// Ends the pending form the way the daemon's QuestionAnswered would
// (answered elsewhere, timed out or cancelled).
void formEnded(AgentBridge &bridge, const char *askId)
{
    const QString runId = QStringLiteral("run_test");
    const QString id = QString::fromLatin1(askId);
    const QString status = QStringLiteral("timeout");
    const QString answerJson = QStringLiteral("{}");
    const bool invoked = QMetaObject::invokeMethod(&bridge, "onQuestionAnswered",
                                                   Qt::DirectConnection, Q_ARG(QString, runId),
                                                   Q_ARG(QString, id), Q_ARG(QString, status),
                                                   Q_ARG(QString, answerJson));
    check(invoked, "the form's end reached the bridge");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ---- Nothing pending: no answer is sent ------------------------------
    {
        AgentBridge bridge;

        check(bridge.pendingForm().isEmpty(), "no form is pending at the start");
        check(!bridge.submitAnswers(ownWords("no")),
              "no answer is sent without a pending form");
    }

    // ---- One text answer for a form of two: refused, form intact ---------
    {
        AgentBridge bridge;
        askForm(bridge, "ask_wizard", kWizardForm);

        const QVariantMap form = bridge.pendingForm();

        // The input bar's single text answer cannot serve the wizard's
        // two questions; the bridge says no before anything is sent.
        check(!bridge.submitAnswers(ownWords("/dev/sdb")),
              "a single text answer cannot serve a wizard");

        // The refusal costs nothing: the form is still pending, exactly
        // as it was — the wizard it drives neither closed nor reopened.
        check(bridge.pendingForm() == form,
              "the refusal leaves the form exactly as it was");
    }

    // ---- One answer per question: sent, the form cleared at once ---------
    {
        AgentBridge bridge;
        askForm(bridge, "ask_wizard", kWizardForm);

        check(bridge.submitAnswers(wizardAnswers()),
              "an answer per question is sent");

        // The optimistic clear: the form is gone the moment the answers
        // leave (the daemon's reply — accept or refusal — comes later,
        // and a refusal restores it).
        check(bridge.pendingForm().isEmpty(),
              "the form clears the moment the answers leave");
    }

    // ---- The inline card: one question, one text answer ------------------
    {
        AgentBridge bridge;
        askForm(bridge, "ask_card", kCardForm);

        // A form of one takes the input bar's single text answer.
        check(bridge.submitAnswers(ownWords("/dev/sdb")),
              "a form of one takes the single text answer");
        check(bridge.pendingForm().isEmpty(), "the card's form clears too");
    }

    // ---- A form that ended while its reply was in flight stays gone ------
    {
        AgentBridge bridge;
        askForm(bridge, "ask_wizard", kWizardForm);

        check(bridge.submitAnswers(wizardAnswers()), "the answers are on their way");
        check(bridge.pendingForm().isEmpty(), "the form was optimistically cleared");

        // The ask ends while the reply is on its way — answered
        // elsewhere, timed out, cancelled. The restore fallback is
        // dropped, so the eventual refusal resurrects nothing: the run
        // has moved on, and a dead form must not reopen its wizard on
        // top of it.
        formEnded(bridge, "ask_wizard");
        check(bridge.pendingForm().isEmpty(),
              "a form that ended while its reply was in flight is not restored");
    }

    if (failures == 0) {
        printf("all checks passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

// The trail of a run — the steps the results window shows while the
// agent works — checked without a window: the order of the steps, the
// grouping of the tool calls, the answer's own place and the steps that
// are still running.
//
//   conversationmodeltest
//
// Exits non-zero when a check fails. (Plain printf, not qInfo: the
// categorized logging prints nothing in this environment.)

#include "conversationmodel.h"

#include <QCoreApplication>
#include <QVariantMap>

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

QVariantList trail(ConversationModel &model)
{
    return model.data(model.index(0), ConversationModel::StepsRole).toList();
}

QString kind(const QVariantList &steps, int index)
{
    return steps.at(index).toMap().value(QStringLiteral("kind")).toString();
}

QString answer(ConversationModel &model)
{
    return model.data(model.index(0), ConversationModel::ResultRole).toString();
}

bool running(ConversationModel &model)
{
    return model.data(model.index(0), ConversationModel::RunningRole).toBool();
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ---- A fresh exchange is running: the pane's spinner turns on it ---
    //
    // The results pane opens the moment the daemon picks the prompt up,
    // before the model has said anything: the exchange is already there,
    // marked as running, so the window comes up with the prompt in it and
    // the agent's spinner turning while the answer is on its way.
    {
        ConversationModel model;
        model.beginExchange(QStringLiteral("what is the weather"),
                            QStringLiteral("michelle"));
        check(running(model),
              "a fresh exchange runs: the pane opens with a spinner on it");

        // The model's first words: still the same running exchange.
        model.appendResult(QStringLiteral("It is sunny"));
        check(running(model),
              "the exchange still runs while the answer streams in");

        // The run is over: it stops.
        model.finishExchange();
        check(!running(model),
              "the exchange stops running when the run is over");
    }

    // ---- A run that thinks, says something, calls a tool and answers ---
    {
        ConversationModel model;
        model.beginExchange(QStringLiteral("make it brighter"),
                            QStringLiteral("michelle"));
        model.appendThinking(QStringLiteral("The panel uses paneFill."));
        model.appendResult(QStringLiteral("Let me look at the overlay."));
        model.demoteResult();
        model.beginToolCall(QStringLiteral("read"), QStringLiteral("head"));
        model.endToolCall(QStringLiteral("success"), QStringLiteral("tail"));
        model.appendThinking(QStringLiteral("Now I know where it is."));
        model.appendResult(QStringLiteral("Done — the overlay is darker now."));
        model.setResult(QStringLiteral("Done — the overlay is darker now."));
        model.finishExchange();

        const QVariantList steps = trail(model);
        check(steps.size() == 4, "the trail holds four steps");
        check(kind(steps, 0) == QStringLiteral("thought"),
              "the notes come first");
        check(kind(steps, 1) == QStringLiteral("text"),
              "the preamble comes next, as words of its own");
        check(kind(steps, 2) == QStringLiteral("tool"),
              "the tool call follows the preamble");
        check(kind(steps, 3) == QStringLiteral("thought"),
              "the second stretch of notes follows the tool");
        check(answer(model) == QStringLiteral("Done — the overlay is darker now."),
              "the answer is the words after the last tool call");
        check(steps.at(2).toMap().value(QStringLiteral("tools")).toStringList()
                  == QStringList{QStringLiteral("read")},
              "the tool step names the tool it called");
        check(steps.at(2).toMap().value(QStringLiteral("count")).toInt() == 1,
              "one call is one step");
        check(!steps.at(2).toMap().value(QStringLiteral("running")).toBool(),
              "the finished call does not run any more");
    }

    // ---- A stretch of tool calls is one step ---------------------------
    {
        ConversationModel model;
        model.beginExchange(QStringLiteral("clean up"), QStringLiteral("michelle"));
        model.beginToolCall(QStringLiteral("edit"), QStringLiteral("head"));
        model.endToolCall(QStringLiteral("success"), QStringLiteral("tail"));
        model.beginToolCall(QStringLiteral("edit"), QStringLiteral("head"));
        model.endToolCall(QStringLiteral("success"), QStringLiteral("tail"));
        model.beginToolCall(QStringLiteral("bash"), QStringLiteral("head"));
        model.endToolCall(QStringLiteral("error"), QStringLiteral("tail"));

        const QVariantList steps = trail(model);
        check(steps.size() == 1, "three calls in a row are one step");
        check(steps.at(0).toMap().value(QStringLiteral("count")).toInt() == 3,
              "the step counts every call");
        check(steps.at(0).toMap().value(QStringLiteral("tools")).toStringList()
                  == QStringList({QStringLiteral("edit"), QStringLiteral("bash")}),
              "the step names the tools it called, each of them once");
        check(steps.at(0).toMap().value(QStringLiteral("status")).toString()
                  == QStringLiteral("error"),
              "the step carries the failure");
        check(!steps.at(0).toMap().value(QStringLiteral("running")).toBool(),
              "the step is done");
    }

    // ---- A transfer says which way its bytes move ----------------------
    {
        ConversationModel model;
        model.beginExchange(QStringLiteral("fetch and file it"),
                            QStringLiteral("michelle"));
        // A download, an upload of what arrived, and a plain command
        // around them: one step, its tools and their directions kept.
        model.beginToolCall(QStringLiteral("transfer"), QStringLiteral("head"),
                            QStringLiteral("download"));
        model.endToolCall(QStringLiteral("success"), QStringLiteral("tail"));
        model.beginToolCall(QStringLiteral("transfer"), QStringLiteral("head"),
                            QStringLiteral("upload"));
        model.endToolCall(QStringLiteral("success"), QStringLiteral("tail"));
        model.beginToolCall(QStringLiteral("bash"), QStringLiteral("head"));
        model.endToolCall(QStringLiteral("success"), QStringLiteral("tail"));

        const QVariantList steps = trail(model);
        check(steps.size() == 1, "the three calls are one step");
        const QVariantMap step = steps.at(0).toMap();
        check(step.value(QStringLiteral("tools")).toStringList()
                  == QStringList({QStringLiteral("transfer"),
                                  QStringLiteral("transfer"),
                                  QStringLiteral("bash")}),
              "the transfer tool is listed once per direction");
        check(step.value(QStringLiteral("directions")).toStringList()
                  == QStringList({QStringLiteral("download"),
                                  QStringLiteral("upload"), QString()}),
              "each tool carries its direction, blanks for the rest");

        // Two downloads of one stretch name the tool once: the direction
        // is what the pane needs, not the repetition.
        ConversationModel twice;
        twice.beginExchange(QStringLiteral("fetch two"), QStringLiteral("michelle"));
        twice.beginToolCall(QStringLiteral("transfer"), QStringLiteral("head"),
                            QStringLiteral("download"));
        twice.endToolCall(QStringLiteral("success"), QStringLiteral("tail"));
        twice.beginToolCall(QStringLiteral("transfer"), QStringLiteral("head"),
                            QStringLiteral("download"));
        twice.endToolCall(QStringLiteral("success"), QStringLiteral("tail"));
        const QVariantList two = trail(twice);
        check(two.at(0).toMap().value(QStringLiteral("tools")).toStringList()
                  == QStringList({QStringLiteral("transfer")}),
              "two downloads of a stretch name the tool once");
        check(two.at(0).toMap().value(QStringLiteral("count")).toInt() == 2,
              "but the step counts them both");
    }

    // ---- A call that is still running says so -------------------------
    {
        ConversationModel model;
        model.beginExchange(QStringLiteral("look around"), QStringLiteral("michelle"));
        model.beginToolCall(QStringLiteral("bash"), QStringLiteral("head"));

        const QVariantList steps = trail(model);
        check(steps.size() == 1, "the running call is in the trail");
        check(steps.at(0).toMap().value(QStringLiteral("running")).toBool(),
              "the running call says it runs");
        check(steps.at(0).toMap().value(QStringLiteral("endedAt")).toLongLong() == 0,
              "the running call has no end yet");
        check(steps.at(0).toMap().value(QStringLiteral("startedAt")).toLongLong() > 0,
              "the running call says when it began");
    }

    // ---- The answer is never in the trail ------------------------------
    {
        ConversationModel model;
        model.beginExchange(QStringLiteral("hello"), QStringLiteral("michelle"));
        model.appendResult(QStringLiteral("Hello there."));
        check(trail(model).isEmpty(), "a plain answer leaves the trail empty");
        check(answer(model) == QStringLiteral("Hello there."),
              "the answer is the answer");
    }

    // ---- The finished text wins over the streamed one ------------------
    {
        ConversationModel model;
        model.beginExchange(QStringLiteral("hello"), QStringLiteral("michelle"));
        model.appendResult(QStringLiteral("Hello"));
        model.setResult(QStringLiteral("Hello there."));
        check(trail(model).isEmpty(), "the answer leaves the trail");
        check(answer(model) == QStringLiteral("Hello there."),
              "the finished text is the answer");
    }

    // ---- A run that ran out of turns keeps its words where they were --
    {
        ConversationModel model;
        model.beginExchange(QStringLiteral("go on"), QStringLiteral("michelle"));
        model.appendResult(QStringLiteral("Let me check that."));
        model.demoteResult();
        model.beginToolCall(QStringLiteral("bash"), QStringLiteral("head"));
        model.endToolCall(QStringLiteral("success"), QStringLiteral("tail"));
        model.setResult(QStringLiteral("Let me check that."));

        const QVariantList steps = trail(model);
        check(steps.size() == 1, "the tool call is the trail");
        check(kind(steps, 0) == QStringLiteral("tool"), "the tool call is the trail");
        check(answer(model) == QStringLiteral("Let me check that."),
              "the words of the last turn are the answer");
    }

    // ---- The read-aloud contract ----------------------------------------
    // The "Read aloud" button speaks the model's answer role, and the
    // daemon speaks the run's final text: neither may ever pick up a
    // working note. So the answer role must hold the answer alone, even
    // while the run is still going on.
    {
        ConversationModel model;
        model.beginExchange(QStringLiteral("set SELinux up"),
                            QStringLiteral("michelle"));
        model.appendThinking(QStringLiteral("The mode is enforcing."));
        model.appendResult(QStringLiteral("Let me check the policy."));
        check(answer(model) == QStringLiteral("Let me check the policy."),
              "the answer holds the words of the turn being written");
        model.demoteResult();
        model.beginToolCall(QStringLiteral("bash"), QStringLiteral("head"));
        model.endToolCall(QStringLiteral("success"), QStringLiteral("tail"));
        check(answer(model).isEmpty(),
              "a working note is never the answer once the tool call came");
        model.appendResult(QStringLiteral("The policy is permissive."));
        check(answer(model) == QStringLiteral("The policy is permissive."),
              "the answer is the words after the last tool call");
        check(!answer(model).contains(QStringLiteral("Let me check")),
              "no working note is ever part of the answer");
    }

    // ---- The exchanges are numbered, and the fork keeps a prefix -------
    //
    // The fork is taken at one of the user's queries, so the model has
    // to say which row carries which query: the first thing the user
    // asked is number 1, the next one 2. A run begun elsewhere (the
    // REPL, another widget) carries no query and so no number.
    //
    // The fork then keeps the exchanges up to that row and drops the
    // rest: what came after stayed in the session the fork was taken
    // from.
    {
        ConversationModel model;
        model.beginExchange(QStringLiteral("first question"), QStringLiteral("michelle"));
        model.appendResult(QStringLiteral("first answer"));
        model.finishExchange();
        // A run begun elsewhere: no query, no number.
        model.beginExchange(QString(), QString());
        model.appendResult(QStringLiteral("someone else's answer"));
        model.finishExchange();
        model.beginExchange(QStringLiteral("second question"), QStringLiteral("michelle"));
        model.appendResult(QStringLiteral("second answer"));
        model.finishExchange();

        const auto number = [&model](int row) {
            return model.data(model.index(row), ConversationModel::ExchangeRole).toInt();
        };
        check(number(0) == 1, "the first query is number 1");
        check(number(1) == 0, "a run without a query carries no number");
        check(number(2) == 2, "the second query is number 2");

        check(model.rowOfExchange(1) == 0, "the first query sits in the first row");
        check(model.rowOfExchange(2) == 2, "the second query sits in the third row");
        check(model.rowOfExchange(3) == -1, "a number the history does not hold is -1");
        check(model.rowOfExchange(0) == -1, "no query is no row");

        // The fork at the first query: the exchange and the run of
        // somebody else go, the rest stays as it was.
        model.truncate(model.rowOfExchange(1) + 1);
        check(model.rowCount() == 1, "the fork keeps the exchange it was taken at");
        check(model.data(model.index(0), ConversationModel::PromptRole).toString()
                  == QStringLiteral("first question"),
              "the exchange the fork was taken at is untouched");
        check(model.data(model.index(0), ConversationModel::ResultRole).toString()
                  == QStringLiteral("first answer"),
              "its answer is untouched too");
        check(number(0) == 1, "the exchange keeps its number");

        // A count beyond the history changes nothing.
        model.truncate(99);
        check(model.rowCount() == 1, "a count beyond the history changes nothing");
        // A negative one empties it.
        model.truncate(-1);
        check(model.rowCount() == 0, "a negative count empties the history");
    }

    if (failures > 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}

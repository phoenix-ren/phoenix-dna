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
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

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

    if (failures > 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}

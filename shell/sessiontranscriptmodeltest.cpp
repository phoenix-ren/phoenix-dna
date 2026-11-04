// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

// The transcript of a session — the queries and responses the session
// search shows on the right — checked without a window: the empty turns
// are dropped, the rows are handed over a chunk at a time, and the
// counters the bar at the top of the window reads are true.
//
//   sessiontranscriptmodeltest
//
// Exits non-zero when a check fails. (Plain printf, not qInfo: the
// categorized logging prints nothing in this environment.)

#include "sessiontranscriptmodel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
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

// Lets the model hand over its chunks: the timer fires every 16 ms, and
// the event loop has the turns it needs.
void waitForChunks()
{
    QEventLoop loop;
    QTimer::singleShot(80, &loop, &QEventLoop::quit);
    loop.exec();
}

QVariantMap message(const QString &role, const QString &content)
{
    return {
        {QStringLiteral("role"), role},
        {QStringLiteral("content"), content},
        {QStringLiteral("ts"), QStringLiteral("2026-11-04 05:00:00")},
    };
}

// The rows that have reached the view so far.
int handed(const SessionTranscriptModel &model)
{
    return model.rowCount();
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ---- A transcript with a tool call in it ------------------------------
    // The tool call is stored as an assistant turn without content: it is
    // neither a query nor an answer, and it must not take a row.
    {
        SessionTranscriptModel model;
        int loadingChanges = 0;
        QObject::connect(&model, &SessionTranscriptModel::loadingChanged,
                         [&loadingChanges]() { ++loadingChanges; });

        model.load({
            message(QStringLiteral("user"), QStringLiteral("Fix the audio")),
            message(QStringLiteral("assistant"), QString()),
            message(QStringLiteral("assistant"),
                    QStringLiteral("The mixer was muted.")),
            message(QStringLiteral("user"), QStringLiteral("   ")),
            message(QStringLiteral("assistant"), QStringLiteral("Unmuted.")),
        });

        check(model.totalCount() == 3,
              "the empty turns are dropped");
        check(model.isLoading(),
              "the transcript is still on its way");
        check(loadingChanges > 0,
              "the loading state is announced");

        waitForChunks();
        check(handed(model) == 3,
              "every row reaches the view");
        check(!model.isLoading(),
              "the handover is over");
        check(model.rowCount() == 3,
              "the view holds three rows");
    }

    // ---- The rows carry the message --------------------------------------
    {
        SessionTranscriptModel model;
        model.load({
            message(QStringLiteral("user"), QStringLiteral("Fix the audio")),
            message(QStringLiteral("assistant"),
                    QStringLiteral("The mixer was muted.")),
        });
        waitForChunks();

        check(model.data(model.index(0), SessionTranscriptModel::RoleRole)
                  .toString() == QStringLiteral("user"),
              "the first row is the query");
        check(model.data(model.index(1), SessionTranscriptModel::RoleContent)
                  .toString() == QStringLiteral("The mixer was muted."),
              "the second row carries the answer");
        check(model.data(model.index(1), SessionTranscriptModel::RoleTs)
                  .toString().length() > 0,
              "the rows carry their timestamp");
    }

    // ---- An empty transcript ----------------------------------------------
    {
        SessionTranscriptModel model;
        model.load({});
        check(model.totalCount() == 0, "an empty transcript holds nothing");
        check(!model.isLoading(), "an empty transcript is not loading");
    }

    // ---- Another session takes the place ----------------------------------
    {
        SessionTranscriptModel model;
        model.load({message(QStringLiteral("user"), QStringLiteral("One"))});
        waitForChunks();
        check(model.rowCount() == 1, "the first transcript is shown");

        model.clear();
        check(model.rowCount() == 0, "the old rows leave the view");
        check(model.totalCount() == 0, "the old count leaves with them");
        check(!model.isLoading(), "the pane is idle again");
    }

    if (failures > 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}

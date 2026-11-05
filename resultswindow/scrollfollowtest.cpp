// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include <QAbstractListModel>
#include <QFile>
#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTimer>
#include <QUrl>

#include <cstdio>

/*
 * The results window's scroll-follow behaviour, run headless.
 *
 * The window under test is the real one, loaded from the results-window
 * QML module (see ScrollFollowTest.qml) — not a copy of it — so what the
 * test measures is what the user sees.
 *
 *   scrollfollowtest
 *
 * The module is found through QML_IMPORT_PATH, which the build sets to
 * the build tree's bin directory.
 */

namespace {

// The test's own messages (console.log) go to the Qt logging system;
// they are printed here so that a failure says what it was.
void messageHandler(QtMsgType type, const QMessageLogContext &context,
                    const QString &message)
{
    if (type == QtInfoMsg || type == QtDebugMsg) {
        std::fprintf(stderr, "%s\n", qPrintable(message));
    } else {
        std::fprintf(stderr, "%s (%s:%d)\n", qPrintable(message),
                     context.file ? context.file : "?", context.line);
    }
}

} // namespace

/*!
 * A conversation of one exchange, in the roles the window's Repeater
 * asks for: prompt, sender, result, running, steps.
 *
 * The window's delegate reads them as *required properties*, which a
 * plain QML array cannot supply — so the test drives the window with a
 * real model, the way the agent bridge drives it in the app.
 */
class TestConversation : public QAbstractListModel
{
    Q_OBJECT

Q_SIGNALS:
    // The test is done.
    void finished();

public:
    enum Roles
    {
        PromptRole = Qt::UserRole + 1,
        SenderRole,
        ResultRole,
        RunningRole,
        StepsRole
    };

    TestConversation()
    {
        Row row;
        row.prompt = QStringLiteral("a question");
        row.sender = QStringLiteral("root");
        append(row);
    }

    int rowCount(const QModelIndex &parent) const override
    {
        return parent.isValid() ? 0 : m_rows.size();
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size()) {
            return {};
        }
        const Row &row = m_rows.at(index.row());
        switch (role) {
        case PromptRole:
            return row.prompt;
        case SenderRole:
            return row.sender;
        case ResultRole:
            return row.result;
        case RunningRole:
            return row.running;
        case StepsRole:
            return row.steps;
        default:
            return {};
        }
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return {{PromptRole, "prompt"},
                {SenderRole, "sender"},
                {ResultRole, "result"},
                {RunningRole, "running"},
                {StepsRole, "steps"}};
    }

    // Replaces the answer of the exchange — the "run" streaming.
    Q_INVOKABLE void setResult(const QString &text)
    {
        if (m_rows.isEmpty()) {
            return;
        }
        m_rows[0].result = text;
        const QModelIndex idx = index(0, 0);
        Q_EMIT dataChanged(idx, idx);
    }

    // Replaces the trail of the exchange — the steps the window renders,
    // in the shape ConversationModel::stepsFor() gives them (kind, body,
    // count, tools, directions, status, running, startedAt, endedAt).
    Q_INVOKABLE void setSteps(const QVariantList &steps)
    {
        if (m_rows.isEmpty()) {
            return;
        }
        m_rows[0].steps = steps;
        const QModelIndex idx = index(0, 0);
        Q_EMIT dataChanged(idx, idx);
    }

    // The test is done.
    Q_INVOKABLE void finish()
    {
        Q_EMIT finished();
    }

private:
    struct Row
    {
        QString prompt;
        QString sender;
        QString result;
        bool running = true;
        QVariantList steps;
    };

    void append(const Row &row)
    {
        beginInsertRows(QModelIndex(), m_rows.size(), m_rows.size());
        m_rows.append(row);
        endInsertRows();
    }

    QList<Row> m_rows;
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");

    QGuiApplication app(argc, argv);
    qInstallMessageHandler(messageHandler);

    // The test itself is a QML file, loaded from the source tree so that
    // it runs against the same build of the window the app runs.
    const QString testPath = QString::fromLatin1(PHOENIX_SCROLLFOLLOW_TEST);

    {
        QFile probe(testPath);
        std::fprintf(stderr, "test file %s: %s\n",
                     qPrintable(testPath),
                     probe.exists() ? "found" : "MISSING");
        if (!probe.exists()) {
            return 2;
        }
    }

    TestConversation conversation;

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("testConversation"), &conversation);

    QQmlComponent test(&engine, QUrl::fromLocalFile(testPath));
    if (test.isError()) {
        for (const QQmlError &error : test.errors()) {
            std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
        }
        return 2;
    }

    QObject *instance = test.create();
    if (!instance) {
        for (const QQmlError &error : test.errors()) {
            std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
        }
        return 2;
    }

    int failures = 0;

    // The test tells the harness when it is done: the model — which the
    // test drives — carries the signal.
    QObject::connect(&conversation, &TestConversation::finished,
                     &app, []() { QCoreApplication::exit(0); });

    // A watchdog: a test that never finishes is a failure, not a hang.
    QTimer::singleShot(30000, &app, SLOT(quit()));
    app.exec();

    failures = instance->property("failures").toInt();
    const QString failure = instance->property("failure").toString();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d check(s) failed. First: %s\n",
                     failures, qPrintable(failure));
        return 1;
    }

    std::fprintf(stderr, "\nall checks passed\n");
    return 0;
}

#include "scrollfollowtest.moc"

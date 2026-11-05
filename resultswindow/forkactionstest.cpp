// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

//
// The results window's fork affordance, run headless.
//
// The window under test is the real one, loaded from the results-window
// QML module (see ForkActionTest.qml) — not a copy of it — so what the
// test measures is what the user sees.
//
// A fork is taken at one of the user's queries: every query of the
// conversation carries the button that asks for it, and the button asks
// for the right one — the exchange it stands next to, and not another.
// A run that carries no query (one begun elsewhere) has nothing to
// fork at, and so no button.
//
//   forkactionstest
//
// Exits non-zero when a check fails. (Plain printf, not qInfo: the
// categorized logging prints nothing in this environment.)

#include <QAbstractListModel>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <cstdio>

namespace
{

// The test's own messages (console.log) go to the Qt logging system;
// they are printed here so that a failure says what it was.
void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
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
 * The test's way of saying that it is done.
 *
 * The QML engine the test runs in is not the application's own, so it
 * cannot end the run by itself: the test asks this object, and the
 * harness ends the event loop when it does.
 */
class TestControl : public QObject
{
    Q_OBJECT

public:
    /*! \brief The test is done — the run ends with \a code. */
    Q_INVOKABLE void finish(int code) { Q_EMIT finished(code); }

Q_SIGNALS:
    void finished(int code);
};

/*!
 * A conversation of three exchanges, in the roles the window's Repeater
 * asks for: prompt, sender, result, running, steps, exchangeNumber.
 *
 * The window's delegate reads them as *required properties*, which a
 * plain QML array cannot supply — so the test drives the window with a
 * real model, the way the agent bridge drives it in the app.
 *
 * The third exchange carries no query: a run begun elsewhere (the REPL,
 * another widget) has no query of the user's to fork at.
 */
class TestConversation : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles
    {
        PromptRole = Qt::UserRole + 1,
        SenderRole,
        ResultRole,
        RunningRole,
        StepsRole,
        ExchangeRole
    };

    TestConversation()
    {
        Row first;
        first.prompt = QStringLiteral("a question");
        first.sender = QStringLiteral("michelle");
        first.result = QStringLiteral("an answer");
        append(first);

        Row second;
        second.prompt = QStringLiteral("another question");
        second.sender = QStringLiteral("michelle");
        second.result = QStringLiteral("another answer");
        append(second);

        Row third;
        third.prompt = QString();
        third.sender = QString();
        third.result = QStringLiteral("someone else's answer");
        append(third);
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
        case ExchangeRole:
            // The exchange's number: the user's queries are counted, so
            // the first thing they asked is 1. A run without a query
            // carries no number.
            return row.prompt.isEmpty() ? 0 : index.row() + 1;
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
                {StepsRole, "steps"},
                {ExchangeRole, "exchangeNumber"}};
    }

private:
    struct Row
    {
        QString prompt;
        QString sender;
        QString result;
        bool running = false;
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
    const QString testPath = QString::fromLatin1(PHOENIX_FORK_ACTION_TEST);

    {
        QFile probe(testPath);
        std::fprintf(stderr, "test file %s: %s\n", qPrintable(testPath),
                     probe.exists() ? "found" : "MISSING");
        if (!probe.exists()) {
            return 2;
        }
    }

    TestConversation conversation;
    TestControl control;

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("testConversation"), &conversation);
    engine.rootContext()->setContextProperty(QStringLiteral("testControl"), &control);

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

    // The test tells the harness when it is done: the control — which
    // the test drives — carries the signal.
    QObject::connect(&control, &TestControl::finished, &app,
                     [](int code) { QCoreApplication::exit(code); });

    // A watchdog: a test that never finishes is a failure, not a hang.
    QTimer::singleShot(30000, &app, SLOT(quit()));
    const int code = app.exec();

    if (code != 0) {
        std::fprintf(stderr, "\n%d check(s) failed. First: %s\n",
                     instance->property("failures").toInt(),
                     qPrintable(instance->property("failure").toString()));
        return 1;
    }

    std::fprintf(stderr, "\nall checks passed\n");
    return 0;
}

#include "forkactionstest.moc"

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

//
// The session search's session actions, run headless.
//
// The overlay under test is the real one, loaded from the session-search
// QML module (see SessionActionsTest.qml) — not a copy of it — so what
// the test measures is what the user sees.
//
// A session that is thrown away cannot be brought back, so the delete
// button only asks: the confirmation names the session, and only the
// confirmation hands the session over to be deleted. A session can also
// be named the user's way, and the field for it comes up with the name
// the session has. The test walks both round trips.
//
//   sessionactionstest
//
// Exits non-zero when a check fails. (Plain printf, not qInfo: the
// categorized logging prints nothing in this environment.)

#include <QFile>
#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTimer>
#include <QUrl>

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
        std::fprintf(stderr, "%s (%s:%d)\n", qPrintable(message), context.file ? context.file : "?",
                     context.line);
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

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_BACKEND", "software");

    QGuiApplication app(argc, argv);
    qInstallMessageHandler(messageHandler);

    // The test itself is a QML file, loaded from the source tree so that
    // it runs against the same build of the overlay the app runs.
    const QString testPath = QString::fromLatin1(PHOENIX_SESSION_ACTIONS_TEST);

    {
        QFile probe(testPath);
        std::fprintf(stderr, "test file %s: %s\n", qPrintable(testPath),
                     probe.exists() ? "found" : "MISSING");
        if (!probe.exists()) {
            return 2;
        }
    }

    QQmlEngine engine;
    TestControl control;
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

#include "sessionactionstest.moc"

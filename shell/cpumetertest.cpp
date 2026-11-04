// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

// The CPU meter of a running bash tool call — the /proc sampling and its
// peak-meter ballistics — checked against real processes:
//
//   * a busy command is tracked, its level rises and its peak holds;
//   * the meter stops by itself when the whole command dies without a
//     ToolProcessFinished (a daemon crash, say);
//   * a leader killed while its children keep running (an orphaned
//     pipeline) keeps being metered through the process-group scan;
//   * release() parks the meter.
//
//   cpumetertest
//
// Exits non-zero when a check fails. (Plain printf, not qInfo: the
// categorized logging prints nothing in this environment.)

#include "cpumeter.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <csignal>
#include <unistd.h>

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

// Runs the event loop for roughly \a ms, so the meter's QTimer fires.
void spin(int ms)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
}

// Kills whatever of \a process still runs, as the daemon kills a
// command: the whole process group.
void killGroup(qint64 pid)
{
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
}

// A process group leader (as the daemon's commands are: spawned with
// start_new_session) that reports its own pid through \a pidFile and
// then keeps a busy child running next to itself.
QString leaderScript(const QString &pidFile)
{
    return QStringLiteral(
               "setsid bash -c 'echo $$ > %1; bash -c \"while :; do :; done\" & sleep 300'")
        .arg(pidFile);
}

// Waits for the leader's pid to appear in \a pidFile; 0 on timeout.
qint64 waitForPid(const QString &pidFile)
{
    for (int i = 0; i < 100; ++i) {
        QFile file(pidFile);
        if (file.open(QIODevice::ReadOnly)) {
            const qint64 pid = file.readAll().trimmed().toLongLong();
            if (pid > 0) {
                return pid;
            }
        }
        QThread::msleep(50);
    }
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const qreal cores = qreal(qMax<qint64>(1, sysconf(_SC_NPROCESSORS_ONLN)));

    // ---- A busy command is metered, and dies unreported -------------
    {
        CpuMeter meter;
        QProcess command;
        command.start(QStringLiteral("bash"),
                      QStringList{QStringLiteral("-c"), QStringLiteral("while :; do :; done")});
        check(command.waitForStarted(5000), "the busy command started");
        spin(300); // let it accumulate some CPU before the first sample

        meter.track(int(command.processId()), QStringLiteral("while :; do :; done"));
        check(meter.isActive(), "tracking makes the meter active");
        check(meter.command() == QStringLiteral("while :; do :; done"),
              "the meter names the command it follows");

        spin(1500);
        check(meter.fraction() > 0.4 / cores, "a busy command lights the meter");
        check(meter.fraction() <= 1.0, "the level never exceeds the machine");
        check(meter.peak() >= meter.fraction(), "the peak holds above the level");

        // No release() here: the command dies without a report, and the
        // meter must notice and stop on its own.
        command.kill();
        command.waitForFinished(5000);
        spin(1500);
        check(!meter.isActive(), "a command that dies unreported stops the meter");
    }

    // ---- A dead leader with live children keeps being metered -------
    {
        QTemporaryDir dir;
        const QString pidFile = QFileInfo(dir.filePath(QStringLiteral("pid"))).absoluteFilePath();
        QProcess command;
        command.start(QStringLiteral("bash"),
                      QStringList{QStringLiteral("-c"),
                                  QStringLiteral("%1 & sleep 300").arg(leaderScript(pidFile))});
        check(command.waitForStarted(5000), "the leader command started");
        const qint64 leader = waitForPid(pidFile);
        check(leader > 0, "the leader reported its pid");

        CpuMeter meter;
        meter.track(int(leader), QStringLiteral("orphaned pipeline"));
        spin(1500);
        check(meter.isActive() && meter.fraction() > 0.4 / cores,
              "the leader's group is metered while it runs");

        // Kill only the leader: the busy child survives it, orphaned in
        // the same process group. The children files are of no help now
        // (their parent is gone) - only the group scan still sees it.
        kill(leader, SIGKILL);
        spin(1500);
        check(meter.isActive(), "the meter follows the group past its dead leader");
        check(meter.fraction() > 0.3 / cores, "the orphan's work still lights the meter");

        killGroup(leader); // as the daemon would: the whole group
        command.kill();
        command.waitForFinished(5000);
        spin(1500);
        check(!meter.isActive(), "the meter stops once the whole group is gone");
    }

    // ---- release() parks the meter -----------------------------------
    {
        CpuMeter meter;
        QProcess command;
        command.start(QStringLiteral("bash"),
                      QStringList{QStringLiteral("-c"), QStringLiteral("while :; do :; done")});
        check(command.waitForStarted(5000), "the second busy command started");
        meter.track(int(command.processId()), QStringLiteral("while :; do :; done"));
        spin(800);
        check(meter.isActive(), "the meter is active before release");
        meter.release();
        check(!meter.isActive(), "release() deactivates the meter");
        command.kill();
        command.waitForFinished(5000);
    }

    return failures == 0 ? 0 : 1;
}

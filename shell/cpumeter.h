// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief The CPU level of the command a bash tool call is running.
 *
 * The agent daemon spawns every command in its own session, so the pid it
 * reports (ToolProcessStarted over D-Bus) is both the process and its
 * group leader. This class follows that process tree through /proc and
 * turns its CPU time into a level a meter can show — with the ballistics
 * of an audio peak meter: the level attacks quickly, falls with a
 * graceful exponential release, and a peak-hold marker lingers above it
 * before it, too, decays.
 *
 * Sampling is deliberately cheap. The common case walks only the
 * command's own process tree, seeded by the kernel's children files
 * (/proc/<pid>/task/<tid>/children) — a handful of stat reads every
 * 100 ms, and nothing at all while no command runs. Two cases need more
 * width: the children files are unreadable when a setuid sudo carries
 * root children (ptrace rules), and a group leader can die while its
 * children keep running (an orphaned pipeline). Both fall back to a
 * single /proc scan that collects everything in the command's process
 * group — /proc/<pid>/stat is world-readable, so root children are
 * metered just the same.
 *
 * The fraction is the command's share of the whole machine's capacity
 * (all cores), 0..1; \a cores restates it in cores ("×2.4"), which reads
 * better than a percentage on a many-core machine.
 *
 * Everything runs on the GUI thread: a QTimer drives the sampling (the
 * D-Bus layer marshals the daemon's events onto the main loop, and the
 * sampling itself is a few file reads).
 */
class CpuMeter : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

public:
    explicit CpuMeter(QObject *parent = nullptr);

    /*! \brief Whether a command is being metered right now. */
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

    /*! \brief The command being metered, as the model asked for it. */
    Q_PROPERTY(QString command READ command NOTIFY commandChanged)

    /*! \brief The command's CPU usage as a share of all cores (0..1),
     *  with peak-meter ballistics: quick attack, graceful release. */
    Q_PROPERTY(qreal fraction READ fraction NOTIFY fractionChanged)

    /*! \brief The recent peak of \a fraction: held briefly, then let
     *  down slowly, the way an audio meter's peak-hold LED behaves. */
    Q_PROPERTY(qreal peak READ peak NOTIFY peakChanged)

    /*! \brief \a fraction restated in cores ("how many cores busy"). */
    Q_PROPERTY(qreal cores READ cores NOTIFY fractionChanged)

    bool isActive() const { return m_active; }
    QString command() const { return m_command; }
    qreal fraction() const { return m_level; }
    qreal peak() const { return m_peak; }
    qreal cores() const { return m_level * m_coreCount; }

    /*! \brief Follows \a pid (a command's session leader) and meters the
     *  CPU usage of its process tree until release().
     *
     * A second track() while one command is metered simply switches to
     * the new one. If the whole process group dies without a
     * ToolProcessFinished — a daemon crash, say — the sampling notices
     * and stops by itself. */
    Q_INVOKABLE void track(int pid, const QString &command);

    /*! \brief Stops metering (ToolProcessFinished, or the run's end). */
    Q_INVOKABLE void release();

Q_SIGNALS:
    void activeChanged();
    void commandChanged();
    void fractionChanged();
    void peakChanged();

private:
    void sample();
    void setActive(bool active);

    struct Sampled
    {
        qint64 ticks = 0; //!< CPU ticks the command's processes used so far
        int members = 0; //!< how many of its processes were seen
    };

    // The command's processes, by the cheap tree walk when it can see
    // them and by the process-group scan once it cannot.
    Sampled collect();
    bool collectFromTree(qint64 *ticks, int *members) const;
    static Sampled collectFromGroup(int pgid);

private:
    bool m_active = false;
    QString m_command;
    int m_pid = 0;
    // Whether the children files could not be read and the sampling
    // switched to the process-group scan for the rest of this command.
    bool m_scanMode = false;
    // The previous sample's tick total (invalid until the first sample).
    qint64 m_lastTicks = 0;
    bool m_haveLast = false;
    // The meter's level and its held peak (see the ballistics in sample()).
    qreal m_level = 0.0;
    qreal m_peak = 0.0;
    qint64 m_peakAtMs = 0;
    // The machine the command runs on.
    qint64 m_coreCount = 1;
    qint64 m_clkTck = 100;
    QTimer m_sampleTimer;
    QElapsedTimer m_sinceSample;
    QElapsedTimer m_sincePeak;
};

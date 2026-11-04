// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "cpumeter.h"

#include <QDir>
#include <QFile>
#include <QList>
#include <QSet>

#include <cmath>
#include <unistd.h>

namespace
{

// The sampling cadence: often enough that a level reads live, rarely
// enough that reading /proc costs nothing next to what it measures.
// (The GUI animates between samples; the samples only set the targets.)
constexpr int kSampleIntervalMs = 100;

// The ballistics of an audio peak program meter: the level attacks
// almost at once, falls over a third of a second, and the peak-hold
// marker stands for a moment before it, too, is let down slowly.
constexpr qreal kAttackSeconds = 0.12;
constexpr qreal kReleaseSeconds = 0.35;
constexpr qint64 kPeakHoldMs = 800;
constexpr qreal kPeakDecaySeconds = 0.65;

// A closed set of processes cannot grow forever; this cap only guards
// against a pid-reuse cycle keeping the walk from converging.
constexpr int kMaxTreeProcesses = 4096;

// One /proc/<pid>/stat line, reduced to what the meter needs.
struct ProcStat
{
    bool ok = false;
    qint64 pgrp = 0;
    qint64 ticks = 0; //!< utime + stime, in clock ticks
};

ProcStat readStat(qint64 pid)
{
    ProcStat stat;
    QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) {
        return stat;
    }
    const QByteArray line = file.readAll();
    // comm is parenthesised and may contain spaces (and parens) itself;
    // the fields after the last ')' are fixed single-token fields.
    const int close = line.lastIndexOf(')');
    if (close < 0 || close + 2 >= line.size()) {
        return stat;
    }
    const QList<QByteArray> fields = line.mid(close + 2).split(' ');
    // fields[0] is stat field 3 (state), so pgrp (field 5) sits at [2],
    // and utime/stime (fields 14/15) at [11]/[12].
    if (fields.size() < 13) {
        return stat;
    }
    stat.pgrp = fields.at(2).toLongLong();
    stat.ticks = fields.at(11).toLongLong() + fields.at(12).toLongLong();
    stat.ok = true;
    return stat;
}

// The pids the kernel lists as children of pid's threads. Sets
// *readable to false when a children file exists but stays closed:
// that is the ptrace rules keeping a setuid sudo's family private.
QList<qint64> readChildren(qint64 pid, bool *readable)
{
    QList<qint64> children;
    const QDir taskDir(QStringLiteral("/proc/%1/task").arg(pid));
    const QStringList threads = taskDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &tid : threads) {
        QFile file(taskDir.filePath(tid + QStringLiteral("/children")));
        if (!file.open(QIODevice::ReadOnly)) {
            *readable = false;
            continue;
        }
        const QByteArray words = file.readAll();
        for (const QByteArray &word : words.split(' ')) {
            const qint64 child = word.toLongLong();
            if (child > 0) {
                children.append(child);
            }
        }
    }
    return children;
}

} // namespace

CpuMeter::CpuMeter(QObject *parent) : QObject(parent)
{
    m_sampleTimer.setInterval(kSampleIntervalMs);
    connect(&m_sampleTimer, &QTimer::timeout, this, &CpuMeter::sample);
}

void CpuMeter::track(int pid, const QString &command)
{
    m_pid = pid;
    if (m_command != command) {
        m_command = command;
        Q_EMIT commandChanged();
    }
    // A fresh measurement: the first sample only records the baseline
    // (nothing has been used yet), the level reads 0 until then.
    m_scanMode = false;
    m_haveLast = false;
    m_lastTicks = 0;
    m_level = 0.0;
    m_peak = 0.0;
    m_coreCount = qMax<qint64>(1, sysconf(_SC_NPROCESSORS_ONLN));
    m_clkTck = qMax<qint64>(1, sysconf(_SC_CLK_TCK));
    m_sincePeak.start();
    m_sinceSample.start();
    setActive(true);
    m_sampleTimer.start(); // restarts a running timer, which is fine
}

void CpuMeter::release()
{
    if (!m_active) {
        return;
    }
    m_sampleTimer.stop();
    m_pid = 0;
    setActive(false);
}

void CpuMeter::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    Q_EMIT activeChanged();
}

CpuMeter::Sampled CpuMeter::collect()
{
    if (!m_scanMode) {
        qint64 ticks = 0;
        int members = 0;
        if (collectFromTree(&ticks, &members)) {
            return Sampled{ticks, members};
        }
        // The tree cannot be seen whole — a setuid sudo carrying root
        // children, or a leader dead with its pipeline orphaned. The
        // process-group scan sees all of those just the same (/proc
        // stat is world-readable), so switch to it for the rest of the
        // command.
        m_scanMode = true;
    }
    return collectFromGroup(m_pid);
}

bool CpuMeter::collectFromTree(qint64 *ticks, int *members) const
{
    const ProcStat leader = readStat(m_pid);
    if (!leader.ok) {
        // The command's leader is gone; whatever still runs is in its
        // process group (the scan collects it).
        return false;
    }
    QSet<qint64> seen;
    QList<qint64> queue{m_pid};
    qint64 total = leader.ticks;
    int count = 1;
    while (!queue.isEmpty()) {
        const qint64 pid = queue.takeFirst();
        bool readable = true;
        const QList<qint64> children = readChildren(pid, &readable);
        if (!readable) {
            return false;
        }
        for (const qint64 child : children) {
            if (seen.contains(child) || seen.size() >= kMaxTreeProcesses) {
                continue;
            }
            seen.insert(child);
            const ProcStat stat = readStat(child);
            if (!stat.ok) {
                continue; // raced out of existence between the two reads
            }
            total += stat.ticks;
            ++count;
            queue.append(child);
        }
    }
    *ticks = total;
    *members = count;
    return true;
}

CpuMeter::Sampled CpuMeter::collectFromGroup(int pgid)
{
    qint64 total = 0;
    int count = 0;
    const QDir proc(QStringLiteral("/proc"));
    const QStringList entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        const qint64 pid = entry.toLongLong();
        if (pid <= 0) {
            continue; // not a process directory
        }
        const ProcStat stat = readStat(pid);
        if (stat.ok && stat.pgrp == pgid) {
            total += stat.ticks;
            ++count;
        }
    }
    return Sampled{total, count};
}

void CpuMeter::sample()
{
    const Sampled sampled = collect();
    if (sampled.members == 0) {
        // The whole command is gone and ToolProcessFinished never came
        // (a daemon crash mid-command, say): stop following it by
        // ourselves instead of metering an empty group forever.
        release();
        return;
    }
    const qreal dt = qreal(m_sinceSample.restart()) / 1000.0;
    if (!m_haveLast || dt <= 0.0) {
        m_haveLast = true;
        m_lastTicks = sampled.ticks;
        return;
    }
    qint64 delta = sampled.ticks - m_lastTicks;
    m_lastTicks = sampled.ticks;
    if (delta < 0) {
        // A process of the command exited and took its ticks out of the
        // total; this sample simply has no new ones to report.
        delta = 0;
    }

    const qreal capacity = qreal(m_clkTck) * qreal(m_coreCount);
    const qreal raw = qBound(0.0, qreal(delta) / (dt * capacity), 1.0);

    const qreal oldLevel = m_level;
    const qreal oldPeak = m_peak;

    // Attack quickly, release gracefully: the meter shows work starting
    // the moment it starts, and the level glides down when it ends.
    if (raw > m_level) {
        const qreal attack = qBound(0.0, dt / kAttackSeconds, 1.0);
        m_level += (raw - m_level) * attack;
    } else {
        m_level = qMax(raw, m_level * std::exp(-dt / kReleaseSeconds));
    }
    // The peak stands still while it is held, then decays on its own.
    if (m_level >= m_peak) {
        m_peak = m_level;
        m_sincePeak.restart();
    } else if (m_sincePeak.elapsed() > kPeakHoldMs) {
        m_peak = qMax(m_level, m_peak * std::exp(-dt / kPeakDecaySeconds));
    }

    if (!qFuzzyCompare(m_level, oldLevel)) {
        Q_EMIT fractionChanged();
    }
    if (!qFuzzyCompare(m_peak, oldPeak)) {
        Q_EMIT peakChanged();
    }
}

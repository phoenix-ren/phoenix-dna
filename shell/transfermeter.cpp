// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "transfermeter.h"

#include <cmath>

namespace
{
// The window of the moving average: three seconds, as the wire meter's
// is — the bursts of a network transfer and the bursts of a streamed
// answer read alike at this scale.
constexpr qreal kWindowSeconds = 3.0;

} // namespace

TransferMeter::TransferMeter(QObject *parent) : QObject(parent)
{
    // The average ages even when nothing arrives: a transfer that goes
    // quiet (a stall, a dying connection) must show a rate that falls,
    // not the rate of the last burst that made it.
    m_tick.setInterval(100);
    connect(&m_tick, &QTimer::timeout, this, &TransferMeter::tick);
}

void TransferMeter::tick()
{
    // The average is aged by the time that has actually passed since
    // the last tick; the arrivals themselves only add their share, so
    // the two never account for the same second twice (the same
    // arithmetic as WireMeter, which is where it comes from).
    const qreal dt = m_sinceTick.isValid() ? m_sinceTick.elapsed() / 1000.0 : 0.1;
    m_sinceTick.restart();
    if (dt <= 0.0) {
        return;
    }
    m_rate *= std::exp(-dt / kWindowSeconds);
    Q_EMIT ratesChanged();
}

qreal TransferMeter::progress() const
{
    if (m_total == 0) {
        return 0.0;
    }
    return qBound(0.0, qreal(m_done) / qreal(m_total), 1.0);
}

qreal TransferMeter::rateFraction() const
{
    return m_peak > 0.0 ? qBound(0.0, m_rate / m_peak, 1.0) : 0.0;
}

void TransferMeter::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    Q_EMIT activeChanged();
}

qreal TransferMeter::fold(qreal accum, qreal bytes, qreal dt)
{
    if (dt <= 0.0) {
        return accum;
    }
    // The report's share, as a rate over the interval that has passed
    // since the last one: (1 - exp(-dt/T))/dt. The factor is what
    // turns a count of bytes into a rate, whatever the spacing of the
    // reports.
    const qreal share = (1.0 - std::exp(-dt / kWindowSeconds)) / dt;
    return accum + bytes * share;
}

void TransferMeter::begin(bool upload, const QString &source)
{
    // The transfer that ran before is over, whether release() saw it
    // end or not: its counts, its rate and its peak described it, and
    // the new transfer starts from nothing.
    m_uploading = upload;
    m_source = source;
    m_done = 0;
    m_total = 0;
    m_rate = 0.0;
    m_peak = 0.0;
    m_sinceArrival.invalidate();
    setActive(true);
    m_tick.start();
    m_sinceTick.start();
    Q_EMIT sourceChanged();
    Q_EMIT progressChanged();
    Q_EMIT ratesChanged();
}

void TransferMeter::addProgress(quint64 done, quint64 total)
{
    if (!m_active) {
        // A report without a transfer: a call that never began() (or a
        // straggler of one that ended). Not ours to meter.
        return;
    }
    // The counts arrive cumulative; the meter keeps the difference.
    qreal delta = 0.0;
    if (done > m_done) {
        delta = qreal(done - m_done);
    }
    m_done = done;
    m_total = total;
    if (delta > 0.0) {
        const qreal dt = m_sinceArrival.isValid()
                             ? m_sinceArrival.elapsed() / 1000.0
                             : kWindowSeconds;
        m_rate = fold(m_rate, delta, dt);
        m_sinceArrival.restart();
        m_peak = qMax(m_peak, m_rate);
        Q_EMIT ratesChanged();
    }
    Q_EMIT progressChanged();
}

void TransferMeter::release()
{
    if (!m_active) {
        return;
    }
    m_tick.stop();
    m_rate = 0.0;
    setActive(false);
    Q_EMIT ratesChanged();
}

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "wiremeter.h"

#include <cmath>

namespace
{
// The window of the moving average: three seconds.
constexpr qreal kWindowSeconds = 3.0;

} // namespace

WireMeter::WireMeter(QObject *parent) : QObject(parent)
{
    // The average ages even when nothing arrives: a connection that
    // goes quiet must show a rate that falls to nothing, not the rate
    // of the last burst. The tick is coarse - the decay over a tenth of
    // a second is invisible next to the three-second window.
    m_tick.setInterval(100);
    connect(&m_tick, &QTimer::timeout, this, &WireMeter::tick);
}

void WireMeter::tick()
{
    // The average of a direction that said nothing is simply older: it
    // decays by the time that has passed since the last tick. The
    // arrivals themselves do not decay anything - the tick ages the
    // average, and an arrival only adds its share - so the two never
    // account for the same second twice.
    const qreal dt = m_sinceTick.isValid() ? m_sinceTick.elapsed() / 1000.0 : 0.1;
    m_sinceTick.restart();
    if (dt <= 0.0) {
        return;
    }
    const qreal decay = std::exp(-dt / kWindowSeconds);
    m_sendRate *= decay;
    m_receiveRate *= decay;
    Q_EMIT ratesChanged();
}

qreal WireMeter::sendFraction() const
{
    return m_sendPeak > 0.0 ? qBound(0.0, m_sendRate / m_sendPeak, 1.0) : 0.0;
}

qreal WireMeter::receiveFraction() const
{
    return m_receivePeak > 0.0 ? qBound(0.0, m_receiveRate / m_receivePeak, 1.0) : 0.0;
}

void WireMeter::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    Q_EMIT activeChanged();
}

qreal WireMeter::fold(qreal accum, qreal bytes, qreal dt)
{
    if (dt <= 0.0) {
        return accum;
    }
    // The packet's share, as a rate over the interval that has passed
    // since the last packet: (1 - exp(-dt/T))/dt. The factor is what
    // turns a packet of bytes into a rate; without it the average would
    // read a byte count, and would read it low for packets that arrive
    // in bursts.
    //
    // The average itself is not decayed here: the tick ages it, by the
    // time that has actually passed. Decaying here as well would
    // account for the same second twice and halve the rate.
    const qreal share = (1.0 - std::exp(-dt / kWindowSeconds)) / dt;
    return accum + bytes * share;
}

void WireMeter::addTraffic(int turn, quint64 bytesSent, quint64 bytesReceived)
{
    if (turn != m_turn) {
        // The traffic of another turn: start counting that one.
        beginTurn(turn);
    }

    if (bytesSent > 0) {
        const qreal dt = m_sinceSend.isValid() ? m_sinceSend.elapsed() / 1000.0 : kWindowSeconds;
        m_sendRate = fold(m_sendRate, qreal(bytesSent), dt);
        m_sinceSend.restart();
        m_sendPeak = qMax(m_sendPeak, m_sendRate);
    }
    if (bytesReceived > 0) {
        const qreal dt =
            m_sinceReceive.isValid() ? m_sinceReceive.elapsed() / 1000.0 : kWindowSeconds;
        m_receiveRate = fold(m_receiveRate, qreal(bytesReceived), dt);
        m_sinceReceive.restart();
        m_receivePeak = qMax(m_receivePeak, m_receiveRate);
    }
    if (!m_active) {
        setActive(true);
        m_tick.start();
        m_sinceTick.start();
    }
    Q_EMIT ratesChanged();
}

void WireMeter::beginTurn(int turn)
{
    if (turn != m_turn) {
        m_turn = turn;
        Q_EMIT turnChanged();
    }
    // The rates of the turn that is over are let go; the peaks stay, as
    // the reference the bars are scaled against.
    m_sendRate = 0.0;
    m_receiveRate = 0.0;
    m_sinceSend.invalidate();
    m_sinceReceive.invalidate();
    Q_EMIT ratesChanged();
}

void WireMeter::release()
{
    if (!m_active) {
        return;
    }
    m_tick.stop();
    m_sendRate = 0.0;
    m_receiveRate = 0.0;
    setActive(false);
}

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief The rate of the conversation with the model, in bytes a second.
 *
 * The daemon reports what crossed the wire for each turn of the
 * conversation: the bytes of the request that went out, the bytes of the
 * answer that came in. This class turns those into rates, and keeps the
 * peak of each direction, so a meter can show how busy the connection is
 * and the numbers beside it can say how busy that is in bytes.
 *
 * The rate is an exponentially weighted moving average over a window of
 * three seconds. The bytes arrive at arbitrary times — a request in one
 * piece, an answer in many — so each arrival is folded into the average
 * over the time that has passed since the last one:
 *
 * \code
 *   accum = exp(-dt/T) * accum + bytes * (1 - exp(-dt/T)) / dt
 *   rate  = accum
 * \endcode
 *
 * The factor (1 - exp(-dt/T))/dt is what turns a packet of bytes into a
 * rate: without it the average would read a byte count, and would read
 * it low for packets that arrive in bursts. With it the average reads
 * the true rate for any spacing of the packets — the logarithm in
 * exp(-dt/T) is what makes the decay exact over the interval that
 * actually passed, rather than over the interval one hoped for.
 *
 * The two directions are kept apart: a request and its answer are not
 * the same traffic, and a meter that averaged them would hide both.
 *
 * Everything runs on the GUI thread.
 */
class WireMeter : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

public:
    explicit WireMeter(QObject *parent = nullptr);

    /*! \brief Whether traffic is being metered right now. */
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

    /*! \brief The rate of the bytes that went out, in bytes a second. */
    Q_PROPERTY(qreal sendRate READ sendRate NOTIFY ratesChanged)

    /*! \brief The rate of the bytes that came in, in bytes a second. */
    Q_PROPERTY(qreal receiveRate READ receiveRate NOTIFY ratesChanged)

    /*! \brief The highest send rate seen since the run began — the
     *  reference the send bar is scaled against. */
    Q_PROPERTY(qreal sendPeak READ sendPeak NOTIFY ratesChanged)

    /*! \brief The highest receive rate seen since the run began. */
    Q_PROPERTY(qreal receivePeak READ receivePeak NOTIFY ratesChanged)

    /*! \brief The send rate as a share of \a sendPeak (0..1): what the
     *  bar shows. */
    Q_PROPERTY(qreal sendFraction READ sendFraction NOTIFY ratesChanged)

    /*! \brief The receive rate as a share of \a receivePeak (0..1). */
    Q_PROPERTY(qreal receiveFraction READ receiveFraction NOTIFY ratesChanged)

    /*! \brief The turn of the conversation the traffic belongs to. */
    Q_PROPERTY(int turn READ turn NOTIFY turnChanged)

    bool isActive() const { return m_active; }
    qreal sendRate() const { return m_sendRate; }
    qreal receiveRate() const { return m_receiveRate; }
    qreal sendPeak() const { return m_sendPeak; }
    qreal receivePeak() const { return m_receivePeak; }
    qreal sendFraction() const;
    qreal receiveFraction() const;
    int turn() const { return m_turn; }

    /*! \brief Folds a turn's traffic into the averages.
     *
     * \a bytesSent and \a bytesReceived are the payload bytes of the
     * turn (the request body and the answer). \a turn names the turn of
     * the conversation they belong to.
     *
     * The counts arrive with the turn they belong to, so the rate they
     * produce describes the whole turn rather than the instant it
     * arrives. */
    Q_INVOKABLE void addTraffic(int turn, quint64 bytesSent, quint64 bytesReceived);

    /*! \brief Starts a new turn: the rates of the previous one are let
     *  go (the meter shows the turn that is running), the peaks are
     *  kept as the reference for the bars. */
    Q_INVOKABLE void beginTurn(int turn);

    /*! \brief Stops metering (the run ended). */
    Q_INVOKABLE void release();

Q_SIGNALS:
    void activeChanged();
    void ratesChanged();
    void turnChanged();

private:
    void setActive(bool active);

    // Ages the averages: a direction that said nothing decays.
    void tick();

    // Folds one packet of bytes into an average over \a dt seconds.
    static qreal fold(qreal accum, qreal bytes, qreal dt);

    bool m_active = false;
    int m_turn = 0;
    // The averages of the two directions, in bytes a second.
    qreal m_sendRate = 0.0;
    qreal m_receiveRate = 0.0;
    // The highest rate each direction reached: the bars' scale.
    qreal m_sendPeak = 0.0;
    qreal m_receivePeak = 0.0;
    // When the last packet of each direction arrived.
    QElapsedTimer m_sinceSend;
    QElapsedTimer m_sinceReceive;
    // The tick that ages the averages, and when it last ran.
    QTimer m_tick;
    QElapsedTimer m_sinceTick;
};

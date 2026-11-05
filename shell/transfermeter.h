// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief The progress of the transfer the agent is running, in bytes.
 *
 * The daemon reports what a transfer tool call has moved so far:
 * the cumulative count of the call's transfer, and the total when the
 * far end promised one (zero when it did not). This class turns those
 * into a rate — the same exponentially weighted moving average the
 * conversation's wire meter uses, over a window of three seconds — and
 * keeps the count and the share of the total, so a meter can show both
 * how fast the bytes move and how much of the thing is here.
 *
 * One transfer runs at a time (the run's tools execute one by one), so
 * one meter serves the whole run: begin() names the direction and the
 * source, addProgress() folds the counts in as they arrive, and
 * release() lets the meter go when the call ends.
 */
class TransferMeter : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

public:
    explicit TransferMeter(QObject *parent = nullptr);

    /*! \brief Whether a transfer is being metered right now. */
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

    /*! \brief Whether the transfer pushes bytes away (an upload) rather
     *  than pulling them in. */
    Q_PROPERTY(bool uploading READ isUploading NOTIFY sourceChanged)

    /*! \brief What is being transferred: the URL a download pulls (a
     *  magnet: link for a torrent) or the address an upload pushes to. */
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)

    /*! \brief The bytes moved so far, cumulative for the transfer. */
    Q_PROPERTY(quint64 done READ done NOTIFY progressChanged)

    /*! \brief The bytes promised (0 when the far end did not say). */
    Q_PROPERTY(quint64 total READ total NOTIFY progressChanged)

    /*! \brief Whether \a total is known: a share can be shown. */
    Q_PROPERTY(bool hasTotal READ hasTotal NOTIFY progressChanged)

    /*! \brief The share of the transfer that is here (0..1); 0 while
     *  the total is unknown. */
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)

    /*! \brief The rate of the transfer, in bytes a second. */
    Q_PROPERTY(qreal rate READ rate NOTIFY ratesChanged)

    /*! \brief The highest rate seen since the transfer began — the
     *  reference the bar is scaled against. */
    Q_PROPERTY(qreal peak READ peak NOTIFY ratesChanged)

    /*! \brief The rate as a share of \a peak (0..1): what the bar shows. */
    Q_PROPERTY(qreal rateFraction READ rateFraction NOTIFY ratesChanged)

    bool isActive() const { return m_active; }
    bool isUploading() const { return m_uploading; }
    QString source() const { return m_source; }
    quint64 done() const { return m_done; }
    quint64 total() const { return m_total; }
    bool hasTotal() const { return m_total > 0; }
    qreal progress() const;
    qreal rate() const { return m_rate; }
    qreal peak() const { return m_peak; }
    qreal rateFraction() const;

    /*! \brief A transfer began: \a source is what moves, \a upload the
     *  direction it moves in.
     *
     * The previous transfer's counts are let go: the counts, the rate
     * and the peak all describe the one transfer that is running. */
    Q_INVOKABLE void begin(bool upload, const QString &source);

    /*! \brief Folds a report into the meter.
     *
     * \a done is the cumulative count of the transfer (not a delta:
     * the meter works the difference out itself), \a total the promised
     * bytes, 0 when the far end did not name them. A count that went
     * backwards — a report of a transfer that never began() — is not
     * metered: it belongs to somebody else. */
    Q_INVOKABLE void addProgress(quint64 done, quint64 total);

    /*! \brief Stops metering (the call ended). */
    Q_INVOKABLE void release();

Q_SIGNALS:
    void activeChanged();
    void sourceChanged();
    void progressChanged();
    void ratesChanged();

private:
    void setActive(bool active);

    // Ages the rate: a transfer that goes quiet must show a rate that
    // falls to nothing, not the rate of the last burst.
    void tick();

    // Folds one packet of bytes into the average over \a dt seconds.
    static qreal fold(qreal accum, qreal bytes, qreal dt);

    bool m_active = false;
    bool m_uploading = false;
    QString m_source;
    quint64 m_done = 0;
    quint64 m_total = 0;
    // The average of the bytes that moved, in bytes a second, and the
    // highest it reached: the bar's scale.
    qreal m_rate = 0.0;
    qreal m_peak = 0.0;
    // When the last packet arrived.
    QElapsedTimer m_sinceArrival;
    // The tick that ages the average, and when it last ran.
    QTimer m_tick;
    QElapsedTimer m_sinceTick;
};

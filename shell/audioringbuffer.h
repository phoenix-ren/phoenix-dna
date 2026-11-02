// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QAbstractListModel>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief A ring buffer holding the most recent audio peaks.
 *
 * The buffer keeps a fixed number of buckets, each holding the minimum and
 * maximum amplitude observed over a slice of time. That is enough to draw a
 * DAW-style envelope without ever exposing raw PCM to QML.
 *
 * The buffer is a ring: once it is full, the oldest bucket is overwritten by
 * the newest one, and the model reports the change so the view can scroll.
 */
class AudioRingBuffer : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int capacity READ capacity NOTIFY capacityChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool active READ isActive WRITE setActive NOTIFY activeChanged)

    /*!
     * \brief The largest min/max span of any bucket currently in the ring,
     * in raw sample units.
     *
     * The view normalises bar heights against this, so the loudest slice of
     * the recent audio fills the widget and quieter speech keeps its shape
     * whatever the microphone level.
     */
    Q_PROPERTY(qreal peakSpan READ peakSpan NOTIFY peakSpanChanged)

    /*!
     * \brief The RMS level of the most recent processing block.
     *
     * Recomputed for every full audio buffer (see \c kRmsBlockFrames) and
     * smoothed into \c rmsLevel.
     */
    Q_PROPERTY(qreal instantaneousLevel READ instantaneousLevel NOTIFY rmsLevelChanged)

    /*!
     * \brief An exponentially weighted moving average of \c instantaneousLevel.
     *
     * The EWMA averages over roughly \c kRmsTimeConstantMs of audio; its
     * coefficient accounts for the processing block size, so the smoothing
     * window is the same whatever block size is negotiated.
     *
     * The view normalises bar heights against this level, which keeps the
     * display steady through quiet passages without amplifying silence.
     */
    Q_PROPERTY(qreal rmsLevel READ rmsLevel NOTIFY rmsLevelChanged)

public:
    explicit AudioRingBuffer(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int capacity() const { return m_capacity; }
    qreal peakSpan() const { return m_peakSpan; }
    bool isActive() const { return m_active; }
    void setActive(bool active);
    qreal instantaneousLevel() const { return m_instantaneousLevel; }
    qreal rmsLevel() const { return m_rmsLevel; }

    /*!
     * \brief Resets the buffer and starts accepting samples.
     *
     * \a seconds how much history to keep; sub-second values are fine (the
     * ring granularity is \c kBucketsPerSecond buckets per second).
     */
    void start(qreal seconds);

    /*!
     * \brief Stops accepting samples, keeping the last window on screen.
     *
     * The view dims the frozen waveform; \c clear() (when the results pane
     * pops up) or the next \c start() empties it.
     */
    void stop();

    /*!
     * \brief Empties the buffer, wiping the frozen waveform off screen.
     *
     * Called when the results pane pops up — the run's first output, long
     * before it finishes: by then the user has had the whole transcription
     * to read the last moments of audio, and the widget should rest empty
     * instead of showing a stale envelope until the next recording. A live
     * waveform is never cleared — output can arrive while the user is
     * already dictating again, and that recording must keep its bars — so
     * this only wipes the frozen state left by \c stop().
     */
    void clear();

    /*! \brief Appends a block of interleaved 16-bit PCM samples. */
    void append(const QByteArray &pcm, int sampleRate, int channels);

Q_SIGNALS:
    void countChanged();
    void capacityChanged();
    void peakSpanChanged();
    void activeChanged();
    void rmsLevelChanged();

private:
    struct Bucket {
        qint16 minimum = 0;
        qint16 maximum = 0;
        qreal meanLevel = 0.0; // EWMA of |x| over the bucket, raw units
    };

    void pushBucket(const Bucket &bucket);
    void advanceRms(qint16 sample, int sampleRate);

    int m_capacity = 0;
    int m_head = 0;
    int m_used = 0;
    bool m_active = false;
    qreal m_peakSpan = 0.0;

    // RMS metering: an accumulator over the current processing block, the
    // level of the last completed block, and its smoothed value.
    qreal m_rmsAccumulator = 0.0;
    int m_rmsBlockFill = 0;
    qreal m_instantaneousLevel = 0.0;
    qreal m_rmsLevel = 0.0;

    // Accumulator for the bucket currently being filled.
    qint16 m_accMin = 0;
    qint16 m_accMax = 0;
    int m_accCount = 0;
    int m_samplesPerBucket = 0;

    // Mean-|level| metering for the light band of the waveform: an
    // exponentially weighted moving average of |x|, accumulated across
    // buffers and averaged per bucket (see the waveform component design).
    double m_meanEwma = 0.0;
    double m_meanSum = 0.0;

    QList<Bucket> m_buckets;
};

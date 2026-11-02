// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "audioringbuffer.h"

#include <cmath>

namespace
{
constexpr qreal kDefaultSeconds = 0.75;

// Temporal resolution of the envelope: 240 buckets/s gives ~4 ms slices,
// so the 0.75 s window spans 180 buckets — fine enough that the view reads
// as a continuous waveform at widget width (30 buckets/s made every bar a
// ~15 px block).
constexpr int kBucketsPerSecond = 240;

// EWMA time constant of the mean-|level| band and its silence floor — the
// same values the waveform component uses (design_docs/WAVEFORM_COMPONENT_1.md).
constexpr double kMeanTimeConstantMs = 10.0;
constexpr double kDenormalFloor = 1e-12;
constexpr int kRoleMinimum = Qt::UserRole + 1;
constexpr int kRoleMaximum = Qt::UserRole + 2;
constexpr int kRoleMeanLevel = Qt::UserRole + 3;

// Size of one audio processing block: the RMS metering runs once per full
// block, independent of how large the delivered chunks are.
constexpr int kRmsBlockFrames = 1024;

// The EWMA smoothing window; \c rmsLevel lags the input by this much.
constexpr qreal kRmsTimeConstantMs = 750.0;
} // namespace

AudioRingBuffer::AudioRingBuffer(QObject *parent) : QAbstractListModel(parent) {}

int AudioRingBuffer::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_used;
}

QVariant AudioRingBuffer::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_used) {
        return {};
    }

    // The oldest bucket sits at the tail of the ring; expose the buffer
    // oldest-first so the view can draw it left-to-right in time order.
    const int ringIndex = (m_head - m_used + index.row() + m_capacity * 2) % m_capacity;
    const Bucket &bucket = m_buckets.at(ringIndex);

    switch (role) {
    case kRoleMinimum:
        return bucket.minimum;
    case kRoleMaximum:
        return bucket.maximum;
    case kRoleMeanLevel:
        return bucket.meanLevel;
    default:
        return {};
    }
}

QHash<int, QByteArray> AudioRingBuffer::roleNames() const
{
    return {
        {kRoleMinimum, QByteArrayLiteral("minimum")},
        {kRoleMaximum, QByteArrayLiteral("maximum")},
        {kRoleMeanLevel, QByteArrayLiteral("meanLevel")},
    };
}

void AudioRingBuffer::setActive(bool active)
{
    if (m_active == active) {
        return;
    }
    m_active = active;
    Q_EMIT activeChanged();
}

void AudioRingBuffer::start(qreal seconds)
{
    if (seconds <= 0) {
        seconds = kDefaultSeconds;
    }

    beginResetModel();
    m_capacity = qMax(1, int(seconds * kBucketsPerSecond));
    m_buckets = QList<Bucket>(m_capacity);
    m_head = 0;
    m_used = 0;
    m_accMin = 0;
    m_accMax = 0;
    m_accCount = 0;
    m_samplesPerBucket = 0;
    m_meanEwma = 0.0;
    m_meanSum = 0.0;
    m_rmsAccumulator = 0.0;
    m_rmsBlockFill = 0;
    m_instantaneousLevel = 0.0;
    m_rmsLevel = 0.0;
    m_peakSpan = 0.0;
    endResetModel();

    // The view lays buckets out against the capacity; QML bindings on it
    // only re-evaluate if the change is notified (it is 0 until start()).
    Q_EMIT capacityChanged();
    Q_EMIT rmsLevelChanged();
    Q_EMIT countChanged();
    Q_EMIT peakSpanChanged();
    setActive(true);
}

void AudioRingBuffer::stop()
{
    if (!m_active) {
        return;
    }

    // Freezing, not wiping: the buckets stay on screen after stop, dimmed
    // by the view (which keys off the recording state), so the user can
    // still read the last moments of audio. clear() — when the results
    // pane pops up — or the next start() empties the ring.
    m_active = false;
    Q_EMIT activeChanged();
}

void AudioRingBuffer::clear()
{
    // Never wipe a live waveform: output can arrive while the user is
    // already dictating again, and that recording must keep its bars. An
    // empty buffer needs no reset either.
    if (m_active || m_used == 0) {
        return;
    }

    // Same reset start() performs, minus the capacity change and the
    // reactivation: the view lays buckets out against the capacity, so
    // keeping it spares an empty view a needless relayout.
    beginResetModel();
    m_buckets = QList<Bucket>(m_capacity);
    m_head = 0;
    m_used = 0;
    m_accMin = 0;
    m_accMax = 0;
    m_accCount = 0;
    m_samplesPerBucket = 0;
    m_meanEwma = 0.0;
    m_meanSum = 0.0;
    m_rmsAccumulator = 0.0;
    m_rmsBlockFill = 0;
    m_instantaneousLevel = 0.0;
    m_rmsLevel = 0.0;
    m_peakSpan = 0.0;
    endResetModel();

    Q_EMIT rmsLevelChanged();
    Q_EMIT countChanged();
    Q_EMIT peakSpanChanged();
}

void AudioRingBuffer::append(const QByteArray &pcm, int sampleRate, int channels)
{
    if (!m_active || pcm.isEmpty() || channels <= 0 || sampleRate <= 0) {
        return;
    }

    const int bytesPerSample = 2; // 16-bit
    const int frameSize = bytesPerSample * channels;
    const int frames = pcm.size() / frameSize;

    if (m_samplesPerBucket == 0) {
        // kBucketsPerSecond slices of a second each.
        m_samplesPerBucket = qMax(1, (sampleRate / kBucketsPerSecond));
        m_accCount = 0;
        m_meanSum = 0.0;
    }

    // The mean band's EWMA coefficient must be derived from the sampling
    // frequency — α = 1 − exp(−1 / (τ · fs)) averages over τ seconds of
    // audio whatever the rate (waveform component, step 1).
    const double tauSeconds = kMeanTimeConstantMs / 1000.0;
    const double meanAlpha = 1.0 - std::exp(-1.0 / (tauSeconds * sampleRate));

    const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());

    for (int frame = 0; frame < frames; ++frame) {
        // Mix down to mono by taking the first channel.
        const qint16 value = samples[frame * channels];
        advanceRms(value, sampleRate);

        // Mean |level| for the light band, with the denormal clamp that
        // keeps the accumulator's decay out of the slow FPU range.
        m_meanEwma = meanAlpha * std::fabs(double(value)) + (1.0 - meanAlpha) * m_meanEwma;
        if (m_meanEwma < kDenormalFloor) {
            m_meanEwma = 0.0;
        }
        m_meanSum += m_meanEwma;

        if (m_accCount == 0) {
            m_accMin = value;
            m_accMax = value;
        } else {
            m_accMin = qMin(m_accMin, value);
            m_accMax = qMax(m_accMax, value);
        }

        if (++m_accCount >= m_samplesPerBucket) {
            pushBucket({m_accMin, m_accMax, m_meanSum / m_accCount});
            m_accCount = 0;
            m_meanSum = 0.0;
        }
    }
}

void AudioRingBuffer::advanceRms(qint16 sample, int sampleRate)
{
    // Accumulate mean square over one processing block.
    m_rmsAccumulator += qreal(sample) * sample;
    if (++m_rmsBlockFill < kRmsBlockFrames) {
        return;
    }

    m_instantaneousLevel = std::sqrt(m_rmsAccumulator / kRmsBlockFrames);
    m_rmsBlockFill = 0;
    m_rmsAccumulator = 0.0;

    // Exponentially weighted moving average over ~kRmsTimeConstantMs of
    // audio. The coefficient accounts for how long a full block lasts, so
    // the smoothing window is identical whatever the block size (1024
    // frames at 48 kHz ≈ 21.3 ms ⇒ α ≈ 0.028).
    const qreal blockMs = 1000.0 * kRmsBlockFrames / qMax(1, sampleRate);
    const qreal alpha = 1.0 - std::exp(-blockMs / kRmsTimeConstantMs);
    m_rmsLevel = alpha * m_instantaneousLevel + (1.0 - alpha) * m_rmsLevel;

    Q_EMIT rmsLevelChanged();
}

void AudioRingBuffer::pushBucket(const Bucket &bucket)
{
    if (m_capacity <= 0) {
        return;
    }

    if (m_used == m_capacity) {
        // The ring is full: overwrite the oldest bucket in place so the view
        // can animate the scroll rather than rebuilding the whole model.
        m_buckets[m_head] = bucket;
        m_head = (m_head + 1) % m_capacity;
        Q_EMIT dataChanged(index(0, 0), index(m_used - 1, 0),
                           {kRoleMinimum, kRoleMaximum, kRoleMeanLevel});
    } else {
        const int row = m_used;
        beginInsertRows(QModelIndex(), row, row);
        m_buckets[m_head] = bucket;
        m_head = (m_head + 1) % m_capacity;
        ++m_used;
        endInsertRows();
        Q_EMIT countChanged();
    }

    // The scale the view normalises against: the widest span in the ring.
    // At 40 buckets/s over at most a few hundred buckets this recomputation
    // is far cheaper than any incremental scheme would be complicated.
    qreal peak = 0.0;
    for (int i = 0; i < m_used; ++i) {
        const Bucket &b = m_buckets.at(i);
        peak = qMax(peak, qreal(b.maximum) - b.minimum);
    }
    if (!qFuzzyCompare(peak, m_peakSpan)) {
        m_peakSpan = peak;
        Q_EMIT peakSpanChanged();
    }
}

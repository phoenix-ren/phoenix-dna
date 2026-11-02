// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "waveformbuffer.h"

#include <QElapsedTimer>

#include <cmath>

namespace
{
// Repaint kicks are throttled to roughly one per frame; anything above
// ~30 Hz cannot be displayed anyway.
constexpr qint64 kUpdateIntervalMs = 33;

// Silence floor for the EWMA accumulator (≈ −240 dBFS): clamping the
// exponential decay to zero here keeps it far above the denormal range,
// where FPU arithmetic slows to a crawl (step-1 design document).
constexpr double kDenormalFloor = 1e-12;

constexpr int kMinimumColumns = 8;
} // namespace

WaveformBuffer::WaveformBuffer(QObject *parent)
    : QObject(parent)
{
}

int WaveformBuffer::columns() const
{
    QMutexLocker locker(&m_mutex);
    return m_columns;
}

void WaveformBuffer::setColumns(int columns)
{
    columns = qMax(kMinimumColumns, columns);
    {
        QMutexLocker locker(&m_mutex);
        if (columns == m_columns) {
            return;
        }
        m_columns = columns;
    }
    // The ring geometry changes: fall back to the zeroed state.
    if (m_active) {
        start();
    }
    Q_EMIT columnsChanged();
}

qreal WaveformBuffer::windowSeconds() const
{
    QMutexLocker locker(&m_mutex);
    return m_windowSeconds;
}

void WaveformBuffer::setWindowSeconds(qreal seconds)
{
    if (seconds <= 0.0) {
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        if (qFuzzyCompare(seconds, m_windowSeconds)) {
            return;
        }
        m_windowSeconds = seconds;
    }
    if (m_active) {
        start();
    }
    Q_EMIT windowSecondsChanged();
}

qreal WaveformBuffer::tauMs() const
{
    QMutexLocker locker(&m_mutex);
    return m_tauMs;
}

void WaveformBuffer::setTauMs(qreal tauMs)
{
    if (tauMs <= 0.0) {
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        if (qFuzzyCompare(tauMs, m_tauMs)) {
            return;
        }
        m_tauMs = tauMs;
    }
    Q_EMIT tauMsChanged();
}

bool WaveformBuffer::isActive() const
{
    QMutexLocker locker(&m_mutex);
    return m_active;
}

int WaveformBuffer::sampleRate() const
{
    QMutexLocker locker(&m_mutex);
    return m_sampleRate;
}

void WaveformBuffer::resetLocked()
{
    // Complication 1: zero the arrays until enough samples arrive to fill
    // the bitmap. Unwritten columns legitimately read as silence.
    m_ring = QVector<Column>(m_columns);
    m_head = 0;
    m_used = 0;
    m_accMinimum = 0.0;
    m_accMaximum = 0.0;
    m_accMeanSum = 0.0;
    m_accCount = 0;
    m_samplesPerColumn = 0;
    m_sampleRate = 0;
    m_ewma = 0.0;
    m_lastUpdateMs = 0;
    m_clock.start();
}

void WaveformBuffer::start()
{
    {
        QMutexLocker locker(&m_mutex);
        resetLocked();
        m_active = true;
    }
    Q_EMIT activeChanged();
    Q_EMIT updated();
}

void WaveformBuffer::stop()
{
    {
        QMutexLocker locker(&m_mutex);
        if (!m_active) {
            return;
        }
        resetLocked();
        m_active = false;
    }
    Q_EMIT activeChanged();
    Q_EMIT updated();
}

void WaveformBuffer::append(const QByteArray &pcm, int sampleRate, int channels)
{
    QMutexLocker locker(&m_mutex);

    if (!m_active || pcm.isEmpty() || channels <= 0 || sampleRate <= 0) {
        return;
    }

    if (sampleRate != m_sampleRate || m_samplesPerColumn == 0) {
        // A column spans windowSeconds / columns of audio; lock that as a
        // sample quota at the rate the capture negotiated.
        m_sampleRate = sampleRate;
        m_samplesPerColumn = qMax<qsizetype>(
            1, qRound64(sampleRate * m_windowSeconds / m_columns));
        m_accCount = 0; // a partial column at the old rate would be wrong
    }

    // The EWMA coefficient must be derived from the sampling frequency:
    // with α = 1 − exp(−1 / (τ · fs)) the accumulator averages over τ
    // seconds of audio whatever the rate (step-1 design document).
    const double tauSeconds = m_tauMs / 1000.0;
    const double alpha = 1.0 - std::exp(-1.0 / (tauSeconds * m_sampleRate));

    const int frameSize = 2 * channels; // 16-bit
    const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    const qsizetype frames = pcm.size() / frameSize;

    bool published = false;

    for (qsizetype frame = 0; frame < frames; ++frame) {
        // Mix down to mono by averaging, matching the offline renderer.
        double mixed = 0.0;
        for (int channel = 0; channel < channels; ++channel) {
            mixed += samples[frame * channels + channel];
        }
        const double sample = mixed / channels;

        m_ewma = alpha * std::fabs(sample) + (1.0 - alpha) * m_ewma;
        if (m_ewma < kDenormalFloor) {
            m_ewma = 0.0; // dodge denormal-number slowdowns in silence
        }

        // Chunked computation: fold O(1) per sample and publish a column
        // only when its quota is full, whatever sizes the buffers have.
        if (m_accCount == 0) {
            m_accMinimum = sample;
            m_accMaximum = sample;
        } else {
            m_accMinimum = qMin(m_accMinimum, sample);
            m_accMaximum = qMax(m_accMaximum, sample);
        }
        m_accMeanSum += m_ewma;
        ++m_accCount;

        if (m_accCount >= m_samplesPerColumn) {
            Column &column = m_ring[m_head];
            column.minimum = m_accMinimum / 32768.0;
            column.maximum = m_accMaximum / 32768.0;
            column.meanLevel = m_accMeanSum / m_accCount / 32768.0;
            m_head = (m_head + 1) % m_columns;
            m_used = qMin(m_used + 1, m_columns);
            m_accMinimum = 0.0;
            m_accMaximum = 0.0;
            m_accMeanSum = 0.0;
            m_accCount = 0;
            published = true;
        }
    }

    // Throttle the repaint kick under the lock (the clock belongs to the
    // append side), but emit outside it so a direct-connection receiver
    // could safely call back.
    bool kick = false;
    if (published) {
        const qint64 elapsedMs = m_clock.elapsed();
        if (elapsedMs - m_lastUpdateMs >= kUpdateIntervalMs) {
            m_lastUpdateMs = elapsedMs;
            kick = true;
        }
    }
    locker.unlock();

    if (kick) {
        Q_EMIT updated();
    }
}

int WaveformBuffer::used() const
{
    QMutexLocker locker(&m_mutex);
    return m_used;
}

void WaveformBuffer::snapshot(QVector<Column> *out) const
{
    QMutexLocker locker(&m_mutex);

    out->resize(m_columns);
    if (m_used == 0) {
        out->fill(Column{});
        return;
    }

    // Oldest column first so the view can draw left-to-right in time order:
    // the oldest sits just behind the head.
    for (int i = 0; i < m_used; ++i) {
        (*out)[i] = m_ring[(m_head - m_used + i + m_columns * 2) % m_columns];
    }
    for (int i = m_used; i < m_columns; ++i) {
        (*out)[i] = Column{};
    }
}

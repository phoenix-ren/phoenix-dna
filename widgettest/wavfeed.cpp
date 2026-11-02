// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "wavfeed.h"

#include <QElapsedTimer>

namespace
{
// Real capture devices deliver buffers of a few dozen milliseconds; pw-cat
// in particular tends to hand over ~25 ms at a time.
constexpr int kChunkMs = 25;
}

WavFeedThread::WavFeedThread(WaveformBuffer *buffer, QByteArray pcm, int sampleRate,
                             qreal speed, QObject *parent)
    : QThread(parent)
    , m_buffer(buffer)
    , m_pcm(std::move(pcm))
    , m_sampleRate(sampleRate)
    , m_speed(speed)
{
}

void WavFeedThread::stopFeed()
{
    m_stop = true;
}

void WavFeedThread::run()
{
    const qsizetype totalFrames = m_pcm.size() / 2; // 16-bit mono
    if (totalFrames == 0 || m_sampleRate <= 0) {
        return;
    }

    const qsizetype chunkFrames = qMax<qsizetype>(1, qint64(m_sampleRate) * kChunkMs / 1000);
    qsizetype framesSent = 0; // cumulative, keeps pacing gap-free across loops
    QElapsedTimer clock;
    clock.start();

    while (!m_stop) {
        const qsizetype start = framesSent % totalFrames;
        const qsizetype count = qMin(chunkFrames, totalFrames - start);
        const QByteArray chunk = m_pcm.mid(qsizetype(start) * 2, qsizetype(count) * 2);

        // Due time (in wall-clock ms) for the end of this chunk.
        const qint64 dueMs = qint64((framesSent + count) * 1000.0 / (m_sampleRate * m_speed));
        while (!m_stop && clock.elapsed() < dueMs) {
            msleep(2);
        }
        if (m_stop) {
            break;
        }

        m_buffer->append(chunk, m_sampleRate, 1);
        framesSent += count;
    }
}

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QByteArray>
#include <QThread>

#include <atomic>

#include "waveformbuffer.h"

/*!
 * \brief Plays a WAV into a WaveformBuffer the way a capture device would.
 *
 * Real audio never arrives as one block: the device hands over a buffer
 * every few dozen milliseconds. This thread recreates that exactly — it
 * delivers the file as 25 ms chunks of 16-bit PCM, paced against a wall
 * clock (scaled by \a speed), looping until stopped. It stands in for the
 * pw-cat capture thread of the real widget, so the buffer's mutex is
 * exercised by genuinely concurrent append() and snapshot() calls.
 */
class WavFeedThread : public QThread
{
    Q_OBJECT

public:
    /*!
     * \a pcm is interleaved 16-bit mono PCM ready for WaveformBuffer::append.
     */
    WavFeedThread(WaveformBuffer *buffer,
                  QByteArray pcm,
                  int sampleRate,
                  qreal speed,
                  QObject *parent = nullptr);

    /*! \brief Stops the feed; call from any thread, then wait(). */
    void stopFeed();

protected:
    void run() override;

private:
    WaveformBuffer *m_buffer;
    const QByteArray m_pcm;
    const int m_sampleRate;
    const qreal m_speed;
    std::atomic<bool> m_stop{false};
};

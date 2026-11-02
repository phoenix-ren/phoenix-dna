// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QByteArray>
#include <QObject>

/*!
 * \brief Accumulates the PCM of one dictation and finalizes it as a WAV clip.
 *
 * AudioCapture publishes the live microphone stream but keeps no history:
 * AudioRingBuffer reduces every chunk to min/max buckets for the waveform and
 * the samples themselves are dropped. This class is the second consumer of
 * pcmReady(): it appends the raw interleaved s16le frames of the whole
 * utterance and, on takeWav(), prepares them for the transcription API —
 *
 *   * pw-cat's full-scale start-up burst is trimmed from the head (it is
 *     invisible in the span-drawn waveform, but would be a loud click in
 *     the clip),
 *   * the channels are mixed down to mono by averaging,
 *   * the rate is decimated to 16 kHz when it divides evenly (48 kHz → 3),
 *     because "speech recognition does not benefit from high sample rates
 *     or stereo" (MiniMax API docs) and a mono 16 kHz clip is three times
 *     smaller to upload,
 *   * a canonical RIFF header is prepended, since neither provider accepts
 *     raw PCM without a container.
 */
class ClipRecorder : public QObject
{
    Q_OBJECT

public:
    explicit ClipRecorder(QObject *parent = nullptr);

    /*! \brief The sample rate handed to the transcription API. */
    static constexpr int kTargetRate = 16000;

    /*! \brief Discards any partial clip and prepares for a new recording. */
    void start();

    /*! \brief Appends a pcmReady() block (interleaved s16le, whole frames). */
    void append(const QByteArray &pcm, int sampleRate, int channels);

    /*!
     * \brief Finalizes the clip as a mono WAV file and resets the recorder.
     *
     * Returns an empty byte array when nothing usable was captured; a clip
     * that was pure start-up burst counts as nothing usable.
     */
    QByteArray takeWav();

private:
    QByteArray m_pcm;
    int m_sampleRate = 0; // latched from the first append() of a clip
    int m_channels = 0;
};

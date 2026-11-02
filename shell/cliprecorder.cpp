// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "cliprecorder.h"

#include <QDebug>
#include <QVector>

#include <QtGlobal>

#include <cstring>

namespace {

// pw-cat's first frames after stream start are a full-scale burst (every
// sample +32767, see DESIGN.md). A frame whose samples are all identical at
// (near) full scale cannot be speech; anything that loud would also clip.
constexpr int kFullScaleThreshold = 32000;

} // namespace

ClipRecorder::ClipRecorder(QObject *parent)
    : QObject(parent)
{
}

void ClipRecorder::start()
{
    m_pcm.clear();
    m_sampleRate = 0;
    m_channels = 0;
}

void ClipRecorder::append(const QByteArray &pcm, int sampleRate, int channels)
{
    if (pcm.isEmpty() || sampleRate <= 0 || channels <= 0) {
        return;
    }

    if (m_channels == 0) {
        m_sampleRate = sampleRate;
        m_channels = channels;
    } else if (sampleRate != m_sampleRate || channels != m_channels) {
        // The format is negotiated before the first publish and cannot
        // change mid-stream; a mismatched block would corrupt the clip.
        qWarning() << "ClipRecorder: format changed mid-clip, dropping block";
        return;
    }

    m_pcm.append(pcm);
}

QByteArray ClipRecorder::takeWav()
{
    const QByteArray pcm = m_pcm;
    const int rate = m_sampleRate;
    const int channels = m_channels;

    start();

    if (pcm.isEmpty() || rate <= 0 || channels <= 0) {
        return {};
    }

    const auto *frames = reinterpret_cast<const qint16 *>(pcm.constData());
    const qsizetype frameCount = pcm.size() / qsizetype(channels * 2);

    // 1. Trim the start-up burst from the head of the clip.
    qsizetype begin = 0;
    while (begin < frameCount) {
        const qint16 *frame = frames + begin * channels;
        const int peak = qAbs(int(frame[0]));
        bool burst = peak >= kFullScaleThreshold;
        for (int c = 1; burst && c < channels; ++c) {
            burst = frame[c] == frame[0];
        }
        if (!burst) {
            break;
        }
        ++begin;
    }
    if (begin >= frameCount) {
        // Only the burst was captured; there is no speech to transcribe.
        return {};
    }

    // 2. Mix down to mono by averaging the channels.
    QVector<qint16> mono;
    mono.reserve(frameCount - begin);
    for (qsizetype f = begin; f < frameCount; ++f) {
        const qint16 *frame = frames + f * channels;
        int sum = 0;
        for (int c = 0; c < channels; ++c) {
            sum += frame[c];
        }
        mono.append(qint16(sum / channels));
    }

    // 3. Decimate to the target rate when it divides evenly (48 kHz → 3).
    //    Averaging each group doubles as a cheap low-pass; content above
    //    8 kHz does not help recognition. Rates that are not an integer
    //    multiple (44.1 kHz) pass through untouched — the API accepts
    //    them, the upload is just larger.
    int outRate = rate;
    if (rate > kTargetRate && rate % kTargetRate == 0) {
        const qsizetype factor = rate / kTargetRate;
        QVector<qint16> decimated;
        decimated.reserve(mono.size() / factor);
        for (qsizetype i = 0; i + factor <= mono.size(); i += factor) {
            int sum = 0;
            for (qsizetype j = 0; j < factor; ++j) {
                sum += mono[qsizetype(i + j)];
            }
            decimated.append(qint16(sum / factor));
        }
        mono = std::move(decimated);
        outRate = kTargetRate;
    }

    if (mono.isEmpty()) {
        return {};
    }

    // 4. Canonical 44-byte RIFF header around the samples. The stream is
    //    little-endian, so the header is assembled byte-wise.
    const quint32 dataSize = quint32(mono.size() * 2);
    QByteArray out;
    out.reserve(44 + int(dataSize));
    auto le16 = [&out](quint16 value) {
        out.append(char(value & 0xff));
        out.append(char(value >> 8));
    };
    auto le32 = [&out](quint32 value) {
        out.append(char(value & 0xff));
        out.append(char((value >> 8) & 0xff));
        out.append(char((value >> 16) & 0xff));
        out.append(char((value >> 24) & 0xff));
    };

    out.append("RIFF", 4);
    le32(36 + dataSize); // rest of the file after this size field
    out.append("WAVE", 4);

    out.append("fmt ", 4);
    le32(16); // fmt chunk size
    le16(1); // PCM
    le16(1); // mono
    le32(quint32(outRate));
    le32(quint32(outRate) * 2); // byte rate: rate * 1 channel * 2 bytes
    le16(2); // block align
    le16(16); // bits per sample

    out.append("data", 4);
    le32(dataSize);
    out.append(reinterpret_cast<const char *>(mono.constData()), int(dataSize));

    return out;
}

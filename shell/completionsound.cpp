// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "completionsound.h"

#include <QStandardPaths>
#include <QtEndian>

#include <cmath>

namespace
{

constexpr int kSampleRate = 44100;
constexpr double kDuration = 0.55; // s, both tones included

// Two tones a fourth apart, the second a little later and a little
// quieter: an up-hand of a chime, rising where the shutter's click snaps.
// Frequency in Hz, start in s, relative amplitude.
constexpr double kNotes[][3] = {
    { 880.0,   0.000, 1.00 }, // A5
    { 1174.66, 0.130, 0.85 }, // D6
};
constexpr int kNoteCount = int(sizeof(kNotes) / sizeof(kNotes[0]));

constexpr double kDecay = 0.16;     // s, per-tone envelope time constant
constexpr double kAttack = 0.008;   // s from silence to full envelope
constexpr double kSecondHarmonic = 0.25; // of the fundamental, for warmth
constexpr double kPeak = 0.5;       // of full scale, after normalizing —
                                    // a notification, not a shutter click

constexpr double kPi = 3.14159265358979323846;

} // namespace

CompletionSound::CompletionSound(QObject *parent) : QObject(parent) {}

const QByteArray &CompletionSound::effect()
{
    if (m_pcm.isEmpty()) {
        m_pcm = synthesize();
    }
    return m_pcm;
}

void CompletionSound::play()
{
    const QString program = QStandardPaths::findExecutable(QStringLiteral("pw-cat"));
    if (program.isEmpty()) {
        return;
    }

    // A chime still ringing (a run finishing on the heels of another) is
    // replaced by the new one: pw-cat exits within milliseconds of
    // SIGTERM.
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(100)) {
            m_process.kill();
            m_process.waitForFinished(100);
        }
    }

    // "pw-cat --playback --raw ... -" plays raw PCM from stdin on the
    // default output, resampling to whatever the device negotiated.
    m_process.setProgram(program);
    m_process.setArguments(QStringList{
        QStringLiteral("--playback"),
        QStringLiteral("--raw"),
        QStringLiteral("--format"),
        QStringLiteral("s16"),
        QStringLiteral("--rate"),
        QString::number(kSampleRate),
        QStringLiteral("--channels"),
        QStringLiteral("1"),
        QStringLiteral("--latency"),
        QStringLiteral("50ms"),
        QStringLiteral("-"),
    });
    m_process.start();
    if (!m_process.waitForStarted(2000)) {
        return;
    }

    // The whole effect fits into the pipe buffer, so one blocking write
    // hands it over; pw-cat plays on and exits by itself.
    m_process.write(m_pcm);
    m_process.closeWriteChannel();
}

QByteArray CompletionSound::synthesize() const
{
    const int count = int(kDuration * kSampleRate);
    QVector<double> out(count, 0.0);

    for (int note = 0; note < kNoteCount; ++note) {
        const double frequency = kNotes[note][0];
        const double start = kNotes[note][1];
        const double amplitude = kNotes[note][2];
        const int begin = int(start * kSampleRate);
        const double omega = 2.0 * kPi * frequency;
        for (int i = begin; i < count; ++i) {
            const double t = double(i - begin) / kSampleRate;
            // Every tone rings until the effect's end, fading the whole
            // time — the second tone fades over the first's tail, which
            // is what makes the two read as one chime.
            const double envelope = std::exp(-t / kDecay) * qMin(1.0, t / kAttack);
            const double tone = std::sin(omega * t)
                + kSecondHarmonic * std::sin(2.0 * omega * t);
            out[i] += amplitude * envelope * tone;
        }
    }

    double peak = 0.0;
    for (const double sample : out) {
        peak = qMax(peak, qAbs(sample));
    }
    if (peak <= 0.0) {
        peak = 1.0;
    }

    QByteArray pcm;
    pcm.resize(count * int(sizeof(qint16)));
    auto *samples = reinterpret_cast<qint16 *>(pcm.data());
    const double gain = kPeak / peak;
    for (int i = 0; i < count; ++i) {
        samples[i] = qToLittleEndian<qint16>(qint16(qBound(-1.0, out[i] * gain, 1.0) * 32767.0));
    }
    return pcm;
}

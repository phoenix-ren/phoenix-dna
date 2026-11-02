// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "shuttersound.h"

#include <QRandomGenerator>
#include <QStandardPaths>
#include <QtEndian>

#include <cmath>

namespace
{

// The click in one sentence: two transients of noise with a metallic
// resonance, high-passed so they snap instead of thump.
constexpr int kSampleRate = 44100;
constexpr double kDuration = 0.28; // s, both transients included

// A shutter is two curtains. Each transient is a burst of noise whose
// envelope decays exponentially from a near-instant attack, plus a damped
// sine pair at the resonance (and its 1.62x harmonic) that lends the
// metallic snap. The second transient is the louder, brighter one.
struct Transient
{
    double start;      // s after the effect begins
    double amplitude;
    double resonance;  // Hz
    double noiseDecay; // envelope time constant, s
    double ringDecay;  // resonance time constant, s
};

constexpr Transient kTransients[] = {
    {0.004, 0.75, 2600.0, 0.006, 0.010},
    {0.082, 1.00, 3400.0, 0.005, 0.008},
};

constexpr double kAttack = 0.0003;   // s from silence to full envelope
constexpr double kRingHarmonic = 1.62;
constexpr double kHighPassHz = 700.0; // corners off the low thump
constexpr double kPeak = 0.82;        // of full scale, after normalizing

constexpr double kPi = 3.14159265358979323846;

} // namespace

ShutterSound::ShutterSound(QObject *parent) : QObject(parent) {}

void ShutterSound::play()
{
    if (m_pcm.isEmpty()) {
        m_pcm = synthesize();
    }

    const QString program = QStandardPaths::findExecutable(QStringLiteral("pw-cat"));
    if (program.isEmpty()) {
        return;
    }

    // A click still draining (rapid shots) is replaced by the new one:
    // pw-cat exits within milliseconds of SIGTERM.
    if (m_process.state() != QProcess::NotRunning) {
        m_process.terminate();
        if (!m_process.waitForFinished(100)) {
            m_process.kill();
            m_process.waitForFinished(100);
        }
    }

    // "pw-cat --playback --raw ... -" plays raw PCM from stdin on the
    // default output, resampling to whatever the device negotiated. The
    // small latency keeps the click close on the heels of the shot.
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

    // The whole effect (about 24 kB) fits into the pipe buffer, so one
    // blocking write hands it over; pw-cat plays on and exits by itself.
    m_process.write(m_pcm);
    m_process.closeWriteChannel();
}

QByteArray ShutterSound::synthesize() const
{
    const int count = int(kDuration * kSampleRate);
    QVector<double> mix(count, 0.0);

    // A fixed seed keeps the click the same every time - it is an identity,
    // not a coin toss.
    QRandomGenerator noise(7);

    for (const Transient &transient : kTransients) {
        const int begin = int(transient.start * kSampleRate);
        const int length = int(0.12 * kSampleRate);
        const double omega1 = 2.0 * kPi * transient.resonance;
        const double omega2 = omega1 * kRingHarmonic;
        for (int i = 0; i < length && begin + i < count; ++i) {
            const double t = double(i) / kSampleRate;
            const double envelope = std::exp(-t / transient.noiseDecay);
            const double attack = qMin(1.0, t / kAttack);
            const double hiss = noise.generateDouble() * 2.0 - 1.0;
            const double ring = 0.7 * std::sin(omega1 * t) + 0.3 * std::sin(omega2 * t + 1.2345);
            mix[begin + i] += transient.amplitude * attack * envelope *
                              (0.55 * hiss + 0.45 * ring * std::exp(-t / transient.ringDecay));
        }
    }

    // One-pole high-pass: subtracting a low-passed copy of the mix removes
    // the low thump the raw transients carry, keeping the snap.
    const double a = std::exp(-2.0 * kPi * kHighPassHz / kSampleRate);
    double lowpass = 0.0;
    double peak = 0.0;
    QVector<double> out(count);
    for (int i = 0; i < count; ++i) {
        lowpass = a * lowpass + (1.0 - a) * mix[i];
        out[i] = mix[i] - lowpass;
        peak = qMax(peak, qAbs(out[i]));
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

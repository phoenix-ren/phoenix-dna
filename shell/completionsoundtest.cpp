// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

// Offline check of the completion chime: renders the effect the widget
// would stream through pw-cat and checks it is a sound at all — as long as
// the expected effect, not silence, and not a clipped square wave. With an
// output path it also writes the effect as a .wav file, so the chime can
// be heard (or plotted) without the widget.
//
//   completionsoundtest [out.wav]
//   completionsoundtest --play     (plays the effect through pw-cat)

#include "completionsound.h"

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <QProcess>
#include <QtEndian>
#include <cmath>
#include <cstdio>

namespace
{

constexpr int kSampleRate = 44100;
constexpr double kDuration = 0.55; // s; keep in step with completionsound.cpp

void writeWav(const QString &path, const QByteArray &pcm)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        fprintf(stderr, "cannot write %s\n", qPrintable(path));
        return;
    }
    QByteArray header;
    const quint32 byteRate = quint32(kSampleRate * sizeof(qint16));
    header.append("RIFF");
    auto appendU32 = [&header](quint32 v) {
        quint32 le = qToLittleEndian(v);
        header.append(reinterpret_cast<const char *>(&le), 4);
    };
    auto appendU16 = [&header](quint16 v) {
        quint16 le = qToLittleEndian(v);
        header.append(reinterpret_cast<const char *>(&le), 2);
    };
    appendU32(quint32(36 + pcm.size()));
    header.append("WAVEfmt ");
    appendU32(16);            // fmt chunk size
    appendU16(1);             // PCM
    appendU16(1);             // mono
    appendU32(kSampleRate);
    appendU32(byteRate);
    appendU16(2);             // block align
    appendU16(16);            // bits per sample
    header.append("data");
    appendU32(quint32(pcm.size()));
    file.write(header);
    file.write(pcm);
    fprintf(stderr, "wrote %s\n", qPrintable(path));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QStringList args = app.arguments().mid(1);
    CompletionSound sound;
    const QByteArray &pcm = sound.effect();

    // The effect is 0.55 s of mono 16-bit PCM.
    const qsizetype expected = qsizetype(kDuration * kSampleRate) * 2;
    if (pcm.size() != expected) {
        fprintf(stderr, "FAIL: effect is %lld bytes, expected %lld\n",
                static_cast<long long>(pcm.size()), static_cast<long long>(expected));
        return 1;
    }

    // Something audible is in it: a healthy peak, well clear of silence
    // and of the clipping ceiling the normalization is meant to avoid.
    const auto *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    const qsizetype count = pcm.size() / qsizetype(sizeof(qint16));
    qint16 peak = 0;
    for (qsizetype i = 0; i < count; ++i) {
        const qint16 v = qFromLittleEndian<qint16>(samples[i]);
        if (qAbs(int(v)) > qAbs(int(peak))) {
            peak = v;
        }
    }
    const double peakFraction = qAbs(double(peak)) / 32767.0;
    if (peakFraction < 0.35 || peakFraction > 0.95) {
        fprintf(stderr, "FAIL: peak at %.2f of full scale (expected ~0.5)\n",
                peakFraction);
        return 1;
    }
    fprintf(stderr, "PASS: %.2f s of PCM, peak at %.2f of full scale\n",
            double(count) / kSampleRate, peakFraction);

    // An output path: the effect as a .wav file.
    const QString out = args.contains(QStringLiteral("--play"))
        ? QStringLiteral("/tmp/opencode/completion-chime.wav")
        : (args.size() > 0 ? args.last() : QString());
    if (!out.isEmpty() && !out.startsWith(QStringLiteral("--"))) {
        writeWav(out, pcm);
    }

    if (args.contains(QStringLiteral("--play"))) {
        // The effect is small enough to pipe whole; pw-cat plays it and
        // exits.
        QProcess player;
        player.setProgram(QStandardPaths::findExecutable(QStringLiteral("pw-cat")));
        player.setArguments(QStringList{
            QStringLiteral("--playback"), QStringLiteral("--raw"),
            QStringLiteral("--format"), QStringLiteral("s16"),
            QStringLiteral("--rate"), QString::number(kSampleRate),
            QStringLiteral("--channels"), QStringLiteral("1"),
            QStringLiteral("-"),
        });
        player.start();
        if (player.waitForStarted(2000)) {
            player.write(pcm);
            player.closeWriteChannel();
            player.waitForFinished(3000);
            fprintf(stderr, "played the chime\n");
        }
    }
    return 0;
}

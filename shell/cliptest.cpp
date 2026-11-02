// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

// Offline verifier for the transcription path, no microphone involved.
//
//   cliptest input.wav [output.wav]   convert only, write the upload WAV
//   cliptest --stt input.wav          convert and transcribe for real
//
// The input is split into 20 ms blocks and pushed through ClipRecorder the
// way AudioCapture::pcmReady() delivers it — interleaved frames, the format
// latched from the first block — with pw-cat's full-scale start-up burst
// prepended so the head-trimming is exercised as well. Without --stt the
// result is only written to disk (default output: <input>.mono16k.wav in the
// current directory); with --stt it is posted through the live Transcriber
// with the same configuration resolution the plasmoid uses.

#include "cliprecorder.h"
#include "transcriber.h"

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QtEndian>

#include <cstdio>
#include <cstring>

namespace
{

struct WavInfo
{
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;
    QByteArray pcm; // interleaved
};

bool readWav(const QString &path, WavInfo *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    const QByteArray all = file.readAll();
    if (all.size() < 12 || !all.startsWith("RIFF") || all.mid(8, 4) != "WAVE") {
        *error = QStringLiteral("%1 is not a RIFF/WAVE file").arg(path);
        return false;
    }

    quint32 offset = 12;
    bool haveFormat = false;
    bool haveData = false;
    while (offset + 8 <= quint32(all.size())) {
        const auto chunkId = all.mid(int(offset), 4);
        const quint32 size = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar *>(all.constData() + offset + 4));
        const auto *body = reinterpret_cast<const uchar *>(all.constData()) + offset + 8;

        if (chunkId == QByteArrayLiteral("fmt ") && size >= 16) {
            out->channels = qFromLittleEndian<quint16>(body + 2);
            out->sampleRate = qFromLittleEndian<quint32>(body + 4);
            out->bits = qFromLittleEndian<quint16>(body + 14);
            haveFormat = out->channels > 0 && out->sampleRate > 0 && out->bits == 16;
        } else if (chunkId == QByteArrayLiteral("data")) {
            out->pcm = all.mid(int(offset + 8), int(size));
            haveData = true;
        }
        offset += 8 + size + (size & 1); // chunks are word-aligned
    }

    if (!haveFormat || !haveData) {
        *error = QStringLiteral("%1 lacks a 16-bit PCM fmt/data chunk").arg(path);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cliptest"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Feeds a WAV through the dictation clip pipeline; "
                       "optionally transcribes it for real."));
    parser.addHelpOption();
    parser.addOptions({
        {{QStringLiteral("s"), QStringLiteral("stt")},
         QStringLiteral("Send the clip through the transcription API.")},
    });
    parser.addPositionalArgument(QStringLiteral("input"),
                                 QStringLiteral("input WAV file (16-bit PCM)"));
    parser.addPositionalArgument(QStringLiteral("output"),
                                 QStringLiteral("output WAV file "
                                                "(default: <input>.mono16k.wav)"),
                                 QStringLiteral("[output]"));
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        parser.showHelp(1);
    }
    const QString inputPath = args.at(0);
    const QString outputPath =
        args.size() > 1 ? args.at(1) : inputPath + QStringLiteral(".mono16k.wav");

    WavInfo input;
    QString error;
    if (!readWav(inputPath, &input, &error)) {
        fprintf(stderr, "error : %s\n", qPrintable(error));
        return 1;
    }
    const qsizetype frameBytes = qsizetype(input.channels) * 2;
    const qsizetype frameCount = input.pcm.size() / frameBytes;
    printf("input : %d Hz, %d ch, %.2f s, %lld bytes\n", input.sampleRate, input.channels,
           double(frameCount) / input.sampleRate, (long long)input.pcm.size());

    // Emulate the capture: a full-scale burst first (pw-cat does this), then
    // the audio itself in 20 ms blocks.
    ClipRecorder recorder;
    recorder.start();

    QByteArray burst(int(frameBytes * 3), 0);
    auto *burstSamples = reinterpret_cast<qint16 *>(burst.data());
    for (qsizetype i = 0; i < burst.size() / 2; ++i) {
        burstSamples[i] = 32767;
    }
    recorder.append(burst, input.sampleRate, input.channels);

    const qsizetype blockFrames = input.sampleRate / 50;
    for (qsizetype offset = 0; offset < input.pcm.size(); offset += blockFrames * frameBytes) {
        recorder.append(input.pcm.mid(int(offset), int(blockFrames * frameBytes)), input.sampleRate,
                        input.channels);
    }

    const QByteArray wav = recorder.takeWav();
    if (wav.isEmpty()) {
        printf("clip  : empty (only the start-up burst?)\n");
        return 2;
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly)) {
        fprintf(stderr, "error : cannot write %s\n", qPrintable(outputPath));
        return 1;
    }
    output.write(wav);
    output.close();

    // The header ClipRecorder just wrote is trustworthy enough to report
    // from: sample rate at byte 24, data size at byte 40.
    const auto *header = reinterpret_cast<const uchar *>(wav.constData());
    const int outRate = int(qFromLittleEndian<quint32>(header + 24));
    const quint32 outSize = qFromLittleEndian<quint32>(header + 40);
    printf("clip  : %d Hz, 1 ch, %.2f s, %lld bytes\n", outRate, double(outSize) / 2 / outRate,
           (long long)wav.size());

    if (!parser.isSet(QStringLiteral("stt"))) {
        return 0;
    }

    // The real request, exactly as the plasmoid would issue it. The
    // completion handlers must work both from inside exec() (network
    // reply) and before it (a synchronous no-key failure) — hence the
    // explicit done flag, since QCoreApplication::exit() is a no-op
    // before the loop runs.
    bool done = false;
    int resultCode = 0;
    Transcriber transcriber;
    QObject::connect(
        &transcriber, &Transcriber::finished, &app,
        [&app, &done, &resultCode](const QString &text) {
            printf("text  : %s\n",
                   qPrintable(text.isEmpty() ? QStringLiteral("<nothing recognized>") : text));
            fflush(stdout);
            done = true;
            resultCode = 0;
            app.exit(0);
        });
    QObject::connect(&transcriber, &Transcriber::failed, &app,
                     [&app, &done, &resultCode](const QString &message) {
                         fprintf(stderr, "error : %s\n", qPrintable(message));
                         done = true;
                         resultCode = 3;
                         app.exit(3);
                     });
    transcriber.transcribe(wav);
    if (done) {
        // The failure path already ran synchronously.
        return resultCode;
    }
    return app.exec();
}

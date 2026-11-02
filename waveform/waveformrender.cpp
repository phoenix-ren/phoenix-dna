// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <cstdio>

#include "palette.h"
#include "wavreader.h"

/*!
 * \brief Step 1 of the waveform component: an offline .wav → .png renderer.
 *
 * See design_docs/WAVEFORM_COMPONENT_1.md. Even though the eventual display
 * is a 2D bitmap, the waveform collapses into three 1D arrays whose length
 * is the number of horizontal pixels:
 *
 *   1. the minimum sample per pixel (darker blue),
 *   2. the maximum sample per pixel (darker blue),
 *   3. the mean absolute level per pixel, computed as an exponentially
 *      weighted moving average of |x| (lighter blue, symmetric).
 *
 * The image is painted with a painter's algorithm: background, then the
 * min/max envelope, then the lighter mean band on top of it. The palette
 * lives in palette.h, shared with the live renderer.
 *
 * This is deliberately a bespoke test program: it exists to eyeball the DSP
 * against static files; the same three-array scheme drives the live,
 * ring-buffered display (waveformbuffer.h).
 */

namespace
{
using WaveformPalette::kBackground;
using WaveformPalette::kMeanColor;
using WaveformPalette::kPeakColor;

constexpr int kDefaultWidth = 1000;
constexpr int kDefaultHeight = 140;

/*! \brief Default EWMA time constant, in milliseconds, for the mean band. */
constexpr double kDefaultTauMs = 10.0;

/*!
 * \brief Silence floor for the EWMA accumulator (≈ −240 dBFS).
 *
 * The accumulator decays exponentially through silence. Clamping it to zero
 * below this floor halts the decay far above the denormal range, where FPU
 * arithmetic slows to a crawl (see the design document).
 */
constexpr double kDenormalFloor = 1e-12;

/*! \brief Per-pixel waveform data: the three 1D arrays of the design. */
struct ColumnStats
{
    double minimum = 0.0;   // most negative sample in the pixel's time slice
    double maximum = 0.0;   // most positive sample in the same slice
    double meanLevel = 0.0; // EWMA of |x|, averaged over the slice
};

/*!
 * \brief Reduces the sample stream to \a columns per-pixel statistics.
 *
 * The EWMA coefficient must be derived from the sampling frequency: with
 * α = 1 − exp(−1 / (τ · fs)) the accumulator averages over τ *seconds* of
 * audio, so files sampled at different rates smooth identically.
 */
QVector<ColumnStats> computeColumns(const QVector<double> &samples, int sampleRate,
                                    int columns, double tauMs)
{
    const qsizetype total = samples.size();
    QVector<ColumnStats> result(columns);

    const double tauSeconds = tauMs / 1000.0;
    const double alpha = 1.0 - std::exp(-1.0 / (tauSeconds * sampleRate));

    double accumulator = 0.0;
    for (int column = 0; column < columns; ++column) {
        // Exact integer boundaries share the samples out without drift.
        const qsizetype begin = qint64(column) * total / columns;
        const qsizetype end = qint64(column + 1) * total / columns;

        double sum = 0.0;
        double minimum = 0.0;
        double maximum = 0.0;
        qsizetype count = 0;
        for (qsizetype i = begin; i < end; ++i) {
            const double sample = samples[i];
            accumulator = alpha * std::fabs(sample) + (1.0 - alpha) * accumulator;
            if (accumulator < kDenormalFloor) {
                accumulator = 0.0; // dodge denormal-number slowdowns in silence
            }
            sum += accumulator;

            if (count == 0) {
                minimum = sample;
                maximum = sample;
            } else {
                minimum = qMin(minimum, sample);
                maximum = qMax(maximum, sample);
            }
            ++count;
        }

        ColumnStats &stats = result[column];
        stats.minimum = minimum;
        stats.maximum = maximum;
        stats.meanLevel = count > 0 ? sum / count : accumulator;
    }
    return result;
}

/*! \brief Paints the three arrays into a .png, darkest layer first. */
bool renderPng(const QVector<ColumnStats> &columns, int height, const QString &path,
               QString *error)
{
    QImage image(int(columns.size()), height, QImage::Format_ARGB32);
    image.fill(kBackground);

    QPainter painter(&image);
    const double mid = double(height - 1) / 2.0;
    const auto yFor = [mid, height](double level) {
        const double bounded = qBound(-1.0, level, 1.0);
        return qBound(0, int(std::lround(mid - bounded * mid)), height - 1);
    };

    for (int x = 0; x < columns.size(); ++x) {
        const ColumnStats &stats = columns[x];

        // The darker envelope goes down first...
        painter.setPen(kPeakColor);
        painter.drawLine(x, yFor(stats.maximum), x, yFor(stats.minimum));

        // ...then the lighter, symmetric mean band on top of it.
        painter.setPen(kMeanColor);
        painter.drawLine(x, yFor(stats.meanLevel), x, yFor(-stats.meanLevel));
    }
    painter.end();

    if (!image.save(path, "png")) {
        *error = QStringLiteral("QImage::save failed");
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("waveformrender"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Render .wav files as static waveform .png images (waveform component, "
        "step 1 — see design_docs/WAVEFORM_COMPONENT_1.md)."));
    parser.addHelpOption();
    parser.addOptions({
        {QStringLiteral("width"),
         QStringLiteral("Image width in pixels, the n_pixels of the display "
                        "(default: %1).")
             .arg(kDefaultWidth),
         QStringLiteral("pixels")},
        {QStringLiteral("height"),
         QStringLiteral("Image height in pixels (default: %1).").arg(kDefaultHeight),
         QStringLiteral("pixels")},
        {QStringLiteral("tau-ms"),
         QStringLiteral("EWMA time constant of the mean band, in milliseconds "
                        "(default: %1).")
             .arg(kDefaultTauMs),
         QStringLiteral("ms")},
    });
    parser.addPositionalArgument(
        QStringLiteral("wav"),
        QStringLiteral("Input .wav file(s); each renders to <name>_waveform.png "
                       "next to it."),
        QStringLiteral("<wav> [wav ...]"));
    parser.process(app);

    int width = kDefaultWidth;
    int height = kDefaultHeight;
    double tauMs = kDefaultTauMs;
    bool optionsValid = true;
    const auto intOption = [&parser, &optionsValid](const QString &name, int fallback,
                                                    int minimum, int maximum) {
        if (!parser.isSet(name)) {
            return fallback;
        }
        bool ok = false;
        const int value = parser.value(name).toInt(&ok);
        if (!ok || value < minimum || value > maximum) {
            std::fprintf(stderr, "waveformrender: --%s must be an integer between %d and %d\n",
                         qPrintable(name), minimum, maximum);
            optionsValid = false;
            return fallback;
        }
        return value;
    };
    width = intOption(QStringLiteral("width"), width, 8, 100000);
    height = intOption(QStringLiteral("height"), height, 8, 4096);
    if (parser.isSet(QStringLiteral("tau-ms"))) {
        bool ok = false;
        tauMs = parser.value(QStringLiteral("tau-ms")).toDouble(&ok);
        if (!ok || tauMs <= 0.0 || tauMs > 10000.0) {
            std::fprintf(stderr, "waveformrender: --tau-ms must be a number between 0 (exclusive) and 10000\n");
            optionsValid = false;
        }
    }
    if (!optionsValid) {
        return 2;
    }

    const QStringList inputs = parser.positionalArguments();
    if (inputs.isEmpty()) {
        std::fprintf(stderr, "waveformrender: no input files given\n");
        parser.showHelp(2);
    }

    int failures = 0;
    for (const QString &input : inputs) {
        QString error;
        WavData wav;
        if (!WavReader::read(input, &wav, &error)) {
            std::fprintf(stderr, "waveformrender: %s: %s\n", qPrintable(input), qPrintable(error));
            ++failures;
            continue;
        }
        if (wav.mono.isEmpty()) {
            std::fprintf(stderr, "waveformrender: %s: no audio data\n", qPrintable(input));
            ++failures;
            continue;
        }

        // Every column needs at least one sample; cap so it always gets one.
        const int columns = qMin<qsizetype>(width, wav.mono.size());
        const QVector<ColumnStats> stats =
            computeColumns(wav.mono, wav.sampleRate, columns, tauMs);

        const QFileInfo info(input);
        const QString output = info.dir().filePath(
            info.completeBaseName() + QStringLiteral("_waveform.png"));
        if (!renderPng(stats, height, output, &error)) {
            std::fprintf(stderr, "waveformrender: %s: %s\n", qPrintable(output), qPrintable(error));
            ++failures;
            continue;
        }

        const double seconds = double(wav.mono.size()) / wav.sampleRate;
        // A plain stdout line, not qInfo: this is the tool's result, and the
        // ambient logging rules on some systems swallow Info-level messages.
        std::printf("%s: %d x %d px, %.2f s, %s, %d Hz, %d ch -> %s\n",
                    qPrintable(input), columns, height, seconds, qPrintable(wav.format),
                    wav.sampleRate, wav.channelCount, qPrintable(output));
    }

    return failures == 0 ? 0 : 1;
}

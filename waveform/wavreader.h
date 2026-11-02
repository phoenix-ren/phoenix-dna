// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QString>
#include <QVector>

/*!
 * \brief A parsed WAV file: header facts plus mono samples in [-1, 1].
 */
struct WavData
{
    int sampleRate = 0;
    int channelCount = 0;
    QString format;       // e.g. "16-bit PCM"
    QVector<double> mono; // channels mixed down by averaging
};

namespace WavReader
{
/*! \brief Reads a RIFF/WAVE file, mixing all channels down to mono. */
bool read(const QString &path, WavData *out, QString *error);
} // namespace WavReader

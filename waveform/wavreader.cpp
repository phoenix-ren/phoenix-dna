// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "wavreader.h"

#include <QFile>
#include <QtEndian>

#include <cstring>

namespace
{
double decodePcm(const uchar *sample, int bits)
{
    switch (bits) {
    case 8:
        // 8-bit WAV is unsigned with a 128 midpoint.
        return (int(*sample) - 128) / 128.0;
    case 16:
        return qint16(qFromLittleEndian<quint16>(sample)) / 32768.0;
    case 24: {
        qint32 value = quint32(sample[0]) | quint32(sample[1]) << 8 | quint32(sample[2]) << 16;
        if (value & 0x800000) {
            value -= 0x1000000;
        }
        return value / 8388608.0;
    }
    default:
        return qint32(qFromLittleEndian<quint32>(sample)) / 2147483648.0;
    }
}

double decodeFloat(const uchar *sample, int bytes)
{
    if (bytes == 4) {
        float value = 0.0f;
        memcpy(&value, sample, 4);
        return double(value);
    }
    double value = 0.0;
    memcpy(&value, sample, 8);
    return value;
}
} // namespace

bool WavReader::read(const QString &path, WavData *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot open: %1").arg(file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    const auto *p = reinterpret_cast<const uchar *>(bytes.constData());
    const qsizetype size = bytes.size();

    const auto u16 = [p](qsizetype offset) -> int {
        return int(qFromLittleEndian<quint16>(p + offset));
    };
    const auto u32 = [p](qsizetype offset) -> quint32 {
        return qFromLittleEndian<quint32>(p + offset);
    };

    if (size < 12 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) {
        *error = QStringLiteral("not a RIFF/WAVE file");
        return false;
    }

    bool haveFormat = false;
    int formatTag = 0;
    int bits = 0;
    qsizetype dataOffset = -1;
    qsizetype dataSize = 0;

    // Walk the chunks; they are word-aligned, so odd sizes carry a pad byte.
    qsizetype position = 12;
    while (position + 8 <= size) {
        const uchar *id = p + position;
        quint32 chunkSize = u32(position + 4);
        const qsizetype body = position + 8;

        bool truncated = false;
        if (body + qint64(chunkSize) > size) {
            // A chunk overrunning the file means an interrupted recording;
            // keep whatever bytes are actually there.
            chunkSize = quint32(size - body);
            truncated = true;
        }

        if (memcmp(id, "fmt ", 4) == 0 && chunkSize >= 16) {
            out->channelCount = u16(body + 2);
            out->sampleRate = int(u32(body + 4));
            formatTag = u16(body);
            bits = u16(body + 14);
            // WAVE_FORMAT_EXTENSIBLE keeps the real tag at the start of the
            // SubFormat GUID, 24 bytes into an extended fmt chunk.
            if (formatTag == 0xFFFE && chunkSize >= 40) {
                formatTag = u16(body + 24);
            }
            haveFormat = true;
        } else if (memcmp(id, "data", 4) == 0) {
            dataOffset = body;
            dataSize = chunkSize;
        }

        if (truncated) {
            break;
        }
        position = body + chunkSize + (chunkSize & 1);
    }

    if (!haveFormat) {
        *error = QStringLiteral("missing or short fmt chunk");
        return false;
    }
    if (dataOffset < 0 || dataSize == 0) {
        *error = QStringLiteral("missing or empty data chunk");
        return false;
    }
    if (out->sampleRate <= 0 || out->channelCount <= 0 || out->channelCount > 256) {
        *error = QStringLiteral("nonsensical fmt chunk (rate %1, %2 channels)")
                     .arg(out->sampleRate)
                     .arg(out->channelCount);
        return false;
    }

    const bool isFloat = formatTag == 3; // 1 = PCM, 3 = IEEE float
    const bool bitsOk = isFloat ? (bits == 32 || bits == 64)
                                : (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!bitsOk) {
        *error = QStringLiteral("unsupported format: tag %1, %2-bit").arg(formatTag).arg(bits);
        return false;
    }
    out->format = QStringLiteral("%1-bit %2").arg(bits).arg(isFloat ? "float" : "PCM");

    const int sampleBytes = bits / 8;
    const int frameSize = sampleBytes * out->channelCount;
    const qsizetype frames = dataSize / frameSize; // drop a trailing partial frame
    out->mono.resize(frames);

    for (qsizetype frame = 0; frame < frames; ++frame) {
        const uchar *f = p + dataOffset + frame * frameSize;
        double mixed = 0.0;
        for (int channel = 0; channel < out->channelCount; ++channel) {
            const uchar *sample = f + channel * sampleBytes;
            mixed += isFloat ? decodeFloat(sample, sampleBytes) : decodePcm(sample, bits);
        }
        out->mono[frame] = mixed / out->channelCount;
    }
    return true;
}

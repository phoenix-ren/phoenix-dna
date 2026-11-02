// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "windowpicture.h"

#include <cstring>

namespace
{
//: The largest picture we believe in: a screen of 16 million pixels with
//: four bytes each. Anything bigger is a broken answer, not a picture.
const qint64 kMaxPictureBytes = 64ll * 1024ll * 1024ll;

//: The largest width and height we believe in.
const int kMaxDimension = 16384;
} // namespace

namespace WindowPicture
{
qint64 size(const QVariantMap &info)
{
    const int width = info.value(QStringLiteral("width")).toInt();
    const int height = info.value(QStringLiteral("height")).toInt();
    const int stride = info.value(QStringLiteral("stride")).toInt();

    if (width <= 0 || height <= 0 || stride <= 0) {
        return 0;
    }
    if (width > kMaxDimension || height > kMaxDimension || stride > kMaxDimension * 8) {
        return 0;
    }

    const qint64 bytes = qint64(stride) * qint64(height);
    if (bytes <= 0 || bytes > kMaxPictureBytes) {
        return 0;
    }
    return bytes;
}

bool isComplete(const QVariantMap &info, const QByteArray &raw)
{
    const qint64 expected = size(info);
    return expected > 0 && raw.size() >= expected;
}

bool decode(const QVariantMap &info, const QByteArray &raw, QImage &image)
{
    const int width = info.value(QStringLiteral("width")).toInt();
    const int height = info.value(QStringLiteral("height")).toInt();
    const int stride = info.value(QStringLiteral("stride")).toInt();
    const auto format = static_cast<QImage::Format>(info.value(QStringLiteral("format")).toUInt());

    if (size(info) == 0) {
        return false;
    }

    // The picture has to be there in full before a single pixel is read
    // from it: a half-read picture would send the copy past the end of
    // the buffer.
    if (!isComplete(info, raw)) {
        return false;
    }

    image = QImage(width, height, format);
    if (image.isNull()) {
        return false;
    }

    // KWin lays the lines out exactly as QImage does for the same width
    // and format. When the two disagree the picture cannot be copied
    // over line by line.
    if (image.bytesPerLine() != stride) {
        image = QImage();
        return false;
    }

    for (int y = 0; y < height; ++y) {
        std::memcpy(image.scanLine(y), raw.constData() + qint64(y) * qint64(stride), size_t(stride));
    }

    // The pixels are in the resolution of the screen; the scale says how
    // large the window was on it.
    const double scale = info.value(QStringLiteral("scale")).toDouble();
    if (scale > 0.0) {
        image.setDevicePixelRatio(scale);
    }

    return true;
}
}

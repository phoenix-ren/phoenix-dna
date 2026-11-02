// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QByteArray>
#include <QImage>
#include <QVariantMap>

/*!
 * \brief The picture KWin writes into a pipe.
 *
 * KWin's screenshot service answers its call with the geometry of the
 * picture - its width, its height, the length of a line and the pixel
 * format - and writes the pixels themselves into a pipe. The two do not
 * arrive together: the answer comes first, the pixels follow in pieces.
 *
 * This turns them into an image, and - just as important - says when
 * there is not enough of the picture yet to look at it. Reading a
 * picture that is only half there sends the decoder past the end of the
 * buffer.
 */
namespace WindowPicture
{
/*! \brief How many bytes the picture \a info describes has.
 *
 * Zero when \a info does not describe a picture at all. */
qint64 size(const QVariantMap &info);

/*! \brief Whether \a raw holds the whole picture \a info describes.
 *
 * The picture is only complete once it has as many bytes as its
 * geometry asks for; until then it must not be looked at. */
bool isComplete(const QVariantMap &info, const QByteArray &raw);

/*! \brief Turns the picture into an image.
 *
 * Returns false when the geometry and the pixels do not fit together -
 * the picture is then incomplete or was not written by KWin at all. */
bool decode(const QVariantMap &info, const QByteArray &raw, QImage &image);
}

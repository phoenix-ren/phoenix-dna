// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "lucideicons.h"

#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QScreen>
#include <QSvgRenderer>

namespace {

/*! The id the image provider answers to. */
const QString ProviderId = QStringLiteral("phoenixlucide");

/*! Where the drawings are, in the theme module's resources.
 *
 *  The prefix the build gives holds the path inside the resource system;
 *  the colon in front of it is what makes it one. */
const QString ResourceDir = QStringLiteral(":") + QStringLiteral(PHOENIX_LUCIDE_RESOURCE_DIR);

/*! \brief The pixel ratio of the screen the icons are drawn on.
 *
 *  The icons are drawn at the size they are shown at, times this: on a
 *  scaled display a drawing rendered at its logical size would be
 *  enlarged by the screen and come out soft. */
qreal screenRatio()
{
    if (const QScreen *screen = QGuiApplication::primaryScreen()) {
        return screen->devicePixelRatio();
    }
    return 1.0;
}

/*! \brief The colour the request carries, read back from its own digits.
 *
 *  The digits are the colour's own components, in the order they are
 *  written: red, green, blue and the alpha. */
QColor colorFromDigits(const QString &digits)
{
    if (digits.size() != 8) {
        return {};
    }
    return QColor::fromRgba(digits.toUInt(nullptr, 16));
}

/*! \brief The drawing named \a name, with its ink set to \a color.
 *
 *  Lucide's drawings say "currentColor" where the ink goes; no renderer
 *  resolves that word on its own, so it is replaced by the colour asked
 *  for before the drawing is rendered. */
QByteArray svgBytes(const QString &name, const QColor &color)
{
    QFile file(ResourceDir + QLatin1Char('/') + name + QLatin1String(".svg"));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QByteArray bytes = file.readAll();
    bytes.replace("currentColor", color.name(QColor::HexRgb).toUtf8());
    return bytes;
}

/*! \brief Draws one of the app's icons, in the ink the request carries.
 *
 *  The request says which drawing and in which colour: the colour's own
 *  digits, then the name of the drawing, separated by a slash --
 *  "e8e8e8ff/mic-audio-lines".
 */
class LucideImageProvider : public QQuickImageProvider
{
public:
    LucideImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
    {
        *size = QSize();

        const int separator = id.indexOf(QLatin1Char('/'));
        if (separator < 0) {
            return {};
        }

        const QColor color = colorFromDigits(id.left(separator));
        const QString name = id.mid(separator + 1);
        if (!color.isValid() || name.isEmpty()) {
            return {};
        }

        QSize target = requestedSize;
        if (target.width() <= 0 || target.height() <= 0) {
            target = QSize(24, 24);
        }

        // The same drawing is asked for again and again — every window
        // that shows it, every time it is laid out again — so what has
        // been drawn is kept.
        const QString key = QStringLiteral("%1/%2/%3x%4")
                                .arg(name, color.name(QColor::HexArgb))
                                .arg(target.width())
                                .arg(target.height());
        const auto drawn = m_drawn.constFind(key);
        if (drawn != m_drawn.constEnd()) {
            *size = drawn->size();
            return *drawn;
        }

        const qreal ratio = screenRatio();
        QImage image(target * ratio, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        image.setDevicePixelRatio(ratio);

        QSvgRenderer renderer(svgBytes(name, color));
        if (!renderer.isValid()) {
            return {};
        }

        QPainter painter(&image);
        renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(image.size())));

        // A colour may be half transparent, and an SVG colour cannot say
        // so: the drawing is inked in the colour's own hue and the
        // transparency is laid over it afterwards.
        if (color.alpha() < 255) {
            fade(image, color.alpha());
        }

        *size = image.size();
        m_drawn.insert(key, image);
        return image;
    }

private:
    /*! \brief Scales the transparency of every pixel of \a image.
     *
     *  The image is premultiplied, so the colour of a pixel is scaled
     *  along with its transparency. */
    static void fade(QImage &image, int alpha)
    {
        for (int y = 0; y < image.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const QRgb pixel = line[x];
                line[x] = qRgba(qRed(pixel) * alpha / 255,
                                qGreen(pixel) * alpha / 255,
                                qBlue(pixel) * alpha / 255,
                                qAlpha(pixel) * alpha / 255);
            }
        }
    }

    /*! What has been drawn already, by drawing, colour and size. */
    QHash<QString, QImage> m_drawn;
};

}

LucideIcons::LucideIcons(QObject *parent) : QObject(parent) {}

QString LucideIcons::url(const QString &name, const QColor &color) const
{
    if (name.isEmpty()) {
        return {};
    }

    ensureProvider();

    // The ink travels with the request: the drawing says "currentColor"
    // where the ink goes, and only the one that draws it knows in which
    // colour.
    return QStringLiteral("image://%1/%2/%3")
        .arg(ProviderId,
             QStringLiteral("%1").arg(color.rgba(), 8, 16, QLatin1Char('0')),
             name);
}

void LucideIcons::ensureProvider() const
{
    if (m_providerAdded) {
        return;
    }

    QQmlEngine *engine = nullptr;
    if (QQmlContext *context = QQmlEngine::contextForObject(this)) {
        engine = context->engine();
    }
    if (!engine) {
        return;
    }

    engine->addImageProvider(ProviderId, new LucideImageProvider);
    m_providerAdded = true;
}

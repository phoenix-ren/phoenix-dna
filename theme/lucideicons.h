// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief The app's own icon set: Lucide's line drawings, in the colour of
 * the place they stand in.
 *
 * The desktop's icon theme is not the app's: the buttons of the panel
 * widget and of the app's own windows are drawn in the app's palette, and
 * an icon from the theme carries its own colours with it — the highlight
 * colour of a checked button would leave the icon behind. So the app
 * brings its own set: Lucide's drawings, which are line art in a single
 * ink, inked in whatever colour the caller asks for.
 *
 * Reached from QML as \c LucideIcons. Its url() hands out the address of
 * one of those drawings:
 *
 * \code
 * LucideIcons.url("mic-audio-lines", PhoenixTheme.textColor)
 * \endcode
 *
 * The address is the one of an image the engine draws through the
 * provider this class puts in place: the name of the drawing and the
 * colour to ink it in travel with the request, and what comes back is the
 * drawing itself, at the size the caller wants it.
 *
 * The drawings are Lucide's, in Lucide's own licence; they are bundled
 * with the theme module (icons/lucide/) and read from its resources.
 */
class LucideIcons : public QObject
{
    Q_OBJECT
    QML_SINGLETON
    QML_NAMED_ELEMENT(LucideIcons)

public:
    explicit LucideIcons(QObject *parent = nullptr);

    /*! \brief The address of the drawing named \a name, inked in \a color.
     *
     *  The empty string is returned for an empty name: an icon that is
     *  not there is an icon that is not drawn, and a button without an
     *  icon is a button that still works. */
    Q_INVOKABLE QString url(const QString &name, const QColor &color) const;

private:
    /*! \brief Puts the image provider in place, once per engine.
     *
     *  The provider belongs to the engine that draws the icons, and the
     *  engine is only known once the singleton is used: the singleton
     *  itself is created by it. */
    void ensureProvider() const;

    mutable bool m_providerAdded = false;
};

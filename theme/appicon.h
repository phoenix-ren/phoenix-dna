// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <QWindow>

/*!
 * \brief The app's own icon, put on its windows.
 *
 * The desktop's icon theme is not the app's: the windows of an app show
 * whatever the theme has for them, and that has nothing to do with the
 * app itself. So the app brings its own mark — the avatar — and puts it
 * on every window it opens, so a window is recognisable as this app's
 * wherever it is opened from.
 *
 * QML has no window-icon API: \c QIcon is not exposed as a QML type,
 * and \c QWindow's icon is a C++-only property. So the setting of it
 * is done here, from C++, and the windows ask for it.
 *
 * Reached from QML as \c AppIcon:
 *
 * \code
 * AppIcon.setWindowIcon(window)
 * \endcode
 *
 * The picture is bundled with this module and read from its resources,
 * so the windows carry the icon with them wherever the app is installed.
 */
class AppIcon : public QObject
{
    Q_OBJECT
    QML_SINGLETON
    QML_NAMED_ELEMENT(AppIcon)

public:
    explicit AppIcon(QObject *parent = nullptr);

    /*! \brief Puts the app's icon on \a window.
     *
     * \a window is the window to put it on, as the object it is - a
     * QWindow, which is what a QML Window is. It is taken as a plain
     * object because QML cannot hand a \c QWindow over: the type is not
     * among the ones QML knows how to pass.
     *
     * A window that is not there, or is not a window at all, is left
     * alone: the icon is a nicety, and a window that has gone is not
     * worth an error. */
    Q_INVOKABLE void setWindowIcon(QObject *window);

    /*! \brief The address of the app's icon, for the places that draw it
     *  themselves rather than handing it to the windowing system. */
    Q_INVOKABLE QString url() const;
};

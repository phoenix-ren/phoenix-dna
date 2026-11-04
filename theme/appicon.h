// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <QWindow>

#include <functional>

/*!
 * \brief The app's own name and icon, as the windowing system sees them.
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
 * The taskbar's own icon is a different thing, and a desktop file: on
 * Wayland a window names the application it belongs to by its app id,
 * and the taskbar looks that name up among the installed desktop files
 * to find the icon to show. A window that carries no name of its own
 * falls back to whatever the shell can guess — which is not this app's
 * mark. So the app names itself here too, with the name of the desktop
 * file that carries the mark.
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

    /*! \brief The name the app's windows carry, and the name of the
     *  desktop file that carries the app's icon.
     *
     * This is the app id of the windows on Wayland: the taskbar looks it
     * up among the installed desktop files to find the icon to show. */
    Q_INVOKABLE static QString desktopFileName();

    /*! \brief Shows \a window, under the app's own name.
     *
     * The name a window carries is read when its shell surface is made,
     * and that happens as the window is shown. So the name is put in
     * place first, the window is shown under it, and the name is put
     * back afterwards - it is the whole process's, and an applet lives
     * inside plasmashell, whose own windows must keep their own name.
     *
     * \a window is the window to show, as the object it is: a QWindow,
     * which is what a QML Window is. It is taken as a plain object
     * because QML cannot hand a \c QWindow over - the type is not among
     * the ones QML knows how to pass.
     *
     * A window that is not there, or is not a window at all, is left
     * alone: the icon is a nicety, and a window that has gone is not
     * worth an error. */
    Q_INVOKABLE static void showWindow(QObject *window);

    /*! \brief The name the app's windows carry. */
    Q_INVOKABLE static QString applicationName();

private:
    /*! \brief Puts the app's name in place for as long as it takes to
     *  make \a window's shell surface, and takes it away again. */
    static void withApplicationName(const std::function<void()> &make);
};

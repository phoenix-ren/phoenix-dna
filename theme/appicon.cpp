// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "appicon.h"

#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QPixmap>
#include <QWindow>

namespace {
/*! \brief Where the app's icon is, in the theme module's resources.
 *
 *  The prefix the build gives holds the path inside the resource system;
 *  the colon in front of it is what makes it one. */
const QString IconPath =
    QStringLiteral(":") + QStringLiteral(PHOENIX_APPICON_RESOURCE_DIR);

/*! \brief The app's icon, read from the module's own resources.
 *
 *  The picture is read here, inside the module that carries it: the
 *  resources of a QML module live in its own library, and only code
 *  inside that library can open them by their path. Handing a
 *  \c qrc: address to the QML engine instead would not find it - the
 *  engine's own resource root does not hold the module's files.
 *
 *  The picture is read once and kept: an icon is asked for once per
 *  window, and reading the file each time would be wasteful. */
QIcon appIcon()
{
    static const QIcon icon = []() {
        QFile file(IconPath + QStringLiteral("/phoenix-dna-avatar.png"));
        if (!file.open(QIODevice::ReadOnly)) {
            return QIcon();
        }
        QPixmap pixmap;
        if (!pixmap.loadFromData(file.readAll(), "PNG")) {
            return QIcon();
        }
        return QIcon(pixmap);
    }();
    return icon;
}
} // namespace

AppIcon::AppIcon(QObject *parent)
    : QObject(parent)
{
}

void AppIcon::setWindowIcon(QObject *window)
{
    // The window comes as a plain object, because QML cannot hand a
    // QWindow over: the type is not among the ones it knows how to pass.
    auto *win = qobject_cast<QWindow *>(window);
    if (!win) {
        return;
    }
    win->setIcon(appIcon());
}

QString AppIcon::url() const
{
    return IconPath + QStringLiteral("/phoenix-dna-avatar.png");
}

QString AppIcon::desktopFileName()
{
    // The name of the app's desktop entry, without the suffix: that is
    // what a window names itself with, and what the taskbar looks up
    // among the installed desktop files to find the icon to show.
    //
    // It is the same name the applet itself is known by, so the entry
    // and the widget in the panel agree.
    return QStringLiteral(PHOENIX_APPICON_DESKTOP_FILE);
}

void AppIcon::showWindow(QObject *window)
{
    // The window comes as a plain object, because QML cannot hand a
    // QWindow over: the type is not among the ones it knows how to pass.
    auto *win = qobject_cast<QWindow *>(window);
    if (!win) {
        return;
    }
    // The window is shown under the app's name: the shell surface, and
    // with it the app id, is made as the window comes up.
    withApplicationName([win]() { win->show(); });
}

void AppIcon::withApplicationName(const std::function<void()> &make)
{
    // The name a window carries is read when its shell surface is made.
    // So the name is put in place first, the surface is made under it,
    // and the name is put back afterwards - it is the whole process's,
    // and an applet lives inside plasmashell, whose own windows must keep
    // their own name. The windows that are already up keep theirs either
    // way - theirs was read when they were made.
    const QString shellName = QGuiApplication::desktopFileName();
    QGuiApplication::setDesktopFileName(desktopFileName());
    make();
    QGuiApplication::setDesktopFileName(shellName);
}

QString AppIcon::applicationName()
{
    return QGuiApplication::desktopFileName();
}

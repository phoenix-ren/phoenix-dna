// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "appicon.h"

#include <QFile>
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

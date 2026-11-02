// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "pickerhoverservice.h"
#include "windowscreenshotpicker.h"

#include <QDBusConnection>

namespace
{
//: The bus name and object the KWin script calls.
const QString kServiceName = QStringLiteral("org.phoenixdna.agent.Picker");
const QString kObjectPath = QStringLiteral("/org/phoenixdna/Picker");
} // namespace

PickerHoverService::PickerHoverService(WindowScreenshotPicker *picker, QObject *parent)
    : QObject(parent), m_picker(picker)
{
}

bool PickerHoverService::registerOnBus()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(kObjectPath, this, QDBusConnection::ExportScriptableContents)) {
        return false;
    }
    if (!bus.registerService(kServiceName)) {
        bus.unregisterObject(kObjectPath);
        return false;
    }
    return true;
}

void PickerHoverService::unregisterFromBus()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.unregisterService(kServiceName);
    bus.unregisterObject(kObjectPath);
}

void PickerHoverService::hoveredWindow(const QString &uuid, const QString &title)
{
    if (m_picker) {
        m_picker->setHoveredWindow(uuid, title);
    }
}

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "windowscreenshotpicker.h"
#include "pickerhoverservice.h"

#include <KGlobalAccel>
#include <KLocalizedString>

#include <QAction>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeySequence>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

#include <cstring>
#include <unistd.h>

namespace
{
//: KWin's main interface: the window under the pointer, the window the
//: user picks and the window that is highlighted.
const QString kKwinService = QStringLiteral("org.kde.KWin");
const QString kKwinPath = QStringLiteral("/KWin");
const QString kKwinInterface = QStringLiteral("org.kde.KWin");

//: KWin's script engine: the hover script is loaded from here.
const QString kScriptingPath = QStringLiteral("/Scripting");
const QString kScriptingInterface = QStringLiteral("org.kde.kwin.Scripting");
const QString kScriptInterface = QStringLiteral("org.kde.kwin.Script");
const QString kScriptPluginName = QStringLiteral("phoenixdna-window-picker");

//: KWin's screenshot service: it writes the picture into a pipe we hand it.
const QString kScreenshotService = QStringLiteral("org.kde.KWin.ScreenShot2");
const QString kScreenshotPath = QStringLiteral("/org/kde/KWin/ScreenShot2");
const QString kScreenshotInterface = QStringLiteral("org.kde.KWin.ScreenShot2");

//: KWin's highlight effect: it frames the window that would be captured.
const QString kHighlightService = QStringLiteral("org.kde.KWin.HighlightWindow");
const QString kHighlightPath = QStringLiteral("/org/kde/KWin/HighlightWindow");
const QString kHighlightInterface = QStringLiteral("org.kde.KWin.HighlightWindow");

//: How long the highlight is left to fade out before the picture is
//: taken - the picture must show the window as it is, not as the effect
//: had it.
const int kHighlightFadeMs = 250;

//: The script that reports the window under the pointer.
//
// A declarative (QML) KWin script: it polls the pointer on a timer and
// tells the widget which window it is over, so the widget can highlight
// it. The script is written to the cache directory when the picker
// starts.
const char kHoverScript[] = R"QML(
// Phoenix DNA: tell the widget which window the mouse is over.
//
// The widget draws the highlight over that window, so the user can see
// what a click would capture.

import QtQuick
import org.kde.kwin as KWin

Item {
    id: root

    Timer {
        interval: 80
        repeat: true
        running: true
        onTriggered: root.poll()
    }

    KWin.DBusCall {
        id: report
        service: "org.phoenixdna.agent.Picker"
        path: "/org/phoenixdna/Picker"
        dbusInterface: "org.phoenixdna.agent.Picker"
        method: "hoveredWindow"
    }

    function poll() {
        const windows = KWin.Workspace.windowAt(KWin.Workspace.cursorPos)
        report.arguments = windows.length > 0 ? [windows[0].internalId.toString()] : [""]
        report.call()
    }

    Component.onCompleted: poll()
}
)QML";
} // namespace

WindowScreenshotPicker::WindowScreenshotPicker(QObject *parent)
    : QObject(parent), m_hoverService(new PickerHoverService(this, this))
{
    m_captureDelay = new QTimer(this);
    m_captureDelay->setSingleShot(true);
    connect(m_captureDelay, &QTimer::timeout, this, [this] { captureWindow(m_pendingUuid); });

    // The script reports the window under the pointer over D-Bus. Without
    // the service (a second panel with the widget loaded) the picker
    // still works, only without the highlight.
    if (!m_hoverService->registerOnBus()) {
        qWarning() << "The window picker's hover service could not register on the bus"
                   << "- the highlight over the target window is not available";
    }

    // The shortcut that starts the picker. KGlobalAccel owns the binding:
    // it shows up in System Settings -> Shortcuts, is announced over
    // D-Bus to kglobalacceld and survives restarts. The sequence below is
    // only the first-run default; the Autoloading flag keeps whatever the
    // user re-bound it to later.
    m_shortcut = new QAction(this);
    m_shortcut->setObjectName(QStringLiteral("screenshot-window"));
    m_shortcut->setText(i18n("Screenshot a window"));
    m_shortcut->setProperty("componentName", QStringLiteral("org.phoenixdna.agent"));
    m_shortcut->setProperty("componentDisplayName", QStringLiteral("Phoenix DNA"));
    const QList<QKeySequence> defaultHotkey{QKeySequence(QStringLiteral("Meta+Alt"))};
    KGlobalAccel::self()->setDefaultShortcut(m_shortcut, defaultHotkey);
    KGlobalAccel::self()->setShortcut(m_shortcut, defaultHotkey);
    connect(m_shortcut, &QAction::triggered, this, &WindowScreenshotPicker::start);

    // A re-binding in System Settings must be reflected wherever the QML
    // side shows the shortcut.
    connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, this,
            [this](QAction *action, const QKeySequence &) {
                if (action == m_shortcut) {
                    Q_EMIT shortcutChanged();
                }
            });
}

WindowScreenshotPicker::~WindowScreenshotPicker()
{
    stopHoverScript();
    if (m_hoverService) {
        m_hoverService->unregisterFromBus();
    }
}

QString WindowScreenshotPicker::shortcut() const
{
    // Report the live binding rather than the default: it stays truthful
    // after the user re-binds the hotkey, and reads empty when
    // KGlobalAccel could not register anything.
    const QList<QKeySequence> shortcuts = KGlobalAccel::self()->shortcut(m_shortcut);
    for (const QKeySequence &shortcut : shortcuts) {
        if (!shortcut.isEmpty()) {
            return shortcut.toString(QKeySequence::NativeText);
        }
    }
    return QString();
}

void WindowScreenshotPicker::start()
{
    if (m_picking) {
        return;
    }

    m_picking = true;
    Q_EMIT pickingChanged();

    // Follow the pointer, so the user sees which window a click would
    // capture.
    trackPointer();

    // KWin's window selector takes the pointer over and waits for the
    // click that picks the window.
    requestPickedWindow();
}

void WindowScreenshotPicker::trackPointer()
{
    startHoverScript();
}

void WindowScreenshotPicker::setHoveredWindow(const QString &uuid)
{
    if (uuid != m_hoveredWindow) {
        m_hoveredWindow = uuid;
        Q_EMIT hoveredWindowChanged(uuid);
    }

    if (!m_picking) {
        return;
    }
    highlightWindow(uuid);
}

void WindowScreenshotPicker::requestPickedWindow()
{
    // queryWindowInfo() is KWin's "the user picks a window" call: it
    // grabs the pointer and answers with the window that was clicked.
    QDBusMessage message = QDBusMessage::createMethodCall(kKwinService, kKwinPath, kKwinInterface,
                                                          QStringLiteral("queryWindowInfo"));
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            &WindowScreenshotPicker::onWindowPicked);
}

void WindowScreenshotPicker::onWindowPicked(QDBusPendingCallWatcher *watcher)
{
    watcher->deleteLater();

    const QDBusReply<QVariantMap> reply = *watcher;
    if (!reply.isValid()) {
        // Escape or the right mouse button: the pick was cancelled.
        finishPicking();
        return;
    }

    const QVariantMap info = reply.value();
    m_pendingUuid = info.value(QStringLiteral("uuid")).toString();
    m_pendingTitle = info.value(QStringLiteral("caption")).toString();
    if (m_pendingUuid.isEmpty()) {
        finishPicking();
        return;
    }

    // The highlight fades out over a moment; the picture is taken once it
    // is gone, so it shows the window as it really looks.
    highlightWindow(QString());
    m_captureDelay->start(kHighlightFadeMs);
}

void WindowScreenshotPicker::captureWindow(const QString &uuid)
{
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        finishPicking();
        Q_EMIT failed(i18n("The screenshot could not be taken: no pipe for the picture."));
        return;
    }

    // KWin writes the picture into the pipe. The descriptor that travels
    // over the bus is a duplicate, so ours can go already.
    QDBusUnixFileDescriptor descriptor(fds[1]);
    ::close(fds[1]);

    QVariantMap options;
    options.insert(QStringLiteral("include-decoration"), false);
    options.insert(QStringLiteral("include-shadow"), false);

    QDBusMessage message = QDBusMessage::createMethodCall(
        kScreenshotService, kScreenshotPath, kScreenshotInterface, QStringLiteral("CaptureWindow"));
    message << uuid << options << QVariant::fromValue(descriptor);

    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            &WindowScreenshotPicker::onCaptureStarted);

    // The picture itself arrives through the pipe.
    m_picture.clear();
    m_pipeFd = fds[0];
    m_pipeNotifier = new QSocketNotifier(fds[0], QSocketNotifier::Read, this);
    connect(m_pipeNotifier, &QSocketNotifier::activated, this,
            &WindowScreenshotPicker::readPicture);
}

void WindowScreenshotPicker::onCaptureStarted(QDBusPendingCallWatcher *watcher)
{
    watcher->deleteLater();

    const QDBusReply<QVariantMap> reply = *watcher;
    if (!reply.isValid()) {
        closePipe();
        finishPicking();
        Q_EMIT failed(i18n("The screenshot could not be taken."));
        return;
    }

    m_captureInfo = reply.value();
    savePictureIfReady();
}

void WindowScreenshotPicker::readPicture()
{
    char buffer[64 * 1024];
    const ssize_t count = ::read(m_pipeFd, buffer, sizeof(buffer));
    if (count > 0) {
        m_picture.append(buffer, int(count));
        return;
    }
    if (count < 0) {
        return;
    }

    // The end of the pipe: the picture is complete.
    closePipe();
    savePictureIfReady();
}

void WindowScreenshotPicker::closePipe()
{
    if (m_pipeNotifier) {
        m_pipeNotifier->setEnabled(false);
        m_pipeNotifier->deleteLater();
        m_pipeNotifier = nullptr;
    }
    if (m_pipeFd >= 0) {
        ::close(m_pipeFd);
        m_pipeFd = -1;
    }
}

void WindowScreenshotPicker::savePictureIfReady()
{
    if (m_captureInfo.isEmpty() || m_picture.isEmpty()) {
        return;
    }

    const int width = m_captureInfo.value(QStringLiteral("width")).toInt();
    const int height = m_captureInfo.value(QStringLiteral("height")).toInt();
    const int stride = m_captureInfo.value(QStringLiteral("stride")).toInt();
    const auto format =
        static_cast<QImage::Format>(m_captureInfo.value(QStringLiteral("format")).toUInt());

    QImage image;
    if (width > 0 && height > 0 && stride > 0) {
        image = QImage(width, height, format);
        const int bytes = qMin(stride, image.bytesPerLine());
        for (int y = 0; y < height; ++y) {
            memcpy(image.scanLine(y), m_picture.constData() + y * stride, bytes);
        }
    }

    if (image.isNull()) {
        finishPicking();
        Q_EMIT failed(i18n("The screenshot could not be decoded."));
        return;
    }

    savePicture(image, m_pendingTitle);
}

void WindowScreenshotPicker::savePicture(const QImage &image, const QString &title)
{
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
        QStringLiteral("/phoenix-dna");
    QDir().mkpath(directory);
    const QString path = QStringLiteral("%1/window-%2.png")
                             .arg(directory)
                             .arg(QString::number(QDateTime::currentMSecsSinceEpoch()));

    if (!image.save(path, "PNG")) {
        finishPicking();
        Q_EMIT failed(i18n("The screenshot could not be saved."));
        return;
    }

    finishPicking();
    Q_EMIT captured(path, title);
}

void WindowScreenshotPicker::highlightWindow(const QString &uuid)
{
    if (uuid == m_highlightedWindow) {
        return;
    }
    m_highlightedWindow = uuid;

    // highlightWindows() frames the given windows and dims everything
    // else; an empty list drops the highlight again.
    QDBusMessage message = QDBusMessage::createMethodCall(
        kHighlightService, kHighlightPath, kHighlightInterface, QStringLiteral("highlightWindows"));
    message << QStringList{uuid};
    QDBusConnection::sessionBus().asyncCall(message);
}

bool WindowScreenshotPicker::startHoverScript()
{
    // The script is written every time: it is tiny, and a stale copy from
    // an older version of the widget would be a silent bug.
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
        QStringLiteral("/phoenix-dna");
    if (!QDir().mkpath(directory)) {
        return false;
    }
    const QString path = directory + QStringLiteral("/window-picker.qml");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Cannot write the window picker's script to" << path;
        return false;
    }
    file.write(kHoverScript);
    file.close();
    m_scriptPath = path;

    // loadDeclarativeScript(file, pluginName) -> the id of the script
    // object, which lives at /Scripting/Script<id> and can be run there.
    QDBusMessage message = QDBusMessage::createMethodCall(
        kKwinService, kScriptingPath, kScriptingInterface, QStringLiteral("loadDeclarativeScript"));
    message << m_scriptPath << kScriptPluginName;
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            &WindowScreenshotPicker::onScriptLoaded);
    return true;
}

void WindowScreenshotPicker::onScriptLoaded(QDBusPendingCallWatcher *watcher)
{
    watcher->deleteLater();

    const QDBusReply<int> reply = *watcher;
    if (!reply.isValid() || reply.value() < 0) {
        qWarning() << "KWin refused the window picker's hover script";
        return;
    }

    QDBusMessage run = QDBusMessage::createMethodCall(
        kKwinService, QStringLiteral("/Scripting/Script%1").arg(reply.value()), kScriptInterface,
        QStringLiteral("run"));
    QDBusConnection::sessionBus().asyncCall(run);
}

void WindowScreenshotPicker::stopHoverScript()
{
    if (m_scriptPath.isEmpty()) {
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(
        kKwinService, kScriptingPath, kScriptingInterface, QStringLiteral("unloadScript"));
    message << kScriptPluginName;
    QDBusConnection::sessionBus().asyncCall(message);
}

void WindowScreenshotPicker::finishPicking()
{
    stopHoverScript();
    highlightWindow(QString());

    if (!m_picking) {
        return;
    }
    m_picking = false;
    Q_EMIT pickingChanged();
}

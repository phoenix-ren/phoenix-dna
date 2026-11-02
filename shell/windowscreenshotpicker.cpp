// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "windowscreenshotpicker.h"
#include "pickerhoverservice.h"
#include "windowpicture.h"

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
#include <QImage>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <QVariantMap>

#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace
{
//: KWin's main interface: it knows the title of the window that was
//: photographed.
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

//: KWin's answer when the user gives up on the pick.
const QString kCancelledError = QStringLiteral("org.kde.KWin.ScreenShot2.Error.Cancelled");

//: How long the picker waits for the title of the window it photographed.
//: The title is only a name for the status line - the picture is worth
//: more than its name and must not wait for it forever.
const int kTitleTimeoutMs = 2000;

//: How long the picker waits before it tries to load the hover script
//: again, and how often it tries: KWin removes the script of an earlier
//: pick only when its event loop comes round.
const int kLoadRetryMs = 100;
const int kLoadAttempts = 10;

//: The script that reports the window under the pointer.
//
// A declarative (QML) KWin script: it polls the pointer on a timer and
// tells the widget which window it is over, so the widget can name it.
// The script is written to the cache directory when the picker starts.
const char kHoverScript[] = R"QML(
// Phoenix DNA: tell the widget which window the mouse is over.
//
// The widget shows the window's title, so the user can see what a click
// would photograph.

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
        let uuid = ""
        let title = ""
        if (windows.length > 0) {
            uuid = windows[0].internalId.toString()
            title = windows[0].caption
        }
        report.arguments = [uuid, title]
        report.call()
    }

    Component.onCompleted: poll()
}
)QML";
} // namespace

WindowScreenshotPicker::WindowScreenshotPicker(QObject *parent)
    : QObject(parent), m_hoverService(new PickerHoverService(this, this))
{
    // The script of an earlier pick may still be on its way out when the
    // next one starts; the load is retried until KWin has it.
    m_loadTimer.setSingleShot(true);
    m_loadTimer.setInterval(kLoadRetryMs);
    connect(&m_loadTimer, &QTimer::timeout, this, &WindowScreenshotPicker::loadHoverScript);

    // The title of the window under the pointer is looked up while the
    // picker waits; it is only a name for the status line.
    m_titleTimeout.setSingleShot(true);
    m_titleTimeout.setInterval(kTitleTimeoutMs);
    connect(&m_titleTimeout, &QTimer::timeout, this, &WindowScreenshotPicker::onTitleTimeout);

    // The script reports the window under the pointer over D-Bus. Without
    // the service (a second panel with the widget loaded) the picker still
    // works, only without the title of the target.
    if (!m_hoverService->registerOnBus()) {
        qWarning() << "The window picker's hover service could not register on the bus"
                   << "- the title of the window under the pointer is not available";
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
    // photograph.
    trackPointer();

    // KWin takes the pointer over, shows its hint and waits for the click
    // that picks the window - and photographs it right away.
    requestPickedWindow();
}

void WindowScreenshotPicker::trackPointer()
{
    startHoverScript();
}

void WindowScreenshotPicker::setHoveredWindow(const QString &uuid, const QString &title)
{
    if (uuid != m_hoveredWindow) {
        m_hoveredWindow = uuid;
        Q_EMIT hoveredWindowChanged(uuid, title);
    }

    if (title != m_targetTitle) {
        m_targetTitle = title;
        Q_EMIT targetTitleChanged();
    }
}

void WindowScreenshotPicker::requestPickedWindow()
{
    // CaptureInteractive(kind=0) is KWin's "the user picks a window"
    // call: it grabs the pointer, shows its hint and answers with the
    // picture of the window that was clicked.
    sendCapture(QStringLiteral("CaptureInteractive"), QVariant::fromValue(uint(0)));
}

void WindowScreenshotPicker::captureWindow(const QString &uuid)
{
    // CaptureWindow photographs the window with the given handle.
    sendCapture(QStringLiteral("CaptureWindow"), QVariant::fromValue(uuid));
}

void WindowScreenshotPicker::sendCapture(const QString &method, const QVariant &handle)
{
    QDBusUnixFileDescriptor descriptor;
    if (!openPicturePipe(descriptor)) {
        finishPicking();
        Q_EMIT failed(i18n("The screenshot could not be taken: no pipe for the picture."));
        return;
    }

    QVariantMap options;
    options.insert(QStringLiteral("include-decoration"), false);
    options.insert(QStringLiteral("include-shadow"), false);

    QDBusMessage message = QDBusMessage::createMethodCall(kScreenshotService, kScreenshotPath,
                                                          kScreenshotInterface, method);
    message << handle << options << QVariant::fromValue(descriptor);

    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            &WindowScreenshotPicker::onCaptureStarted);
}

bool WindowScreenshotPicker::openPicturePipe(QDBusUnixFileDescriptor &descriptor)
{
    // A picture that is still on its way belongs to a pick that was
    // given up on: it is dropped.
    closePipe();

    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        return false;
    }

    // KWin writes the picture into the pipe. The descriptor that travels
    // over the bus is a duplicate, so ours can go already.
    descriptor = QDBusUnixFileDescriptor(fds[1]);
    ::close(fds[1]);

    // The picture of this pick starts from scratch: the metadata of an
    // earlier pick must never be mixed with the pixels of this one.
    m_picture.clear();
    m_captureInfo.clear();
    m_pendingUuid.clear();
    m_pendingTitle.clear();
    m_pictureComplete = false;
    m_titleKnown = false;
    m_titleTimeout.stop();

    // The picture itself arrives through the pipe.
    m_pipeFd = fds[0];
    m_pipeNotifier = new QSocketNotifier(fds[0], QSocketNotifier::Read, this);
    connect(m_pipeNotifier, &QSocketNotifier::activated, this,
            &WindowScreenshotPicker::readPicture);
    return true;
}

void WindowScreenshotPicker::onCaptureStarted(QDBusPendingCallWatcher *watcher)
{
    watcher->deleteLater();

    const QDBusReply<QVariantMap> reply = *watcher;
    if (!reply.isValid()) {
        // Escape or the right mouse button: the pick was cancelled.
        closePipe();
        finishPicking();
        if (watcher->error().name() != kCancelledError) {
            Q_EMIT failed(i18n("The screenshot could not be taken."));
        }
        return;
    }

    m_captureInfo = reply.value();

    // The picture is on its way; make room for it.
    const qint64 expected = WindowPicture::size(m_captureInfo);
    if (expected > 0) {
        m_picture.reserve(expected);
    }

    // The title of the window that was photographed is looked up in the
    // meantime: it is only a name for the status line.
    m_pendingUuid = m_captureInfo.value(QStringLiteral("windowId")).toString();
    if (!m_pendingUuid.isEmpty()) {
        lookUpTitle(m_pendingUuid);
    } else {
        m_titleKnown = true;
    }

    savePictureIfReady();
}

void WindowScreenshotPicker::lookUpTitle(const QString &uuid)
{
    // The title is a nicety: the picture is saved with or without it.
    m_titleTimeout.start();

    QDBusMessage message = QDBusMessage::createMethodCall(kKwinService, kKwinPath, kKwinInterface,
                                                          QStringLiteral("getWindowInfo"));
    message << uuid;
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, uuid](QDBusPendingCallWatcher *w) {
        w->deleteLater();

        // A pick that has been given up on in the meantime: the answer
        // belongs to a window that is long gone.
        if (uuid != m_pendingUuid) {
            return;
        }

        const QDBusReply<QVariantMap> reply = *w;
        // KWin answers with an empty map when the window is gone.
        if (reply.isValid() && !reply.value().isEmpty()) {
            onWindowInfo(reply.value());
        } else {
            m_titleKnown = true;
            m_titleTimeout.stop();
            savePictureIfReady();
        }
    });
}

void WindowScreenshotPicker::onWindowInfo(const QVariantMap &info)
{
    m_pendingTitle = info.value(QStringLiteral("caption")).toString();
    m_titleKnown = true;
    m_titleTimeout.stop();
    savePictureIfReady();
}

void WindowScreenshotPicker::onTitleTimeout()
{
    // The window does not tell us its name in time: the picture is worth
    // more than its title.
    m_titleKnown = true;
    savePictureIfReady();
}

void WindowScreenshotPicker::readPicture()
{
    if (m_pipeFd < 0) {
        return;
    }

    char buffer[64 * 1024];
    const ssize_t count = ::read(m_pipeFd, buffer, sizeof(buffer));
    if (count > 0) {
        m_picture.append(buffer, int(count));
        return;
    }
    if (count < 0) {
        if (errno == EAGAIN || errno == EINTR) {
            // Nothing to read after all: the notifier tells us again.
            return;
        }
        failPicture(i18n("The screenshot could not be read."));
        return;
    }

    // The end of the pipe: the picture is complete.
    m_pictureComplete = true;
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
    // KWin answers its call before it writes the picture, so the two
    // arrive in either order: the picture is saved when the last of them
    // is in.
    if (m_captureInfo.isEmpty() || !m_pictureComplete || !m_titleKnown) {
        return;
    }

    // The picture is saved once: a late answer must not save it again.
    const QVariantMap info = m_captureInfo;
    const QByteArray raw = m_picture;
    m_captureInfo.clear();
    m_picture.clear();
    m_pictureComplete = false;

    QImage image;
    if (!WindowPicture::decode(info, raw, image)) {
        failPicture(i18n("The screenshot could not be decoded."));
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

    // The name of the window makes the attachment tell what it is; the
    // time keeps two pictures of the same window apart.
    QString name = title;
    name.replace(QRegularExpression(QStringLiteral("[^\\w\\-. ]")), QStringLiteral("_"));
    name = name.simplified();
    if (name.isEmpty()) {
        name = QStringLiteral("window");
    }
    if (name.length() > 60) {
        name.truncate(60);
    }

    const QString path = QStringLiteral("%1/%2-%3.png")
                             .arg(directory)
                             .arg(name)
                             .arg(QString::number(QDateTime::currentMSecsSinceEpoch()));

    if (!image.save(path, "PNG")) {
        failPicture(i18n("The screenshot could not be saved."));
        return;
    }

    finishPicking();
    Q_EMIT captured(path, title);
}

void WindowScreenshotPicker::failPicture(const QString &message)
{
    closePipe();
    finishPicking();
    Q_EMIT failed(message);
}

bool WindowScreenshotPicker::startHoverScript()
{
    // The script is written into a directory of its own, and the
    // directories of the earlier picks are removed.
    //
    // KWin's QML engine remembers the file list of every directory it
    // has looked at, and a file that appears in a directory it already
    // knows is invisible to it: it reports "File name case mismatch" and
    // the script never runs. A directory of its own is always looked at
    // afresh, so the script is found whatever happened before.
    const QString root =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
        QStringLiteral("/phoenix-dna");
    const QString directory =
        QStringLiteral("%1/hover-%2").arg(root).arg(QDateTime::currentMSecsSinceEpoch());
    if (!QDir().mkpath(directory)) {
        return false;
    }
    removeOldScriptDirectories(root, directory);

    const QString path = directory + QStringLiteral("/window-picker.qml");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Cannot write the window picker's script to" << path;
        return false;
    }
    file.write(kHoverScript);
    file.close();
    m_scriptPath = path;

    m_loadAttempts = 0;
    loadHoverScript();
    return true;
}

void WindowScreenshotPicker::removeOldScriptDirectories(const QString &root, const QString &keep)
{
    const QDir rootDir(root);
    const QStringList entries = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        if (!entry.startsWith(QStringLiteral("hover-"))) {
            continue;
        }
        const QString path = rootDir.absoluteFilePath(entry);
        if (path == keep) {
            continue;
        }
        QDir(path).removeRecursively();
    }
}

void WindowScreenshotPicker::loadHoverScript()
{
    // loadDeclarativeScript(file, pluginName) -> the id of the script
    // object, which lives at /Scripting/Script<id> and can be run there.
    QDBusMessage message = QDBusMessage::createMethodCall(
        kKwinService, kScriptingPath, kScriptingInterface, QStringLiteral("loadDeclarativeScript"));
    message << m_scriptPath << kScriptPluginName;
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            &WindowScreenshotPicker::onScriptLoaded);
}

void WindowScreenshotPicker::onScriptLoaded(QDBusPendingCallWatcher *watcher)
{
    watcher->deleteLater();

    const QDBusReply<int> reply = *watcher;
    // A negative id means that KWin still has a script of this name -
    // the one of an earlier pick, which it removes when its event loop
    // comes round. Try again in a moment.
    if (!reply.isValid() || reply.value() < 0) {
        if (m_loadAttempts < kLoadAttempts) {
            ++m_loadAttempts;
            stopHoverScript();
            m_loadTimer.start();
            return;
        }
        qWarning() << "KWin refused the window picker's hover script";
        return;
    }
    m_loadAttempts = 0;

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

    if (!m_picking) {
        return;
    }
    m_picking = false;
    Q_EMIT pickingChanged();

    // The name of the window under the pointer is of no use once the
    // picker is down.
    m_hoveredWindow.clear();
    if (!m_targetTitle.isEmpty()) {
        m_targetTitle.clear();
        Q_EMIT targetTitleChanged();
    }
}

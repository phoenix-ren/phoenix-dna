// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QDBusPendingCallWatcher>
#include <QDBusUnixFileDescriptor>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class PickerHoverService;
class QAction;
class QImage;
class QSocketNotifier;

/*!
 * \brief Picks a window on the desktop and photographs it.
 *
 * The picker is started by a global shortcut ("Meta+Alt" unless the user
 * rebinds it in System Settings) or by one of the buttons in the widget,
 * the dictation overlay and the results pane. It then works like this:
 *
 * \list
 * \li KWin takes the pointer over and shows its own hint ("select a
 *     window to screen shot with left click or enter"). The click never
 *     reaches the window underneath.
 * \li While the pointer moves, the window under it is reported to the
 *     widget, which shows its title - that is the window the next click
 *     would photograph.
 * \li The click hands the window over; KWin photographs it and writes the
 *     picture into a pipe, which the widget turns into a PNG and attaches
 *     to the prompt.
 * \endlist
 *
 * The window itself is not highlighted: KWin's highlight effect fades
 * every other window to nothing, which takes the whole desktop away while
 * the picker is up. The title of the window under the pointer says the
 * same thing without that.
 */
class WindowScreenshotPicker : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    /*! \brief Whether the picker is waiting for the user to pick a window. */
    Q_PROPERTY(bool picking READ isPicking NOTIFY pickingChanged)

    /*! \brief The bound global shortcut that starts the picker.
     *
     * Empty when nothing is bound (kglobalacceld is not running). */
    Q_PROPERTY(QString shortcut READ shortcut NOTIFY shortcutChanged)

    /*! \brief The title of the window under the pointer.
     *
     * Empty when the pointer is not over any window, or over a window
     * without a caption. */
    Q_PROPERTY(QString targetTitle READ targetTitle NOTIFY targetTitleChanged)

public:
    explicit WindowScreenshotPicker(QObject *parent = nullptr);
    ~WindowScreenshotPicker() override;

    bool isPicking() const { return m_picking; }

    /*! \brief The bound global shortcut that starts the picker.
     *
     * Empty when nothing is bound (kglobalacceld is not running). */
    QString shortcut() const;

    /*! \brief The title of the window under the pointer. */
    QString targetTitle() const { return m_targetTitle; }

    /*! \brief The KGlobalAccel action behind the shortcut.
     *
     * For the settings store, which re-binds the picker's hotkey through
     * the same action System Settings would edit. */
    QAction *shortcutAction() const { return m_shortcut; }

    /*! \brief Starts the picker.
     *
     * Does nothing while a pick is already in flight. */
    Q_INVOKABLE void start();

    /*! \brief Follows the pointer, so the window under it can be named.
     *
     * The picker does this on its own; it is separate so the target can
     * be followed without picking a window. */
    Q_INVOKABLE void trackPointer();

    /*! \brief Photographs the window with the given uuid.
     *
     * The picture is saved and reported through captured(). */
    Q_INVOKABLE void captureWindow(const QString &uuid);

    /*! \brief The window under the pointer, as the KWin script sees it.
     *
     * An empty uuid means the pointer is not over any window. */
    void setHoveredWindow(const QString &uuid, const QString &title);

Q_SIGNALS:
    void pickingChanged();

    /*! \brief The global shortcut of the picker changed (the user
     *  re-bound it in System Settings). */
    void shortcutChanged();

    /*! \brief The window under the pointer changed. */
    void hoveredWindowChanged(const QString &uuid, const QString &title);

    /*! \brief The title of the window under the pointer changed. */
    void targetTitleChanged();

    /*! \brief A window was photographed.
     *
     * \a filePath is the PNG that was written, \a title the window's
     * caption (empty for a window without one). */
    void captured(const QString &filePath, const QString &title);

    /*! \brief The pick failed; \a message says why. */
    void failed(const QString &message);

private:
    /*! \brief Writes the hover script and asks KWin to load it. */
    bool startHoverScript();

    /*! \brief Asks KWin to load the hover script. */
    void loadHoverScript();

    /*! \brief Removes the script directories of the earlier picks.
     *
     * \a root is the directory they all live in, \a keep the one that
     * is in use. */
    void removeOldScriptDirectories(const QString &root, const QString &keep);

    /*! \brief Stops the script. */
    void stopHoverScript();

    /*! \brief Asks KWin for the window the user picks - and its picture. */
    void requestPickedWindow();

    /*! \brief Asks KWin for the picture of a window, by name. */
    void sendCapture(const QString &method, const QVariant &handle);

    /*! \brief Ends the pick: the script stops. */
    void finishPicking();

    /*! \brief Writes the picture to a PNG and reports it. */
    void savePicture(const QImage &image, const QString &title);

    /*! \brief Saves the picture once the metadata, the pixels and the
     *  title of the window are all here.
     *
     * KWin answers its call before it writes the picture, so the two
     * arrive in either order; the picture is saved when the last of them
     * comes in. */
    void savePictureIfReady();

    /*! \brief Reads what KWin wrote into the pipe. */
    void readPicture();

    /*! \brief Opens the pipe KWin writes the picture into.
     *
     * \a descriptor is the end that travels over the bus. */
    bool openPicturePipe(QDBusUnixFileDescriptor &descriptor);

    /*! \brief Closes the pipe the picture arrives in. */
    void closePipe();

    /*! \brief Looks the title of the captured window up. */
    void lookUpTitle(const QString &uuid);

    /*! \brief Gives up on the picture and says why. */
    void failPicture(const QString &message);

private Q_SLOTS:
    /*! \brief The KWin script was loaded; run it. */
    void onScriptLoaded(QDBusPendingCallWatcher *watcher);

    /*! \brief KWin took the picture of the window the user picked. */
    void onCaptureStarted(QDBusPendingCallWatcher *watcher);

    /*! \brief The title of the captured window is known. */
    void onWindowInfo(const QVariantMap &info);

    /*! \brief The title of the captured window did not come in time. */
    void onTitleTimeout();

private:
    PickerHoverService *m_hoverService = nullptr;
    QAction *m_shortcut = nullptr;
    QSocketNotifier *m_pipeNotifier = nullptr;
    QTimer m_loadTimer;
    QTimer m_titleTimeout;
    QByteArray m_picture;
    QVariantMap m_captureInfo;
    QString m_hoveredWindow;
    QString m_targetTitle;
    QString m_scriptPath;
    QString m_pendingUuid;
    QString m_pendingTitle;
    int m_pipeFd = -1;
    int m_loadAttempts = 0;
    bool m_pictureComplete = false;
    bool m_titleKnown = false;
    bool m_picking = false;
};

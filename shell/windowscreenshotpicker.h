// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QDBusPendingCallWatcher>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class PickerHoverService;
class QAction;
class QImage;
class QSocketNotifier;
class QTimer;

/*!
 * \brief Picks a window on the desktop and photographs it.
 *
 * The picker is started by a global shortcut ("Meta+Alt" unless the user
 * rebinds it in System Settings) or by one of the buttons in the widget,
 * the dictation overlay and the results pane. It then works like this:
 *
 * \list
 * \li KWin's window selector takes over the pointer: the user moves the
 *     mouse over the window they want and clicks. The click never reaches
 *     that window.
 * \li While they move, the window under the pointer is highlighted -
 *     KWin's highlight effect draws a frame over it and dims the rest -
 *     so it is obvious what a click would capture.
 * \li The click hands the window over; its picture is taken by KWin and
 *     saved as a PNG, which the caller attaches to the prompt.
 * \endlist
 *
 * Everything the picker needs comes from KWin over D-Bus: the window
 * under the pointer (through a small KWin script, see PickerHoverService),
 * the window the user picked, and the picture of it.
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

public:
    explicit WindowScreenshotPicker(QObject *parent = nullptr);
    ~WindowScreenshotPicker() override;

    bool isPicking() const { return m_picking; }

    /*! \brief The bound global shortcut that starts the picker.
     *
     * Empty when nothing is bound (kglobalacceld is not running). */
    QString shortcut() const;
    /*! \brief Starts the picker.
     *
     * Does nothing while a pick is already in flight. */
    Q_INVOKABLE void start();

    /*! \brief Follows the pointer, so the window under it can be
     *  highlighted.
     *
     * The picker does this on its own; it is separate so the target can
     * be followed without picking a window. */
    Q_INVOKABLE void trackPointer();

    /*! \brief Photographs the window with the given uuid.
     *
     * The picture is saved and reported through captured(). */
    Q_INVOKABLE void captureWindow(const QString &uuid);

    /*! \brief The window under the mouse, as the KWin script reports it.
     *
     * An empty uuid means the pointer is not over any window; the
     * highlight is dropped then. */
    void setHoveredWindow(const QString &uuid);

Q_SIGNALS:
    void pickingChanged();

    /*! \brief The global shortcut of the picker changed (the user
     *  re-bound it in System Settings). */
    void shortcutChanged();

    /*! \brief The window under the pointer changed.
     *
     * \a uuid is empty when the pointer is not over any window. */
    void hoveredWindowChanged(const QString &uuid);

    /*! \brief A window was photographed.
     *
     * \a filePath is the PNG that was written, \a title the window's
     * caption (empty for a window without one). */
    void captured(const QString &filePath, const QString &title);

    /*! \brief The pick failed; \a message says why. */
    void failed(const QString &message);

private:
    /*! \brief Loads and starts the KWin script that tracks the pointer. */
    bool startHoverScript();

    /*! \brief Stops the script and drops the highlight. */
    void stopHoverScript();

    /*! \brief Draws (or drops) the highlight over the given window. */
    void highlightWindow(const QString &uuid);

    /*! \brief Asks KWin for the window the user picks. */
    void requestPickedWindow();

    /*! \brief Ends the pick: the script stops, the highlight goes away. */
    void finishPicking();

    /*! \brief Writes the picture to a PNG and reports it. */
    void savePicture(const QImage &image, const QString &title);

    /*! \brief Saves the picture once both the metadata and the pixels are
     *  here: KWin answers the call with the metadata and writes the
     *  pixels into the pipe. */
    void savePictureIfReady();

    /*! \brief Reads what KWin wrote into the pipe. */
    void readPicture();

    /*! \brief Closes the pipe the picture arrives in. */
    void closePipe();

private Q_SLOTS:
    /*! \brief The KWin script was loaded; run it. */
    void onScriptLoaded(QDBusPendingCallWatcher *watcher);

    /*! \brief The user picked a window (or cancelled the pick). */
    void onWindowPicked(QDBusPendingCallWatcher *watcher);

    /*! \brief KWin started taking the picture. */
    void onCaptureStarted(QDBusPendingCallWatcher *watcher);

private:
    PickerHoverService *m_hoverService = nullptr;
    QAction *m_shortcut = nullptr;
    QSocketNotifier *m_pipeNotifier = nullptr;
    QTimer *m_captureDelay = nullptr;
    QByteArray m_picture;
    QVariantMap m_captureInfo;
    QString m_hoveredWindow;
    QString m_highlightedWindow;
    QString m_scriptPath;
    QString m_pendingUuid;
    QString m_pendingTitle;
    int m_pipeFd = -1;
    bool m_picking = false;
};

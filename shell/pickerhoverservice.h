// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QString>

class WindowScreenshotPicker;

/*!
 * \brief The D-Bus service the KWin hover script reports to.
 *
 * KWin can tell which window the mouse is over, but only from a script:
 * the picker loads a small script (see WindowScreenshotPicker) that polls
 * the pointer and reports the window under it here. The widget answers
 * with the highlight over that window.
 *
 * The service lives on the session bus under the name
 * \c org.phoenixdna.agent.Picker.
 */
class PickerHoverService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.phoenixdna.agent.Picker")

public:
    explicit PickerHoverService(WindowScreenshotPicker *picker, QObject *parent = nullptr);

    /*! \brief Puts the service on the session bus.
     *
     * Returns false when the name is already taken (a second panel with
     * the widget loaded): the picker then works without the hover
     * highlight. */
    bool registerOnBus();

    /*! \brief Takes the service off the session bus. */
    void unregisterFromBus();

public Q_SLOTS:
    /*! \brief The window under the mouse, as the KWin script sees it.
     *
     * \a uuid is empty when the pointer is not over any window. */
    Q_SCRIPTABLE void hoveredWindow(const QString &uuid);

private:
    WindowScreenshotPicker *m_picker = nullptr;
};

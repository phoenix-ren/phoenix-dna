// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QProcess>

/*!
 * \brief The camera click a photographed window plays.
 *
 * Spectacle-style feedback for the screenshot path: the moment a window is
 * photographed, the shutter clicks. Every entry point - the panel's camera
 * button, the dictation overlay's, the results pane's, the global shortcut -
 * funnels into the same picker, and the picker reports the finished picture,
 * so the click sounds exactly when the shot lands.
 *
 * The click is synthesized in-process rather than shipped as a file:
 * \li 0.28 s of 44.1 kHz mono PCM is about 12k samples, computed once and
 *     cached, so there is no binary asset to carry through the QRC (where it
 *     would have to be extracted to a file before anything could play it);
 * \li playback goes through pw-cat, the same helper the dictation path
 *     already depends on, spawned the same way AudioCapture spawns it.
 */
class ShutterSound : public QObject
{
    Q_OBJECT

public:
    explicit ShutterSound(QObject *parent = nullptr);

    /*! \brief Plays the click.
     *
     * Spawns pw-cat in playback mode and streams the pre-synthesized PCM
     * into it; the effect is fire-and-forget: pw-cat plays its 0.28 s and
     * exits. Does nothing when pw-cat is not installed - the effect is
     * decoration, and a missing helper is the dictation path's failure to
     * report, not this one's.
     */
    void play();

private:
    /*! \brief Renders the click as raw little-endian 16-bit mono PCM. */
    QByteArray synthesize() const;

    QProcess m_process;
    QByteArray m_pcm;
};

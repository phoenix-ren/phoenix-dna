// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QProcess>

/*!
 * \brief The chime a finished query plays.
 *
 * The agent can take a while — tools run, models think — and the user is
 * free to work somewhere else while it does. The moment a run completes,
 * this chime says so, so the answer no longer has to be watched for: the
 * user reads it when they hear it.
 *
 * The chime is synthesized in-process rather than shipped as a file, for
 * the same reasons the camera click is (see ShutterSound):
 * \li 0.55 s of 44.1 kHz mono PCM is about 24k samples, computed once and
 *     cached, so there is no binary asset to carry through the QRC (where it
 *     would have to be extracted to a file before anything could play it);
 * \li playback goes through pw-cat, the same helper the dictation path
 *     already depends on, spawned the same way AudioCapture spawns it.
 *
 * A finished query is a gentle event, unlike a photographed window: two
 * sine tones a fourth apart, rising, each fading out on its own — a door
 * chime, not an alarm. Quieter than the click, too.
 */
class CompletionSound : public QObject
{
    Q_OBJECT

public:
    explicit CompletionSound(QObject *parent = nullptr);

    /*! \brief The rendered effect: raw little-endian 16-bit mono PCM.
     *
     * Synthesized once and cached; play() streams exactly this, so the
     * offline check can inspect (or write out, or play) what would be
     * heard without an audio server of its own. */
    const QByteArray &effect();

    /*! \brief Plays the chime.
     *
     * Spawns pw-cat in playback mode and streams the pre-synthesized PCM
     * into it; the effect is fire-and-forget: pw-cat plays its 0.55 s and
     * exits. Does nothing when pw-cat is not installed - the effect is
     * decoration, and a missing helper is the dictation path's failure to
     * report, not this one's.
     */
    void play();

private:
    /*! \brief Renders the chime as raw little-endian 16-bit mono PCM. */
    QByteArray synthesize() const;

    QProcess m_process;
    QByteArray m_pcm;
};

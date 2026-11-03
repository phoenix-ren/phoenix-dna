// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

/*! \brief Everything the speech client needs, resolved on demand. */
struct SpeechConfig
{
    bool enabled = false; ///< whether the daemon's TTS is configured on
    QString voice;        ///< informational: the configured voice id
};

/*!
 * \brief Client for the daemon's read-aloud half (dna.phoenix.Agent).
 *
 * The shell never synthesizes audio — that is the daemon's job (it owns
 * the answer, the sentence segmentation and the MiniMax credential).
 * This class only *requests* speech over D-Bus:
 *
 *   SpeakText(text)         speak an arbitrary string on demand
 *   StopSpeaking()          interrupt immediately
 *   GetSpeechState()        enabled plus "speaking"/"idle"
 *   SpeechStateChanged(s)   the signal the pane reflects live
 *
 * The results window's "Read aloud" affordance and its stop control go
 * through this (via AgentBridge); the record button goes through
 * stop() as a barge-in, so the microphone never picks up the agent's
 * own voice (see PhoenixController::setRecording).
 *
 * Configuration follows the conventions of Transcriber::resolveConfig()
 * and agent/phoenix_dna/config.py: environment beats
 * ~/.config/phoenix-dna/keys.json, and within the environment the
 * PHOENIX_-prefixed spelling wins:
 *
 *   PHOENIX_TTS_ENABLED  "true"/"false"; without it, the "tts_enabled"
 *                       entry of keys.json decides
 *   PHOENIX_TTS_VOICE   the voice id shown as configured (informational)
 *
 * The daemon's own agent.toml is authoritative for the engine settings;
 * what the shell resolves here is only whether to show the affordance
 * at all and which voice to name. GetSpeechState refreshes both from
 * the running daemon, so a daemon restart with TTS turned on makes the
 * affordance appear without reloading the widget.
 */
class Speaker : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

public:
    explicit Speaker(QObject *parent = nullptr);

    /*! \brief Whether the daemon is currently reading something aloud. */
    Q_PROPERTY(bool speaking READ isSpeaking NOTIFY speakingChanged)

    /*! \brief Whether read-aloud is configured (env or keys.json, or the
     *  daemon said so). The pane shows its affordance only when true. */
    Q_PROPERTY(bool available READ isAvailable NOTIFY availableChanged)

    bool isSpeaking() const { return m_speaking; }
    bool isAvailable() const { return m_available; }

    /*! \brief Resolves enabled/voice as described above. */
    static SpeechConfig resolveConfig();

public Q_SLOTS:
    /*! \brief Asks the daemon to speak \a text (normalized there). */
    void speak(const QString &text);

    /*! \brief Asks the daemon to interrupt playback immediately. */
    void stop();

private Q_SLOTS:
    /*! \brief SpeechStateChanged from the daemon: adopt "speaking"/"idle". */
    void onSpeechStateChanged(const QString &state);

Q_SIGNALS:
    void speakingChanged();
    void availableChanged();

private:
    void setSpeaking(bool speaking);
    void setAvailable(bool available);
    void probeState();

    bool m_speaking = false;
    bool m_available = false;
};

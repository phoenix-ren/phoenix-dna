// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

class QNetworkReply;

/*! \brief Everything one transcription request needs, resolved on demand. */
struct TranscriptionConfig
{
    QString provider; ///< "minimax" (default) or "openai"
    QString endpoint; ///< full URL of the transcriptions endpoint
    QString model;    ///< API model name
    QString apiKey;   ///< bearer token; empty means "not configured"
};

/*!
 * \brief Sends a finished dictation clip to a speech-to-text API.
 *
 * Both providers expose the same multipart contract — a "model" field plus
 * the audio as "file", authorized with a bearer key — because MiniMax
 * deliberately mirrors the OpenAI schema (even its error responses), so one
 * code path serves both:
 *
 *   minimax: POST https://api.minimax.io/v1/speech_to_text, model asr-1.0
 *   openai:  POST https://api.openai.com/v1/audio/transcriptions
 *
 * MiniMax is the default since that key is already configured for the agent
 * daemon. Configuration follows the conventions of agent/phoenix_dna/config.py
 * (environment beats ~/.config/phoenix-dna/keys.json, and the PHOENIX_-prefixed
 * spelling wins within the environment) and is re-resolved for every request,
 * so a changed key takes effect without restarting plasmashell:
 *
 *   PHOENIX_STT_PROVIDER  "minimax" (default) or "openai"
 *   PHOENIX_STT_MODEL     model override (e.g. "whisper-1")
 *   PHOENIX_STT_ENDPOINT  endpoint override
 *
 * Keys are read from keys.json ("minimax_api_key" / "openai_api_key") or the
 * corresponding MINIMAX_API_KEY / PHOENIX_MINIMAX_API_KEY style variables.
 */
class Transcriber : public QObject
{
    Q_OBJECT

public:
    explicit Transcriber(QObject *parent = nullptr);

    /*! \brief Whether a request is in flight. */
    bool isBusy() const { return m_reply != nullptr; }

    /*! \brief POSTs the WAV clip; exactly one of finished()/failed() follows. */
    void transcribe(const QByteArray &wav);

    /*! \brief Resolves provider, endpoint, model and key, as described above. */
    static TranscriptionConfig resolveConfig();

Q_SIGNALS:
    /*! \brief The recognized text; empty when nothing intelligible was said. */
    void finished(const QString &text);

    /*! \brief A human-readable reason the clip could not be transcribed. */
    void failed(const QString &message);

private:
    QNetworkAccessManager m_nam;
    QNetworkReply *m_reply = nullptr;
    int m_generation = 0; // replies superseded by a newer clip are ignored
};

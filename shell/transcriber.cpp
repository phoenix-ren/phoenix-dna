// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "transcriber.h"

#include <QDir>
#include <QFile>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QVariant>

#include <chrono>

namespace {

struct ProviderSpec
{
    const char *name;
    const char *endpoint;
    const char *model;
    const char *keyField;    // ~/.config/phoenix.ai/keys.json entry
    const char *primaryEnv;  // e.g. MINIMAX_API_KEY
    const char *secondaryEnv; // e.g. PHOENIX_MINIMAX_API_KEY; wins, as in config.py
};

// MiniMax asr-1.0 ≈ $0.006/min; OpenAI whisper-1 $0.006/min and
// gpt-4o-mini-transcribe $0.003/min. Both accept WAV up to 500 s / 50 MB.
const ProviderSpec kProviders[] = {
    { "minimax",
      "https://api.minimax.io/v1/speech_to_text",
      "asr-1.0",
      "minimax_api_key",
      "MINIMAX_API_KEY",
      "PHOENIX_MINIMAX_API_KEY" },
    { "openai",
      "https://api.openai.com/v1/audio/transcriptions",
      "gpt-4o-mini-transcribe",
      "openai_api_key",
      "OPENAI_API_KEY",
      "PHOENIX_OPENAI_API_KEY" },
};

const ProviderSpec &providerSpec(const QString &name)
{
    for (const auto &spec : kProviders) {
        if (name.compare(QLatin1String(spec.name), Qt::CaseInsensitive) == 0) {
            return spec;
        }
    }
    qWarning() << "Transcriber: unknown provider" << name
               << ", falling back to" << kProviders[0].name;
    return kProviders[0];
}

QString keysJsonValue(const char *field)
{
    QString configDir = qEnvironmentVariable("PHOENIX_CONFIG_DIR");
    if (configDir.isEmpty()) {
        configDir = QDir::homePath() + QStringLiteral("/.config/phoenix.ai");
    }
    QFile file(QDir(configDir).filePath(QStringLiteral("keys.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String(field)).toString();
}

QString environmentOrEmpty(const char *name)
{
    const QString value = qEnvironmentVariable(name);
    return value;
}

} // namespace

Transcriber::Transcriber(QObject *parent)
    : QObject(parent)
{
}

SttConfig Transcriber::resolveConfig()
{
    SttConfig config;

    QString provider = qEnvironmentVariable("PHOENIX_STT_PROVIDER");
    if (provider.isEmpty()) {
        provider = QStringLiteral("minimax");
    }
    const ProviderSpec &spec = providerSpec(provider);

    config.provider = QLatin1String(spec.name);
    config.endpoint = QLatin1String(spec.endpoint);
    config.model = QLatin1String(spec.model);

    const QString model = qEnvironmentVariable("PHOENIX_STT_MODEL");
    if (!model.isEmpty()) {
        config.model = model;
    }
    const QString endpoint = qEnvironmentVariable("PHOENIX_STT_ENDPOINT");
    if (!endpoint.isEmpty()) {
        config.endpoint = endpoint;
    }

    // Environment beats keys.json; within the environment the PHOENIX_-
    // prefixed spelling wins. Same precedence as config.py.
    config.apiKey = environmentOrEmpty(spec.secondaryEnv);
    if (config.apiKey.isEmpty()) {
        config.apiKey = environmentOrEmpty(spec.primaryEnv);
    }
    if (config.apiKey.isEmpty()) {
        config.apiKey = keysJsonValue(spec.keyField);
    }

    return config;
}

void Transcriber::transcribe(const QByteArray &wav)
{
    const SttConfig config = resolveConfig();
    if (config.apiKey.isEmpty()) {
        qWarning() << "Transcriber: no" << config.provider << "API key found";
        Q_EMIT failed(QStringLiteral("no %1 API key found — add it to "
                                     "~/.config/phoenix.ai/keys.json or export "
                                     "its environment variable")
                          .arg(config.provider));
        return;
    }

    // Journal evidence: the request itself is the first observable step of
    // the dictation → transcript chain.
    qWarning() << "Transcriber: posting" << wav.size() << "bytes to"
               << config.endpoint << "as model" << config.model;

    // Starting a new request invalidates any in-flight one; its finished()
    // handler observes a stale generation and stays silent.
    ++m_generation;
    if (m_reply) {
        m_reply->abort();
        m_reply = nullptr;
    }

    QNetworkRequest request(QUrl(config.endpoint));
    request.setRawHeader(QByteArrayLiteral("Authorization"),
                         QStringLiteral("Bearer %1").arg(config.apiKey).toUtf8());
    // A dictation clip transcribes in a few seconds; half a minute covers a
    // long utterance on a slow link without hanging the overlay forever.
    request.setTransferTimeout(std::chrono::seconds(30));

    auto *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    QHttpPart modelPart;
    modelPart.setRawHeader(QByteArrayLiteral("Content-Disposition"),
                           QByteArrayLiteral("form-data; name=\"model\""));
    modelPart.setBody(config.model.toUtf8());
    multiPart->append(modelPart);

    QHttpPart filePart;
    filePart.setRawHeader(QByteArrayLiteral("Content-Disposition"),
                           QByteArrayLiteral("form-data; name=\"file\"; filename=\"clip.wav\""));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant(QStringLiteral("audio/wav")));
    filePart.setBody(wav);
    multiPart->append(filePart);

    m_reply = m_nam.post(request, multiPart);
    multiPart->setParent(m_reply); // the reply owns and frees the multipart

    const int generation = m_generation;
    connect(m_reply, &QNetworkReply::finished, this, [this, generation] {
        auto *reply = static_cast<QNetworkReply *>(sender());
        reply->deleteLater();

        // A newer clip superseded this request; its outcome is irrelevant.
        if (generation != m_generation) {
            return;
        }
        m_reply = nullptr;

        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            // Both providers answer with OpenAI-style error JSON
            // ({"error": {"message": ...}}); fall back to Qt's error string.
            const QJsonObject error =
                QJsonDocument::fromJson(body).object().value(QLatin1String("error")).toObject();
            QString detail = error.value(QLatin1String("message")).toString();
            if (detail.isEmpty()) {
                detail = reply->errorString();
            }
            const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            const QString code = status.isValid() ? status.toString() : QStringLiteral("-");
            qWarning() << "Transcriber: request failed —" << code << detail;
            Q_EMIT failed(QStringLiteral("HTTP %1: %2").arg(code, detail));
            return;
        }

        const QJsonObject response = QJsonDocument::fromJson(body).object();
        const QString text = response.value(QLatin1String("text")).toString();
        // Journal evidence for the dictation → transcript chain; a preview
        // only, so full transcripts do not pile up in the journal.
        qWarning() << "Transcriber: recognized"
                   << response.value(QLatin1String("duration")).toDouble() << "s of audio as"
                   << text.length() << "chars:" << text.left(80);
        Q_EMIT finished(text);
    });
}

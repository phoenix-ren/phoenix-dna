// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

// How the speech-to-text configuration is resolved: which provider is
// in force, which endpoint and model it names, and which key it uses.
//
// The rules are the ones the settings window and the daemon's
// config.py share: the environment beats keys.json, the PHOENIX_-
// prefixed spelling beats the plain one, and a provider only supplies
// defaults — an explicitly named endpoint or model always wins.
//
//   transcriberconfigtest
//
// Exits non-zero when a check fails. (Plain printf, not qInfo: the
// categorized logging prints nothing in this environment.)

#include "transcriber.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

// Writes a keys.json with the given entries, in a directory of its own,
// and points the configuration at it.
void writeKeys(const QString &configDir, const QJsonObject &keys)
{
    QDir(configDir).mkpath(QStringLiteral("."));
    QFile file(configDir + QStringLiteral("/keys.json"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(keys).toJson());
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QTemporaryDir dir;
    if (!dir.isValid()) {
        printf("FAIL: no temporary directory\n");
        return 1;
    }
    const QString configDir = dir.path();
    // The configuration reads the keys file from the directory the
    // environment names, so the test controls it entirely.
    qputenv("PHOENIX_CONFIG_DIR", configDir.toUtf8());

    // Nothing set at all: MiniMax is the default, with its own
    // endpoint, model and key.
    writeKeys(configDir, QJsonObject{{QStringLiteral("minimax_api_key"),
                                      QStringLiteral("sk-minimax")}});
    {
        const TranscriptionConfig config = Transcriber::resolveConfig();
        check(config.provider == QStringLiteral("minimax"),
              "the default provider is minimax");
        check(config.endpoint
                  == QStringLiteral("https://api.minimax.io/v1/speech_to_text"),
              "minimax's own endpoint is the default");
        check(config.model == QStringLiteral("asr-1.0"),
              "minimax's own model is the default");
        check(config.apiKey == QStringLiteral("sk-minimax"),
              "the key comes from the key file");
    }

    // The OpenAI provider brings its own endpoint, model and key.
    writeKeys(configDir, QJsonObject{{QStringLiteral("openai_api_key"),
                                      QStringLiteral("sk-openai")}});
    qputenv("PHOENIX_STT_PROVIDER", "openai");
    {
        const TranscriptionConfig config = Transcriber::resolveConfig();
        check(config.provider == QStringLiteral("openai"),
              "the provider comes from the environment");
        check(config.endpoint
                  == QStringLiteral("https://api.openai.com/v1/audio/transcriptions"),
              "openai's own endpoint is the default");
        check(config.model == QStringLiteral("gpt-4o-mini-transcribe"),
              "openai's own model is the default");
        check(config.apiKey == QStringLiteral("sk-openai"),
              "openai's own key is used");
    }
    qunsetenv("PHOENIX_STT_PROVIDER");

    // An explicitly named endpoint and model beat the provider's.
    qputenv("PHOENIX_STT_PROVIDER", "openai");
    qputenv("PHOENIX_STT_ENDPOINT", "https://my.gateway/v1/audio/transcriptions");
    qputenv("PHOENIX_STT_MODEL", "whisper-1");
    {
        const TranscriptionConfig config = Transcriber::resolveConfig();
        check(config.endpoint == QStringLiteral("https://my.gateway/v1/audio/transcriptions"),
              "an explicit endpoint wins");
        check(config.model == QStringLiteral("whisper-1"),
              "an explicit model wins");
    }
    qunsetenv("PHOENIX_STT_ENDPOINT");
    qunsetenv("PHOENIX_STT_MODEL");

    // The PHOENIX_-prefixed spelling of the key beats the plain one.
    qputenv("OPENAI_API_KEY", "sk-plain");
    qputenv("PHOENIX_OPENAI_API_KEY", "sk-prefixed");
    {
        const TranscriptionConfig config = Transcriber::resolveConfig();
        check(config.apiKey == QStringLiteral("sk-prefixed"),
              "the PHOENIX_-prefixed key wins over the plain one");
    }
    qunsetenv("PHOENIX_OPENAI_API_KEY");
    {
        const TranscriptionConfig config = Transcriber::resolveConfig();
        check(config.apiKey == QStringLiteral("sk-plain"),
              "the plain key is used when no prefixed one is set");
    }
    qunsetenv("OPENAI_API_KEY");

    // The environment beats the key file.
    writeKeys(configDir, QJsonObject{{QStringLiteral("openai_api_key"),
                                      QStringLiteral("sk-file")}});
    qputenv("OPENAI_API_KEY", "sk-environment");
    {
        const TranscriptionConfig config = Transcriber::resolveConfig();
        check(config.apiKey == QStringLiteral("sk-environment"),
              "the environment beats the key file");
    }
    qunsetenv("OPENAI_API_KEY");
    qunsetenv("PHOENIX_STT_PROVIDER");

    // The Groq provider: the OpenAI shape on Groq's own base URL.
    writeKeys(configDir, QJsonObject{{QStringLiteral("groq_api_key"),
                                      QStringLiteral("gsk-groq")}});
    qputenv("PHOENIX_STT_PROVIDER", "groq");
    {
        const TranscriptionConfig config = Transcriber::resolveConfig();
        check(config.provider == QStringLiteral("groq"),
              "the groq provider is in force");
        check(config.endpoint
                  == QStringLiteral("https://api.groq.com/openai/v1/audio/transcriptions"),
              "groq's own endpoint is the default");
        check(config.model == QStringLiteral("whisper-large-v3-turbo"),
              "groq's own model is the default");
        check(config.apiKey == QStringLiteral("gsk-groq"),
              "groq's own key is used");
    }
    qunsetenv("PHOENIX_STT_PROVIDER");

    // The custom provider: no endpoint or model of its own, so they
    // must be named; its key lives in its own entry.
    writeKeys(configDir, QJsonObject{{QStringLiteral("stt_api_key"),
                                      QStringLiteral("sk-custom")}});
    qputenv("PHOENIX_STT_PROVIDER", "custom");
    {
        const TranscriptionConfig config = Transcriber::resolveConfig();
        check(config.provider == QStringLiteral("custom"),
              "the custom provider is in force");
        check(config.endpoint.isEmpty(),
              "the custom provider has no endpoint of its own");
        check(config.apiKey == QStringLiteral("sk-custom"),
              "the custom provider's key is used");
    }
    // Named in the key file, it works without the environment.
    qunsetenv("PHOENIX_STT_PROVIDER");
    writeKeys(configDir, QJsonObject{{QStringLiteral("stt_provider"),
                                      QStringLiteral("custom")},
                                     {QStringLiteral("stt_endpoint"),
                                      QStringLiteral("https://elsewhere/v1/audio/transcriptions")},
                                     {QStringLiteral("stt_model"),
                                      QStringLiteral("whisper-large")},
                                     {QStringLiteral("stt_api_key"),
                                      QStringLiteral("sk-custom")}});
    {
        const TranscriptionConfig config = Transcriber::resolveConfig();
        check(config.provider == QStringLiteral("custom"),
              "the custom provider can be chosen in the key file");
        check(config.endpoint
                  == QStringLiteral("https://elsewhere/v1/audio/transcriptions"),
              "the custom endpoint comes from the key file");
        check(config.model == QStringLiteral("whisper-large"),
              "the custom model comes from the key file");
    }

    // An unknown provider falls back to MiniMax, as the settings
    // window's own fallback does.
    writeKeys(configDir, QJsonObject{{QStringLiteral("stt_provider"),
                                      QStringLiteral("nonsense")},
                                     {QStringLiteral("minimax_api_key"),
                                      QStringLiteral("sk-minimax")}});
    {
        const TranscriptionConfig config = Transcriber::resolveConfig();
        check(config.provider == QStringLiteral("minimax"),
              "an unknown provider falls back to minimax");
        check(config.apiKey == QStringLiteral("sk-minimax"),
              "the fallback provider brings its own key");
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}

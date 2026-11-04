// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

// The settings store's provider handling: which provider is in force,
// which keys.json field its key lives in, and that the environment
// overrides the file.
//
// The store needs no window and no shortcuts for this, so it is
// instantiated directly.
//
//   settingsstoretest
//
// Exits non-zero when a check fails. (Plain printf, not qInfo: the
// categorized logging prints nothing in this environment.)

#include "settingsstore.h"

#include <QCoreApplication>
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

void writeKeys(const QString &configDir, const QJsonObject &keys)
{
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
    qputenv("PHOENIX_CONFIG_DIR", configDir.toUtf8());

    // Nothing set: MiniMax is the default everywhere.
    writeKeys(configDir, QJsonObject{{QStringLiteral("minimax_api_key"),
                                      QStringLiteral("sk-minimax")}});
    {
        SettingsStore store(nullptr, nullptr, nullptr, nullptr);
        check(store.llmProvider() == QStringLiteral("minimax"),
              "the default LLM provider is minimax");
        check(store.llmKeyField() == QStringLiteral("minimax_api_key"),
              "minimax's key field is the default");
        check(store.llmApiKey() == QStringLiteral("sk-minimax"),
              "the LLM key comes from the key file");
        check(store.sttProvider() == QStringLiteral("minimax"),
              "the default speech-to-text provider is minimax");
        check(store.speechProvider() == QStringLiteral("minimax"),
              "the default speech provider is minimax");
    }

    // Groq: its own key field, for all three functions.
    writeKeys(configDir, QJsonObject{{QStringLiteral("llm_provider"),
                                      QStringLiteral("groq")},
                                     {QStringLiteral("groq_api_key"),
                                      QStringLiteral("gsk-groq")},
                                     {QStringLiteral("stt_provider"),
                                      QStringLiteral("groq")},
                                     {QStringLiteral("tts_provider"),
                                      QStringLiteral("groq")}});
    {
        SettingsStore store(nullptr, nullptr, nullptr, nullptr);
        check(store.llmProvider() == QStringLiteral("groq"),
              "the LLM provider comes from the key file");
        check(store.llmKeyField() == QStringLiteral("groq_api_key"),
              "groq's key field is used for the LLM");
        check(store.llmApiKey() == QStringLiteral("gsk-groq"),
              "groq's key is the LLM key");
        check(store.sttProvider() == QStringLiteral("groq"),
              "the speech-to-text provider comes from the key file");
        check(store.sttKeyField() == QStringLiteral("groq_api_key"),
              "groq's key field is used for speech-to-text");
        check(store.speechProvider() == QStringLiteral("groq"),
              "the speech provider comes from the key file");
    }

    // The environment beats the file.
    qputenv("PHOENIX_LLM_PROVIDER", "openai");
    {
        SettingsStore store(nullptr, nullptr, nullptr, nullptr);
        check(store.llmProvider() == QStringLiteral("openai"),
              "the environment beats the key file");
        check(store.llmKeyField() == QStringLiteral("openai_api_key"),
              "openai's key field is used when the environment names it");
    }
    qunsetenv("PHOENIX_LLM_PROVIDER");

    // An unknown provider falls back to MiniMax, as the daemon does.
    writeKeys(configDir, QJsonObject{{QStringLiteral("llm_provider"),
                                      QStringLiteral("nonsense")},
                                     {QStringLiteral("tts_provider"),
                                      QStringLiteral("nonsense")},
                                     {QStringLiteral("stt_provider"),
                                      QStringLiteral("nonsense")}});
    {
        SettingsStore store(nullptr, nullptr, nullptr, nullptr);
        check(store.llmProvider() == QStringLiteral("minimax"),
              "an unknown LLM provider falls back to minimax");
        check(store.speechProvider() == QStringLiteral("minimax"),
              "an unknown speech provider falls back to minimax");
        check(store.sttProvider() == QStringLiteral("minimax"),
              "an unknown speech-to-text provider falls back to minimax");
    }

    // The custom providers carry their own key fields.
    writeKeys(configDir, QJsonObject{{QStringLiteral("llm_provider"),
                                      QStringLiteral("custom")},
                                     {QStringLiteral("stt_provider"),
                                      QStringLiteral("custom")}});
    {
        SettingsStore store(nullptr, nullptr, nullptr, nullptr);
        check(store.llmKeyField() == QStringLiteral("llm_api_key"),
              "the custom LLM provider uses the dedicated key field");
        check(store.sttKeyField() == QStringLiteral("stt_api_key"),
              "the custom speech-to-text provider uses its own key field");
    }

    // OpenRouter: its own key field, for all three functions.
    writeKeys(configDir, QJsonObject{{QStringLiteral("llm_provider"),
                                      QStringLiteral("openrouter")},
                                     {QStringLiteral("openrouter_api_key"),
                                      QStringLiteral("sk-or-test")},
                                     {QStringLiteral("stt_provider"),
                                      QStringLiteral("openrouter")},
                                     {QStringLiteral("tts_provider"),
                                      QStringLiteral("openrouter")}});
    {
        SettingsStore store(nullptr, nullptr, nullptr, nullptr);
        check(store.llmProvider() == QStringLiteral("openrouter"),
              "the openrouter LLM provider comes from the key file");
        check(store.llmKeyField() == QStringLiteral("openrouter_api_key"),
              "openrouter's key field is used for the LLM");
        check(store.llmApiKey() == QStringLiteral("sk-or-test"),
              "openrouter's key is the LLM key");
        check(store.sttProvider() == QStringLiteral("openrouter"),
              "the openrouter speech-to-text provider comes from the key file");
        check(store.sttKeyField() == QStringLiteral("openrouter_api_key"),
              "openrouter's key field is used for speech-to-text");
        check(store.speechProvider() == QStringLiteral("openrouter"),
              "the openrouter speech provider comes from the key file");
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}

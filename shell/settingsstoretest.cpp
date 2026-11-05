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
#include "keysfile.h"

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

    // The endpoint is the whole of the configuration: a recognised host
    // decides the provider, and with it the key field.
    writeKeys(configDir, QJsonObject{{QStringLiteral("llm_base_url"),
                                      QStringLiteral("https://api.openai.com/v1")},
                                     {QStringLiteral("openai_api_key"),
                                      QStringLiteral("sk-openai")}});
    {
        SettingsStore store(nullptr, nullptr, nullptr, nullptr);
        check(store.llmProvider() == QStringLiteral("openai"),
              "the endpoint's host decides the LLM provider");
        check(store.llmKeyField() == QStringLiteral("openai_api_key"),
              "the endpoint's provider brings its own key field");
        check(store.llmApiKey() == QStringLiteral("sk-openai"),
              "the endpoint's provider brings its own key");
        check(store.llmBaseUrl() == QStringLiteral("https://api.openai.com/v1"),
              "the endpoint is shown as it was stored");
    }

    // An endpoint of one's own is nobody else's provider.
    writeKeys(configDir, QJsonObject{{QStringLiteral("llm_base_url"),
                                      QStringLiteral("https://my.gateway/v1")},
                                     {QStringLiteral("llm_api_key"),
                                      QStringLiteral("sk-own")}});
    {
        SettingsStore store(nullptr, nullptr, nullptr, nullptr);
        check(store.llmProvider() == QStringLiteral("custom"),
              "an endpoint of one's own means the custom provider");
        check(store.llmKeyField() == QStringLiteral("llm_api_key"),
              "an endpoint of one's own has the dedicated key field");
    }

    // The same for the dictation.
    writeKeys(configDir, QJsonObject{{QStringLiteral("stt_endpoint"),
                                      QStringLiteral("https://api.groq.com/openai/v1/audio/transcriptions")},
                                     {QStringLiteral("groq_api_key"),
                                      QStringLiteral("gsk-groq")}});
    {
        SettingsStore store(nullptr, nullptr, nullptr, nullptr);
        check(store.sttProvider() == QStringLiteral("groq"),
              "the endpoint's host decides the speech-to-text provider");
        check(store.sttKeyField() == QStringLiteral("groq_api_key"),
              "the endpoint's provider brings its own key field");
        check(store.sttEndpoint()
                  == QStringLiteral("https://api.groq.com/openai/v1/audio/transcriptions"),
              "the speech-to-text endpoint is shown as it was stored");
    }

    // What the window writes is what the daemon reads: the endpoint and
    // the model land in keys.json, under the names the daemon's
    // config.py reads them by.
    writeKeys(configDir, QJsonObject{{QStringLiteral("minimax_api_key"),
                                      QStringLiteral("sk-minimax")}});
    {
        SettingsStore store(nullptr, nullptr, nullptr, nullptr);
        check(store.saveLlmEndpoint(QStringLiteral("https://api.openai.com/v1"),
                                    QStringLiteral("gpt-4o")),
              "the endpoint is stored");
        check(store.saveSttEndpoint(
                  QStringLiteral("https://api.groq.com/openai/v1/audio/transcriptions"),
                  QStringLiteral("whisper-large-v3-turbo")),
              "the speech-to-text endpoint is stored");
        check(store.saveTtsEndpoint(
                  QStringLiteral("https://openrouter.ai/api/v1/audio/speech"),
                  QStringLiteral("elevenlabs/eleven-turbo-v2")),
              "the speech endpoint is stored");

        const QJsonObject keys = PhoenixKeys::readKeys();
        check(keys.value(QStringLiteral("llm_base_url")).toString()
                  == QStringLiteral("https://api.openai.com/v1"),
              "the model's endpoint is in keys.json under llm_base_url");
        check(keys.value(QStringLiteral("model")).toString()
                  == QStringLiteral("gpt-4o"),
              "the model is in keys.json under model");
        check(keys.value(QStringLiteral("stt_endpoint")).toString()
                  == QStringLiteral("https://api.groq.com/openai/v1/audio/transcriptions"),
              "the speech-to-text endpoint is in keys.json under stt_endpoint");
        check(keys.value(QStringLiteral("tts_endpoint")).toString()
                  == QStringLiteral("https://openrouter.ai/api/v1/audio/speech"),
              "the speech endpoint is in keys.json under tts_endpoint");
        // The keys that were there before are still there.
        check(keys.value(QStringLiteral("minimax_api_key")).toString()
                  == QStringLiteral("sk-minimax"),
              "the other keys survive the write");
        // And the store now reads back what it wrote.
        check(store.llmBaseUrl() == QStringLiteral("https://api.openai.com/v1"),
              "the endpoint is read back");
        check(store.llmProvider() == QStringLiteral("openai"),
              "the endpoint's host decides the provider");
    }

    // The context window: how much the model holds at once. It lands in
    // keys.json under the name the daemon's config.py reads it by, and
    // a window of zero removes the entry again — the provider's own
    // default is in force then.
    writeKeys(configDir, QJsonObject{{QStringLiteral("minimax_api_key"),
                                      QStringLiteral("sk-minimax")}});
    {
        SettingsStore store(nullptr, nullptr, nullptr, nullptr);
        store.refresh();

        check(store.llmContextWindow() == 0,
              "no context window is named at the start");

        check(store.saveLlmContextWindow(32000),
              "the context window is stored");
        check(PhoenixKeys::readKeys().value(QStringLiteral("context_window")).toString()
                  == QStringLiteral("32000"),
              "the context window is in keys.json under context_window");
        check(store.llmContextWindow() == 32000,
              "the context window is read back");

        // Zero removes it: the provider's own default takes over.
        check(store.saveLlmContextWindow(0),
              "the context window is removed");
        check(!PhoenixKeys::readKeys().contains(QStringLiteral("context_window")),
              "the context window is out of keys.json again");
        check(store.llmContextWindow() == 0,
              "no context window is named any more");
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}

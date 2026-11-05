// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

class QAction;
class QTimer;

/*!
 * \brief The user-level settings the applet owns, read and written from QML.
 *
 * Reached as PhoenixController.settings. Two groups:
 *
 * API keys — the LLM key and the speech-to-text key, stored in
 * ~/.config/phoenix-dna/keys.json (see PhoenixKeys), the file the agent
 * daemon and the transcriber already read. The LLM key is the MiniMax key
 * ("minimax_api_key", the field config.py falls back to for
 * llm_api_key). The speech-to-text key belongs to the configured STT
 * provider — "minimax" by default, "openai" optionally — whose choice is
 * stored as "stt_provider" in the same file and honoured by
 * Transcriber::resolveConfig(). A changed speech-to-text key applies to
 * the very next dictation (the transcriber re-resolves per request); the
 * daemon reads its key once at startup, so a changed LLM key makes the
 * shell restart phoenix.service for the user (via Boost.Process).
 *
 * Global shortcuts — re-binds the four system-wide hotkeys (recording,
 * screenshot, session search, typed prompt) through KGlobalAccel. The
 * bindings land in the same store System Settings edits, so they survive
 * restarts and can still be re-bound there; the properties here report the
 * live bindings.
 *
 * Appearance — the colour scheme of the app's own windows: "system"
 * (the default, following the desktop), "light" or "dark", stored as
 * "ui_theme" in the same keys.json. The shell pushes the property into
 * the shared theme singleton every window reads, so a change applies
 * to all of them the moment it is made.
 *
 * The two fonts of the agent's answers belong here too: the family the
 * answers are set in in light mode and the one they are set in in dark
 * mode, stored as "ui_font_light" and "ui_font_dark", and the sizes
 * they are set in, stored as "ui_font_size_light" and
 * "ui_font_size_dark". They are chosen independently of the colour
 * scheme — the scheme decides which of the two applies, nothing else
 * does.
 *
 * Read-aloud — whether the daemon speaks the agent's answers, and how:
 * stored as "tts_enabled" ("true"/"false"), "tts_read_mode"
 * ("final"/"stream") and "tts_voice" in the same keys.json, the same
 * entries the daemon's config.py reads (below its agent.toml and the
 * environment). A change applies at once: after saving, the shell asks
 * the daemon to rebuild its speech pipeline (ReloadSpeech over D-Bus),
 * so no service restart — and unlike the LLM key above, flipping this
 * off mid-utterance falls silent.
 *
 * Everything the user does is answered with a statusMessage (errors in
 * statusIsError), which the settings window shows. Success messages fade
 * out by themselves; errors stay until the next action.
 */
class SettingsStore : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

    /*! \brief The stored LLM API key ("" when none is). */
    Q_PROPERTY(QString llmApiKey READ llmApiKey NOTIFY llmApiKeyChanged)

    /*! \brief The LLM provider: "minimax" (default), "openai" or "custom".
     *
     *  The provider decides which API key the agent's model calls use
     *  (see llmKeyField) and, in the daemon, which endpoint and model
     *  are the defaults. "custom" is any other OpenAI-compatible
     *  server: its endpoint and model are named in agent.toml, and its
     *  key in the "llm_api_key" entry. */
    Q_PROPERTY(QString llmProvider READ llmProvider NOTIFY llmProviderChanged)

    /*! \brief The endpoint the model's calls go to.
     *
     *  This is the whole of it, really: an OpenAI-compatible base URL
     *  is enough to name a service. The daemon works out the provider
     *  from the host, so a recognised service brings its own model and
     *  key with it, and any other server is the user's own. */
    Q_PROPERTY(QString llmBaseUrl READ llmBaseUrl NOTIFY llmEndpointChanged)

    /*! \brief The model the calls name ("gpt-4o-mini", "llama-3.3-70b").
     *
     *  Empty when the provider's own default is in force. */
    Q_PROPERTY(QString llmModel READ llmModel NOTIFY llmEndpointChanged)

    /*! \brief How many tokens the model holds at once: its context
     *  window.
     *
     *  The context view (the ring in the results window's corner)
     *  measures the conversation against it. Zero when nothing is
     *  named — the provider's own default is in force then, and a
     *  server of the user's own has none until the user says how
     *  much its model holds. */
    Q_PROPERTY(int llmContextWindow READ llmContextWindow NOTIFY llmContextWindowChanged)

    /*! \brief The endpoint the dictation is transcribed at. */
    Q_PROPERTY(QString sttEndpoint READ sttEndpoint NOTIFY sttEndpointChanged)

    /*! \brief The model the transcription names. */
    Q_PROPERTY(QString sttModel READ sttModel NOTIFY sttEndpointChanged)

    /*! \brief The endpoint the voice is synthesized at. */
    Q_PROPERTY(QString ttsEndpoint READ ttsEndpoint NOTIFY ttsEndpointChanged)

    /*! \brief The model the voice names. */
    Q_PROPERTY(QString ttsModel READ ttsModel NOTIFY ttsEndpointChanged)

    /*! \brief The stored speech-to-text API key for the active provider. */
    Q_PROPERTY(QString sttApiKey READ sttApiKey NOTIFY sttApiKeyChanged)

    /*! \brief The speech-to-text provider: "minimax" (default) or "openai". */
    Q_PROPERTY(QString sttProvider READ sttProvider NOTIFY sttProviderChanged)

    /*! \brief The colour scheme of the app's windows: "system" (default),
     *  "light" or "dark". */
    Q_PROPERTY(QString theme READ theme NOTIFY themeChanged)

    /*! \brief The family the agent's answers are set in, in light mode.
     *
     * Empty when nothing is chosen — the app's own default for the
     * mode then. The word "system" for the system's own font. */
    Q_PROPERTY(QString lightFont READ lightFont NOTIFY lightFontChanged)

    /*! \brief The family the agent's answers are set in, in dark mode.
     *
     * Empty when nothing is chosen — the app's own default for the
     * mode then. The word "system" for the system's own font. */
    Q_PROPERTY(QString darkFont READ darkFont NOTIFY darkFontChanged)

    /*! \brief The size the agent's answers are set in, in light mode,
     *  in pixels.
     *
     * Zero when nothing is chosen — the app's own size for the mode
     * then. */
    Q_PROPERTY(int lightFontSize READ lightFontSize NOTIFY lightFontSizeChanged)

    /*! \brief The size the agent's answers are set in, in dark mode,
     *  in pixels.
     *
     * Zero when nothing is chosen — the app's own size for the mode
     * then. */
    Q_PROPERTY(int darkFontSize READ darkFontSize NOTIFY darkFontSizeChanged)

    /*! \brief The keys.json field behind llmApiKey, shown by the UI. */
    Q_PROPERTY(QString llmKeyField READ llmKeyField NOTIFY llmProviderChanged)

    /*! \brief The keys.json field behind sttApiKey, shown by the UI. */
    Q_PROPERTY(QString sttKeyField READ sttKeyField NOTIFY sttProviderChanged)

    /*! \brief Where the keys are stored, for display ("~/..." abbreviated). */
    Q_PROPERTY(QString keysFilePath READ keysFilePath CONSTANT)

    /*! \brief Whether the environment overrides the stored LLM key.
     *
     * Environment variables beat keys.json (same precedence as
     * config.py); a key entered here would be ignored then, so the UI
     * warns. Best effort: the daemon's own environment cannot be seen
     * from the widget, so plasmashell's is checked. */
    Q_PROPERTY(bool llmKeyFromEnvironment READ llmKeyFromEnvironment NOTIFY keysEnvironmentChanged)

    /*! \brief Whether the environment overrides the stored STT key. */
    Q_PROPERTY(bool sttKeyFromEnvironment READ sttKeyFromEnvironment NOTIFY keysEnvironmentChanged)

    /*! \brief Whether the agent reads its answers aloud ("tts_enabled"). */
    Q_PROPERTY(bool speechEnabled READ speechEnabled NOTIFY speechEnabledChanged)

    /*! \brief When the answers are spoken: "final" (default) or "stream". */
    Q_PROPERTY(QString speechReadMode READ speechReadMode NOTIFY speechReadModeChanged)

    /*! \brief The voice id the daemon synthesizes with ("tts_voice"). */
    Q_PROPERTY(QString speechVoice READ speechVoice NOTIFY speechVoiceChanged)

    /*! \brief The speech engine: "minimax" (default), "openai" or "local".
     *
     *  The provider decides which voice ids exist and where the audio is
     *  made: MiniMax and OpenAI send the text to their service, Local
     *  speaks it on this computer. */
    Q_PROPERTY(QString speechProvider READ speechProvider NOTIFY speechProviderChanged)

    /*! \brief How fast the spoken answers play ("tts_playback_speed").
     *
     *  A time-stretch with the pitch kept: 1.0 is the speed the voice
     *  was spoken at, 1.5 half again as fast. Clamped to the window the
     *  daemon's player reaches (0.25–4.0). */
    Q_PROPERTY(qreal speechPlaybackSpeed READ speechPlaybackSpeed NOTIFY speechPlaybackSpeedChanged)

    /*! \brief The keys.json field behind speechEnabled, shown by the UI. */
    Q_PROPERTY(QString speechEnabledField READ speechEnabledField CONSTANT)

    /*! \brief Whether the environment overrides the speech settings.
     *
     * Same caveat as the keys above: plasmashell's environment is
     * checked, best effort. */
    Q_PROPERTY(bool speechFromEnvironment READ speechFromEnvironment NOTIFY keysEnvironmentChanged)

    /*! \brief The answer to the last action, shown by the settings window. */
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

    /*! \brief Whether statusMessage is a complaint rather than a success. */
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusIsErrorChanged)

public:
    /*! \brief \a record, \a search, \a screenshot and \a prompt are the
     *  four actions whose KGlobalAccel bindings applyShortcut()/
     *  clearShortcut() change. */
    SettingsStore(QAction *recordShortcut,
                  QAction *searchShortcut,
                  QAction *screenshotShortcut,
                  QAction *promptShortcut,
                  QObject *parent = nullptr);

    QString llmApiKey() const { return m_llmApiKey; }
    QString llmProvider() const { return m_llmProvider; }
    QString llmBaseUrl() const { return m_llmBaseUrl; }
    QString llmModel() const { return m_llmModel; }
    int llmContextWindow() const { return m_llmContextWindow; }
    QString sttEndpoint() const { return m_sttEndpoint; }
    QString sttModel() const { return m_sttModel; }
    QString ttsEndpoint() const { return m_ttsEndpoint; }
    QString ttsModel() const { return m_ttsModel; }
    QString sttApiKey() const { return m_sttApiKey; }
    QString sttProvider() const { return m_sttProvider; }
    QString theme() const { return m_theme; }
    QString lightFont() const { return m_lightFont; }
    QString darkFont() const { return m_darkFont; }
    int lightFontSize() const { return m_lightFontSize; }
    int darkFontSize() const { return m_darkFontSize; }
    QString llmKeyField() const;
    QString sttKeyField() const;
    QString keysFilePath() const;
    bool llmKeyFromEnvironment() const { return m_llmKeyFromEnvironment; }
    bool sttKeyFromEnvironment() const { return m_sttKeyFromEnvironment; }
    bool speechEnabled() const { return m_speechEnabled; }
    QString speechReadMode() const { return m_speechReadMode; }
    QString speechVoice() const { return m_speechVoice; }
    QString speechProvider() const { return m_speechProvider; }
    qreal speechPlaybackSpeed() const { return m_speechPlaybackSpeed; }
    QString speechEnabledField() const { return QStringLiteral("tts_enabled"); }
    bool speechFromEnvironment() const { return m_speechFromEnvironment; }
    QString statusMessage() const { return m_statusMessage; }
    bool statusIsError() const { return m_statusIsError; }

    /*! \brief Re-reads keys.json and re-derives everything from it.
     *
     * Called when the settings window opens, so a hand-edited file (or one
     * written by the installer) is reflected without a restart. */
    Q_INVOKABLE void refresh();

    /*! \brief Binds the shortcut of \a role ("record", "screenshot",
     *  "search" or "prompt") to the combination the settings window
     *  captured.
     *
     * \a key and \a modifiers are the QKeyEvent's values; \a key 0 means a
     * modifiers-only combination (the picker's "Meta+Alt" style), which
     * only the screenshot picker accepts — a recording, search or prompt
     * hotkey needs a regular key or it would fire on every press of the
     * modifiers alone. */
    Q_INVOKABLE bool applyShortcut(const QString &role, int key, int modifiers);

    /*! \brief Unbinds the shortcut of \a role. */
    Q_INVOKABLE bool clearShortcut(const QString &role);

    /*! \brief Stores (or, when empty, removes) the LLM API key.
     *
     *  The key belongs to the active provider's field (see
     *  llmKeyField), so switching providers and then storing a key
     *  fills in the new one's slot.
     *
     *  The daemon reads its key when it starts, so this restarts
     *  phoenix.service for the user: the new key is in force at once.
     *  A restart that fails is reported in the status line, which then
     *  tells the user to run it by hand. */
    Q_INVOKABLE bool saveLlmApiKey(const QString &apiKey);

    /*! \brief Stores the LLM provider ("minimax"/"openai"/"custom").
     *
     *  The daemon reads it when it starts, so the change needs a
     *  restart of the daemon to take effect. */
    Q_INVOKABLE bool saveLlmProvider(const QString &provider);

    /*! \brief Stores the endpoint the model's calls go to.
     *
     *  An OpenAI-compatible base URL is enough to name a service: the
     *  daemon works out the provider from the host, so a recognised
     *  service brings its own model and key with it. */
    Q_INVOKABLE bool saveLlmEndpoint(const QString &endpoint, const QString &model);

    /*! \brief Stores how many tokens the model holds at once.
     *
     *  Zero removes the entry: the provider's own default is in force
     *  then. The daemon reads it when it starts, so the change needs a
     *  restart of the daemon to take effect. */
    Q_INVOKABLE bool saveLlmContextWindow(int tokens);

    /*! \brief Stores the endpoint the dictation is transcribed at. */
    Q_INVOKABLE bool saveSttEndpoint(const QString &endpoint, const QString &model);

    /*! \brief Stores the endpoint the voice is synthesized at. */
    Q_INVOKABLE bool saveTtsEndpoint(const QString &endpoint, const QString &model);

    /*! \brief Stores (or removes) the speech-to-text key of the active
     *  provider; the next dictation already uses it. */
    Q_INVOKABLE bool saveSttApiKey(const QString &apiKey);

    /*! \brief Stores the speech-to-text provider ("minimax"/"openai"). */
    Q_INVOKABLE bool saveSttProvider(const QString &provider);

    /*! \brief Stores the colour scheme ("system"/"light"/"dark"); every
     *  window follows it immediately through the theme singleton. */
    Q_INVOKABLE bool saveTheme(const QString &theme);

    /*! \brief Stores the family the agent's answers are set in, in light
     *  mode; an empty family restores the app's own default for the
     *  mode, and the word "system" the system's own font. */
    Q_INVOKABLE bool saveLightFont(const QString &family);

    /*! \brief Stores the family the agent's answers are set in, in dark
     *  mode; an empty family restores the app's own default for the
     *  mode, and the word "system" the system's own font. */
    Q_INVOKABLE bool saveDarkFont(const QString &family);

    /*! \brief Stores the size the agent's answers are set in, in light
     *  mode; zero restores the app's own size for the mode. */
    Q_INVOKABLE bool saveLightFontSize(int size);

    /*! \brief Stores the size the agent's answers are set in, in dark
     *  mode; zero restores the app's own size for the mode. */
    Q_INVOKABLE bool saveDarkFontSize(int size);

    /*! \brief Stores whether the agent reads its answers aloud. The
     *  shell separately asks the daemon to reload, so it applies at
     *  once. */
    Q_INVOKABLE bool saveSpeechEnabled(bool enabled);

    /*! \brief Stores when the answers are spoken ("final"/"stream");
     *  see Speaker for what the two modes mean. */
    Q_INVOKABLE bool saveSpeechReadMode(const QString &mode);

    /*! \brief Stores the voice id the daemon synthesizes with. */
    Q_INVOKABLE bool saveSpeechVoice(const QString &voice);

    /*! \brief Stores the speech engine ("minimax"/"openai"/"local").
     *
     *  The daemon rebuilds its speech pipeline when the shell asks it
     *  to, so the change takes effect at once. */
    Q_INVOKABLE bool saveSpeechProvider(const QString &provider);

    /*! \brief Stores the speed the spoken answers play at; the daemon
     *  re-reads it when the shell asks it to reload speech. */
    Q_INVOKABLE bool saveSpeechPlaybackSpeed(qreal speed);

Q_SIGNALS:
    void llmApiKeyChanged();
    void llmProviderChanged();
    void llmEndpointChanged();

    /*! \brief Emitted when the model's context window changed. */
    void llmContextWindowChanged();
    void sttEndpointChanged();
    void ttsEndpointChanged();
    void sttApiKeyChanged();
    void sttProviderChanged();
    void themeChanged();
    void lightFontChanged();
    void darkFontChanged();
    void lightFontSizeChanged();
    void darkFontSizeChanged();
    void speechEnabledChanged();
    void speechReadModeChanged();
    void speechVoiceChanged();
    void speechProviderChanged();
    void speechPlaybackSpeedChanged();
    void keysEnvironmentChanged();
    void statusMessageChanged();
    void statusIsErrorChanged();

    /*! \brief The named hotkey was re-bound or cleared; the controllers
     *  forward these so QML bindings to the live bindings update. */
    void recordShortcutChanged();
    void searchShortcutChanged();
    void screenshotShortcutChanged();
    void promptShortcutChanged();

private:
    QAction *actionForRole(const QString &role) const;

    /*! \brief Stores the family the answers are set in, under \a field,
     *  and says so; \a mode ("light"/"dark") is what the message names.
     *
     * An empty family removes the field again, which is how the
     * system's own font is stored. */
    bool saveAnswerFont(const QString &field, const QString &family,
                        const QString &mode);

    /*! \brief Stores the size the answers are set in, under \a field,
     *  and says so; \a mode ("light"/"dark") is what the message names.
     *
     * A size of zero removes the field again, which is how "the size
     * the window would give the answer anyway" is stored. */
    bool saveAnswerFontSize(const QString &field, int size,
                            const QString &mode);

    /*! \brief Announces that the binding of \a role changed, through the
     *  signal that belongs to it. */
    void emitShortcutChanged(const QString &role);

    /*! \brief Reads keys.json, sets \a field to \a value (or removes it
     *  when empty) and writes the file back — atomically, mode 0600, other
     *  fields untouched. The outcome becomes the status message. */
    bool writeKeyField(const QString &field, const QString &value);

    /*! \brief Sets the status message; successes fade out, errors stay. */
    void setStatus(const QString &message, bool isError);

    /*! \brief The live binding of \a action, in native notation. */
    static QString liveShortcut(QAction *action);

    void refreshEnvironmentFlags();

    QAction *m_recordShortcut = nullptr;
    QAction *m_searchShortcut = nullptr;
    QAction *m_screenshotShortcut = nullptr;
    QAction *m_promptShortcut = nullptr;
    QTimer *m_statusClear = nullptr;
    QString m_llmApiKey;
    QString m_llmProvider = QStringLiteral("minimax");
    QString m_llmBaseUrl;
    QString m_llmModel;
    int m_llmContextWindow = 0;
    QString m_sttEndpoint;
    QString m_sttModel;
    QString m_ttsEndpoint;
    QString m_ttsModel;
    QString m_sttApiKey;
    QString m_sttProvider = QStringLiteral("minimax");
    QString m_theme = QStringLiteral("system");
    QString m_lightFont;
    QString m_darkFont;
    int m_lightFontSize = 0;
    int m_darkFontSize = 0;
    bool m_speechEnabled = false;
    QString m_speechReadMode = QStringLiteral("final");
    QString m_speechVoice = QStringLiteral("English_expressive_narrator");
    QString m_speechProvider = QStringLiteral("minimax");
    qreal m_speechPlaybackSpeed = 1.0;
    QString m_statusMessage;
    bool m_llmKeyFromEnvironment = false;
    bool m_sttKeyFromEnvironment = false;
    bool m_speechFromEnvironment = false;
    bool m_statusIsError = false;
};

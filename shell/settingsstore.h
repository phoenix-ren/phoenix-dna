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
 * daemon reads its key once at startup, so a changed LLM key needs
 * "systemctl --user restart phoenix.service".
 *
 * Global shortcuts — re-binds the three system-wide hotkeys (recording,
 * screenshot, session search) through KGlobalAccel. The bindings land in
 * the same store System Settings edits, so they survive restarts and can
 * still be re-bound there; the properties here report the live bindings.
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

    /*! \brief The stored speech-to-text API key for the active provider. */
    Q_PROPERTY(QString sttApiKey READ sttApiKey NOTIFY sttApiKeyChanged)

    /*! \brief The speech-to-text provider: "minimax" (default) or "openai". */
    Q_PROPERTY(QString sttProvider READ sttProvider NOTIFY sttProviderChanged)

    /*! \brief The keys.json field behind llmApiKey, shown by the UI. */
    Q_PROPERTY(QString llmKeyField READ llmKeyField CONSTANT)

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

    /*! \brief The answer to the last action, shown by the settings window. */
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)

    /*! \brief Whether statusMessage is a complaint rather than a success. */
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusIsErrorChanged)

public:
    /*! \brief \a record, \a search and \a screenshot are the three actions
     *  whose KGlobalAccel bindings applyShortcut()/clearShortcut() change. */
    SettingsStore(QAction *recordShortcut,
                  QAction *searchShortcut,
                  QAction *screenshotShortcut,
                  QObject *parent = nullptr);

    QString llmApiKey() const { return m_llmApiKey; }
    QString sttApiKey() const { return m_sttApiKey; }
    QString sttProvider() const { return m_sttProvider; }
    QString llmKeyField() const { return QStringLiteral("minimax_api_key"); }
    QString sttKeyField() const;
    QString keysFilePath() const;
    bool llmKeyFromEnvironment() const { return m_llmKeyFromEnvironment; }
    bool sttKeyFromEnvironment() const { return m_sttKeyFromEnvironment; }
    QString statusMessage() const { return m_statusMessage; }
    bool statusIsError() const { return m_statusIsError; }

    /*! \brief Re-reads keys.json and re-derives everything from it.
     *
     * Called when the settings window opens, so a hand-edited file (or one
     * written by the installer) is reflected without a restart. */
    Q_INVOKABLE void refresh();

    /*! \brief Binds the shortcut of \a role ("record", "screenshot" or
     *  "search") to the combination the settings window captured.
     *
     * \a key and \a modifiers are the QKeyEvent's values; \a key 0 means a
     * modifiers-only combination (the picker's "Meta+Alt" style), which
     * only the screenshot picker accepts — a recording or search hotkey
     * needs a regular key or it would fire on every press of the
     * modifiers alone. */
    Q_INVOKABLE bool applyShortcut(const QString &role, int key, int modifiers);

    /*! \brief Unbinds the shortcut of \a role. */
    Q_INVOKABLE bool clearShortcut(const QString &role);

    /*! \brief Stores (or, when empty, removes) the LLM API key. */
    Q_INVOKABLE bool saveLlmApiKey(const QString &apiKey);

    /*! \brief Stores (or removes) the speech-to-text key of the active
     *  provider; the next dictation already uses it. */
    Q_INVOKABLE bool saveSttApiKey(const QString &apiKey);

    /*! \brief Stores the speech-to-text provider ("minimax"/"openai"). */
    Q_INVOKABLE bool saveSttProvider(const QString &provider);

Q_SIGNALS:
    void llmApiKeyChanged();
    void sttApiKeyChanged();
    void sttProviderChanged();
    void keysEnvironmentChanged();
    void statusMessageChanged();
    void statusIsErrorChanged();

    /*! \brief The named hotkey was re-bound or cleared; the controllers
     *  forward these so QML bindings to the live bindings update. */
    void recordShortcutChanged();
    void searchShortcutChanged();
    void screenshotShortcutChanged();

private:
    QAction *actionForRole(const QString &role) const;

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
    QTimer *m_statusClear = nullptr;
    QString m_llmApiKey;
    QString m_sttApiKey;
    QString m_sttProvider = QStringLiteral("minimax");
    QString m_statusMessage;
    bool m_llmKeyFromEnvironment = false;
    bool m_sttKeyFromEnvironment = false;
    bool m_statusIsError = false;
};

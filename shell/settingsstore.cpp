// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "settingsstore.h"
#include "keysfile.h"

#include <KGlobalAccel>
#include <KLocalizedString>

#include <QAction>
#include <QDir>
#include <QFileDevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QSaveFile>
#include <QTimer>

namespace
{

// The roles the settings window addresses the four hotkeys by; they match
// the model in SettingsWindow.qml.
const QString kRecordRole = QStringLiteral("record");
const QString kSearchRole = QStringLiteral("search");
const QString kScreenshotRole = QStringLiteral("screenshot");
const QString kPromptRole = QStringLiteral("prompt");

// The speech-to-text providers Transcriber knows; anything else in
// keys.json falls back to MiniMax, like Transcriber::resolveConfig() does.
const QString kSttProviderMiniMax = QStringLiteral("minimax");
const QString kSttProviderOpenAI = QStringLiteral("openai");

// The colour schemes the settings window offers; anything else in
// keys.json follows the system, the resolved default.
const QString kThemeSystem = QStringLiteral("system");
const QString kThemeLight = QStringLiteral("light");
const QString kThemeDark = QStringLiteral("dark");

/*! \brief The modifier bits that can make up a shortcut's modifier part. */
Qt::KeyboardModifiers relevantModifiers(int modifiers)
{
    return Qt::KeyboardModifiers(modifiers)
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
}

/*! \brief Whether \a key is itself a modifier key. */
bool isModifierKey(int key)
{
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
        return true;
    default:
        return false;
    }
}

/*! \brief "Ctrl+Alt+Shift+Meta" for the set bits — the string spelling of
 *  a modifiers-only sequence, as the first-run defaults are built. */
QString modifierString(Qt::KeyboardModifiers modifiers)
{
    QString text;
    if (modifiers & Qt::ControlModifier) {
        text += QStringLiteral("Ctrl+");
    }
    if (modifiers & Qt::AltModifier) {
        text += QStringLiteral("Alt+");
    }
    if (modifiers & Qt::ShiftModifier) {
        text += QStringLiteral("Shift+");
    }
    if (modifiers & Qt::MetaModifier) {
        text += QStringLiteral("Meta+");
    }
    return text.chopped(1); // drop the trailing "+"
}

} // namespace

SettingsStore::SettingsStore(QAction *recordShortcut,
                             QAction *searchShortcut,
                             QAction *screenshotShortcut,
                             QAction *promptShortcut,
                             QObject *parent)
    : QObject(parent),
      m_recordShortcut(recordShortcut),
      m_searchShortcut(searchShortcut),
      m_screenshotShortcut(screenshotShortcut),
      m_promptShortcut(promptShortcut)
{
    // Success notes fade out on their own; complaints stay until the next
    // action, so the user has time to read them.
    m_statusClear = new QTimer(this);
    m_statusClear->setSingleShot(true);
    connect(m_statusClear, &QTimer::timeout, this, [this] {
        if (!m_statusIsError) {
            setStatus(QString(), false);
        }
    });

    refresh();
}

QString SettingsStore::sttKeyField() const
{
    // The field of the active provider, as in Transcriber's provider table.
    return m_sttProvider == kSttProviderOpenAI ? QStringLiteral("openai_api_key")
                                               : QStringLiteral("minimax_api_key");
}

QString SettingsStore::keysFilePath() const
{
    // Display form: the home directory reads as "~", like KDE dialogs show
    // paths. Only informational — the file itself is addressed through
    // PhoenixKeys with the full path.
    const QString path = PhoenixKeys::keysFilePath();
    const QString home = QDir::homePath() + QStringLiteral("/");
    return path.startsWith(home) ? QStringLiteral("~/") + path.mid(home.length()) : path;
}

void SettingsStore::refresh()
{
    const QJsonObject keys = PhoenixKeys::readKeys();

    // Provider precedence, as Transcriber::resolveConfig() resolves it:
    // environment beats keys.json beats the MiniMax default. Anything but
    // "openai" means MiniMax — the fallback there and here agree.
    QString provider = qEnvironmentVariable("PHOENIX_STT_PROVIDER");
    if (provider.isEmpty()) {
        provider = keys.value(QStringLiteral("stt_provider")).toString();
    }
    provider = provider.compare(kSttProviderOpenAI, Qt::CaseInsensitive) == 0
                   ? kSttProviderOpenAI
                   : kSttProviderMiniMax;

    const QString sttKey = keys.value(provider == kSttProviderOpenAI
                                            ? QStringLiteral("openai_api_key")
                                            : QStringLiteral("minimax_api_key"))
                              .toString();
    const QString llmKey = keys.value(QStringLiteral("minimax_api_key")).toString();

    // The colour scheme resolves like the provider does: only the three
    // spellings mean anything, everything else (including no entry at
    // all) is the default, "follow the system".
    QString theme = keys.value(QStringLiteral("ui_theme")).toString();
    theme = theme.compare(kThemeLight, Qt::CaseInsensitive) == 0
                ? kThemeLight
            : theme.compare(kThemeDark, Qt::CaseInsensitive) == 0
                ? kThemeDark
                : kThemeSystem;

    if (llmKey != m_llmApiKey) {
        m_llmApiKey = llmKey;
        Q_EMIT llmApiKeyChanged();
    }
    if (sttKey != m_sttApiKey) {
        m_sttApiKey = sttKey;
        Q_EMIT sttApiKeyChanged();
    }
    if (provider != m_sttProvider) {
        m_sttProvider = provider;
        Q_EMIT sttProviderChanged();
    }
    if (theme != m_theme) {
        m_theme = theme;
        Q_EMIT themeChanged();
    }
    refreshEnvironmentFlags();
}

void SettingsStore::refreshEnvironmentFlags()
{
    // Best effort: the variables are checked in plasmashell's environment,
    // which the daemon (a systemd user service) usually shares. Any of the
    // spellings beats the key file — but only when actually set to
    // something, like config.py's "or" chain treats an empty value as
    // absent — so a key entered here would be ignored.
    const auto overrides = [](const char *primary, const char *secondary) {
        return (!qEnvironmentVariableIsEmpty(primary) && qEnvironmentVariableIsSet(primary))
            || (!qEnvironmentVariableIsEmpty(secondary) && qEnvironmentVariableIsSet(secondary));
    };
    const bool llm = overrides("MINIMAX_API_KEY", "PHOENIX_MINIMAX_API_KEY")
        || !qEnvironmentVariableIsEmpty("PHOENIX_LLM_API_KEY");
    const bool stt = m_sttProvider == kSttProviderOpenAI
        ? overrides("OPENAI_API_KEY", "PHOENIX_OPENAI_API_KEY")
        : overrides("MINIMAX_API_KEY", "PHOENIX_MINIMAX_API_KEY");

    if (llm != m_llmKeyFromEnvironment || stt != m_sttKeyFromEnvironment) {
        m_llmKeyFromEnvironment = llm;
        m_sttKeyFromEnvironment = stt;
        Q_EMIT keysEnvironmentChanged();
    }
}

QAction *SettingsStore::actionForRole(const QString &role) const
{
    if (role == kRecordRole) {
        return m_recordShortcut;
    }
    if (role == kSearchRole) {
        return m_searchShortcut;
    }
    if (role == kScreenshotRole) {
        return m_screenshotShortcut;
    }
    if (role == kPromptRole) {
        return m_promptShortcut;
    }
    return nullptr;
}

QString SettingsStore::liveShortcut(QAction *action)
{
    // The live binding rather than any default — the same reading the
    // controller and the picker expose as their shortcut properties.
    const QList<QKeySequence> shortcuts = KGlobalAccel::self()->shortcut(action);
    for (const QKeySequence &shortcut : shortcuts) {
        if (!shortcut.isEmpty()) {
            return shortcut.toString(QKeySequence::NativeText);
        }
    }
    return QString();
}

bool SettingsStore::applyShortcut(const QString &role, int key, int modifiers)
{
    QAction *action = actionForRole(role);
    if (!action) {
        setStatus(i18n("Unknown shortcut: %1", role), true);
        return false;
    }

    // Modifier-only combinations (the picker's "Meta+Alt" style) arrive as
    // key 0 from the capture button, or as the modifier key itself; both
    // spell the sequence from the modifier bits alone, exactly how the
    // first-run defaults are built.
    const Qt::KeyboardModifiers mods = relevantModifiers(modifiers);
    QKeySequence sequence;
    if (key == 0 || isModifierKey(key)) {
        if (mods == 0) {
            setStatus(i18n("That is not a usable key combination."), true);
            return false;
        }
        // Only the picker accepts a modifiers-only binding: for recording,
        // search and the typed prompt it would swallow every press of the
        // bare modifiers (and collide with the picker's grab).
        if (role != kScreenshotRole) {
            setStatus(i18n("The %1 shortcut needs a regular key besides the modifiers.",
                           action->text()),
                      true);
            return false;
        }
        sequence = QKeySequence(modifierString(mods));
    } else {
        if (key == Qt::Key_unknown) {
            setStatus(i18n("That is not a usable key combination."), true);
            return false;
        }
        sequence = QKeySequence(QKeyCombination(mods, Qt::Key(key)));
    }
    if (sequence.isEmpty() || sequence.count() != 1) {
        setStatus(i18n("That is not a usable key combination."), true);
        return false;
    }

    // NoAutoloading: with the default flag KGlobalAccel would answer with
    // the stored binding and the call would change nothing.
    if (!KGlobalAccel::self()->setShortcut(action, {sequence}, KGlobalAccel::NoAutoloading)) {
        setStatus(i18n("“%1” could not be registered — it is probably already in use.",
                       sequence.toString(QKeySequence::NativeText)),
                  true);
        return false;
    }

    // Confirm the binding really took hold rather than trusting the call.
    if (liveShortcut(action) != sequence.toString(QKeySequence::NativeText)) {
        setStatus(i18n("“%1” could not be registered — it is probably already in use.",
                       sequence.toString(QKeySequence::NativeText)),
                  true);
        return false;
    }

    setStatus(i18n("%1 is now %2.", action->text(), sequence.toString(QKeySequence::NativeText)),
              false);
    emitShortcutChanged(role);
    return true;
}

bool SettingsStore::clearShortcut(const QString &role)
{
    QAction *action = actionForRole(role);
    if (!action) {
        setStatus(i18n("Unknown shortcut: %1", role), true);
        return false;
    }

    if (!KGlobalAccel::self()->setShortcut(action, {}, KGlobalAccel::NoAutoloading)) {
        setStatus(i18n("The shortcut of %1 could not be removed.", action->text()), true);
        return false;
    }

    setStatus(i18n("The shortcut of %1 was removed.", action->text()), false);
    emitShortcutChanged(role);
    return true;
}

void SettingsStore::emitShortcutChanged(const QString &role)
{
    // The changed signal that belongs to the role: the controllers
    // forward them, so every place that shows a live binding (the
    // buttons' tooltips, the settings window's rows) updates at once.
    if (role == kRecordRole) {
        Q_EMIT recordShortcutChanged();
    } else if (role == kSearchRole) {
        Q_EMIT searchShortcutChanged();
    } else if (role == kPromptRole) {
        Q_EMIT promptShortcutChanged();
    } else {
        Q_EMIT screenshotShortcutChanged();
    }
}

bool SettingsStore::writeKeyField(const QString &field, const QString &value)
{
    // Read-modify-write, so the fields the UI does not show (tavily, exa)
    // survive. QSaveFile commits atomically: a crash mid-write can never
    // leave a half-written keys.json behind, and the mode is kept at 0600,
    // the mode the daemon expects of the file.
    const QJsonObject keys = PhoenixKeys::readKeys();
    QJsonObject updated = keys;
    if (value.isEmpty()) {
        updated.remove(field);
    } else {
        updated.insert(field, value);
    }

    QDir().mkpath(PhoenixKeys::configDir());
    QSaveFile file(PhoenixKeys::keysFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(i18n("Could not write %1: %2",
                       PhoenixKeys::keysFilePath(), file.errorString()),
                  true);
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(updated).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        setStatus(i18n("Could not write %1: %2",
                       PhoenixKeys::keysFilePath(), file.errorString()),
                  true);
        return false;
    }

    if (updated != keys) {
        refresh();
    }
    return true;
}

bool SettingsStore::saveLlmApiKey(const QString &apiKey)
{
    if (!writeKeyField(llmKeyField(), apiKey.trimmed())) {
        return false;
    }

    // The daemon resolves its key once, at startup — unlike the
    // transcriber, which re-reads keys.json per request.
    if (apiKey.trimmed().isEmpty()) {
        setStatus(i18n("The LLM API key was removed from %1.", keysFilePath()), false);
    } else {
        setStatus(i18n("The LLM API key was stored. Restart the agent daemon to apply it: "
                       "systemctl --user restart phoenix.service"),
                  false);
    }
    return true;
}

bool SettingsStore::saveSttApiKey(const QString &apiKey)
{
    if (!writeKeyField(sttKeyField(), apiKey.trimmed())) {
        return false;
    }

    // The transcriber re-resolves its configuration for every clip, so the
    // very next dictation already runs with the new key.
    if (apiKey.trimmed().isEmpty()) {
        setStatus(i18n("The speech-to-text key was removed from %1.", keysFilePath()), false);
    } else {
        setStatus(i18n("The speech-to-text key was stored; the next dictation uses it."), false);
    }
    return true;
}

bool SettingsStore::saveSttProvider(const QString &provider)
{
    if (provider.compare(kSttProviderOpenAI, Qt::CaseInsensitive) == 0
        || provider.compare(kSttProviderMiniMax, Qt::CaseInsensitive) == 0) {
        if (!writeKeyField(QStringLiteral("stt_provider"), provider.toLower())) {
            return false;
        }
        setStatus(i18n("Speech-to-text provider set to %1.",
                       provider.compare(kSttProviderOpenAI, Qt::CaseInsensitive) == 0
                           ? i18n("OpenAI")
                           : i18n("MiniMax")),
                  false);
        return true;
    }

    setStatus(i18n("Unknown speech-to-text provider: %1", provider), true);
    return false;
}

bool SettingsStore::saveTheme(const QString &theme)
{
    // Unknown values become "system" rather than an error: a hand-edited
    // keys.json with a typo should not lock the combo (and refresh()
    // normalises the same way).
    const QString normalized = theme.compare(kThemeLight, Qt::CaseInsensitive) == 0
        ? kThemeLight
        : theme.compare(kThemeDark, Qt::CaseInsensitive) == 0
        ? kThemeDark
        : kThemeSystem;
    if (!writeKeyField(QStringLiteral("ui_theme"), normalized)) {
        return false;
    }

    setStatus(i18n("Color scheme set to %1.",
                   normalized == kThemeLight ? i18n("Light")
                   : normalized == kThemeDark ? i18n("Dark")
                                              : i18n("Follow system")),
              false);
    return true;
}

void SettingsStore::setStatus(const QString &message, bool isError)
{
    if (m_statusMessage == message && m_statusIsError == isError) {
        return;
    }
    m_statusMessage = message;
    m_statusIsError = isError;
    Q_EMIT statusMessageChanged();
    Q_EMIT statusIsErrorChanged();

    if (!isError && !message.isEmpty()) {
        m_statusClear->start(4000);
    } else {
        m_statusClear->stop();
    }
}

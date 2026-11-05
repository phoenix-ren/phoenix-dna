// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "settingsstore.h"
#include "keysfile.h"
#include "providerendpoint.h"

#include <boost/asio.hpp>
#include <boost/process/v2/process.hpp>

#include <KGlobalAccel>
#include <KLocalizedString>

#include <QAction>
#include <QDir>
#include <QFile>
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
const QString kSttProviderGroq = QStringLiteral("groq");
const QString kSttProviderOpenRouter = QStringLiteral("openrouter");
const QString kSttProviderCustom = QStringLiteral("custom");

// The LLM providers the agent daemon knows. Each brings its own API
// key field; the daemon's config.py resolves the same names (see
// agent/phoenix_dna/config.py, LLM_PROVIDERS).
const QString kLlmProviderMiniMax = QStringLiteral("minimax");
const QString kLlmProviderOpenAI = QStringLiteral("openai");
const QString kLlmProviderGroq = QStringLiteral("groq");
const QString kLlmProviderOpenRouter = QStringLiteral("openrouter");
const QString kLlmProviderCustom = QStringLiteral("custom");

// The speech engines the daemon knows; anything else in keys.json
// falls back to MiniMax, like the daemon's config.py does.
const QString kSpeechProviderMiniMax = QStringLiteral("minimax");
const QString kSpeechProviderOpenAI = QStringLiteral("openai");
const QString kSpeechProviderGroq = QStringLiteral("groq");
const QString kSpeechProviderOpenRouter = QStringLiteral("openrouter");
const QString kSpeechProviderLocal = QStringLiteral("local");

/*! \brief The keys.json field the API key of \a provider lives in.
 *
 * "custom" is a gateway or a self-hosted server: it has no key of its
 * own, so the dedicated "llm_api_key" entry serves it. */
QString llmKeyFieldFor(const QString &provider)
{
    if (provider.compare(kLlmProviderOpenAI, Qt::CaseInsensitive) == 0) {
        return QStringLiteral("openai_api_key");
    }
    if (provider.compare(kLlmProviderGroq, Qt::CaseInsensitive) == 0) {
        return QStringLiteral("groq_api_key");
    }
    if (provider.compare(kLlmProviderOpenRouter, Qt::CaseInsensitive) == 0) {
        return QStringLiteral("openrouter_api_key");
    }
    if (provider.compare(kLlmProviderCustom, Qt::CaseInsensitive) == 0) {
        return QStringLiteral("llm_api_key");
    }
    return QStringLiteral("minimax_api_key");
}

// The colour schemes the settings window offers; anything else in
// keys.json follows the system, the resolved default.
const QString kThemeSystem = QStringLiteral("system");
const QString kThemeLight = QStringLiteral("light");
const QString kThemeDark = QStringLiteral("dark");

// The word that stands for the system's own font among the three a
// family can be: nothing at all (the app's own default for the mode),
// this word, or a family by name.
const QString kSystemFontChoice = QStringLiteral("system");

/*! \brief The modifier bits that can make up a shortcut's modifier part. */
Qt::KeyboardModifiers relevantModifiers(int modifiers)
{
    return Qt::KeyboardModifiers(modifiers)
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
}

/*! \brief The size stored under \a field in keys.json, in pixels.
 *
 * Every value of the file is a string, but a hand-edited one may be
 * a number; both are read, and anything that is not a size at all
 * comes out as zero — "the size the window would give the answer
 * anyway". */
/*! \brief The playback speed stored under \a field in keys.json.
 *
 * The window writes strings; a hand-edited file may carry a real
 * number, and both are honoured (the same arrangement as fontSize).
 * A speed that cannot be read keeps the voice's own: 1.0. Anything
 * readable is clamped to the window the daemon's player stretches
 * within (atempo chaining, see speech/player.py there). */
qreal playbackSpeed(const QJsonObject &keys, const QString &field)
{
    const QJsonValue value = keys.value(field);
    bool isNumber = value.isDouble();
    const qreal speed = value.isDouble()
        ? value.toDouble()
        : value.toString().toDouble(&isNumber);
    return isNumber ? qBound(0.25, speed, 4.0) : 1.0;
}

int fontSize(const QJsonObject &keys, const QString &field)
{
    const QJsonValue value = keys.value(field);
    if (value.isDouble()) {
        return value.toInt();
    }
    bool isNumber = false;
    const int size = value.toString().toInt(&isNumber);
    return isNumber ? size : 0;
}

/*! \brief The family stored under \a field in keys.json.
 *
 * No entry at all: nothing was chosen, and the app's own default for
 * the mode applies. The word "system": the system's own font.
 * Anything else: a family by name. */
QString answerFont(const QJsonObject &keys, const QString &field)
{
    if (!keys.contains(field)) {
        return QString();
    }
    const QString value = keys.value(field).toString();
    return value.isEmpty() ? kSystemFontChoice : value;
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

/*! \brief Whether a keys.json value means true.
 *
 * The settings window writes strings ("true"/"false"), but a
 * hand-edited file may carry a real boolean; both are read, exactly
 * like config.py's bool casting. */
bool truthyValue(const QJsonValue &value)
{
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isDouble()) {
        return value.toInt() != 0;
    }
    const QString text = value.toString().trimmed().toLower();
    return text == QLatin1String("true") || text == QLatin1String("1")
        || text == QLatin1String("yes") || text == QLatin1String("on");
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

QString SettingsStore::llmKeyField() const
{
    // The field of the active provider, as the daemon's config.py
    // resolves it (LLM_PROVIDERS).
    return llmKeyFieldFor(m_llmProvider);
}

QString SettingsStore::sttKeyField() const
{
    // The field of the active provider, as in Transcriber's provider table.
    if (m_sttProvider == kSttProviderOpenAI) {
        return QStringLiteral("openai_api_key");
    }
    if (m_sttProvider == kSttProviderGroq) {
        return QStringLiteral("groq_api_key");
    }
    if (m_sttProvider == kSttProviderOpenRouter) {
        return QStringLiteral("openrouter_api_key");
    }
    if (m_sttProvider == kSttProviderCustom) {
        return QStringLiteral("stt_api_key");
    }
    return QStringLiteral("minimax_api_key");
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
    // environment beats keys.json beats the MiniMax default. Anything
    // that is not one of the known names means MiniMax — the fallback
    // there and here agree.
    QString provider = qEnvironmentVariable("PHOENIX_STT_PROVIDER");
    if (provider.isEmpty()) {
        provider = keys.value(QStringLiteral("stt_provider")).toString();
    }
    provider = provider.toLower();
    if (provider != kSttProviderOpenAI && provider != kSttProviderGroq
        && provider != kSttProviderOpenRouter && provider != kSttProviderCustom) {
        provider = kSttProviderMiniMax;
    }

    // A recognised host in the endpoint is a better signal than the
    // provider name: the user who typed the URL means that service.
    // The same rule as Transcriber::resolveConfig() and the daemon's
    // config.py, so all three read the same provider out of the same
    // settings.
    const QString sttEndpoint =
        keys.value(QStringLiteral("stt_endpoint")).toString();
    const QString inferred = providerForEndpoint(sttEndpoint);
    if (!inferred.isEmpty()) {
        provider = inferred;
    }

    const QString sttKey = keys.value(provider == kSttProviderOpenAI
                                            ? QStringLiteral("openai_api_key")
                                            : provider == kSttProviderGroq
                                                  ? QStringLiteral("groq_api_key")
                                                  : provider == kSttProviderOpenRouter
                                                        ? QStringLiteral("openrouter_api_key")
                                                        : provider == kSttProviderCustom
                                                              ? QStringLiteral("stt_api_key")
                                                              : QStringLiteral("minimax_api_key"))
                              .toString();

    // The LLM provider decides which key the agent's own model calls
    // use. The environment wins over the file, as everywhere else;
    // an unknown name falls back to MiniMax, which is the daemon's own
    // default (config.py's LLM_PROVIDERS).
    QString llmProvider = qEnvironmentVariable("PHOENIX_LLM_PROVIDER");
    if (llmProvider.isEmpty()) {
        llmProvider = keys.value(QStringLiteral("llm_provider")).toString();
    }
    llmProvider = llmProvider.toLower();
    if (llmProvider != kLlmProviderOpenAI && llmProvider != kLlmProviderGroq
        && llmProvider != kLlmProviderOpenRouter
        && llmProvider != kLlmProviderCustom) {
        llmProvider = kLlmProviderMiniMax;
    }

    // A recognised host in the endpoint decides, the same way it does
    // in the daemon and in the transcriber: the user who typed the URL
    // means that service.
    const QString llmBaseUrl =
        keys.value(QStringLiteral("llm_base_url")).toString();
    const QString llmInferred = providerForEndpoint(llmBaseUrl);
    if (!llmInferred.isEmpty()) {
        llmProvider = llmInferred;
    } else if (!llmBaseUrl.isEmpty()) {
        // An endpoint of one's own is nobody else's provider, so it
        // gets the dedicated key.
        llmProvider = kLlmProviderCustom;
    }

    const QString llmKey = keys.value(llmKeyFieldFor(llmProvider)).toString();

    // The colour scheme resolves like the provider does: only the three
    // spellings mean anything, everything else (including no entry at
    // all) is the default, "follow the system".
    QString theme = keys.value(QStringLiteral("ui_theme")).toString();
    theme = theme.compare(kThemeLight, Qt::CaseInsensitive) == 0
                ? kThemeLight
            : theme.compare(kThemeDark, Qt::CaseInsensitive) == 0
                ? kThemeDark
                : kThemeSystem;

    // The fonts of the answers: a family name, the word "system" for
    // the system's own font, or nothing at all — no entry — for the
    // app's own default for the mode. A hand-edited file with a family
    // that is not installed is kept as it is — the settings window
    // falls back to the system's font for it, and the choice becomes
    // selectable again once the family is there.
    const QString lightFont = answerFont(keys, QStringLiteral("ui_font_light"));
    const QString darkFont = answerFont(keys, QStringLiteral("ui_font_dark"));

    // The sizes of the answers, in pixels; zero means "the app's own
    // size for the mode". A size that makes no sense is read as that
    // zero rather than as a size of nothing.
    // The file keeps every value as a string, but a hand-edited one
    // may well be a number.
    const int lightFontSize = qMax(0, fontSize(keys, QStringLiteral("ui_font_size_light")));
    const int darkFontSize = qMax(0, fontSize(keys, QStringLiteral("ui_font_size_dark")));

    // Read-aloud: the same entries the daemon's config.py reads (see
    // its _TTS_KEY_FIELDS). The file writes strings; a hand-edited real
    // boolean is honoured too. "stream" is the only other mode than the
    // default "final" (see Speaker for what they mean).
    const bool speechEnabled = truthyValue(keys.value(QStringLiteral("tts_enabled")));
    QString speechReadMode = keys.value(QStringLiteral("tts_read_mode")).toString();
    if (speechReadMode.compare(QStringLiteral("stream"), Qt::CaseInsensitive) != 0) {
        speechReadMode = QStringLiteral("final");
    }
    // The speech engine: the daemon's config.py knows the same three
    // names (TTS_PROVIDERS); anything else means MiniMax.
    QString speechProvider = keys.value(QStringLiteral("tts_provider")).toString().toLower();
    if (speechProvider != kSpeechProviderOpenAI && speechProvider != kSpeechProviderGroq
        && speechProvider != kSpeechProviderOpenRouter
        && speechProvider != kSpeechProviderLocal) {
        speechProvider = kSpeechProviderMiniMax;
    }
    QString speechVoice = keys.value(QStringLiteral("tts_voice")).toString();
    if (speechVoice.isEmpty()) {
        speechVoice = QStringLiteral("English_expressive_narrator");
    }
    const qreal speechPlaybackSpeed =
        playbackSpeed(keys, QStringLiteral("tts_playback_speed"));

    if (llmKey != m_llmApiKey) {
        m_llmApiKey = llmKey;
        Q_EMIT llmApiKeyChanged();
    }
    if (llmProvider != m_llmProvider) {
        m_llmProvider = llmProvider;
        Q_EMIT llmProviderChanged();
    }

    // The endpoints and the models, as the window wrote them. These
    // are the whole of the configuration: the daemon works out the
    // provider from the endpoint's host, so a recognised service brings
    // its own model and key with it.
    const QString llmModel = keys.value(QStringLiteral("model")).toString();
    const QString sttModel = keys.value(QStringLiteral("stt_model")).toString();
    const QString ttsEndpoint = keys.value(QStringLiteral("tts_endpoint")).toString();
    const QString ttsModel = keys.value(QStringLiteral("tts_model")).toString();

    if (llmBaseUrl != m_llmBaseUrl || llmModel != m_llmModel) {
        m_llmBaseUrl = llmBaseUrl;
        m_llmModel = llmModel;
        Q_EMIT llmEndpointChanged();
    }
    if (sttEndpoint != m_sttEndpoint || sttModel != m_sttModel) {
        m_sttEndpoint = sttEndpoint;
        m_sttModel = sttModel;
        Q_EMIT sttEndpointChanged();
    }
    if (ttsEndpoint != m_ttsEndpoint || ttsModel != m_ttsModel) {
        m_ttsEndpoint = ttsEndpoint;
        m_ttsModel = ttsModel;
        Q_EMIT ttsEndpointChanged();
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
    if (lightFont != m_lightFont) {
        m_lightFont = lightFont;
        Q_EMIT lightFontChanged();
    }
    if (darkFont != m_darkFont) {
        m_darkFont = darkFont;
        Q_EMIT darkFontChanged();
    }
    if (lightFontSize != m_lightFontSize) {
        m_lightFontSize = lightFontSize;
        Q_EMIT lightFontSizeChanged();
    }
    if (darkFontSize != m_darkFontSize) {
        m_darkFontSize = darkFontSize;
        Q_EMIT darkFontSizeChanged();
    }
    if (speechEnabled != m_speechEnabled) {
        m_speechEnabled = speechEnabled;
        Q_EMIT speechEnabledChanged();
    }
    if (speechReadMode != m_speechReadMode) {
        m_speechReadMode = speechReadMode;
        Q_EMIT speechReadModeChanged();
    }
    if (speechVoice != m_speechVoice) {
        m_speechVoice = speechVoice;
        Q_EMIT speechVoiceChanged();
    }
    if (speechProvider != m_speechProvider) {
        m_speechProvider = speechProvider;
        Q_EMIT speechProviderChanged();
    }
    if (speechPlaybackSpeed != m_speechPlaybackSpeed) {
        m_speechPlaybackSpeed = speechPlaybackSpeed;
        Q_EMIT speechPlaybackSpeedChanged();
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
    const auto overrides = [](const char *prefixed, const char *plain) {
        return (!qEnvironmentVariableIsEmpty(prefixed) && qEnvironmentVariableIsSet(prefixed))
            || (!qEnvironmentVariableIsEmpty(plain) && qEnvironmentVariableIsSet(plain));
    };
    const bool llm = (m_llmProvider == kLlmProviderOpenAI
                      ? overrides("PHOENIX_OPENAI_API_KEY", "OPENAI_API_KEY")
                      : m_llmProvider == kLlmProviderGroq
                            ? overrides("PHOENIX_GROQ_API_KEY", "GROQ_API_KEY")
                            : m_llmProvider == kLlmProviderOpenRouter
                                  ? overrides("PHOENIX_OPENROUTER_API_KEY", "OPENROUTER_API_KEY")
                                  : overrides("PHOENIX_MINIMAX_API_KEY", "MINIMAX_API_KEY"))
        || !qEnvironmentVariableIsEmpty("PHOENIX_LLM_API_KEY");
    const bool stt = m_sttProvider == kSttProviderOpenAI
        ? overrides("PHOENIX_OPENAI_API_KEY", "OPENAI_API_KEY")
        : m_sttProvider == kSttProviderGroq
              ? overrides("PHOENIX_GROQ_API_KEY", "GROQ_API_KEY")
              : m_sttProvider == kSttProviderOpenRouter
                    ? overrides("PHOENIX_OPENROUTER_API_KEY", "OPENROUTER_API_KEY")
                    : m_sttProvider == kSttProviderCustom
                          ? overrides("PHOENIX_STT_API_KEY", "")
                          : overrides("PHOENIX_MINIMAX_API_KEY", "MINIMAX_API_KEY");
    // The speech settings are read by the daemon from keys.json, but the
// environment overrides them — and with them the key the engine uses,
// which belongs to the provider in force.
const bool speech = overrides("PHOENIX_TTS_API_KEY", "TTS_API_KEY")
        || (m_speechProvider == kSpeechProviderOpenAI
                ? overrides("PHOENIX_OPENAI_API_KEY", "OPENAI_API_KEY")
                : m_speechProvider == kSpeechProviderGroq
                      ? overrides("PHOENIX_GROQ_API_KEY", "GROQ_API_KEY")
                      : m_speechProvider == kSpeechProviderOpenRouter
                            ? overrides("PHOENIX_OPENROUTER_API_KEY", "OPENROUTER_API_KEY")
                            : m_speechProvider == kSpeechProviderMiniMax
                                  ? overrides("PHOENIX_MINIMAX_API_KEY", "MINIMAX_API_KEY")
                                  : false)
        || !qEnvironmentVariableIsEmpty("PHOENIX_TTS_ENABLED")
        || !qEnvironmentVariableIsEmpty("PHOENIX_TTS_READ_MODE")
        || !qEnvironmentVariableIsEmpty("PHOENIX_TTS_VOICE");

    if (llm != m_llmKeyFromEnvironment || stt != m_sttKeyFromEnvironment
        || speech != m_speechFromEnvironment) {
        m_llmKeyFromEnvironment = llm;
        m_sttKeyFromEnvironment = stt;
        m_speechFromEnvironment = speech;
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
    // transcriber, which re-reads keys.json per request. Restart the
    // agent daemon ourselves, so the new key is in force at once: the
    // user does not have to run systemctl by hand. Boost.Process does
    // the spawning, so no shell is involved.
    bool restarted = false;
    QString restartError;
    try {
        boost::asio::io_context io;
        boost::system::error_code ec;
        QString systemctlPath = QStringLiteral("/usr/bin/systemctl");
        if (!QFile::exists(systemctlPath)) {
            systemctlPath = QStringLiteral("/bin/systemctl");
        }
        if (!QFile::exists(systemctlPath)) {
            systemctlPath = QStringLiteral("systemctl");
        }
        boost::process::v2::process proc(
            io, systemctlPath.toStdString(),
            {QStringLiteral("--user").toStdString(),
             QStringLiteral("restart").toStdString(),
             QStringLiteral("phoenix.service").toStdString()},
            ec);
        if (ec) {
            restartError = QString::fromStdString(ec.message());
        } else {
            proc.wait(ec);
            if (ec) {
                restartError = QString::fromStdString(ec.message());
            } else if (proc.exit_code() != 0) {
                restartError = i18n("exit code %1", proc.exit_code());
            } else {
                restarted = true;
            }
        }
    } catch (const std::exception &ex) {
        restartError = QString::fromUtf8(ex.what());
    }

    if (apiKey.trimmed().isEmpty()) {
        if (restarted) {
            setStatus(i18n("The LLM API key was removed from %1 and the "
                           "agent daemon restarted.", keysFilePath()),
                      false);
        } else {
            setStatus(i18n("The LLM API key was removed from %1. Restart the "
                           "agent daemon to apply it: systemctl --user "
                           "restart phoenix.service (%2)",
                           keysFilePath(), restartError),
                      false);
        }
    } else if (restarted) {
        setStatus(i18n("The LLM API key was stored and the agent daemon "
                       "restarted."),
                  false);
    } else {
        setStatus(i18n("The LLM API key was stored. Restart the agent daemon "
                       "to apply it: systemctl --user restart phoenix.service "
                       "(%1)", restartError),
                  false);
    }
    return true;
}

bool SettingsStore::saveLlmProvider(const QString &provider)
{
    const QString name = provider.trimmed().toLower();
    if (name != kLlmProviderMiniMax && name != kLlmProviderOpenAI
        && name != kLlmProviderGroq && name != kLlmProviderOpenRouter
        && name != kLlmProviderCustom) {
        setStatus(i18n("Unknown LLM provider: %1", provider), true);
        return false;
    }
    if (!writeKeyField(QStringLiteral("llm_provider"), name)) {
        return false;
    }
    // The daemon reads its provider when it starts, like its key.
    setStatus(i18n("LLM provider set to %1. Restart the agent daemon to apply it: "
                   "systemctl --user restart phoenix.service"),
              false);
    return true;
}

bool SettingsStore::saveLlmEndpoint(const QString &endpoint, const QString &model)
{
    // Two fields, written together: the endpoint and the model belong
    // to the same choice, and a half-written pair is a configuration
    // that cannot work.
    if (!writeKeyField(QStringLiteral("llm_base_url"), endpoint.trimmed())) {
        return false;
    }
    if (!writeKeyField(QStringLiteral("model"), model.trimmed())) {
        return false;
    }
    // The daemon resolves its endpoint when it starts, like its key.
    setStatus(i18n("The endpoint was stored. Restart the agent daemon to apply it: "
                   "systemctl --user restart phoenix.service"),
              false);
    return true;
}

bool SettingsStore::saveSttEndpoint(const QString &endpoint, const QString &model)
{
    if (!writeKeyField(QStringLiteral("stt_endpoint"), endpoint.trimmed())) {
        return false;
    }
    if (!writeKeyField(QStringLiteral("stt_model"), model.trimmed())) {
        return false;
    }
    // The transcriber re-resolves its configuration for every clip, so
    // the very next dictation already runs with the new endpoint.
    setStatus(i18n("The speech-to-text endpoint was stored; the next dictation uses it."),
              false);
    return true;
}

bool SettingsStore::saveTtsEndpoint(const QString &endpoint, const QString &model)
{
    if (!writeKeyField(QStringLiteral("tts_endpoint"), endpoint.trimmed())) {
        return false;
    }
    if (!writeKeyField(QStringLiteral("tts_model"), model.trimmed())) {
        return false;
    }
    setStatus(i18n("The speech endpoint was stored."), false);
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
    const QString name = provider.trimmed().toLower();
    if (name == kSttProviderOpenAI || name == kSttProviderMiniMax
        || name == kSttProviderGroq || name == kSttProviderOpenRouter
        || name == kSttProviderCustom) {
        if (!writeKeyField(QStringLiteral("stt_provider"), name)) {
            return false;
        }
        QString label;
        if (name == kSttProviderOpenAI) {
            label = i18n("OpenAI");
        } else if (name == kSttProviderGroq) {
            label = i18n("Groq");
        } else if (name == kSttProviderOpenRouter) {
            label = i18n("OpenRouter");
        } else if (name == kSttProviderCustom) {
            label = i18n("Custom");
        } else {
            label = i18n("MiniMax");
        }
        setStatus(i18n("Speech-to-text provider set to %1.", label), false);
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

bool SettingsStore::saveLightFont(const QString &family)
{
    return saveAnswerFont(QStringLiteral("ui_font_light"), family, kThemeLight);
}

bool SettingsStore::saveDarkFont(const QString &family)
{
    return saveAnswerFont(QStringLiteral("ui_font_dark"), family, kThemeDark);
}

bool SettingsStore::saveLightFontSize(int size)
{
    return saveAnswerFontSize(QStringLiteral("ui_font_size_light"), size,
                               kThemeLight);
}

bool SettingsStore::saveDarkFontSize(int size)
{
    return saveAnswerFontSize(QStringLiteral("ui_font_size_dark"), size,
                               kThemeDark);
}

bool SettingsStore::saveSpeechEnabled(bool enabled)
{
    if (!writeKeyField(QStringLiteral("tts_enabled"),
                       enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return false;
    }
    setStatus(enabled ? i18n("The agent will read its answers aloud.")
                      : i18n("The agent will stay silent."),
              false);
    return true;
}

bool SettingsStore::saveSpeechReadMode(const QString &mode)
{
    const QString normalized = mode.compare(QStringLiteral("stream"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("stream")
        : QStringLiteral("final");
    if (!writeKeyField(QStringLiteral("tts_read_mode"), normalized)) {
        return false;
    }
    setStatus(normalized == QLatin1String("stream")
                  ? i18n("Answers will be spoken as they are generated.")
                  : i18n("Answers will be spoken once they are complete."),
              false);
    return true;
}

bool SettingsStore::saveSpeechVoice(const QString &voice)
{
    const QString trimmed = voice.trimmed();
    if (!writeKeyField(QStringLiteral("tts_voice"), trimmed)) {
        return false;
    }
    setStatus(i18n("Read-aloud voice set to %1.",
                   trimmed.isEmpty() ? i18n("the default") : trimmed),
              false);
    return true;
}

bool SettingsStore::saveSpeechProvider(const QString &provider)
{
    const QString name = provider.trimmed().toLower();
    if (name != kSpeechProviderMiniMax && name != kSpeechProviderOpenAI
        && name != kSpeechProviderGroq && name != kSpeechProviderOpenRouter
        && name != kSpeechProviderLocal) {
        setStatus(i18n("Unknown speech provider: %1", provider), true);
        return false;
    }
    if (!writeKeyField(QStringLiteral("tts_provider"), name)) {
        return false;
    }
    QString label;
    if (name == kSpeechProviderOpenAI) {
        label = i18n("OpenAI");
    } else if (name == kSpeechProviderGroq) {
        label = i18n("Groq");
    } else if (name == kSpeechProviderOpenRouter) {
        label = i18n("OpenRouter");
    } else if (name == kSpeechProviderLocal) {
        label = i18n("Local");
    } else {
        label = i18n("MiniMax");
    }
    setStatus(i18n("Read-aloud provider set to %1.", label), false);
    return true;
}

bool SettingsStore::saveSpeechPlaybackSpeed(qreal speed)
{
    // The same window the daemon's player stretches within (see
    // speech/player.py there): the ends are reached by chaining atempo
    // filters, the middle by one.
    const qreal clamped = qBound(0.25, speed, 4.0);
    if (!writeKeyField(QStringLiteral("tts_playback_speed"),
                       QString::number(clamped))) {
        return false;
    }
    setStatus(clamped == 1.0
                  ? i18n("The answers play at the speed they were spoken at.")
                  : i18n("The answers play at %1x the speed they were spoken at.",
                         QString::number(clamped)),
              false);
    return true;
}

bool SettingsStore::saveAnswerFont(const QString &field,
                                   const QString &family,
                                   const QString &mode)
{
    // An empty family means "the app's own default for the mode" and
    // is stored by removing the field — the same way an emptied API
    // key is stored by removing its field. The word "system" stands
    // for the system's own font and is stored as it is.
    if (!writeKeyField(field, family)) {
        return false;
    }

    setStatus(family.isEmpty()
                  ? i18n("The answers are set in the app's own face in %1 mode.",
                         mode == kThemeLight ? i18n("light") : i18n("dark"))
                  : family == kSystemFontChoice
                  ? i18n("The answers are set in the system's font in %1 mode.",
                         mode == kThemeLight ? i18n("light") : i18n("dark"))
                  : i18n("The answers are set in %1 in %2 mode.",
                         family, mode == kThemeLight ? i18n("light")
                                                     : i18n("dark")),
              false);
    return true;
}

bool SettingsStore::saveAnswerFontSize(const QString &field, int size,
                                        const QString &mode)
{
    // A size of zero means "the app's own size for the mode" and is
    // stored by removing the field, like an emptied API key.
    if (!writeKeyField(field, size > 0 ? QString::number(size) : QString())) {
        return false;
    }

    setStatus(size > 0
                  ? i18n("The answers are set in %1 pixels in %2 mode.",
                         size, mode == kThemeLight ? i18n("light")
                                                  : i18n("dark"))
                  : i18n("The answers keep their own size in %1 mode.",
                         mode == kThemeLight ? i18n("light") : i18n("dark")),
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

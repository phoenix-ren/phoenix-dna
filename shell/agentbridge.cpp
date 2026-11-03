// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "agentbridge.h"
#include "conversationmodel.h"
#include "speaker.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <pwd.h>
#include <unistd.h>

namespace
{
const QString kService = QStringLiteral("org.phoenixdna.agent");
const QString kPath = QStringLiteral("/org/phoenixdna/Agent");
const QString kInterface = QStringLiteral("org.phoenixdna.agent.Agent");

// The live agent daemon, as implemented by agent/phoenix_dna/service.py.
// The dictation contract above is still aspirational; mode selection is
// what already targets the running service.
const QString kDaemonService = QStringLiteral("dna.phoenix.Agent");
const QString kDaemonPath = QStringLiteral("/dna/phoenix/Agent");
const QString kDaemonInterface = QStringLiteral("dna.phoenix.Agent");
const QString kWatchedService = QStringLiteral("org.freedesktop.DBus");
const QString kWatchedPath = QStringLiteral("/org/freedesktop/DBus");
const QString kWatchedInterface = QStringLiteral("org.freedesktop.DBus");

// Per-tool-call output cap for the pane's activity log: enough to see why a
// command failed without the log dwarfing the answer it belongs to.
const int kToolLogOutputLimit = 2400;

// Parses a JSON array into a variant list; an unparsable payload yields an
// empty list (the daemon always answers with an array, so a malformed one
// means the daemon is not the one this bridge expects).
QVariantList parseJsonArray(const QString &payload)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return QVariantList();
    }
    return document.array().toVariantList();
}

// The same, for an object; an unparsable payload yields an empty map.
QVariantMap parseJsonObject(const QString &payload)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return QVariantMap();
    }
    return document.object().toVariantMap();
}
} // namespace

AgentBridge::AgentBridge(QObject *parent) : QObject(parent)
{
    QDBusConnection::sessionBus().connect(kWatchedService, kWatchedPath, kWatchedInterface,
                                          QStringLiteral("NameOwnerChanged"), this,
                                          SLOT(onServiceOwnerChanged(QString, QString, QString)));

    // Initial availability probe.
    QDBusMessage probe = QDBusMessage::createMethodCall(
        kService, kPath, QStringLiteral("org.freedesktop.DBus.Introspectable"),
        QStringLiteral("Introspect"));
    const QDBusMessage reply = QDBusConnection::sessionBus().call(probe, QDBus::Block, 1000);
    setAvailable(reply.type() == QDBusMessage::ReplyMessage);
    if (m_available) {
        subscribeToServiceSignals();
    }

    // The live daemon announces a finished run with RunFinished; that is
    // the moment a result is complete for the widget. The match rules bind
    // to the well-known name, so they can be installed before the daemon
    // is even running — delivery starts as soon as the name is acquired.
    // RunStarted/TextDelta/ThinkingDelta/ToolCall* stream the run into
    // resultText, the row's thinking and the activity log as it happens:
    // the pane opens at the run's first output (activityStarted) instead
    // of waiting for the result, and the finished result arms the pane's
    // dismiss countdown.
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("RunStarted"), this,
                                          SLOT(onRunStarted(QString, QString, QString)));
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("TextDelta"), this,
                                          SLOT(onTextDelta(QString, QString)));
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("ThinkingDelta"), this,
                                          SLOT(onThinkingDelta(QString, QString)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("RunFinished"), this,
        SLOT(onRunFinished(QString, QString, QString, uint, uint, uint)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("AuthFinished"), this,
        SLOT(onAuthFinished(QString, bool)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("ToolCallStarted"), this,
        SLOT(onToolCallStarted(QString, QString, QString, QString)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("ToolCallFinished"), this,
        SLOT(onToolCallFinished(QString, QString, QString, QString, QString)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("TranscriptCleaned"), this,
        SLOT(onTranscriptCleaned(QString, QString, QString, QString)));
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("SessionTitleChanged"), this,
                                          SLOT(onSessionTitleChanged(QString, QString, QString)));
    // ask_user: the run blocks on a question until it is answered. The
    // question renders as the pane's question card; its end (answered,
    // timeout, cancelled) takes the card away again.
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("QuestionAsked"), this,
                                          SLOT(onQuestionAsked(QString, QString, QString)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("QuestionAnswered"), this,
        SLOT(onQuestionAnswered(QString, QString, QString, QString)));

    // Text deltas arrive per token; the pane need not re-layout for each.
    m_deltaTimer.setInterval(100);
    m_deltaTimer.setSingleShot(true);
    connect(&m_deltaTimer, &QTimer::timeout, this, &AgentBridge::flushDeltas);

    // The read-aloud client: the daemon owns synthesis, this side only
    // requests it (SpeakText / StopSpeaking) and mirrors its state.
    m_speech = new Speaker(this);
    connect(m_speech, &Speaker::speakingChanged, this, &AgentBridge::agentSpeakingChanged);
    connect(m_speech, &Speaker::availableChanged, this, &AgentBridge::speechAvailableChanged);
}

AgentBridge::~AgentBridge()
{
    QDBusConnection::sessionBus().disconnect(
        kWatchedService, kWatchedPath, kWatchedInterface, QStringLiteral("NameOwnerChanged"), this,
        SLOT(onServiceOwnerChanged(QString, QString, QString)));
}

void AgentBridge::startDictation()
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface,
                                                          QStringLiteral("startDictation"));
    QDBusConnection::sessionBus().asyncCall(message);
}

void AgentBridge::stopDictation()
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface,
                                                          QStringLiteral("stopDictation"));
    QDBusConnection::sessionBus().asyncCall(message);
}

void AgentBridge::setWorkingDirectory(const QString &path)
{
    if (m_workingDirectory == path) {
        return;
    }
    m_workingDirectory = path;

    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kInterface,
                                                          QStringLiteral("setWorkingDirectory"));
    message << path;
    QDBusConnection::sessionBus().asyncCall(message);
}

void AgentBridge::setElevated(bool elevated)
{
    if (m_elevated == elevated) {
        return;
    }
    m_elevated = elevated;

    QDBusMessage message =
        QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("setElevated"));
    message << elevated;
    QDBusConnection::sessionBus().asyncCall(message);
}

void AgentBridge::setMode(const QString &mode)
{
    m_modeName = mode;
    // Sessions are filed per mode by the daemon: a follow-on after a mode
    // switch must start a new history instead of landing in (or aliasing)
    // the previous mode's conversation. The previous mode's session title
    // goes with it — the next title to arrive here belongs to the new
    // session.
    m_sessionId.clear();
    if (!m_sessionTitle.isEmpty()) {
        m_sessionTitle.clear();
        Q_EMIT sessionTitleChanged();
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("SetMode"));
    message << mode;
    QDBusConnection::sessionBus().asyncCall(message);
}

void AgentBridge::sendPrompt(const QString &prompt,
                             bool newSession,
                             const QVariantList &attachments)
{
    // SubmitPrompt(prompt, mode, session_id): the session id continues the
    // conversation of the widget's latest run — unless the prompt starts a
    // new session (record button on the desktop pane): the id is dropped,
    // so the daemon opens a fresh conversation. The text, mode and intent
    // are remembered so RunStarted can pair them with the run they begin
    // (queued prompts begin in submission order) — the mode names the
    // exchange's sender in the pane's history, the intent decides whether
    // that history restarts.
    if (newSession) {
        m_sessionId.clear();
    }
    m_pendingPrompts.append({prompt, m_modeName, newSession});

    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("SubmitPrompt"));
    message << prompt << m_modeName << m_sessionId;

    if (!attachments.isEmpty()) {
        // SubmitPromptWithAttachments(prompt, mode, session_id, attachments):
        // the same call with the attached files as a JSON payload. Only
        // used when there are attachments, so a daemon that predates the
        // method still serves plain prompts.
        const QJsonArray files = QJsonArray::fromVariantList(attachments);
        const QByteArray payload = QJsonDocument(files).toJson(QJsonDocument::Compact);
        message = QDBusMessage::createMethodCall(kDaemonService, kDaemonPath, kDaemonInterface,
                                                 QStringLiteral("SubmitPromptWithAttachments"));
        message << prompt << m_modeName << m_sessionId << QString::fromUtf8(payload);
    }

    QDBusConnection::sessionBus().asyncCall(message);
}

void AgentBridge::requestTranscriptCleanup(const QString &text)
{
    // SubmitTranscriptCleanup(text) -> cleanup id; the daemon answers
    // asynchronously via TranscriptCleaned. Only the newest request is
    // tracked: an older clip's cleanup is dropped when its id no longer
    // matches (see onTranscriptCleaned), so a stale result can never
    // overwrite the text of a newer dictation.
    m_pendingCleanupId.clear();
    m_pendingCleanupSource = text;

    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("SubmitTranscriptCleanup"));
    message << text;
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *watcher) {
                const QString id = watcher->reply().arguments().value(0).toString();
                watcher->deleteLater();
                if (id.isEmpty()) {
                    // Cleanup not configured on the daemon: the raw transcript
                    // stays, exactly as before this feature existed.
                    m_pendingCleanupSource.clear();
                    return;
                }
                m_pendingCleanupId = id;
            });
}

void AgentBridge::setAvailable(bool available)
{
    if (m_available == available) {
        return;
    }
    m_available = available;
    Q_EMIT availabilityChanged();
}

void AgentBridge::requestSessionSearch(const QString &user, const QString &query)
{
    // SearchSessions(user, query) -> JSON array of session summaries. The
    // daemon matches the query fuzzily against the sessions' names and
    // answers with the ones that still match.
    const quint64 request = ++m_searchRequest;

    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("SearchSessions"));
    message << user << query;

    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, request](QDBusPendingCallWatcher *watcher) {
                const QDBusReply<QString> reply = *watcher;
                watcher->deleteLater();
                if (!reply.isValid()) {
                    qWarning() << "session search failed:" << reply.error().message();
                    return;
                }
                if (request != m_searchRequest) {
                    // A newer search was started meanwhile: this answer
                    // belongs to a query the user has already replaced.
                    return;
                }
                m_sessionSummaries = parseJsonArray(reply.value());
                Q_EMIT sessionSummariesChanged();
            });
}

void AgentBridge::requestSessionTranscript(const QString &user, const QString &sessionId)
{
    // GetSessionTranscript(user, session_id) -> JSON array of messages.
    const quint64 request = ++m_transcriptRequest;

    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("GetSessionTranscript"));
    message << user << sessionId;

    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, request](QDBusPendingCallWatcher *watcher) {
                const QDBusReply<QString> reply = *watcher;
                watcher->deleteLater();
                if (!reply.isValid()) {
                    qWarning() << "session transcript failed:" << reply.error().message();
                    return;
                }
                if (request != m_transcriptRequest) {
                    // Another session was selected meanwhile.
                    return;
                }
                m_sessionMessages = parseJsonArray(reply.value());
                Q_EMIT sessionMessagesChanged();
            });
}

void AgentBridge::openStoredSession(const QString &user,
                                    const QString &sessionId,
                                    const QString &mode,
                                    const QString &title)
{
    // The widget follows this session from now on: a follow-on typed in
    // the results pane continues that very conversation.
    m_sessionId = sessionId;
    if (m_sessionTitle != title) {
        m_sessionTitle = title;
        Q_EMIT sessionTitleChanged();
    }

    // The transcript of the session, fetched like the search overlay's
    // own copy - but into the conversation model this time, so the
    // results pane can show it.
    const quint64 request = ++m_storedRequest;

    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("GetSessionTranscript"));
    message << user << sessionId;

    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, request, mode](QDBusPendingCallWatcher *watcher) {
                const QDBusReply<QString> reply = *watcher;
                watcher->deleteLater();
                if (!reply.isValid()) {
                    qWarning() << "stored session failed:" << reply.error().message();
                    return;
                }
                if (request != m_storedRequest) {
                    // Another session was opened meanwhile.
                    return;
                }
                loadStoredSession(parseJsonArray(reply.value()), mode);
                Q_EMIT storedSessionOpened();
            });
}

void AgentBridge::loadStoredSession(const QVariantList &messages, const QString &mode)
{
    // The history of the pane: the session's exchanges, oldest first.
    m_conversation.reset();

    const QString sender = senderNameForMode(mode);
    QString prompt;
    QStringList answers;
    bool open = false;
    for (const QVariant &entry : messages) {
        const QVariantMap message = entry.toMap();
        const QString role = message.value(QStringLiteral("role")).toString();
        const QString content = message.value(QStringLiteral("content")).toString();
        if (role == QLatin1String("user")) {
            if (open) {
                storeExchange(prompt, sender, answers);
            }
            prompt = content;
            answers.clear();
            open = true;
        } else if (role == QLatin1String("assistant") && !content.isEmpty()) {
            // One assistant message per turn of the run: the last one is
            // the answer, the ones before it are the notes the agent
            // wrote while it worked.
            answers << content;
        }
    }
    if (open) {
        storeExchange(prompt, sender, answers);
    }
}

void AgentBridge::storeExchange(const QString &prompt,
                                const QString &sender,
                                const QStringList &messages)
{
    m_conversation.beginExchange(prompt, sender);
    if (!messages.isEmpty()) {
        // The last message is the answer; the ones before it are the
        // agent's working notes.
        m_conversation.setThinking(
            messages.mid(0, messages.size() - 1).join(QStringLiteral("\n\n")));
        m_conversation.setResult(messages.last());
    }
    m_conversation.finishExchange();
}

bool AgentBridge::answerQuestion(const QVariantList &selected, const QString &text)
{
    // AnswerQuestion(ask_id, answer_json): selected entries are 0-based
    // option indices (the daemon also accepts exact labels), text is the
    // user's own words. Either part may carry the answer alone. A false
    // reply (question no longer pending, payload rejected) is only
    // logged: the question stays pending, and the card lets the user
    // retry.
    if (m_pendingQuestion.isEmpty()) {
        return false;
    }

    QJsonObject answer;
    const QJsonArray chosen = QJsonArray::fromVariantList(selected);
    if (!chosen.isEmpty()) {
        answer.insert(QStringLiteral("selected"), chosen);
    }
    const QString own = text.trimmed();
    if (!own.isEmpty()) {
        answer.insert(QStringLiteral("text"), own);
    }
    if (answer.isEmpty()) {
        return false;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("AnswerQuestion"));
    message << m_pendingQuestion.value(QStringLiteral("askId")).toString()
            << QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact));

    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *watcher) {
                const QDBusReply<bool> reply = *watcher;
                watcher->deleteLater();
                if (!reply.isValid() || !reply.value()) {
                    // The question stays pending: the run is still blocked
                    // on it, and the card remains to retry.
                    qWarning() << "answer was not accepted:" << reply.error().message();
                }
            });
    return true;
}

void AgentBridge::onQuestionAsked(const QString &runId,
                                  const QString &askId,
                                  const QString &questionJson)
{
    Q_UNUSED(runId)

    // A question is activity like any other: the pane opens even when the
    // model asked before producing any text.
    beginStreaming();

    // question_json: {question, options: [{label, description}], header?,
    // multiple?, goal?, step?, est_steps?}. The card reads exactly these
    // keys (camelCase here), so keep the mapping in one place.
    const QVariantMap question = parseJsonObject(questionJson);

    QVariantList options;
    const QVariantList rawOptions = question.value(QStringLiteral("options")).toList();
    for (const QVariant &entry : rawOptions) {
        const QVariantMap option = entry.toMap();
        QVariantMap mapped;
        mapped.insert(QStringLiteral("label"),
                      option.value(QStringLiteral("label")).toString());
        const QString description = option.value(QStringLiteral("description")).toString().trimmed();
        if (!description.isEmpty()) {
            mapped.insert(QStringLiteral("description"), description);
        }
        options.append(mapped);
    }

    QVariantMap payload;
    payload.insert(QStringLiteral("askId"), askId);
    payload.insert(QStringLiteral("question"),
                   question.value(QStringLiteral("question")).toString());
    payload.insert(QStringLiteral("options"), options);
    // The harness marker: a daemon-authored confirmation (destructive
    // root command), not a model-authored question. The card styles it
    // as a system confirmation, and it never joins a wizard.
    if (question.value(QStringLiteral("harness")).toBool()) {
        payload.insert(QStringLiteral("harness"), true);
    }
    const QString header = question.value(QStringLiteral("header")).toString().trimmed();
    if (!header.isEmpty()) {
        payload.insert(QStringLiteral("header"), header);
    }
    if (question.value(QStringLiteral("multiple")).toBool()) {
        payload.insert(QStringLiteral("multiple"), true);
    }
    const QString goal = question.value(QStringLiteral("goal")).toString().trimmed();
    if (!goal.isEmpty()) {
        payload.insert(QStringLiteral("goal"), goal);
    }
    const QVariant step = question.value(QStringLiteral("step"));
    if (!step.isNull() && step.toInt() > 0) {
        payload.insert(QStringLiteral("step"), step.toInt());
    }
    const QVariant estSteps = question.value(QStringLiteral("est_steps"));
    if (!estSteps.isNull() && estSteps.toInt() > 0) {
        payload.insert(QStringLiteral("estSteps"), estSteps.toInt());
    }
    setPendingQuestion(payload);

    // A question with step metadata is part of a series: the wizard
    // begins (or advances). A plain question mid-series is shown in the
    // wizard pane as-is, without touching the progress.
    const int wizardStep = payload.value(QStringLiteral("step")).toInt();
    if (wizardStep > 0) {
        QVariantMap wizard = m_wizardState;
        const bool wasActive = wizard.value(QStringLiteral("active")).toBool();
        wizard.insert(QStringLiteral("active"), true);
        wizard.insert(QStringLiteral("waiting"), false);
        wizard.insert(QStringLiteral("goal"), payload.value(QStringLiteral("goal")));
        wizard.insert(QStringLiteral("step"), wizardStep);
        wizard.insert(
            QStringLiteral("estSteps"),
            payload.contains(QStringLiteral("estSteps"))
                ? payload.value(QStringLiteral("estSteps"))
                : QVariant(step));
        if (!wasActive) {
            // A fresh wizard collects from scratch; a continuing series
            // keeps what it gathered.
            wizard.insert(QStringLiteral("answers"), QVariantList());
        }
        setWizardState(wizard);
    } else if (m_wizardState.value(QStringLiteral("active")).toBool()) {
        QVariantMap wizard = m_wizardState;
        wizard.insert(QStringLiteral("waiting"), false);
        setWizardState(wizard);
    }
}

void AgentBridge::onQuestionAnswered(const QString &runId,
                                     const QString &askId,
                                     const QString &status,
                                     const QString &answerJson)
{
    Q_UNUSED(runId)

    // Only the question this card is showing ends here: a late answer for
    // an already-replaced question must not clear its successor.
    if (m_pendingQuestion.value(QStringLiteral("askId")).toString() != askId) {
        return;
    }

    // A wizard records what it collected — the answered-so-far list is
    // the running summary of the series. An answered question keeps the
    // wizard up for the next one (waiting); a timeout or a cancellation
    // ends it: nothing more will be answered here.
    QVariantMap wizard = m_wizardState;
    if (wizard.value(QStringLiteral("active")).toBool()) {
        QVariantList answers = wizard.value(QStringLiteral("answers")).toList();
        QVariantMap entry;
        entry.insert(QStringLiteral("question"),
                     m_pendingQuestion.value(QStringLiteral("question")));
        entry.insert(QStringLiteral("answer"), parseJsonObject(answerJson));
        entry.insert(QStringLiteral("status"), status);
        answers.append(entry);
        wizard.insert(QStringLiteral("answers"), answers);
        if (status == QLatin1String("answered")) {
            wizard.insert(QStringLiteral("waiting"), true);
        } else {
            wizard.insert(QStringLiteral("active"), false);
            wizard.insert(QStringLiteral("waiting"), false);
        }
        setWizardState(wizard);
    }
    setPendingQuestion(QVariantMap());
}

void AgentBridge::setPendingQuestion(const QVariantMap &question)
{
    if (m_pendingQuestion == question) {
        return;
    }
    m_pendingQuestion = question;
    Q_EMIT pendingQuestionChanged();
}

void AgentBridge::setWizardState(const QVariantMap &state)
{
    if (m_wizardState == state) {
        return;
    }
    m_wizardState = state;
    Q_EMIT wizardStateChanged();
}

void AgentBridge::endWizard()
{
    if (!m_wizardState.value(QStringLiteral("active")).toBool()) {
        return;
    }
    QVariantMap wizard = m_wizardState;
    wizard.insert(QStringLiteral("active"), false);
    wizard.insert(QStringLiteral("waiting"), false);
    setWizardState(wizard);
}

void AgentBridge::onServiceOwnerChanged(const QString &service,
                                        const QString &oldOwner,
                                        const QString &newOwner)
{
    Q_UNUSED(oldOwner)
    if (service != kService) {
        return;
    }

    if (newOwner.isEmpty()) {
        // The service went away.
        unsubscribeFromServiceSignals();
        setAvailable(false);
        return;
    }

    // The service (re)appeared: re-probe and (re)subscribe.
    QDBusMessage probe = QDBusMessage::createMethodCall(
        kService, kPath, QStringLiteral("org.freedesktop.DBus.Introspectable"),
        QStringLiteral("Introspect"));
    const QDBusMessage reply = QDBusConnection::sessionBus().call(probe, QDBus::Block, 1000);
    setAvailable(reply.type() == QDBusMessage::ReplyMessage);
    if (m_available) {
        subscribeToServiceSignals();
    }
}

void AgentBridge::subscribeToServiceSignals()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kInterface, QStringLiteral("partialTranscript"), this,
                SLOT(onPartialTranscript(QString)));
    bus.connect(kService, kPath, kInterface, QStringLiteral("finalTranscript"), this,
                SLOT(onFinalTranscript(QString)));
    bus.connect(kService, kPath, kInterface, QStringLiteral("agentMessage"), this,
                SLOT(onAgentMessage(QString)));
    bus.connect(kService, kPath, kInterface, QStringLiteral("actionProposed"), this,
                SLOT(onActionProposed(QString)));
    bus.connect(kService, kPath, kInterface, QStringLiteral("actionResult"), this,
                SLOT(onActionResult(QString, bool)));
}

void AgentBridge::unsubscribeFromServiceSignals()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.disconnect(kService, kPath, kInterface, QStringLiteral("partialTranscript"), this,
                   SLOT(onPartialTranscript(QString)));
    bus.disconnect(kService, kPath, kInterface, QStringLiteral("finalTranscript"), this,
                   SLOT(onFinalTranscript(QString)));
    bus.disconnect(kService, kPath, kInterface, QStringLiteral("agentMessage"), this,
                   SLOT(onAgentMessage(QString)));
}

void AgentBridge::onPartialTranscript(const QString &text)
{
    setPartialTranscript(text);
}

void AgentBridge::onFinalTranscript(const QString &text)
{
    setFinalTranscript(text);
}

void AgentBridge::onAgentMessage(const QString &text)
{
    setAgentMessage(text);
}

void AgentBridge::onActionProposed(const QString &text)
{
    if (m_proposedAction == text) {
        return;
    }
    m_proposedAction = text;
    Q_EMIT proposedActionChanged();
}

void AgentBridge::onActionResult(const QString &text, bool ok)
{
    Q_UNUSED(text)
    Q_UNUSED(ok)
    // The result arrives once the action completed; the pane clears the
    // pending entry when it sees the matching result.
}

void AgentBridge::onRunStarted(const QString &runId, const QString &mode, const QString &sessionId)
{
    Q_UNUSED(runId)
    Q_UNUSED(mode)

    // A fresh run opens a row in the pane's history. A run submitted as a
    // *new session* (record button on the desktop pane) first drops the
    // history: earlier, unrelated conversations must not bleed into the
    // new one. Follow-on runs (record button or input field in the pane)
    // append below the existing rows instead. The daemon runs one prompt
    // at a time, so no run-id matching is needed here. A run that consumed
    // one of our queued prompts is one we started: its prompt pairs with
    // the row, and its session id is the conversation every following
    // sendPrompt() continues. Runs begun elsewhere (e.g. the REPL) do not
    // adopt their session — the widget keeps following its own thread.
    const bool ours = !m_pendingPrompts.isEmpty();
    m_streaming = false;
    m_deltaTimer.stop();
    m_pendingDelta.clear();
    m_pendingThinking.clear();
    // A question never survives its run: if the previous run ended without
    // QuestionAnswered (daemon restart mid-question), its card must go.
    setPendingQuestion(QVariantMap());
    // Neither does a wizard: a run boundary ends whatever it was asking.
    endWizard();
    if (ours) {
        const QueuedPrompt queued = m_pendingPrompts.takeFirst();
        if (queued.newSession) {
            m_conversation.reset();
            // The new session's title is unknown until its first run
            // completes; drop the old session's so the header never
            // shows a title that names a different conversation.
            if (!m_sessionTitle.isEmpty()) {
                m_sessionTitle.clear();
                Q_EMIT sessionTitleChanged();
            }
        }
        m_conversation.beginExchange(queued.text, senderNameForMode(queued.mode));
    } else {
        m_conversation.beginExchange(QString(), QString());
    }
    if (ours && m_sessionId != sessionId) {
        m_sessionId = sessionId;
    }
}

void AgentBridge::onTextDelta(const QString &runId, const QString &text)
{
    Q_UNUSED(runId)

    beginStreaming();
    m_pendingDelta += text;
    if (!m_deltaTimer.isActive()) {
        m_deltaTimer.start();
    }
}

void AgentBridge::onThinkingDelta(const QString &runId, const QString &text)
{
    Q_UNUSED(runId)

    // The notes are activity too: the pane opens even while the model is
    // only thinking.
    beginStreaming();
    m_pendingThinking += text;
    if (!m_deltaTimer.isActive()) {
        m_deltaTimer.start();
    }
}

void AgentBridge::flushDeltas()
{
    if (!m_pendingThinking.isEmpty()) {
        m_conversation.appendThinking(m_pendingThinking);
        m_pendingThinking.clear();
    }
    if (m_pendingDelta.isEmpty()) {
        return;
    }
    m_conversation.appendResult(m_pendingDelta);
    m_pendingDelta.clear();
}

void AgentBridge::beginStreaming()
{
    if (m_streaming) {
        return;
    }
    m_streaming = true;
    Q_EMIT activityStarted();
}

void AgentBridge::onToolCallStarted(const QString &runId,
                                    const QString &callId,
                                    const QString &tool,
                                    const QString &argsJson)
{
    Q_UNUSED(runId)
    Q_UNUSED(callId)

    // A tool call is activity too: the pane opens even if the model calls
    // tools before producing any text.
    beginStreaming();

    // The text the model produced before it reached for a tool is its
    // working notes, not the answer: move it out of the answer, so the
    // answer keeps the place it deserves in the pane.
    flushDeltas();
    m_conversation.demoteResultToThinking();
    m_conversation.noteToolCall();

    // The agent went to work: if a wizard was collecting answers, it is
    // over — the conversation returns. (ask_user's own call is the
    // wizard continuing, not ending.)
    if (tool != QLatin1String("ask_user")) {
        endWizard();
    }

    // Head of a log entry: the tool name and its arguments. Written at
    // "started" rather than "finished" so a command that hangs or dies with
    // the run still shows up in the log. Bash gets its command as a bash
    // block; anything else is shown as the raw JSON arguments.
    QString language = QStringLiteral("json");
    QString body = argsJson;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(argsJson.toUtf8(), &parseError);
    if (parseError.error == QJsonParseError::NoError && tool == QLatin1String("bash")) {
        const QJsonObject args = doc.object();
        if (args.contains(QLatin1String("command"))) {
            language = QStringLiteral("bash");
            body = args.value(QLatin1String("command")).toString();
        }
    }
    m_conversation.appendToolLog(QStringLiteral("\n#### %1 %2\n\n%3\n\n")
                                     .arg(QStringLiteral("⚒"), tool, fencedBlock(body, language)));
}

void AgentBridge::onToolCallFinished(const QString &runId,
                                     const QString &callId,
                                     const QString &tool,
                                     const QString &status,
                                     const QString &output)
{
    Q_UNUSED(runId)
    Q_UNUSED(callId)
    Q_UNUSED(tool)

    // Tail of the entry: outcome plus the command's output, fenced so the
    // pane's Markdown renderer shows it verbatim.
    const QString mark =
        status == QLatin1String("success") ? QStringLiteral("✓") : QStringLiteral("✗ error");
    m_conversation.appendToolLog(
        QStringLiteral("%1\n\n%2\n")
            .arg(mark, fencedBlock(elideOutput(output, kToolLogOutputLimit), QString())));
}

QString AgentBridge::senderNameForMode(const QString &mode)
{
    // The pane's bubbles point at whoever (or what) issued the prompt:
    // root prompts come from "root", user prompts carry the actual login
    // name.
    if (mode == QLatin1String("root")) {
        return QStringLiteral("root");
    }
    return localUserName();
}

QString AgentBridge::localUserName()
{
    // The login name: the environment first (what the session set), the
    // passwd database as the fallback.
    const QByteArray fromEnv = qgetenv("USER");
    if (!fromEnv.isEmpty()) {
        return QString::fromLocal8Bit(fromEnv);
    }
    if (const passwd *pw = getpwuid(geteuid())) {
        return QString::fromLocal8Bit(pw->pw_name);
    }
    return QString();
}

QString AgentBridge::fencedBlock(const QString &text, const QString &language)
{
    // A fence longer than anything inside the text keeps the block intact;
    // the language tag is cosmetic (Qt renders any fenced block monospace).
    QString fence = QStringLiteral("```");
    while (text.contains(fence)) {
        fence += QLatin1Char('`');
    }
    QString block = fence;
    if (!language.isEmpty()) {
        block += language;
    }
    return block + QLatin1Char('\n') + text + QLatin1Char('\n') + fence;
}

QString AgentBridge::elideOutput(const QString &text, int limit)
{
    if (text.size() <= limit) {
        return text;
    }
    // Keep both ends: command banners at the top, the actual error at the
    // bottom.
    const int head = int(limit * 0.75);
    const int tail = limit - head;
    return QStringLiteral("%1\n… [%2 of %3 characters shown] …\n%4")
        .arg(text.left(head))
        .arg(limit)
        .arg(text.size())
        .arg(text.right(tail));
}

void AgentBridge::onRunFinished(const QString &runId,
                                const QString &status,
                                const QString &finalText,
                                uint turns,
                                uint tokensIn,
                                uint tokensOut)
{
    Q_UNUSED(runId)
    Q_UNUSED(status)
    Q_UNUSED(turns)
    Q_UNUSED(tokensIn)
    Q_UNUSED(tokensOut)

    // The finished text is authoritative; drop any unflushed deltas and
    // close the current history row (its streamed partial, if any, stays).
    m_deltaTimer.stop();
    m_pendingDelta.clear();
    m_pendingThinking.clear();
    // ...and any question the run was blocked on is over now, answered or
    // not (QuestionAnswered usually got here first).
    setPendingQuestion(QVariantMap());
    endWizard();
    if (!finalText.trimmed().isEmpty()) {
        m_conversation.setResult(finalText);
    }
    m_conversation.finishExchange();

    // A run with no text (e.g. cancelled before the model answered) is not
    // a result; the pane only re-pins its scroll when there is something
    // new to read.
    if (!finalText.trimmed().isEmpty()) {
        Q_EMIT resultReady();
    }
}

void AgentBridge::onAuthFinished(const QString &runId, bool success)
{
    // Pure pass-through: the bridge is a dumb pipe, the mode policy that
    // reacts to a cancelled password dialog lives in the controller.
    // An empty run id marks the pre-authentication dialog the daemon
    // shows when root mode is selected; a non-empty one belongs to a
    // running prompt that needed the password again.
    Q_EMIT authFinished(runId, success);
}

void AgentBridge::onTranscriptCleaned(const QString &cleanupId,
                                      const QString &source,
                                      const QString &cleaned,
                                      const QString &status)
{
    // Only the newest dictation's cleanup is adopted: an older clip's
    // (late) result must never clobber the text of a newer transcription.
    if (cleanupId.isEmpty() || cleanupId != m_pendingCleanupId) {
        return;
    }
    m_pendingCleanupId.clear();
    m_pendingCleanupSource.clear();
    if (status != QLatin1String("completed") || cleaned.trimmed().isEmpty()) {
        // The cleanup failed; the raw transcript simply stays on screen.
        return;
    }
    Q_EMIT transcriptCleaned(source, cleaned);
}

void AgentBridge::onSessionTitleChanged(const QString &sessionId,
                                        const QString &mode,
                                        const QString &title)
{
    Q_UNUSED(mode)

    // Only the session this widget is following matters: runs started
    // elsewhere (the REPL) name their own sessions, which must not
    // relabel the pane. The title was also persisted daemon-side, so the
    // next session search can list sessions by it.
    if (sessionId.isEmpty() || sessionId != m_sessionId) {
        return;
    }
    if (m_sessionTitle == title) {
        return;
    }
    m_sessionTitle = title;
    Q_EMIT sessionTitleChanged();
}

void AgentBridge::setPartialTranscript(const QString &text)
{
    if (m_partialTranscript == text) {
        return;
    }
    m_partialTranscript = text;
    Q_EMIT partialTranscriptChanged();
}

void AgentBridge::setFinalTranscript(const QString &text)
{
    if (m_finalTranscript == text) {
        return;
    }
    m_finalTranscript = text;
    Q_EMIT finalTranscriptChanged();
}

void AgentBridge::setAgentMessage(const QString &text)
{
    if (m_agentMessage == text) {
        return;
    }
    m_agentMessage = text;
    Q_EMIT agentMessageChanged();
}

bool AgentBridge::isAgentSpeaking() const
{
    return m_speech != nullptr && m_speech->isSpeaking();
}

bool AgentBridge::isSpeechAvailable() const
{
    return m_speech != nullptr && m_speech->isAvailable();
}

void AgentBridge::speakText(const QString &text)
{
    if (m_speech != nullptr) {
        m_speech->speak(text);
    }
}

void AgentBridge::stopSpeaking()
{
    if (m_speech != nullptr) {
        m_speech->stop();
    }
}

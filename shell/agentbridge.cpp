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
#include <QThreadPool>

#include <pwd.h>
#include <unistd.h>

namespace
{
const QString kService = QStringLiteral("org.phoenixdna.agent");
const QString kPath = QStringLiteral("/org/phoenixdna/Agent");
const QString kInterface = QStringLiteral("org.phoenixdna.agent.Agent");

// The live agent daemon, as implemented by agent/phoenix_dna/service.py.
// The dictation contract above is still aspirational; the prompt
// submission is what already targets the running service.
const QString kDaemonService = QStringLiteral("dna.phoenix.Agent");
const QString kDaemonPath = QStringLiteral("/dna/phoenix/Agent");
const QString kDaemonInterface = QStringLiteral("dna.phoenix.Agent");
const QString kWatchedService = QStringLiteral("org.freedesktop.DBus");
const QString kWatchedPath = QStringLiteral("/org/freedesktop/DBus");
const QString kWatchedInterface = QStringLiteral("org.freedesktop.DBus");

// Per-tool-call output cap for the pane's activity log: enough to see why a
// command failed without the log dwarfing the answer it belongs to.
const int kToolLogOutputLimit = 2400;

// How long AnswerQuestion may take, in milliseconds. The daemon answers in
// milliseconds (it only hands the payload to the blocked run); a wedged or
// dying daemon must not hold the pane in the submitted limbo for the D-Bus
// default of 25 seconds — the reply that restores the form (and with it the
// wizard) arrives that much sooner.
const int kAnswerTimeoutMs = 5000;

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
    // The transcript of the session the search has picked: the model
    // hands its rows to the view a chunk at a time.
    m_transcript = new SessionTranscriptModel(this);

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
                                          SLOT(onRunStarted(QString, QString)));
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("TextDelta"), this,
                                          SLOT(onTextDelta(QString, QString)));
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("ThinkingDelta"), this,
                                          SLOT(onThinkingDelta(QString, QString)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("RunFinished"), this,
        SLOT(onRunFinished(QString, QString, QString, uint, uint, uint)));
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("AuthFinished"), this,
                                          SLOT(onAuthFinished(QString, bool)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("ToolCallStarted"), this,
        SLOT(onToolCallStarted(QString, QString, QString, QString)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("ToolCallFinished"), this,
        SLOT(onToolCallFinished(QString, QString, QString, QString, QString)));
    // The bash tool's command process: its pid (the session leader of
    // the command's process group) is what the CPU meter follows through
    // /proc while the call runs, and the process's end lets it go.
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("ToolProcessStarted"), this,
        SLOT(onToolProcessStarted(QString, QString, QString, uint)));
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("ToolProcessFinished"), this,
                                          SLOT(onToolProcessFinished(QString, QString, uint)));
    // The traffic of the conversation with the model: the bytes of each
    // turn, which the wire meter turns into rates.
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("WireBytes"), this,
                                          SLOT(onWireBytes(QString, uint, uint, uint)));
    // The bytes a transfer tool call moves: cumulative counts with the
    // total when the far end named one, which the transfer meter turns
    // into the pace and the share of the thing that is here.
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("TransferProgress"), this,
        SLOT(onTransferProgress(QString, QString, QString, qulonglong, qulonglong)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("TranscriptCleaned"), this,
        SLOT(onTranscriptCleaned(QString, QString, QString, QString)));
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("SessionTitleChanged"), this,
                                          SLOT(onSessionTitleChanged(QString, QString)));
    // ask_user: the run blocks on a question until it is answered. The
    // question renders as the pane's question card; its end (answered,
    // timeout, cancelled) takes the card away again.
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("QuestionAsked"), this,
                                          SLOT(onQuestionAsked(QString, QString, QString)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("QuestionAnswered"), this,
        SLOT(onQuestionAnswered(QString, QString, QString, QString)));
    // The daemon's own state: whether a run is in flight and how many
    // prompts wait behind it. A prompt submitted while another run is
    // busy waits in the daemon's queue, and the daemon only announces it
    // (RunStarted) when it reaches it — so the queue length is what tells
    // the widget that a prompt is on its way but has not started yet.
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("StateChanged"), this,
        SLOT(onStateChanged(QString, QString, uint)));

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

void AgentBridge::sendPrompt(const QString &prompt,
                             bool newSession,
                             const QVariantList &attachments)
{
    // SubmitPrompt(prompt, session_id): the session id continues the
    // conversation of the widget's latest run — unless the prompt starts
    // a new session (record button on the desktop pane): the id is
    // dropped, so the daemon opens a fresh conversation. The text and the
    // intent are remembered so RunStarted can pair them with the run they
    // begin (queued prompts begin in submission order) — the intent
    // decides whether the pane's history restarts.
    if (newSession) {
        m_sessionId.clear();
    }
    m_pendingPrompts.append({prompt, newSession});

    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("SubmitPrompt"));
    message << prompt << m_sessionId;

    if (!attachments.isEmpty()) {
        // SubmitPromptWithAttachments(prompt, session_id, attachments):
        // the same call with the attached files as a JSON payload. Only
        // used when there are attachments, so a daemon that predates the
        // method still serves plain prompts.
        const QJsonArray files = QJsonArray::fromVariantList(attachments);
        const QByteArray payload = QJsonDocument(files).toJson(QJsonDocument::Compact);
        message = QDBusMessage::createMethodCall(kDaemonService, kDaemonPath, kDaemonInterface,
                                                 QStringLiteral("SubmitPromptWithAttachments"));
        message << prompt << m_sessionId << QString::fromUtf8(payload);
    }

    QDBusConnection::sessionBus().asyncCall(message);
}

void AgentBridge::cancelRun()
{
    // Cancel(run_id): the daemon interrupts the live run and reports the
    // end with a RunFinished of its own (status "cancelled") — the same
    // signal any finished run sends, so the pane needs no special case.
    // Without a run, or without its id, there is nothing to name.
    if (!m_runActive || m_runId.isEmpty()) {
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("Cancel"));
    message << m_runId;
    QDBusConnection::sessionBus().asyncCall(message);
}

void AgentBridge::setRunActive(bool active)
{
    if (m_runActive == active) {
        return;
    }
    m_runActive = active;
    Q_EMIT runActiveChanged();
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

    // The previous session's words leave the pane at once: they would
    // only misattribute themselves to the session that was just
    // picked, and the bar at the top of the window says what is on
    // its way.
    m_transcript->clear();
    m_transcriptSessionId = sessionId;

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
                // The parse happens off the GUI thread: a long
                // transcript is megabytes of JSON, and the window must
                // not wait for it — it shows the progress instead.
                const QByteArray payload = reply.value().toUtf8();
                QThreadPool::globalInstance()->start([this, request, payload]() {
                    const QVariantList messages = parseJsonArray(QString::fromUtf8(payload));
                    QMetaObject::invokeMethod(
                        this,
                        [this, request, messages]() {
                            if (request != m_transcriptRequest) {
                                // Another session was selected while the
                                // transcript was being parsed.
                                return;
                            }
                            m_transcript->load(messages);
                        },
                        Qt::QueuedConnection);
                });
            });
}

void AgentBridge::requestSessionDelete(const QString &user, const QString &sessionId)
{
    if (sessionId.isEmpty()) {
        return;
    }

    // DeleteSession(user, session_id) -> bool: whether there was a
    // session to delete.
    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("DeleteSession"));
    message << user << sessionId;

    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, sessionId](QDBusPendingCallWatcher *watcher) {
                const QDBusReply<bool> reply = *watcher;
                watcher->deleteLater();
                if (!reply.isValid()) {
                    qWarning() << "session delete failed:" << reply.error().message();
                    return;
                }

                // The session leaves the list: the daemon has deleted
                // it, or it never had it — a stale entry in the list
                // is a session that is not in the history either way.
                for (int i = m_sessionSummaries.size() - 1; i >= 0; --i) {
                    const QVariantMap summary = m_sessionSummaries.at(i).toMap();
                    if (summary.value(QStringLiteral("id")).toString() == sessionId) {
                        m_sessionSummaries.removeAt(i);
                    }
                }

                // The transcript of a session that is gone goes with
                // it: what it holds is no longer in the history, and
                // showing it would be showing a conversation the user
                // has just thrown away. This happens before the list
                // says so, so that the view picks another session
                // against an empty transcript.
                if (m_transcriptSessionId == sessionId) {
                    m_transcript->clear();
                    m_transcriptSessionId.clear();
                }

                Q_EMIT sessionSummariesChanged();
            });
}

void AgentBridge::requestSessionRename(const QString &user,
                                       const QString &sessionId,
                                       const QString &title)
{
    if (sessionId.isEmpty()) {
        return;
    }

    // RenameSession(user, session_id, title) -> JSON summary of the
    // session as the search lists it now (empty when there was no such
    // session).
    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("RenameSession"));
    message << user << sessionId << title;

    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, sessionId](QDBusPendingCallWatcher *watcher) {
                const QDBusReply<QString> reply = *watcher;
                watcher->deleteLater();
                if (!reply.isValid()) {
                    qWarning() << "session rename failed:" << reply.error().message();
                    return;
                }
                const QVariantMap summary = parseJsonObject(reply.value());
                if (summary.isEmpty()) {
                    // There was no such session: the list says so
                    // already, and nothing changes.
                    return;
                }

                // The entry in the list is the daemon's own answer: the
                // name a session is listed under is the daemon's to
                // compute, and it is the one the search matches
                // against.
                for (int i = 0; i < m_sessionSummaries.size(); ++i) {
                    const QVariantMap entry = m_sessionSummaries.at(i).toMap();
                    if (entry.value(QStringLiteral("id")).toString() == sessionId) {
                        m_sessionSummaries[i] = summary;
                        break;
                    }
                }
                Q_EMIT sessionSummariesChanged();
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
        // agent's own words from the turns before it — the trail of the
        // run, in the order it was said.
        const QStringList notes = messages.mid(0, messages.size() - 1);
        for (const QString &note : notes) {
            m_conversation.appendTrailText(note);
        }
        m_conversation.setResult(messages.last());
    }
    m_conversation.finishExchange();
}

bool AgentBridge::submitAnswers(const QVariantList &answers)
{
    // AnswerQuestion(ask_id, answer_json): one entry per question of the
    // form, in order — {"selected": [indices], "text": "own words"},
    // either part may carry the answer, an empty entry is a deliberate
    // skip. A payload whose entry count does not match the form is not
    // sent at all (the daemon would refuse it; see the guard below). A
    // false reply (form no longer pending, payload rejected) is only
    // logged: the form is restored, and the view lets the user retry.
    if (m_pendingForm.isEmpty() || answers.isEmpty()) {
        return false;
    }

    // One entry per question: the daemon resolves the payload against the
    // form and refuses a count it cannot pair (a wizard's several
    // questions are answered together or not at all). A payload that can
    // never be taken would still be "sent" below — the form cleared, the
    // caller's text consumed — only for the refusal to bring the form
    // back a round-trip later. Refusing it here lets the caller fall
    // back at once: the pane's input bar, whose single text answer can
    // only serve a form of one, submits its text as a prompt instead.
    const QVariantList formQuestions = m_pendingForm.value(QStringLiteral("questions")).toList();
    if (!formQuestions.isEmpty() && answers.count() != formQuestions.count()) {
        return false;
    }

    QJsonArray entries;
    for (const QVariant &entry : answers) {
        const QVariantMap map = entry.toMap();
        QJsonObject object;
        const QJsonArray chosen =
            QJsonArray::fromVariantList(map.value(QStringLiteral("selected")).toList());
        if (!chosen.isEmpty()) {
            object.insert(QStringLiteral("selected"), chosen);
        }
        const QString own = map.value(QStringLiteral("text")).toString().trimmed();
        if (!own.isEmpty()) {
            object.insert(QStringLiteral("text"), own);
        }
        entries.append(object);
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("answers"), entries);
    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("AnswerQuestion"));
    message << m_pendingForm.value(QStringLiteral("askId")).toString()
            << QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));

    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, kAnswerTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this](QDBusPendingCallWatcher *watcher) {
                const QDBusReply<bool> reply = *watcher;
                watcher->deleteLater();
                if (!reply.isValid() || !reply.value()) {
                    qWarning() << "answers were not accepted:" << reply.error().message();
                    // Restore the form: the run is still blocked on it,
                    // and the user must see their questions again —
                    // unless the form ended while the reply was on its
                    // way (answered elsewhere, timed out, cancelled,
                    // run over: the fallback was dropped or a newer
                    // form took its place), in which case the refusal
                    // is stale and restores nothing.
                    if (!m_lastSubmittedForm.isEmpty() && m_pendingForm.isEmpty()) {
                        setPendingForm(m_lastSubmittedForm);
                    }
                }
            });

    // Hand the answers over optimistically: the view returns to the
    // conversation (with the answers in its tool log) the instant the
    // user submits, not a D-Bus round-trip later. A rejection brings
    // the form back above.
    m_lastSubmittedForm = m_pendingForm;
    setPendingForm(QVariantMap());
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

    // The payload is the form: {questions: [{question, options,
    // header?, multiple?}], goal?, harness?}. The view reads exactly
    // these keys, so keep the mapping in one place.
    const QVariantMap form = parseJsonObject(questionJson);

    QVariantList questions;
    const QVariantList rawQuestions = form.value(QStringLiteral("questions")).toList();
    for (const QVariant &questionEntry : rawQuestions) {
        const QVariantMap question = questionEntry.toMap();
        QVariantMap mapped;
        mapped.insert(QStringLiteral("question"),
                      question.value(QStringLiteral("question")).toString());

        QVariantList options;
        const QVariantList rawOptions = question.value(QStringLiteral("options")).toList();
        for (const QVariant &optionEntry : rawOptions) {
            const QVariantMap option = optionEntry.toMap();
            QVariantMap mappedOption;
            mappedOption.insert(QStringLiteral("label"),
                                option.value(QStringLiteral("label")).toString());
            const QString description =
                option.value(QStringLiteral("description")).toString().trimmed();
            if (!description.isEmpty()) {
                mappedOption.insert(QStringLiteral("description"), description);
            }
            options.append(mappedOption);
        }
        mapped.insert(QStringLiteral("options"), options);

        const QString header = question.value(QStringLiteral("header")).toString().trimmed();
        if (!header.isEmpty()) {
            mapped.insert(QStringLiteral("header"), header);
        }
        if (question.value(QStringLiteral("multiple")).toBool()) {
            mapped.insert(QStringLiteral("multiple"), true);
        }
        questions.append(mapped);
    }

    QVariantMap payload;
    payload.insert(QStringLiteral("askId"), askId);
    payload.insert(QStringLiteral("questions"), questions);
    const QString goal = form.value(QStringLiteral("goal")).toString().trimmed();
    if (!goal.isEmpty()) {
        payload.insert(QStringLiteral("goal"), goal);
    }
    // The harness marker: a daemon-authored confirmation (destructive
    // root command), not a model-authored question. The card styles it
    // as a system confirmation, and it never joins a wizard.
    if (form.value(QStringLiteral("harness")).toBool()) {
        payload.insert(QStringLiteral("harness"), true);
    }
    setPendingForm(payload);
}

void AgentBridge::onQuestionAnswered(const QString &runId,
                                     const QString &askId,
                                     const QString &status,
                                     const QString &answerJson)
{
    Q_UNUSED(runId)
    Q_UNUSED(status)
    Q_UNUSED(answerJson)

    // The form ended — whatever ended it. A submission still in flight
    // for it (its answers optimistically cleared, the reply not yet
    // back) is stale now: the run has moved on, and the reply's
    // refusal must not restore a dead form.
    if (m_lastSubmittedForm.value(QStringLiteral("askId")).toString() == askId) {
        m_lastSubmittedForm.clear();
    }

    // Only the form this view is showing ends here: a late answer for an
    // already-replaced form must not clear its successor.
    if (m_pendingForm.value(QStringLiteral("askId")).toString() != askId) {
        return;
    }
    setPendingForm(QVariantMap());
}

void AgentBridge::setPendingForm(const QVariantMap &form)
{
    if (m_pendingForm == form) {
        return;
    }
    m_pendingForm = form;
    Q_EMIT pendingFormChanged();
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

void AgentBridge::onRunStarted(const QString &runId, const QString &sessionId)
{
    // The run is live from here: the pane's send button is a stop button
    // now, and cancelRun() knows the run by its daemon-side id.
    m_runId = runId;
    setRunActive(true);

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
    // A form never survives its run: if the previous run ended without
    // QuestionAnswered (daemon restart mid-question), its view must go.
    // Its restore fallback goes with it — a refusal still in flight
    // must not bring the dead form back on top of this run.
    setPendingForm(QVariantMap());
    m_lastSubmittedForm.clear();
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
        m_conversation.beginExchange(queued.text, localUserName());
    } else {
        m_conversation.beginExchange(QString(), QString());
    }
    if (ours && m_sessionId != sessionId) {
        m_sessionId = sessionId;
    }

    // The pane opens now, not at the model's first token: the provider
    // round trip and the model's own thinking take seconds, and a window
    // that appears only when the answer starts to stream leaves the user
    // looking at a screen that did not react.
    //
    // It is emitted after the row is opened, so the pane comes up with
    // the prompt in it and the exchange's spinner turning — the answer
    // streams into that row.
    Q_EMIT runStarted();
}

void AgentBridge::onStateChanged(const QString &state,
                                 const QString &currentRunId,
                                 uint queueLength)
{
    Q_UNUSED(state)
    Q_UNUSED(currentRunId)

    // How many prompts wait their turn in the daemon. A prompt submitted
    // while another run is busy waits here — the daemon announces it
    // (RunStarted) only when it reaches it — so the count is what the
    // widget shows while it waits.
    const int queued = static_cast<int>(queueLength);
    if (m_queuedPrompts == queued) {
        return;
    }
    m_queuedPrompts = queued;
    Q_EMIT queuedPromptsChanged();
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
    // answer keeps the place it deserves in the pane — and out of the
    // answer's place in the trail, where it keeps the order it was
    // said in.
    flushDeltas();
    m_conversation.demoteResult();

    // Head of the step: the tool name and its arguments. Written at
    // "started" rather than "finished" so a command that hangs or dies with
    // the run still shows up in the trail. Bash gets its command as a bash
    // block; anything else is shown as the raw JSON arguments.
    QString language = QStringLiteral("json");
    QString body = argsJson;
    QString direction;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(argsJson.toUtf8(), &parseError);
    const bool argsParsed = parseError.error == QJsonParseError::NoError;
    if (argsParsed && tool == QLatin1String("bash")) {
        const QJsonObject args = doc.object();
        if (args.contains(QLatin1String("command"))) {
            language = QStringLiteral("bash");
            body = args.value(QLatin1String("command")).toString();
        }
    }
    if (argsParsed && tool == QLatin1String("transfer")) {
        // The arguments say which way the bytes will go: an upload names
        // the local file it sends (source), a download only the address
        // it pulls. The rule is the tool's own (tools/transfer.py):
        // keep the two in step, or the meter and the label disagree.
        const QJsonObject args = doc.object();
        const bool upload = args.contains(QLatin1String("source"));
        direction = upload ? QStringLiteral("upload") : QStringLiteral("download");
        m_transferMeter.begin(upload, args.value(QLatin1String("url")).toString());
    }
    m_conversation.beginToolCall(tool,
                                 QStringLiteral("#### %1 %2\n\n%3")
                                     .arg(QStringLiteral("⚒"), tool, fencedBlock(body, language)),
                                 direction);
}

void AgentBridge::onToolCallFinished(const QString &runId,
                                     const QString &callId,
                                     const QString &tool,
                                     const QString &status,
                                     const QString &output)
{
    Q_UNUSED(runId)
    Q_UNUSED(callId)

    // Tail of the step: outcome plus the command's output, fenced so the
    // pane's Markdown renderer shows it verbatim.
    const QString mark =
        status == QLatin1String("success") ? QStringLiteral("✓") : QStringLiteral("✗ error");
    m_conversation.endToolCall(
        status, QStringLiteral("%1\n\n%2")
                    .arg(mark, fencedBlock(elideOutput(output, kToolLogOutputLimit), QString())));

    // A finished transfer ends its meter, whatever the outcome: the
    // counts that still arrive belong to nobody.
    if (tool == QLatin1String("transfer")) {
        m_transferMeter.release();
    }
}

void AgentBridge::onToolProcessStarted(const QString &runId,
                                       const QString &callId,
                                       const QString &command,
                                       uint pid)
{
    Q_UNUSED(runId)
    Q_UNUSED(callId)

    // Tools run one at a time, so the one meter this bridge owns serves
    // whoever is running; track() also restarts cleanly should a second
    // command ever overlap.
    m_cpuMeter.track(int(pid), command);
}

void AgentBridge::onToolProcessFinished(const QString &runId, const QString &callId, uint pid)
{
    Q_UNUSED(runId)
    Q_UNUSED(callId)
    Q_UNUSED(pid)

    m_cpuMeter.release();
}

void AgentBridge::onWireBytes(const QString &runId, uint turn, uint bytesSent, uint bytesReceived)
{
    Q_UNUSED(runId)

    m_wireMeter.addTraffic(int(turn), bytesSent, bytesReceived);
}

void AgentBridge::onTransferProgress(const QString &runId,
                                     const QString &callId,
                                     const QString &direction,
                                     qulonglong done,
                                     qulonglong total)
{
    Q_UNUSED(runId)
    Q_UNUSED(callId)
    Q_UNUSED(direction)

    // The meter knows the transfer it is metering (ToolCallStarted
    // began it); the counts are cumulative, and it folds the deltas
    // into the pace itself.
    m_transferMeter.addProgress(done, total);
}

QString AgentBridge::senderNameForMode(const QString &mode)
{
    // The pane's bubbles point at whoever (or what) issued the prompt:
    // the sessions of the older daemon carry "root", everything else the
    // actual login name.
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
    // The run is over — completed, failed or cancelled: the send button
    // is a send button again. The id goes with it, so a later Cancel()
    // can never name a run that no longer exists.
    m_runId.clear();
    setRunActive(false);
    // ...and any form the run was blocked on is over now, answered or
    // not (QuestionAnswered usually got here first). The restore
    // fallback goes with it: the run is done, and a refusal reply
    // still in flight must not resurrect the form after the fact.
    setPendingForm(QVariantMap());
    m_lastSubmittedForm.clear();
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
    // Pure pass-through: the bridge is a dumb pipe, and the reaction to a
    // cancelled password dialog lives in the controller. The run id names
    // the run that needed the password.
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

void AgentBridge::onSessionTitleChanged(const QString &sessionId, const QString &title)
{
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

void AgentBridge::reloadSpeech()
{
    if (m_speech != nullptr) {
        m_speech->reload();
    }
}

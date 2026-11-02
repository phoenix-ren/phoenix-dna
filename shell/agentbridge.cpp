// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "agentbridge.h"
#include "conversationmodel.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
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
    // RunStarted/TextDelta/ToolCall* stream the run into resultText and
    // the activity log as it happens: the pane opens at the run's first
    // output (activityStarted) instead of waiting for the result, and the
    // finished result arms the pane's dismiss countdown.
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("RunStarted"), this,
                                          SLOT(onRunStarted(QString, QString, QString)));
    QDBusConnection::sessionBus().connect(kDaemonService, kDaemonPath, kDaemonInterface,
                                          QStringLiteral("TextDelta"), this,
                                          SLOT(onTextDelta(QString, QString)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("RunFinished"), this,
        SLOT(onRunFinished(QString, QString, QString, uint, uint, uint)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("ToolCallStarted"), this,
        SLOT(onToolCallStarted(QString, QString, QString, QString)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("ToolCallFinished"), this,
        SLOT(onToolCallFinished(QString, QString, QString, QString, QString)));
    QDBusConnection::sessionBus().connect(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("TranscriptCleaned"), this,
        SLOT(onTranscriptCleaned(QString, QString, QString, QString)));

    // Text deltas arrive per token; the pane need not re-layout for each.
    m_deltaTimer.setInterval(100);
    m_deltaTimer.setSingleShot(true);
    connect(&m_deltaTimer, &QTimer::timeout, this, &AgentBridge::flushDeltas);
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
    // the previous mode's conversation.
    m_sessionId.clear();

    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("SetMode"));
    message << mode;
    QDBusConnection::sessionBus().asyncCall(message);
}

void AgentBridge::sendPrompt(const QString &prompt)
{
    // SubmitPrompt(prompt, mode, session_id): the session id continues the
    // conversation of the widget's latest run (empty starts a fresh one —
    // the daemon does not continue anything on an empty id by itself). The
    // text and the mode are remembered so RunStarted can pair them with
    // the run they begin (queued prompts begin in submission order) — the
    // mode is what names the exchange's sender in the pane's history.
    m_pendingPrompts.append({prompt, m_modeName});
    QDBusMessage message = QDBusMessage::createMethodCall(
        kDaemonService, kDaemonPath, kDaemonInterface, QStringLiteral("SubmitPrompt"));
    message << prompt << m_modeName << m_sessionId;
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

    // A fresh run opens a new row in the pane's history; the previous rows
    // keep their results, so follow-on questions flow downwards. The
    // daemon runs one prompt at a time, so no run-id matching is needed
    // here. A run that consumed one of our queued prompts is one we
    // started: its prompt pairs with the row, and its session id is the
    // conversation every following sendPrompt() continues. Runs begun
    // elsewhere (e.g. the REPL) do not adopt their session — the widget
    // keeps following its own thread.
    const bool ours = !m_pendingPrompts.isEmpty();
    m_streaming = false;
    m_deltaTimer.stop();
    m_pendingDelta.clear();
    if (ours) {
        const QueuedPrompt queued = m_pendingPrompts.takeFirst();
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

void AgentBridge::flushDeltas()
{
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
    // root prompts come from "root", web prompts are anonymous, and user
    // prompts carry the actual login name.
    if (mode == QLatin1String("root")) {
        return QStringLiteral("root");
    }
    if (mode == QLatin1String("web")) {
        return QStringLiteral("anon");
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

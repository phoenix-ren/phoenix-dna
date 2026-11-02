// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include "conversationmodel.h"

/*!
 * \brief Client for the Python agent service, spoken over DBus.
 *
 * The agent service owns the heavy lifting: speech-to-text, the LLM round-trip
 * and text-to-speech — and, as this becomes a desktop agent, the tools it acts
 * through (windows, files, input, applications). This class is the widget-side
 * half of that contract and deliberately contains no transport logic of its own.
 *
 * The service interface is:
 *
 * \code
 * service: org.phoenixai.agent
 * path:    /org/phoenixai/Agent
 * iface:   org.phoenixai.agent.Agent
 *
 * methods: startDictation(), stopDictation(), sendPrompt(QString)
 * signals: partialTranscript(QString), finalTranscript(QString),
 *          agentMessage(QString), agentStateChanged(QString),
 *          actionProposed(String), actionResult(QString, bool)
 * \endcode
 *
 * The two action signals are what make this a desktop agent rather than a
 * chat: the service proposes an action and reports whether it succeeded. The
 * docked pane renders them as an activity log, so the user can see what the
 * agent did on their machine.
 *
 * The service is optional: if it is not running the widget still works, it
 * simply never receives transcripts.
 *
 * Mode selection is the one message that already speaks to the *live* daemon
 * (agent/phoenix_ai/service.py): bus name ai.phoenix.Agent, object path
 * /ai/phoenix/Agent, interface ai.phoenix.Agent. Its SetMode(name) method
 * records the widget's mode and shows the KDE password dialog when root is
 * selected, so the credentials are settled before the first root prompt.
 * SubmitPrompt(prompt, mode, session_id) sends a generated query to it.
 * RunStarted clears the slate, TextDelta streams the answer into the
 * \a conversation model as it is generated (activityStarted fires once, at
 * the run's first output — the cue to open the results pane), ToolCall*
 * accumulate the row's tool log, and RunFinished(run_id, status,
 * final_text, turns, tokens_in, tokens_out) completes the run's row.
 *
 * The conversation model is the ChatGPT-style history the results pane
 * renders: one row per prompt/answer pair, appended — never replaced — so
 * follow-on questions extend the history the initial voice prompt created.
 *
 * Sessions: the daemon does *not* continue anything on an empty session id
 * — every empty-id SubmitPrompt begins a fresh conversation. The id of the
 * conversation a run belongs to arrives with RunStarted; this bridge
 * remembers it (for runs it started itself) and passes it back with every
 * following SubmitPrompt, so follow-on queries — typed or dictated from the
 * results pane — continue that history. The id is dropped when the mode
 * changes: sessions are filed per mode by the daemon, and a root follow-on
 * must not land in the user session.
 *
 * The tool log is Markdown (each tool call's arguments and output in
 * fenced code blocks), which the results pane renders under the answer —
 * that is what makes a failed run debuggable from the pane.
 */
class AgentBridge : public QObject
{
    Q_OBJECT
    QML_ANONYMOUS

public:
    explicit AgentBridge(QObject *parent = nullptr);
    ~AgentBridge() override;

    /*! \brief Whether the agent service is currently reachable. */
    Q_PROPERTY(bool available READ isAvailable NOTIFY availabilityChanged)

    /*! \brief The most recent partial transcript, updated as speech is decoded. */
    Q_PROPERTY(QString partialTranscript READ partialTranscript NOTIFY partialTranscriptChanged)

    /*! \brief The final transcript for the utterance just completed. */
    Q_PROPERTY(QString finalTranscript READ finalTranscript NOTIFY finalTranscriptChanged)

    /*! \brief The next action the agent proposes to take on the desktop. */
    Q_PROPERTY(QString proposedAction READ proposedAction NOTIFY proposedActionChanged)

    /*! \brief The latest message produced by the agent. */
    Q_PROPERTY(QString agentMessage READ agentMessage NOTIFY agentMessageChanged)

    /*! \brief The conversation history the results pane renders.
     *
     * One row per prompt/answer pair; sendPrompt() queues the prompt text
     * and RunStarted pairs the queue's head with the run that just began
     * (the daemon executes prompts in submission order), opening a new
     * row. TextDelta streams into the row's answer, ToolCall* into its
     * tool log, and RunFinished finalizes it. Rows are appended, never
     * cleared, so follow-on questions flow downwards. */
    Q_PROPERTY(ConversationModel *conversation READ conversation CONSTANT)

    bool isAvailable() const { return m_available; }
    QString partialTranscript() const { return m_partialTranscript; }
    QString finalTranscript() const { return m_finalTranscript; }
    QString agentMessage() const { return m_agentMessage; }

    ConversationModel *conversation() { return &m_conversation; }

    /*! \brief The id of the conversation the latest widget-started run ran
     *  in; empty until one has started. */
    QString sessionId() const { return m_sessionId; }

    QString proposedAction() const { return m_proposedAction; }

    /*! \brief Asks the agent service to begin capturing and transcribing. */
    Q_INVOKABLE void startDictation();

    /*! \brief Asks the agent service to stop the current utterance. */
    Q_INVOKABLE void stopDictation();

    /*! \brief Tells the service which directory to treat as its working context. */
    Q_INVOKABLE void setWorkingDirectory(const QString &path);

    /*! \brief Tells the service whether it may escalate privileges. */
    Q_INVOKABLE void setElevated(bool elevated);

    /*! \brief Tells the daemon which privilege mode the widget selected.
     *
     * Sent to the live agent service (ai.phoenix.Agent). Selecting root
     * makes the daemon pop the KDE password dialog right away and cache the
     * credential for the root prompts that follow; leaving root drops it. */
    Q_INVOKABLE void setMode(const QString &mode);

    /*! \brief Sends a prompt to the live agent daemon.
     *
     * Spoken with the widget's currently selected mode and the session id
     * of the conversation this widget's last run ran in: follow-on queries
     * — from the results pane's input bar, or the dictation overlay —
     * continue that history. An empty id (nothing started yet, or the mode
     * just changed) makes the daemon begin a fresh conversation. */
    Q_INVOKABLE void sendPrompt(const QString &prompt);

Q_SIGNALS:
    void availabilityChanged();
    void partialTranscriptChanged();
    void finalTranscriptChanged();
    void agentMessageChanged();
    void proposedActionChanged();

    /*! \brief Emitted once per run, on its first streamed content.
     *
     * The moment the active run produces its first text delta or tool
     * call — the cue for the results pane to open and follow the run
     * live. */
    void activityStarted();

    /*! \brief Emitted each time a run finishes and its result appears.
     *
     * Fired for every finished run whose final text is non-empty (a
     * cancelled run's streamed partial stays visible instead). The
     * results pane re-pins its scroll to the newest exchange when this
     * fires. */
    void resultReady();

private Q_SLOTS:
    /*! \brief Re-probes and re-subscribes when the service appears or vanishes. */
    void onServiceOwnerChanged(const QString &service,
                               const QString &oldOwner,
                               const QString &newOwner);

    void onPartialTranscript(const QString &text);
    void onFinalTranscript(const QString &text);
    void onAgentMessage(const QString &text);

    void onActionProposed(const QString &text);
    void onActionResult(const QString &text, bool ok);

    /*! \brief Handles the live daemon's RunFinished: a result appeared. */
    void onRunFinished(const QString &runId,
                       const QString &status,
                       const QString &finalText,
                       uint turns,
                       uint tokensIn,
                       uint tokensOut);

    /*! \brief RunStarted: reset the streaming state for the new run. */
    void onRunStarted(const QString &runId, const QString &mode, const QString &sessionId);

    /*! \brief TextDelta: stream the answer into the current row (throttled). */
    void onTextDelta(const QString &runId, const QString &text);

    /*! \brief ToolCallStarted: append the call's entry head (tool + args). */
    void onToolCallStarted(const QString &runId,
                           const QString &callId,
                           const QString &tool,
                           const QString &argsJson);

    /*! \brief ToolCallFinished: append status and output to the entry. */
    void onToolCallFinished(const QString &runId,
                            const QString &callId,
                            const QString &tool,
                            const QString &status,
                            const QString &output);

private:
    void subscribeToServiceSignals();
    void unsubscribeFromServiceSignals();
    void setAvailable(bool available);
    void setPartialTranscript(const QString &text);
    void setFinalTranscript(const QString &text);
    void setAgentMessage(const QString &text);

    static QString fencedBlock(const QString &text, const QString &language);
    static QString elideOutput(const QString &text, int limit);

    /*! \brief Emits activityStarted() once, at the run's first content. */
    void beginStreaming();

    /*! \brief The sender attribution for a prompt submitted in \a mode:
     *  "root" in root mode, "anon" in web mode, the local user's login
     *  name otherwise. */
    static QString senderNameForMode(const QString &mode);

    /*! \brief The local user's login name (USER environment variable,
     *  falling back to the passwd database). */
    static QString localUserName();

private Q_SLOTS:
    /*! \brief Moves buffered text deltas into the current row's answer. */
    void flushDeltas();

private:
    bool m_available = false;
    QString m_workingDirectory;
    bool m_elevated = false;
    // The daemon mode name ("root"/"user"/"web") last sent via setMode();
    // submitPrompt() needs it to route the prompt.
    QString m_modeName = QStringLiteral("user");
    QString m_partialTranscript;
    QString m_finalTranscript;
    QString m_agentMessage;
    QString m_proposedAction;
    // The pane's history: one row per run, appended per RunStarted.
    ConversationModel m_conversation;
    // Streaming state: whether the active run has produced content yet,
    // and the not-yet-flushed text deltas (see m_deltaTimer).
    bool m_streaming = false;
    QString m_pendingDelta;
    QTimer m_deltaTimer;
    // Prompts sent whose runs have not begun yet, each with the daemon
    // mode name at submit time (the sender attribution of the exchange:
    // the mode can change before the daemon starts the queued prompt).
    // FIFO: the daemon executes prompts in submission order, so
    // RunStarted pairs the queue's head with the row it opens.
    struct QueuedPrompt
    {
        QString text;
        QString mode;
    };
    QList<QueuedPrompt> m_pendingPrompts;
    // The session id reported by RunStarted for the latest run this widget
    // started: the conversation that following sendPrompt() calls continue.
    // Cleared on mode changes — the daemon files sessions per mode.
    QString m_sessionId;
};

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
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
 * service: org.phoenixdna.agent
 * path:    /org/phoenixdna/Agent
 * iface:   org.phoenixdna.agent.Agent
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
 * (agent/phoenix_dna/service.py): bus name dna.phoenix.Agent, object path
 * /dna/phoenix/Agent, interface dna.phoenix.Agent. Its SetMode(name) method
 * records the widget's mode and shows the KDE password dialog when root is
 * selected, so the credentials are settled before the first root prompt.
 * SubmitPrompt(prompt, mode, session_id) sends a generated query to it.
 * RunStarted clears the slate, TextDelta streams the answer into the
 * \a conversation model as it is generated (activityStarted fires once, at
 * the run's first output — the cue to open the results pane), ToolCall*
 * accumulate the row's tool log, and RunFinished(run_id, status,
 * final_text, turns, tokens_in, tokens_out) completes the run's row.
 *
 * Transcript cleanup: SubmitTranscriptCleanup(text) asks the daemon to
 * normalize a finished dictation with a single LLM call ("Multiply five
 * times twenty-five" -> "Multiply 5 x 25"); the daemon answers
 * asynchronously with TranscriptCleaned(cleanup_id, source, cleaned,
 * status). requestTranscriptCleanup() speaks that method and
 * transcriptCleaned() is emitted only for the newest request whose cleanup
 * succeeded — an older clip's result, a failed cleanup (the raw text stays
 * on screen) and a superseded request are all dropped here.
 *
 * Session titles: the daemon (re)generates a session's title with a single
 * LLM call after every completed run and announces it with
 * SessionTitleChanged(session_id, mode, title). onSessionTitleChanged
 * exposes the title of this widget's current session as \a sessionTitle
 * (empty until the first title arrives; dropped when the session or mode
 * changes), for the pane's header.
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
 * must not land in the user session. Submitting with the new-session intent
 * (sendPrompt(prompt, true), wired to the panel's record button — a fresh
 * conversation starts there) also drops the id *and* restarts the pane's
 * history when the run begins, so earlier, unrelated conversations never
 * bleed into a new one; prompts from the pane keep the id and append.
 *
 * The tool log is Markdown (each tool call's arguments and output in
 * fenced code blocks), which the results pane renders under the answer —
 * that is what makes a failed run debuggable from the pane.
 *
 * The text the model streams before it calls a tool is its working
 * notes, not its answer: the bridge moves it into the row's thinking,
 * so the pane can show the answer as the point of the run and the
 * notes as its trail.
 *
 * Session search: the widget can browse the history the daemon keeps in
 * its per-user SQLite store. SearchSessions(user, query) answers with the
 * user's sessions whose summary name matches the query — the match is
 * fuzzy, so every further character narrows the list down — and
 * GetSessionTranscript(user, session_id) answers with one session's
 * queries and responses. Both answers arrive asynchronously, and one
 * that belongs to a superseded request is dropped: the list never shows
 * the results of an older query.
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
     * tool log — and the text streamed before them into its thinking, for
     * the agent was working rather than answering — and RunFinished
     * finalizes it. Rows are appended, never cleared, so follow-on
     * questions flow downwards. */
    Q_PROPERTY(ConversationModel *conversation READ conversation CONSTANT)

    /*! \brief The title of the session the widget is currently in.
     *
     * LLM-generated by the daemon after every completed run — so a
     * follow-on that shifts the conversation renames it — and emptied
     * when a new session starts or the mode changes, until the new
     * session's first title arrives. Empty means "no title (yet)": the
     * pane shows its default heading instead. */
    Q_PROPERTY(QString sessionTitle READ sessionTitle NOTIFY sessionTitleChanged)

    /*! \brief The sessions the session search lists.
     *
     * One entry per session of the searched user: its id, mode, title,
     * the summary name it is listed under, a preview of its first
     * query, its timestamps and how many messages it holds. The daemon
     * narrows the list down to the sessions whose name matches the
     * query; a stale answer (one that arrives after a newer search was
     * started) is dropped. */
    Q_PROPERTY(QVariantList sessionSummaries READ sessionSummaries NOTIFY sessionSummariesChanged)

    /*! \brief The queries and responses of the selected session.
     *
     * One entry per message: its role ("user" or "assistant"), its
     * content and the timestamp it was stored with. Replaced when
     * another session's transcript arrives. */
    Q_PROPERTY(QVariantList sessionMessages READ sessionMessages NOTIFY sessionMessagesChanged)

    bool isAvailable() const { return m_available; }
    QString partialTranscript() const { return m_partialTranscript; }
    QString finalTranscript() const { return m_finalTranscript; }
    QString agentMessage() const { return m_agentMessage; }

    ConversationModel *conversation() { return &m_conversation; }

    /*! \brief The id of the conversation the latest widget-started run ran
     *  in; empty until one has started. */
    QString sessionId() const { return m_sessionId; }

    QString sessionTitle() const { return m_sessionTitle; }

    QVariantList sessionSummaries() const { return m_sessionSummaries; }
    QVariantList sessionMessages() const { return m_sessionMessages; }

    QString proposedAction() const { return m_proposedAction; }

    /*! \brief The local user's login name (USER environment variable,
     *  falling back to the passwd database). */
    static QString localUserName();

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
     * Sent to the live agent service (dna.phoenix.Agent). Selecting root
     * makes the daemon pop the KDE password dialog right away and cache the
     * credential for the root prompts that follow; leaving root drops it. */
    Q_INVOKABLE void setMode(const QString &mode);

    /*! \brief Sends a prompt to the live agent daemon.
     *
     * With \a newSession the widget's remembered session id is dropped, so
     * the daemon starts a fresh conversation (and the pane's history
     * restarts at the run's first output); otherwise the prompt continues
     * the conversation of the widget's latest run. An empty id either way
     * (nothing started yet, or the mode just changed) makes the daemon
     * begin a fresh conversation.
     *
     * \a attachments are the files the user attached to the prompt (see
     * PromptAttachments). They are sent along as a JSON payload; a prompt
     * without attachments takes the plain SubmitPrompt() route, so a daemon
     * that does not know the newer method still works. */
    Q_INVOKABLE void sendPrompt(const QString &prompt,
                                bool newSession,
                                const QVariantList &attachments = QVariantList());

    /*! \brief Asks the daemon to clean up a finished transcript.
     *
     * One LLM round-trip on the daemon side; when the cleaned text comes
     * back (transcriptCleaned), superseded requests and failures are
     * dropped, so exactly one signal follows a successful cleanup of the
     * newest dictated clip. Does nothing while no daemon mode is spoken
     * to. */
    Q_INVOKABLE void requestTranscriptCleanup(const QString &text);

    /*! \brief Asks the daemon for the sessions matching a query.
     *
     * SearchSessions(user, query) answers with the user's sessions
     * whose summary name matches the query — the match is fuzzy, so
     * every further character narrows the list down. The answer arrives
     * asynchronously; a reply that belongs to a superseded request is
     * dropped, so the list never shows the results of an older query.
     */
    Q_INVOKABLE void requestSessionSearch(const QString &user, const QString &query);

    /*! \brief Asks the daemon for one session's queries and responses.
     *
     * GetSessionTranscript(user, session_id) answers with the session's
     * messages; like the search, the answer arrives asynchronously and a
     * superseded one is dropped.
     */
    Q_INVOKABLE void requestSessionTranscript(const QString &user, const QString &sessionId);

    /*! \brief Opens a stored session in the results pane.
     *
     * The session's messages are fetched from the daemon and loaded into
     * the conversation model, so the pane shows the conversation the user
     * opened. The session id is adopted as well: a follow-on typed in
     * the pane continues that very session.
     *
     * \a mode is the session's mode ("root" or "user"): it names
     * the sender of the session's queries. \a title is the name the
     * pane's header shows.
     */
    Q_INVOKABLE void openStoredSession(const QString &user,
                                       const QString &sessionId,
                                       const QString &mode,
                                       const QString &title);

Q_SIGNALS:
    void availabilityChanged();
    void partialTranscriptChanged();
    void finalTranscriptChanged();
    void agentMessageChanged();
    void proposedActionChanged();

    /*! \brief Emitted when the searched sessions changed. */
    void sessionSummariesChanged();

    /*! \brief Emitted when the selected session's messages changed. */
    void sessionMessagesChanged();

    /*! \brief Emitted when a stored session was loaded into the
     *  conversation model — the cue for the results pane to open on it. */
    void storedSessionOpened();

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

    /*! \brief Emitted when the session title changed (or was dropped). */
    void sessionTitleChanged();

    /*! \brief The newest dictated clip's cleaned-up transcript arrived.
     *
     * \a source is the raw text the cleanup was requested for; the
     * receiver replaces it with \a cleaned only if nothing newer has been
     * transcribed in the meantime. */
    void transcriptCleaned(const QString &source, const QString &cleaned);

    /*! \brief The daemon's sudo password dialog closed.
     *
     * \a runId is empty for the pre-authentication dialog the daemon
     * shows when root mode is selected (a non-empty id belongs to a
     * running prompt); \a success is false when the dialog was cancelled
     * or the password rejected. */
    void authFinished(const QString &runId, bool success);

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

    /*! \brief AuthFinished: the daemon's password dialog closed. */
    void onAuthFinished(const QString &runId, bool success);

    /*! \brief TextDelta: stream the answer into the current row (throttled). */
    void onTextDelta(const QString &runId, const QString &text);

    /*! \brief ToolCallStarted: move the text streamed so far into the
     *  row's thinking (the agent was working, not answering) and append
     *  the call's entry head (tool + args). */
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

    /*! \brief TranscriptCleaned: the newest clip's cleanup succeeded.
     *  Superseded requests are ignored (their cleanup id no longer
     *  matches) and failures never reach this slot with text. */
    void onTranscriptCleaned(const QString &cleanupId,
                             const QString &source,
                             const QString &cleaned,
                             const QString &status);

    /*! \brief SessionTitleChanged: adopt the title when it belongs to the
     *  session this widget is following; titles of other sessions (e.g.
     *  REPL runs) are ignored. */
    void onSessionTitleChanged(const QString &sessionId, const QString &mode, const QString &title);

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

    /*! \brief Loads a stored session's exchanges into the conversation
     *  model.
     *
     * The messages are the daemon's transcript of the session (role,
     * content): every user message opens an exchange, the assistant
     * messages that follow are its turns. The last of them is the
     * answer, the ones before it the notes the agent wrote while it
     * worked.
     *
     * \a mode names the sender of the queries ("root" or the login
     * name). */
    void loadStoredSession(const QVariantList &messages, const QString &mode);

    /*! \brief Stores one exchange of a stored session: the prompt, who
     *  sent it and the assistant's turns.
     *
     * The last of the \a messages is the answer; the ones before it are
     * the notes the agent wrote while it worked. */
    void storeExchange(const QString &prompt,
                       const QString &sender,
                       const QStringList &messages);

    /*! \brief The sender attribution for a prompt submitted in \a mode:
     *  "root" in root mode, the local user's login name otherwise. */
    static QString senderNameForMode(const QString &mode);

private Q_SLOTS:
    /*! \brief Moves buffered text deltas into the current row's answer. */
    void flushDeltas();

private:
    bool m_available = false;
    QString m_workingDirectory;
    bool m_elevated = false;
    // The daemon mode name ("root"/"user") last sent via setMode();
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
    // the mode can change before the daemon starts the queued prompt) and
    // whether it was submitted as a *new* session — those restart the
    // pane's history at RunStarted. FIFO: the daemon executes prompts in
    // submission order, so RunStarted pairs the queue's head with the row
    // it opens.
    struct QueuedPrompt
    {
        QString text;
        QString mode;
        bool newSession = false;
    };
    QList<QueuedPrompt> m_pendingPrompts;
    // The session id reported by RunStarted for the latest run this widget
    // started: the conversation that following sendPrompt() calls continue.
    // Cleared on mode changes — the daemon files sessions per mode.
    QString m_sessionId;
    // The daemon-generated title of that session, shown in the pane's
    // header; emptied when the session or mode changes until the new
    // session's first title arrives.
    QString m_sessionTitle;
    // The in-flight transcript cleanup: the daemon-side id and the raw text
    // it was asked to clean (the source the eventual replacement applies
    // to). A new request supersedes the previous one.
    QString m_pendingCleanupId;
    QString m_pendingCleanupSource;
    // The sessions the session search lists, and the messages of the
    // selected one.
    QVariantList m_sessionSummaries;
    QVariantList m_sessionMessages;
    // The counters of the outstanding session-search requests: a reply
    // that belongs to an older request is dropped, so the list never
    // shows the results of a superseded query.
    quint64 m_searchRequest = 0;
    quint64 m_transcriptRequest = 0;
    // The same, for the session the results pane is opened on.
    quint64 m_storedRequest = 0;
};

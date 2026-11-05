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
#include "cpumeter.h"
#include "sessiontranscriptmodel.h"
#include "transfermeter.h"
#include "wiremeter.h"

class Speaker;

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
 * The live agent daemon is the Python service (agent/phoenix_dna/service.py):
 * bus name dna.phoenix.Agent, object path /dna/phoenix/Agent, interface
 * dna.phoenix.Agent. Its SubmitPrompt(prompt, session_id) method sends a
 * generated query to it; an empty session id starts a new conversation.
 * RunStarted clears the slate, TextDelta streams the answer into the
 * \a conversation model as it is generated (activityStarted fires once, at
 * the run's first output — the cue to open the results pane), ThinkingDelta
 * streams the model's reasoning into the row's trail, ToolCall* add their
 * steps to it, and RunFinished(run_id, status,
 * final_text, turns, tokens_in, tokens_out) completes the run's row.
 * While a run is live — between RunStarted and RunFinished — \a runActive
 * is true and the results pane's send button is a stop button: cancelRun()
 * speaks the daemon's Cancel(run_id) method, and the run answers with a
 * RunFinished of its own (status "cancelled").
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
 * Questions: when the model needs decisions only the user can make, it
 * calls the daemon's ask_user tool — one call carrying every question it
 * already knows it needs — and the run blocks until the form is answered
 * (or times out). The daemon announces that with QuestionAsked(run_id,
 * ask_id, question_json), parsed here into \a pendingForm: the askId,
 * the questions (each with options), the goal, and the harness marker
 * when it is a daemon-authored confirmation. submitAnswers() sends the
 * user's answers — one entry per question, indices and/or their own
 * words, blanks as deliberate skips — as a single AnswerQuestion call;
 * QuestionAnswered (answered, timeout or cancelled) or the run's end
 * empties the form again. A question is activity like any other: the
 * pane opens on it.
 *
 * A form of several questions is a wizard: it opens the wizard overlay
 * — a large, transient, centred window of its own, themed like the
 * session search — which walks the questions entirely client-side
 * (instant back, exact progress, the answers reviewed before they are
 * submitted as one). Submitting hands the answers over optimistically:
 * the form clears at once, and a rejected answer restores it. Follow-up
 * questions the model asks after an answer opens a new decision arrive
 * as further forms: one question renders as the inline card, several
 * re-open the wizard.
 *
 * Session titles: the daemon (re)generates a session's title with a single
 * LLM call after every completed run and announces it with
 * SessionTitleChanged(session_id, title). onSessionTitleChanged
 * exposes the title of this widget's current session as \a sessionTitle
 * (empty until the first title arrives; dropped when the session
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
 * results pane — continue that history. Submitting with the new-session intent
 * (sendPrompt(prompt, true), wired to the panel's record button — a fresh
 * conversation starts there) also drops the id *and* restarts the pane's
 * history when the run begins, so earlier, unrelated conversations never
 * bleed into a new one; prompts from the pane keep the id and append.
 *
 * The trail of a run is Markdown (each tool call's arguments and output
 * in fenced code blocks), which the results pane renders step by step,
 * in the order the agent did them — that is what makes a failed run
 * debuggable from the pane.
 *
 * The text the model streams before it calls a tool is its working
 * notes, not its answer: the bridge moves it out of the answer, so the
 * pane can show the answer as the point of the run and the notes as
 * its trail.
 *
 * Session search: the widget can browse the history the daemon keeps in
 * its per-user SQLite store. SearchSessions(user, query) answers with the
 * user's sessions whose summary name matches the query — the match is
 * fuzzy, so every further character narrows the list down — and
 * GetSessionTranscript(user, session_id) answers with one session's
 * queries and responses. Both answers arrive asynchronously, and one
 * that belongs to a superseded request is dropped: the list never shows
 * the results of an older query.
 *
 * The history can be pruned as well: DeleteSession(user, session_id)
 * throws a session away — its queries and responses, the audit trail of
 * its runs — and the session leaves the list at once.
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
     * row. TextDelta streams into the row's answer, ThinkingDelta and
     * ToolCall* into its trail — and the text streamed before them into
     * the trail too, for the agent was working rather than answering —
     * and RunFinished finalizes it. Rows are
     * appended, never cleared, so follow-on questions flow downwards. */
    Q_PROPERTY(ConversationModel *conversation READ conversation CONSTANT)

    /*! \brief The CPU meter of the command the agent is running.
     *
     * The daemon reports the bash tool's command with ToolProcessStarted
     * (the command's pid — a session leader, so its process group can be
     * followed as a whole) and ToolProcessFinished; the meter samples
     * /proc while the command runs and exposes the level, its held peak
     * and the command itself. The results pane renders it as an audio
     * level meter under the running bash step. Inactive (and idle) while
     * no command runs; the run's tools are executed one at a time, so
     * one meter serves the whole run. */
    Q_PROPERTY(CpuMeter *cpuMeter READ cpuMeter CONSTANT)

    /*! \brief The rate of the conversation with the model.
     *
     * The daemon reports the bytes of each turn — the request that went
     * out, the answer that came in — and this meter turns them into
     * rates (a three-second moving average) and keeps the peak of each
     * direction, which is what the bars are scaled against. The results
     * pane draws them as a pair of level meters under the running
     * thought step. */
    Q_PROPERTY(WireMeter *wireMeter READ wireMeter CONSTANT)

    /*! \brief The progress of the transfer the agent is running.
     *
     * The daemon reports the bytes a transfer tool call has moved so
     * far (TransferProgress, cumulative counts and the total when the
     * far end named one); ToolCallStarted names the direction and the
     * source of the transfer that follows, and ToolCallFinished ends
     * it. The meter turns the counts into a rate — the same moving
     * average the wire meter uses — and keeps the share of the total,
     * and the results pane draws it under the running transfer step:
     * a level meter for the pace of the bytes, a progress bar for how
     * much of the thing is here. */
    Q_PROPERTY(TransferMeter *transferMeter READ transferMeter CONSTANT)

    /*! \brief The title of the session the widget is currently in.
     *
     * LLM-generated by the daemon after every completed run — so a
     * follow-on that shifts the conversation renames it — and emptied
     * when a new session starts, until the new session's first title
     * arrives. Empty means "no title (yet)": the pane shows its default
     * heading instead. */
    Q_PROPERTY(QString sessionTitle READ sessionTitle NOTIFY sessionTitleChanged)

    /*! \brief The id of the conversation the widget is following.
     *
     * The session the next follow-up continues: the session of the
     * latest run the widget started, the session the user opened from
     * the session search, or the fork the user took — a fork is a
     * session of its own, and the follow-ups continue it. Empty while
     * no conversation is being followed. */
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY sessionIdChanged)

    /*! \brief The sessions the session search lists.
     *
     * One entry per session of the searched user: its id, mode, title,
     * the summary name it is listed under, a preview of its first
     * query, its timestamps and how many messages it holds. The daemon
     * narrows the list down to the sessions whose name matches the
     * query; a stale answer (one that arrives after a newer search was
     * started) is dropped. */
    Q_PROPERTY(QVariantList sessionSummaries READ sessionSummaries NOTIFY sessionSummariesChanged)

    /*! \brief The transcript of the selected session, as a list model.
     *
     * One row per message: its role ("user" or "assistant"), its
     * content and the timestamp it was stored with. The rows are
     * handed over a chunk at a time (see SessionTranscriptModel), so
     * the view fills while the window stays responsive; the empty
     * turns — the tool calls, which are stored without content — are
     * dropped on the way in. */
    Q_PROPERTY(SessionTranscriptModel *sessionTranscript READ sessionTranscript CONSTANT)

    /*! \brief The form the running prompt is blocked on, or an empty map
     *  while none is.
     *
     * ask_user blocks the run on a form of questions. QuestionAsked fills
     * this map — askId, questions (a list of {question, header, options:
     * [{label, description}], multiple}), goal, and harness when it is a
     * daemon-authored confirmation — and QuestionAnswered or the run's
     * end empties it. One question renders as the inline card; several
     * open the wizard view, which walks them client-side (back, exact
     * progress) and submits all answers together. */
    Q_PROPERTY(QVariantMap pendingForm READ pendingForm NOTIFY pendingFormChanged)

    /*! \brief What the session on screen spent of the model's context.
     *
     * One map: the session's title and timestamps, how many queries and
     * answers it holds, the tokens its runs spent (the fresh input, the
     * output, the cached part of the prompt and the reasoning), the
     * model in force with the size of its context window, the share of
     * that window the session has used, and what the tokens cost.
     *
     * The daemon owns the numbers (it keeps the audit trail every run
     * writes its tokens into); refreshSessionContext() asks for them.
     * The map is empty while no session is being followed. */
    Q_PROPERTY(QVariantMap sessionContext READ sessionContext NOTIFY sessionContextChanged)

    bool isAvailable() const { return m_available; }
    QString partialTranscript() const { return m_partialTranscript; }
    QString finalTranscript() const { return m_finalTranscript; }
    QString agentMessage() const { return m_agentMessage; }

    ConversationModel *conversation() { return &m_conversation; }

    CpuMeter *cpuMeter() { return &m_cpuMeter; }

    WireMeter *wireMeter() { return &m_wireMeter; }

    TransferMeter *transferMeter() { return &m_transferMeter; }

    /*! \brief The id of the conversation the latest widget-started run ran
     *  in; empty until one has started. */
    QString sessionId() const { return m_sessionId; }

    /*! \brief Whether the daemon is currently reading an answer aloud.
     *
     * Backed by the daemon's SpeechStateChanged signal (see Speaker);
     * the results pane shows its stop control while this is true. */
    Q_PROPERTY(bool agentSpeaking READ isAgentSpeaking NOTIFY agentSpeakingChanged)

    /*! \brief Whether the daemon's read-aloud is configured.
     *
     * The results pane hides its per-message "Read aloud" affordance
     * while this is false (no key, tts_enabled off, no daemon answer). */
    Q_PROPERTY(bool speechAvailable READ isSpeechAvailable NOTIFY speechAvailableChanged)

    bool isAgentSpeaking() const;
    bool isSpeechAvailable() const;

    /*! \brief Whether an agent run is live right now.
     *
     * True from a run's RunStarted until its RunFinished — the state the
     * results pane's send button turns into a stop button on. A run the
     * daemon lost (no RunFinished because it died mid-run) keeps this true,
     * like the conversation row it belongs to stays "running". */
    Q_PROPERTY(bool runActive READ isRunActive NOTIFY runActiveChanged)

    /*! \brief How many prompts wait in the daemon's queue.
     *
     * A prompt submitted while another run is busy waits its turn: the
     * daemon runs one prompt at a time and announces a queued prompt
     * (RunStarted) only when it reaches it. The count is what tells the
     * widget that a prompt is on its way but has not started yet — the
     * wait is the daemon's, and the user should see that something is
     * happening. */
    Q_PROPERTY(int queuedPrompts READ queuedPrompts NOTIFY queuedPromptsChanged)

    /*! \brief Whether an agent run is live right now. */
    bool isRunActive() const { return m_runActive; }

    /*! \brief How many prompts wait in the daemon's queue. */
    int queuedPrompts() const { return m_queuedPrompts; }

    QString sessionTitle() const { return m_sessionTitle; }

    QVariantList sessionSummaries() const { return m_sessionSummaries; }
    SessionTranscriptModel *sessionTranscript() const { return m_transcript; }

    /*! \brief The form the running prompt is blocked on ({ } when none
     *  is). */
    QVariantMap pendingForm() const { return m_pendingForm; }

    /*! \brief What the session on screen spent of the model's context. */
    QVariantMap sessionContext() const { return m_sessionContext; }

    QString proposedAction() const { return m_proposedAction; }

    /*! \brief The local user's login name (USER environment variable,
     *  falling back to the passwd database). */
    static QString localUserName();

    /*! \brief Asks the agent service to begin capturing and transcribing. */
    Q_INVOKABLE void startDictation();

    /*! \brief Asks the agent service to stop the current utterance. */
    Q_INVOKABLE void stopDictation();

    /*! \brief Sends a prompt to the live agent daemon.
     *
     * With \a newSession the widget's remembered session id is dropped, so
     * the daemon starts a fresh conversation (and the pane's history
     * restarts at the run's first output); otherwise the prompt continues
     * the conversation of the widget's latest run. An empty id either way
     * (nothing started yet) makes the daemon begin a fresh conversation.
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

    /*! \brief Asks the daemon to delete a session from the user's history.
     *
     * DeleteSession(user, session_id) removes the session and everything
     * that belongs to it - its queries and responses, the audit trail of
     * its runs. The answer arrives asynchronously: a session that was
     * deleted leaves the search list at once (sessionSummariesChanged),
     * and the transcript of a session that is gone goes with it.
     */
    Q_INVOKABLE void requestSessionDelete(const QString &user, const QString &sessionId);

    /*! \brief Asks the daemon to name a session the user's way.
     *
     * RenameSession(user, session_id, title) gives the session the name
     * the user chose: from now on the model's own name - which is
     * rewritten after every completed run - never overwrites it. An
     * empty title takes the user's name away, and the session is listed
     * under its first query again.
     *
     * The answer is the session's entry as the search lists it now, so
     * the list follows the daemon's own naming. */
    Q_INVOKABLE void requestSessionRename(const QString &user,
                                          const QString &sessionId,
                                          const QString &title);

    /*! \brief Asks the daemon to fork a session, at one of its queries.
     *
     * ForkSession(user, session_id, exchange_index) branches the
     * session into a new one: the fork holds the conversation up to
     * that query (the query itself and the answer that followed it),
     * and what came after stays in the session it was forked from.
     *
     * \a exchangeIndex counts the user's queries: 1 is the first thing
     * the user asked in the session, 2 the next one. The queries are
     * counted as the session holds them, so the fork lands on the
     * exchange the user pointed at even when the agent did a dozen
     * things in between.
     *
     * The fork is a session of its own, and the widget follows it from
     * then on: the follow-ups typed in the pane continue the fork, and
     * the pane's history keeps the exchanges up to the fork point (and
     * drops the rest, which stayed where it was). The answer arrives
     * asynchronously; a fork that did not happen (no such session, no
     * such query) leaves the conversation where it was. */
    Q_INVOKABLE void requestSessionFork(const QString &user,
                                        const QString &sessionId,
                                        int exchangeIndex);

    /*! \brief Opens a stored session in the results pane.
     *
     * The session's messages are fetched from the daemon and loaded into
     * the conversation model, so the pane shows the conversation the user
     * opened. The session id is adopted as well: a follow-on typed in
     * the pane continues that very session.
     *
     * \a mode is the session's mode: it names the sender of the session's
     * queries ("root" for the sessions of the older daemon, the login name
     * for the rest). \a title is the name the pane's header shows.
     */
    Q_INVOKABLE void openStoredSession(const QString &user,
                                       const QString &sessionId,
                                       const QString &mode,
                                       const QString &title);

    /*! \brief Asks the daemon to read \a text aloud (SpeakText).
     *
     * The daemon normalizes and segments the text exactly as it does an
     * answer — math is spoken as mathematics, code fences are skipped.
     * This is the results pane's per-message "Read aloud" affordance. */
    Q_INVOKABLE void speakText(const QString &text);

    /*! \brief Asks the daemon to stop reading aloud immediately.
     *
     * Both the pane's stop control and the record button's barge-in
     * (the microphone must not transcribe the agent's own voice) go
     * through here. */
    Q_INVOKABLE void stopSpeaking();

    /*! \brief Cancels the live run.
     *
     * Cancel(run_id) over D-Bus: the daemon interrupts the current run
     * (its tools get the cancellation too) and answers with a RunFinished
     * of its own, status "cancelled". Does nothing while no run is live —
     * and nothing when no run id is known (a run announced before this
     * bridge subscribed cannot be named, so it cannot be cancelled). */
    Q_INVOKABLE void cancelRun();

    /*! \brief Applies the speech settings the settings window just
     *  saved (see Speaker::reload), then refreshes this bridge's
     *  speechAvailable / agentSpeaking so the pane reacts at once. */
    Q_INVOKABLE void reloadSpeech();

    /*! \brief Answers the question the running prompt is blocked on.
     *
     * \a selected are the chosen options, as 0-based indices into the
     * question's options (the daemon also accepts exact labels); \a text
     * is anything the user wrote in their own words. Either may carry
     * the answer alone. Returns whether an answer was sent: false when
     * no question is pending, the answer was empty, or the payload's
     * entry count does not match the form's question count (the daemon
     * refuses such a payload, so it is not sent at all) — a caller (the
     * input bar, which answers while a question waits) falls back to
     * submitting the text as a prompt. A false reply from the daemon
     * (question no longer pending, payload does not resolve) is only
     * logged — the question is restored, and the user can retry.
     */
    Q_INVOKABLE bool submitAnswers(const QVariantList &answers);

    /*! \brief Asks the daemon what the session on screen spent of the
     *  model's context window.
     *
     * GetSessionContext(user, session_id) answers with the session's
     * own numbers — its messages, the tokens its runs spent — and with
     * the model in force: the size of its context window, the share of
     * it the session has used, and what the tokens cost. The answer
     * arrives asynchronously, and one that belongs to a superseded
     * request is dropped.
     *
     * The bridge asks on its own whenever the session changes (a run
     * has ended, a stored session has been opened); this is the way to
     * ask again on purpose — the view does it when it is opened, so
     * the numbers it shows are the ones of the moment. */
    Q_INVOKABLE void refreshSessionContext();

Q_SIGNALS:
    void availabilityChanged();
    void partialTranscriptChanged();
    void finalTranscriptChanged();
    void agentMessageChanged();
    void proposedActionChanged();

    /*! \brief Emitted when the searched sessions changed. */
    void sessionSummariesChanged();

    /*! \brief Emitted when a stored session was loaded into the
     *  conversation model — the cue for the results pane to open on it. */
    void storedSessionOpened();

    /*! \brief Emitted once per run, when the daemon begins it.
     *
     * The daemon picks the prompt up and starts the run: the cue for the
     * results pane to open *now* — the model has not said anything yet,
     * and it will not for a while (the request to the provider, the
     * system prompt going over the wire and the model's own thinking
     * take seconds). Opening the pane here rather than at the run's
     * first token is the difference between a window that appears at
     * once and one that appears minutes later.
     *
     * The pane shows the run as live from here: the exchange's row is
     * open, with its spinner, and the answer streams into it. */
    void runStarted();

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

    /*! \brief Emitted when the conversation the widget follows changed
     *  session: a new session began, a stored one was opened, or a
     *  fork was taken. */
    void sessionIdChanged();

    /*! \brief Emitted when a fork was taken: the conversation on screen
     *  was branched into a new session.
     *
     *  \a sessionId is the fork's id - the session the follow-ups
     *  continue from now on. The pane's history keeps the exchanges up
     *  to the fork point; what came after stayed in the session the
     *  fork was taken from. */
    void sessionForked(const QString &sessionId);

    /*! \brief Emitted when the daemon started or stopped reading aloud. */
    void agentSpeakingChanged();

    /*! \brief Emitted when read-aloud became or stopped being available. */
    void speechAvailableChanged();

    /*! \brief Emitted when the pending question changed: asked, answered,
     *  timed out, cancelled — or dropped with its run. */
    void pendingFormChanged();

    /*! \brief Emitted when a run began or ended: the results pane's send
     *  button is a stop button while a run is live. */
    void runActiveChanged();

    /*! \brief Emitted when the context of the session on screen
     *  changed: its tokens, the model in force, the share of the
     *  model's context window the session has used. */
    void sessionContextChanged();

    /*! \brief Emitted when the number of prompts waiting in the daemon's
     *  queue changed. */
    void queuedPromptsChanged();

    /*! \brief The newest dictated clip's cleaned-up transcript arrived.
     *
     * \a source is the raw text the cleanup was requested for; the
     * receiver replaces it with \a cleaned only if nothing newer has been
     * transcribed in the meantime. */
    void transcriptCleaned(const QString &source, const QString &cleaned);

    /*! \brief The daemon's sudo password dialog closed.
     *
     * \a runId names the run that needed the password; \a success is false
     * when the dialog was cancelled or the password rejected. */
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

    /*! \brief Compaction: the conversation was summarised to make room.
     *
     * The older turns went into a summary and the run goes on with
     * that; the pane says so in the run's trail, because a
     * conversation that suddenly forgets what was said is otherwise a
     * mystery. \a summarised is how many messages the summary stands
     * for. */
    void onCompaction(const QString &runId, const QString &sessionId, uint summarised);

    /*! \brief RunStarted: reset the streaming state for the new run. */
    void onRunStarted(const QString &runId, const QString &sessionId);

    /*! \brief StateChanged: the daemon's own state — whether a run is in
     *  flight and how many prompts wait behind it. */
    void onStateChanged(const QString &state, const QString &currentRunId, uint queueLength);

    /*! \brief AuthFinished: the daemon's password dialog closed. */
    void onAuthFinished(const QString &runId, bool success);

    /*! \brief TextDelta: stream the answer into the current row (throttled). */
    void onTextDelta(const QString &runId, const QString &text);

    /*! \brief ThinkingDelta: stream the agent's working notes into the
     *  current row's thinking (throttled). */
    void onThinkingDelta(const QString &runId, const QString &text);

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

    /*! \brief ToolProcessStarted: the bash tool's command began, and its
     *  pid (the session leader of its process group) is handed to the
     *  CPU meter, which follows it through /proc from here. */
    void onToolProcessStarted(const QString &runId,
                              const QString &callId,
                              const QString &command,
                              uint pid);

    /*! \brief ToolProcessFinished: that command ended (normally, by
     *  timeout or by cancellation) — the meter is let go. */
    void onToolProcessFinished(const QString &runId, const QString &callId, uint pid);

    /*! \brief WireBytes: the bytes of one turn of the conversation with
     *  the model — the request that went out and the answer that came
     *  in. The meter turns them into rates. */
    void onWireBytes(const QString &runId, uint turn, uint bytesSent, uint bytesReceived);

    /*! \brief TransferProgress: the bytes a transfer tool call has
     *  moved so far — cumulative, with the total when the far end
     *  named it. The meter turns them into the pace and the share of
     *  the thing that is here. */
    void onTransferProgress(const QString &runId,
                            const QString &callId,
                            const QString &direction,
                            qulonglong done,
                            qulonglong total);

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
    void onSessionTitleChanged(const QString &sessionId, const QString &title);

    /*! \brief QuestionAsked: the running prompt is blocked on a question.
     *
     * The question's JSON is parsed into \a pendingQuestion for the pane's
     * question card; a question is activity like any other, so the pane
     * opens on it. */
    void onQuestionAsked(const QString &runId, const QString &askId, const QString &questionJson);

    /*! \brief QuestionAnswered: the question ended (answered, timed out
     *  or cancelled) and the card gives way. Answers to an older
     *  question never clear a newer one. */
    void onQuestionAnswered(const QString &runId,
                            const QString &askId,
                            const QString &status,
                            const QString &answerJson);

private:
    void subscribeToServiceSignals();
    void unsubscribeFromServiceSignals();
    void setAvailable(bool available);
    void setPartialTranscript(const QString &text);
    void setFinalTranscript(const QString &text);
    void setAgentMessage(const QString &text);
    void setPendingForm(const QVariantMap &form);
    void setRunActive(bool active);

    /*! \brief Follows \a sessionId from now on.
     *
     * The session the next follow-up continues. The change is announced,
     * so the views that name the session (the fork button, which needs
     * to know what it forks) follow it. */
    void setSessionId(const QString &sessionId);

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
     * \a mode names the sender of the queries ("root" for the sessions of the
     * older daemon, the login name otherwise). */
    void loadStoredSession(const QVariantList &messages, const QString &mode);

    /*! \brief Stores one exchange of a stored session: the prompt, who
     *  sent it and the assistant's turns.
     *
     * The last of the \a messages is the answer; the ones before it are
     * the notes the agent wrote while it worked. */
    void storeExchange(const QString &prompt, const QString &sender, const QStringList &messages);

    /*! \brief The sender attribution for a prompt of the given mode:
     *  "root" for the sessions of the older daemon, the local user's
     *  login name otherwise. */
    static QString senderNameForMode(const QString &mode);

private Q_SLOTS:
    /*! \brief Moves the buffered text and thinking deltas into the
     *  current row (the throttle's flush). */
    void flushDeltas();

private:
    bool m_available = false;
    QString m_partialTranscript;
    QString m_finalTranscript;
    QString m_agentMessage;
    QString m_proposedAction;
    // The pane's history: one row per run, appended per RunStarted.
    ConversationModel m_conversation;
    // The CPU meter of the command the running bash tool call executes
    // (ToolProcessStarted/Finished); owned here so the pane reaches it
    // through the same bridge as everything else.
    CpuMeter m_cpuMeter;
    // The rate of the conversation with the model (WireBytes), metered
    // the same way.
    WireMeter m_wireMeter;
    // The progress of the transfer the transfer tool runs
    // (TransferProgress, bracketed by its tool call): owned here so the
    // pane reaches it through the same bridge as everything else.
    TransferMeter m_transferMeter;
    // Streaming state: whether the active run has produced content yet,
    // and the not-yet-flushed text and thinking deltas (see m_deltaTimer).
    bool m_streaming = false;
    QString m_pendingDelta;
    QString m_pendingThinking;
    QTimer m_deltaTimer;
    // Prompts sent whose runs have not begun yet, each with whether it was
    // submitted as a *new* session — those restart the pane's history at
    // RunStarted. FIFO: the daemon executes prompts in submission order,
    // so RunStarted pairs the queue's head with the row it opens.
    struct QueuedPrompt
    {
        QString text;
        bool newSession = false;
    };
    QList<QueuedPrompt> m_pendingPrompts;
    // The session id reported by RunStarted for the latest run this widget
    // started: the conversation that following sendPrompt() calls continue.
    QString m_sessionId;
    // The user whose history that session belongs to: the local one,
    // except for the sessions of the older daemon, which keep their own
    // (the daemon files a session of the "root" mode under that user).
    QString m_sessionUser;
    // The daemon-generated title of that session, shown in the pane's
    // header; emptied when the session changes until the new session's
    // first title arrives.
    QString m_sessionTitle;
    // The in-flight transcript cleanup: the daemon-side id and the raw text
    // it was asked to clean (the source the eventual replacement applies
    // to). A new request supersedes the previous one.
    QString m_pendingCleanupId;
    QString m_pendingCleanupSource;
    // The sessions the session search lists, and the transcript of the
    // selected one.
    QVariantList m_sessionSummaries;
    SessionTranscriptModel *m_transcript = nullptr;
    // The session whose transcript the search shows: the one that was
    // picked last. A deleted session takes its transcript with it, so
    // the bridge has to know whose it is.
    QString m_transcriptSessionId;
    // The daemon-side read-aloud client (see speaker.h): owned here so
    // every surface reaches it through the same bridge.
    Speaker *m_speech = nullptr;
    // The form the running prompt is blocked on (ask_user): parsed from
    // QuestionAsked, emptied by QuestionAnswered, the run's end - or
    // optimistically by submitAnswers, which restores it if the daemon
    // rejects the answers.
    QVariantMap m_pendingForm;
    QVariantMap m_lastSubmittedForm;
    // The live run: its daemon-side id (the name Cancel() cancels) and
    // whether it is live at all — from RunStarted to RunFinished. Both
    // reset together.
    QString m_runId;
    bool m_runActive = false;
    // How many prompts wait in the daemon's queue: the daemon runs one
    // prompt at a time, and a prompt submitted while another run is busy
    // waits its turn before it is even announced.
    int m_queuedPrompts = 0;
    // The counters of the outstanding session-search requests: a reply
    // that belongs to an older request is dropped, so the list never
    // shows the results of a superseded query.
    quint64 m_searchRequest = 0;
    quint64 m_transcriptRequest = 0;
    // The same, for the session the results pane is opened on.
    quint64 m_storedRequest = 0;
    // What the session on screen spent of the model's context window,
    // and the counter of the request that fills it: an answer that
    // belongs to an older request is dropped.
    QVariantMap m_sessionContext;
    quint64 m_contextRequest = 0;
};

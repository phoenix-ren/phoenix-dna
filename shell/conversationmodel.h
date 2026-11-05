// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief The pane's conversation history: one row per prompt/answer pair.
 *
 * The agent bridge appends a row when a run starts (RunStarted), streams
 * the answer and the trail into it as they arrive (TextDelta,
 * ThinkingDelta, ToolCall*), and finalizes it when the run completes
 * (RunFinished). Rows are never removed: follow-on questions extend the
 * history downwards, ChatGPT style, instead of replacing the previous
 * result.
 *
 * Every row carries the run's *trail*: what the agent did, in the order
 * it did it. The trail is not three separate piles (the notes, the tool
 * log, the answer) but one stream of steps — the notes it thinks in, the
 * words it says, the tools it calls — so a run can be followed while it
 * happens. A step that is still running says so, and says for how long:
 * the difference between an agent at work and an agent that hung.
 *
 * Roles:
 * - \c prompt    the user's prompt (plain text, empty for runs not started
 *                from this widget)
 * - \c sender    who issued the prompt: the local user's login name
 *                ("root" for the sessions of the older daemon); empty
 *                for runs not started from this widget
 * - \c result    the answer so far (Markdown; still streaming while
 *                \c running)
 * - \c running   true until the run's final text arrived
 * - \c steps     the trail: one entry per step of the run (see
 *                stepsFor()), each with its kind, its text, the tools it
 *                called and how long it took
 * - \c exchangeNumber  the exchange's number: its queries are counted, so
 *                the first thing the user asked in the session is 1, the
 *                next one 2 (0 for a row that carries no query - a run
 *                begun elsewhere)
 *
 * All mutations run on the GUI thread (the D-Bus layer marshals the
 * daemon's events onto the main loop).
 */
class ConversationModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles
    {
        PromptRole = Qt::UserRole + 1,
        SenderRole,
        ResultRole,
        RunningRole,
        StepsRole,
        ExchangeRole
    };
    Q_ENUM(Roles)

    /*! \brief The kinds of step the agent's trail is made of. */
    enum class StepKind
    {
        Thought, //!< the notes the model thinks in
        Text,    //!< the words the model says
        Tool     //!< a tool call, with its arguments and its output
    };

    explicit ConversationModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

Q_SIGNALS:
    /*! \brief The number of exchanges in the history changed. */
    void countChanged();

public:
    /*! \brief Drops every exchange: a new session began, so the pane
     *  restarts with an empty history instead of appending to prior,
     *  unrelated conversations. */
    void reset();

    /*! \brief Keeps the first \a count exchanges and drops the rest.
     *
     * The pane's history after a fork: the conversation up to the point
     * the user forked at, and nothing of what came after it - that
     * stayed in the session the fork was taken from. The exchanges
     * that are kept are not touched: their trails and answers stay
     * as they were.
     *
     * A count beyond the history's length changes nothing, and neither
     * does a negative one (the history is emptied then). */
    void truncate(int count);

    /*! \brief The row of the exchange numbered \a number, or -1.
     *
     * The queries of the conversation are counted (the first thing the
     * user asked is 1, the next one 2), and this is where that number
     * sits in the history: the row of the exchange that carries it.
     * -1 stands for a number the history does not hold - a run begun
     * elsewhere carries no query and so no number.
     *
     * The fork is taken at such a number: the daemon branches the
     * session at its n-th query, and the pane keeps the exchanges up
     * to that row. */
    int rowOfExchange(int number) const;

    /*! \brief Starts a new exchange: appends a row with the given prompt
     *  and its sender (the login name, or "root" for the sessions of
     *  the older daemon). */
    void beginExchange(const QString &prompt, const QString &sender);

    /*! \brief Appends streamed answer text to the current exchange.
     *
     * The words join the answer's step, or start one: the answer is the
     * text that follows the last tool call, so the step is created with
     * the answer's flag on. */
    void appendResult(const QString &delta);

    /*! \brief Replaces the current exchange's result (e.g. on finish).
     *
     * The finished text is authoritative: the step that carries these
     * words becomes the answer (and leaves the trail, which shows them
     * only once), whether the words were streamed or arrive only now. */
    void setResult(const QString &result);

    /*! \brief The answer so far is not the answer any more.
     *
     * The agent reached for a tool, so the words it said before that
     * are the preamble to the work, not the answer: they stay in the
     * trail, in the order they were said, and the answer starts over
     * with the next words. */
    void demoteResult();

    /*! \brief Appends streamed thinking to the current exchange.
     *
     * The notes are the model's own reasoning: they join the trail's
     * thought step, or start one. */
    void appendThinking(const QString &delta);

    /*! \brief Adds words of the agent's own to the trail.
     *
     * Used for a stored session, whose history carries the model's
     * messages but not the tool calls that went with them: the words
     * before the answer are part of the trail, in the order they were
     * said. */
    void appendTrailText(const QString &text);

    /*! \brief Starts a tool call in the current exchange.
     *
     * \a markdown is the call's head (its name and its arguments); the
     * output follows with endToolCall(). The calls of one stretch belong
     * together: they are one step of the trail, not one step each. */
    void beginToolCall(const QString &tool, const QString &markdown);

    /*! \brief Ends the tool call that is running.
     *
     * \a status is "success" or "error", \a markdown the call's tail
     * (its outcome and its output). */
    void endToolCall(const QString &status, const QString &markdown);

    /*! \brief The run finished: \c running goes false. */
    void finishExchange();

private:
    /*! \brief One step of the agent's work.
     *
     * A stretch of the trail of the same kind: the notes the model
     * thinks in, the words it says, the tools it calls. The tools of one
     * stretch are gathered in one step, so a run of eight calls is one
     * entry in the trail and not eight. */
    struct Group
    {
        StepKind kind = StepKind::Text;
        QStringList tools;    //!< the tools called (Tool steps)
        QString body;         //!< the step's Markdown
        int count = 0;        //!< how many parts the step holds
        QString status;       //!< "success" or "error" (Tool steps)
        bool running = false; //!< the step is still in flight
        bool answer = false;  //!< the step holds the answer
        qint64 id = 0;        //!< the step's own identity
        qint64 startedAt = 0; //!< when the step began (ms since the epoch)
        qint64 endedAt = 0;   //!< when it ended (0 while it runs)
    };

    struct Exchange
    {
        QString prompt;
        QString sender;
        QString result;
        QList<Group> groups;
        bool running = true;
    };

    /*! \brief Emits dataChanged for the current row's \a roles. */
    void touchCurrent(const QList<int> &roles);

    /*! \brief The trail of an exchange: its steps, in order, without the
     *  answer (which the pane shows on its own, below the trail). */
    QVariantList stepsFor(const Exchange &exchange) const;

    /*! \brief The last step of \a kind, or nullptr if the trail does not
     *  end in one. */
    Group *lastGroupOfKind(Exchange &exchange, StepKind kind);

    /*! \brief Closes the step that is running and starts a new one. */
    Group &pushGroup(Exchange &exchange, StepKind kind);

    QList<Exchange> m_exchanges;
    // The number of each exchange: its queries are counted, so the
    // first thing the user asked in the session is 1. A row that
    // carries no query (a run begun elsewhere) has none (0).
    QList<int> m_exchangeNumbers;
    // How many queries the history holds: the next one is this many
    // plus one.
    int m_promptCount = 0;
    // The identity of the last step: it gives every step of every
    // exchange a name of its own, so the pane can remember which ones
    // the user opened while the trail grows under them.
    qint64 m_groupId = 0;
};

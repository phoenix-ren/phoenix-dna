// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief The pane's conversation history: one row per prompt/answer pair.
 *
 * The agent bridge appends a row when a run starts (RunStarted), streams
 * the answer and tool log into it as they arrive (TextDelta, ToolCall*),
 * and finalizes it when the run completes (RunFinished). Rows are never
 * removed: follow-on questions extend the history downwards, ChatGPT
 * style, instead of replacing the previous result.
 *
 * Roles:
 * - \c prompt    the user's prompt (plain text, empty for runs not started
 *                from this widget)
 * - \c sender    who issued the prompt: "root" in root mode, the local
 *                user's login name in user mode; empty
 *                for runs not started from this widget
 * - \c result    the answer so far (Markdown; still streaming while
 *                \c running)
 * - \c thinking  the notes the agent wrote while it worked (Markdown): the
 *                text of the turns that ended in a tool call. Kept apart
 *                from the answer, so the pane can show the answer as the
 *                point of the run and the notes as its trail.
 * - \c toolLog   the Markdown tool-activity log of the run
 * - \c toolCount how many tool calls the run made
 * - \c running   true until the run's final text arrived
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
        ToolLogRole,
        RunningRole,
        ThinkingRole,
        ToolCountRole
    };
    Q_ENUM(Roles)

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

    /*! \brief Starts a new exchange: appends a row with the given prompt
     *  and its sender ("root" or the login name). */
    void beginExchange(const QString &prompt, const QString &sender);

    /*! \brief Appends streamed answer text to the current exchange. */
    void appendResult(const QString &delta);

    /*! \brief Replaces the current exchange's result (e.g. on finish). */
    void setResult(const QString &result);

    /*! \brief Moves the current exchange's result into its thinking.
     *
     * The text a turn produced before it turned out to call tools is the
     * agent thinking, not the answer: the pane shows it as such, so the
     * answer keeps the place it deserves. */
    void demoteResultToThinking();

    /*! \brief Appends streamed thinking to the current exchange. */
    void appendThinking(const QString &delta);

    /*! \brief Replaces the current exchange's thinking (the notes the
     *  agent wrote while it worked). */
    void setThinking(const QString &thinking);

    /*! \brief Appends tool-activity Markdown to the current exchange. */
    void appendToolLog(const QString &markdown);

    /*! \brief Counts one more tool call in the current exchange. */
    void noteToolCall();

    /*! \brief The run finished: \c running goes false. */
    void finishExchange();

private:
    /*! \brief Emits dataChanged for the current row's \a roles. */
    void touchCurrent(const QList<int> &roles);

    /*! \brief Drops the last chunk of the thinking if it is \a text.
     *
     * The agent's last words before it stopped are the answer now, so
     * the notes must not show them a second time. Returns whether
     * something was dropped. */
    bool dropLastThinkingIf(const QString &text);

    struct Exchange
    {
        QString prompt;
        QString sender;
        QString result;
        QString thinking;
        QString toolLog;
        int toolCount = 0;
        bool running = true;
    };

    QList<Exchange> m_exchanges;
};

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QString>

/*!
 * \brief The session intent of the dictation in flight.
 *
 * A prompt is either a new session or a follow-on of the conversation on
 * screen, and the difference is decided by the *button* that started the
 * recording:
 *
 *   * the panel widget's record button starts a new session — the user
 *     asks something unrelated to whatever was on screen;
 *   * the results pane's own record button continues the conversation
 *     that is on screen — a follow-on.
 *
 * The intent has to be remembered at the moment the recording starts,
 * because that is the only moment the two are distinguishable: when the
 * results pane happens to be open, the transcript of a dictation started
 * from the panel widget lands in the pane's input bar, exactly like one
 * dictated into the pane itself. The surface cannot tell them apart; the
 * button that started the recording can.
 *
 * The state here is small and on its own, so it lives apart from the
 * controller (which needs a session bus, a microphone and an agent
 * daemon): the decision can then be checked on its own.
 */
class DictationIntent : public QObject
{
    Q_OBJECT

public:
    explicit DictationIntent(QObject *parent = nullptr);
    ~DictationIntent() override;

    /*! \brief Whether the recording in flight starts a new session.
     *
     * True for a dictation the panel widget's record button started,
     * false for one the results pane's own record button started. */
    bool startsNewSession() const { return m_startsNewSession; }

    /*! \brief Whether a dictated prompt is waiting to be sent.
     *
     * The transcript of a recording sits in the editor until the user
     * sends it; this says whether the text there came from a recording
     * (and so carries that recording's intent) or was typed. */
    bool promptPending() const { return m_promptPending; }

    /*! \brief The intent of the recording that starts here.
     *
     * \a newSession is true for the panel widget's record button and
     * false for the results pane's own record button. The intent is
     * remembered until the prompt is submitted. */
    void recordingStarted(bool newSession);

    /*! \brief The transcript of a recording arrived: it is a prompt
     *  waiting to be sent.
     *
     * An empty transcript is nothing to send, so no prompt waits. */
    void transcriptArrived(const QString &text);

    /*! \brief The prompt is on its way (or is gone): nothing waits. */
    void promptConsumed();

    /*! \brief The user emptied the editor: the dictated transcript is
     *  gone, so what is typed in its place is a fresh query.
     *
     * A correction of the transcript is not an emptying: it is still the
     * recording's prompt, and it keeps the recording's intent. */
    void promptDiscarded();

    /*! \brief The session intent of a prompt composed in a surface.
     *
     * \a dictated says whether the prompt came from a dictation (a
     * recording), as opposed to being typed. A dictated prompt takes the
     * intent of the button that started the recording — a new session for
     * the panel widget's record button, a follow-on for the results
     * pane's own record button — and a typed prompt continues the
     * conversation on screen. */
    bool newSessionIntent(bool dictated) const;

Q_SIGNALS:
    /*! \brief Emitted when the intent of the recording in flight changed. */
    void startsNewSessionChanged();

    /*! \brief Emitted when a dictated prompt began or stopped waiting to
     *  be sent. */
    void promptPendingChanged();

private:
    /*! \brief Remembers whether a dictated prompt is waiting to be sent.
     *
     * Announces the change, so the surfaces that show the prompt know
     * whether it carries a recording's session intent. */
    void setPromptPending(bool pending);

    // The intent of the recording in flight.
    bool m_startsNewSession = true;
    // Whether a dictated prompt is waiting to be sent.
    bool m_promptPending = false;
};

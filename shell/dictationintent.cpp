// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "dictationintent.h"

DictationIntent::DictationIntent(QObject *parent) : QObject(parent) {}

DictationIntent::~DictationIntent() = default;

void DictationIntent::recordingStarted(bool newSession)
{
    // The button that starts the dictation decides its intent: the panel
    // widget's record button starts a new session, the results pane's own
    // record button continues the one on screen. The intent is remembered
    // for the submission that follows.
    //
    // A new recording also supersedes the prompt that was waiting to be
    // sent: the transcript it replaces is gone.
    if (m_startsNewSession != newSession) {
        m_startsNewSession = newSession;
        Q_EMIT startsNewSessionChanged();
    }
    setPromptPending(false);
}

void DictationIntent::transcriptArrived(const QString &text)
{
    // The transcript is a dictated prompt waiting to be sent: it carries
    // the intent of the button that started the recording. An empty
    // transcript is nothing to send, so no prompt waits.
    setPromptPending(!text.isEmpty());
}

void DictationIntent::promptConsumed()
{
    setPromptPending(false);
}

void DictationIntent::promptDiscarded()
{
    setPromptPending(false);
}

bool DictationIntent::newSessionIntent(bool dictated) const
{
    // A typed prompt continues the conversation on screen: the pane owns
    // the session while it is up, and the keyboard says nothing about
    // where the conversation should go.
    if (!dictated) {
        return false;
    }
    // A dictated prompt takes the intent of the button that started the
    // recording.
    //
    // A corrected transcript is still the recording's: the user fixed the
    // recognition, not the intent.
    if (!m_promptPending) {
        return false;
    }
    return m_startsNewSession;
}

void DictationIntent::setPromptPending(bool pending)
{
    if (m_promptPending == pending) {
        return;
    }
    m_promptPending = pending;
    Q_EMIT promptPendingChanged();
}

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

// The session intent of a prompt — a new session or a follow-on of the
// conversation on screen — checked without a window: the button that
// started the recording decides it, and it is the only thing that can
// decide it, because a dictation started from the panel widget lands in
// the results pane's input bar when the pane happens to be open, exactly
// like one dictated into the pane itself.
//
//   dictationintenttest
//
// Exits non-zero when a check fails. (Plain printf, not qInfo: the
// categorized logging prints nothing in this environment.)

#include "dictationintent.h"

#include <QCoreApplication>

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

// A counter for the signals of the intent, so the test can see what the
// surfaces would see.
class SignalCounter : public QObject
{
    Q_OBJECT

public:
    SignalCounter(DictationIntent *intent)
    {
        connect(intent, &DictationIntent::startsNewSessionChanged, this,
                [this] { ++startsNewSession; });
        connect(intent, &DictationIntent::promptPendingChanged, this, [this] { ++promptPending; });
    }

    int startsNewSession = 0;
    int promptPending = 0;
};

// The panel widget's record button: a new session.
constexpr bool kPanelRecord = true;
// The results pane's own record button: a follow-on.
constexpr bool kPaneRecord = false;
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ---- A typed prompt continues the conversation on screen -----------
    {
        DictationIntent intent;
        // Nothing was dictated, so a typed prompt is a follow-on — even
        // though the panel's record button was the last one pressed.
        intent.recordingStarted(kPanelRecord);
        check(!intent.newSessionIntent(false), "a typed prompt is a follow-on");
        check(!intent.newSessionIntent(false), "a typed prompt stays a follow-on");
    }

    // ---- The panel widget's record button starts a new session ---------
    {
        DictationIntent intent;
        intent.recordingStarted(kPanelRecord);
        intent.transcriptArrived(QStringLiteral("what is the weather"));
        check(intent.promptPending(), "the transcript is a prompt waiting to be sent");
        check(intent.newSessionIntent(true),
              "a dictation from the panel's record button starts a new session");
    }

    // ---- The results pane's own record button continues the session ----
    {
        DictationIntent intent;
        intent.recordingStarted(kPaneRecord);
        intent.transcriptArrived(QStringLiteral("and tomorrow"));
        check(intent.promptPending(), "the transcript is a prompt waiting to be sent");
        check(!intent.newSessionIntent(true),
              "a dictation from the pane's record button is a follow-on");
    }

    // ---- A new recording supersedes the prompt that was waiting --------
    {
        DictationIntent intent;
        intent.recordingStarted(kPanelRecord);
        intent.transcriptArrived(QStringLiteral("first"));
        check(intent.promptPending(), "the first prompt waits");
        // A second recording: the transcript it replaces is gone.
        intent.recordingStarted(kPaneRecord);
        check(!intent.promptPending(), "a new recording supersedes the prompt that waited");
        intent.transcriptArrived(QStringLiteral("second"));
        check(!intent.newSessionIntent(true), "the new recording's own intent is followed");
    }

    // ---- A sent prompt is gone: the next one starts over ---------------
    {
        DictationIntent intent;
        intent.recordingStarted(kPanelRecord);
        intent.transcriptArrived(QStringLiteral("first"));
        intent.promptConsumed();
        check(!intent.promptPending(), "a sent prompt does not wait any more");
        // The panel's button was the last one pressed, but the prompt it
        // made is gone — what is typed now is a follow-on of the
        // conversation on screen.
        check(!intent.newSessionIntent(true),
              "a prompt that was sent is not a pending dictated prompt");
    }

    // ---- An emptied editor is a fresh query, not the recording's -------
    {
        DictationIntent intent;
        intent.recordingStarted(kPanelRecord);
        intent.transcriptArrived(QStringLiteral("what is the weather"));
        check(intent.newSessionIntent(true), "the recording's intent before the editor is emptied");
        intent.promptDiscarded();
        check(!intent.promptPending(), "an emptied editor leaves no prompt waiting");
        check(!intent.newSessionIntent(true), "an emptied editor is a fresh query: a follow-on");
    }

    // ---- An empty transcript is nothing to send ------------------------
    {
        DictationIntent intent;
        intent.recordingStarted(kPanelRecord);
        intent.transcriptArrived(QString());
        check(!intent.promptPending(), "an empty transcript is no prompt to send");
        check(!intent.newSessionIntent(true), "an empty transcript starts no session");
    }

    // ---- The changes are announced --------------------------------------
    {
        DictationIntent intent;
        SignalCounter counter(&intent);

        // The panel's button: the intent is already the default, so
        // nothing is announced.
        intent.recordingStarted(kPanelRecord);
        check(counter.startsNewSession == 0,
              "the panel's record button announces no change of intent");

        // The pane's button: the intent changes.
        intent.recordingStarted(kPaneRecord);
        check(counter.startsNewSession == 1,
              "the pane's record button announces the change of intent");

        // The transcript arrives: a prompt waits.
        intent.transcriptArrived(QStringLiteral("and tomorrow"));
        check(counter.promptPending == 1, "a transcript announces the prompt that waits");

        // Sending it: nothing waits any more.
        intent.promptConsumed();
        check(counter.promptPending == 2, "a sent prompt announces that nothing waits any more");
    }

    if (failures > 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}

#include "dictationintenttest.moc"

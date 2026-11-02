// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "phoenixcontroller.h"
#include "agentbridge.h"
#include "agentmode.h"
#include "audiocapture.h"
#include "audioringbuffer.h"
#include "cliprecorder.h"
#include "transcriber.h"

#include <KGlobalAccel>
#include <KLocalizedString>
#include <QQmlEngine>

#include <QAction>
#include <QFileInfo>
#include <QKeySequence>
#include <cstdio>

PhoenixController::PhoenixController(QObject *parent)
    : QObject(parent),
      m_waveform(new AudioRingBuffer(this)),
      m_agent(new AgentBridge(this)),
      m_capture(new AudioCapture(this)),
      m_clipRecorder(new ClipRecorder(this)),
      m_transcriber(new Transcriber(this)),
      m_attachments(new PromptAttachments(this)),
      m_screenshotPicker(new WindowScreenshotPicker(this))
{
    // A photographed window becomes an attachment of the prompt that is
    // being composed - the next one the user sends.
    connect(m_screenshotPicker, &WindowScreenshotPicker::captured, this,
            [this](const QString &path, const QString &title) {
                addAttachment(QUrl::fromLocalFile(path));
                if (!title.isEmpty()) {
                    setStatusMessage(i18n("Attached the window “%1”.").arg(title));
                }
            });
    connect(m_screenshotPicker, &WindowScreenshotPicker::failed, this,
            [this](const QString &message) { setStatusMessage(message); });

    // The overlay pops up as soon as dictation starts, so the transcript is
    // visible while the user is still speaking.
    connect(m_agent, &AgentBridge::partialTranscriptChanged, this, [this] {
        m_partialTranscript = m_agent->partialTranscript();
        Q_EMIT partialTranscriptChanged();
    });

    connect(m_agent, &AgentBridge::finalTranscriptChanged, this, [this] {
        m_partialTranscript = m_agent->finalTranscript();
        Q_EMIT partialTranscriptChanged();
    });

    // Captured audio feeds the scrolling waveform. The capture helper is a
    // separate process, so a fault there never reaches plasmashell.
    connect(m_capture, &AudioCapture::pcmReady, m_waveform, &AudioRingBuffer::append);
    // The same stream also fills the dictation clip that is sent to the
    // transcription service once recording stops.
    connect(m_capture, &AudioCapture::pcmReady, m_clipRecorder, &ClipRecorder::append);
    connect(m_capture, &AudioCapture::captureFailed, this, [this](const QString &message) {
        qWarning() << "Audio capture failed:" << message;
        setRecording(false);
    });

    // The finished clip comes back as text and lands in the overlay via
    // partialTranscript. An empty answer means nothing was intelligible;
    // the overlay explains that in its placeholder. The raw text is shown
    // immediately (it is what the user said) and a single LLM call on the
    // daemon cleans it up ("Multiply five times twenty-five" -> "Multiply
    // 5 x 25"); the cleaned text replaces the raw one as soon as it
    // arrives, unless a newer transcription has taken its place.
    connect(m_transcriber, &Transcriber::finished, this, [this](const QString &text) {
        setTranscribing(false);
        m_partialTranscript = text;
        Q_EMIT partialTranscriptChanged();
        if (!text.trimmed().isEmpty()) {
            m_agent->requestTranscriptCleanup(text);
        }
    });
    connect(m_agent, &AgentBridge::transcriptCleaned, this,
            [this](const QString &source, const QString &cleaned) {
                // Adopt the cleanup only while it still applies to what is on
                // screen: a newer dictation (or its raw result) supersedes it.
                if (m_partialTranscript != source) {
                    return;
                }
                m_partialTranscript = cleaned;
                Q_EMIT partialTranscriptChanged();
            });
    connect(m_transcriber, &Transcriber::failed, this, [this](const QString &message) {
        setTranscribing(false);
        setStatusMessage(QStringLiteral("Transcription failed: %1").arg(message));
    });

    // The pane pops up the moment the run produces its first output, and
    // by then the frozen waveform has served its purpose: the user has had
    // the whole transcription to read the last moments of audio, so the
    // widget rests empty instead of showing a stale envelope until the
    // next recording. (clear() refuses to wipe a live waveform, in case
    // the user is already dictating again when the pane appears.)
    connect(m_agent, &AgentBridge::activityStarted, m_waveform, &AudioRingBuffer::clear);

    // The record button has a system-wide companion so dictation can be
    // toggled from any application, not just while the panel has focus.
    // KGlobalAccel owns the binding: it shows up in System Settings ->
    // Shortcuts, is announced over D-Bus to kglobalacceld and survives
    // restarts. The sequence below is only the first-run default; the
    // Autoloading flag keeps whatever the user re-bound it to later.
    m_toggleShortcut = new QAction(this);
    m_toggleShortcut->setObjectName(QStringLiteral("toggle-recording"));
    m_toggleShortcut->setText(i18n("Toggle Recording"));
    // Without these KGlobalAccel would file the shortcut under plasmashell's
    // component; an own component keeps it discoverable and independent.
    m_toggleShortcut->setProperty("componentName", QStringLiteral("org.phoenixdna.agent"));
    m_toggleShortcut->setProperty("componentDisplayName", QStringLiteral("Phoenix DNA"));
    const QList<QKeySequence> defaultHotkey{QKeySequence(QStringLiteral("Meta+Alt+Space"))};
    KGlobalAccel::self()->setDefaultShortcut(m_toggleShortcut, defaultHotkey);
    KGlobalAccel::self()->setShortcut(m_toggleShortcut, defaultHotkey);
    connect(m_toggleShortcut, &QAction::triggered, this, &PhoenixController::toggleRecording);
    // A re-binding in System Settings must be reflected wherever the QML
    // side shows the shortcut (the record button's tooltip).
    connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, this,
            [this](QAction *action, const QKeySequence &) {
                if (action == m_toggleShortcut) {
                    Q_EMIT recordShortcutChanged();
                }
            });

    // The session search has a system-wide companion too, so the history
    // can be reached from any application: Meta+Alt+` by default. It is
    // bound the same way as the record hotkey — a first-run default the
    // user can re-bind in System Settings.
    m_searchShortcut = new QAction(this);
    m_searchShortcut->setObjectName(QStringLiteral("search-sessions"));
    m_searchShortcut->setText(i18n("Search Sessions"));
    // Its own KGlobalAccel component: the window picker's binding is the
    // bare "Meta+Alt" (modifiers only), and in the same component that
    // entry swallows every "Meta+Alt" + punctuation combination - the
    // grave key among them. A component of its own keeps the search's
    // shortcut out of its way (and gives it a group of its own in
    // System Settings).
    m_searchShortcut->setProperty("componentName", QStringLiteral("org.phoenixdna.search"));
    m_searchShortcut->setProperty("componentDisplayName", QStringLiteral("Phoenix DNA Search"));
    const QList<QKeySequence> searchHotkey{QKeySequence(QStringLiteral("Meta+Alt+`"))};
    KGlobalAccel::self()->setDefaultShortcut(m_searchShortcut, searchHotkey);
    KGlobalAccel::self()->setShortcut(m_searchShortcut, searchHotkey);
    connect(m_searchShortcut, &QAction::triggered, this, &PhoenixController::toggleSessionSearch);
    connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, this,
            [this](QAction *action, const QKeySequence &) {
                if (action == m_searchShortcut) {
                    Q_EMIT searchShortcutChanged();
                }
            });
}

PhoenixController::~PhoenixController() = default;

PhoenixController *PhoenixController::create(QQmlEngine *, QJSEngine *)
{
    // The engine takes ownership of the singleton.
    return new PhoenixController;
}

void PhoenixController::setMode(int mode)
{
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;

    // The agent service needs to know the working context it operates in.
    const QString directory = AgentMode::workingDirectory(static_cast<AgentMode::Mode>(m_mode));
    m_agent->setWorkingDirectory(directory);
    m_agent->setElevated(AgentMode::isElevated(static_cast<AgentMode::Mode>(m_mode)));

    // The daemon learns the mode over D-Bus: selecting root makes it show
    // the KDE password dialog right away instead of mid-first-prompt.
    m_agent->setMode(AgentMode::modeName(static_cast<AgentMode::Mode>(m_mode)));

    // The sessions the search lists belong to the mode's user: root-mode
    // runs are filed under "root", everything else under the login name.
    Q_EMIT searchUserChanged();

    Q_EMIT modeChanged();
}

QString PhoenixController::modeIconName(int mode) const
{
    return AgentMode::iconName(static_cast<AgentMode::Mode>(mode));
}

QString PhoenixController::modeDisplayName(int mode) const
{
    return AgentMode::displayName(static_cast<AgentMode::Mode>(mode));
}

void PhoenixController::toggleRecording()
{
    setRecording(!m_recording);
}

void PhoenixController::setRecording(bool recording)
{
    if (m_recording == recording) {
        return;
    }

    if (recording) {
        // A new dictation clears the transcript and any stale error.
        m_partialTranscript.clear();
        Q_EMIT partialTranscriptChanged();
        setStatusMessage(QString());

        // Three quarters of a second of history: long enough to read,
        // short enough that the widget keeps up with speech.
        m_waveform->start(0.75);
        if (!m_capture->start()) {
            // Capturing is the whole point of the button; do not pretend it
            // is running when the helper could not start.
            m_waveform->stop();
            return;
        }
        m_clipRecorder->start();
        m_agent->startDictation();
        // Dictating implies the dictation overlay should be visible.
        Q_EMIT dictationOverlayRequested();
    } else {
        m_capture->stop();
        m_waveform->stop();
        m_agent->stopDictation();

        // The finished clip is transcribed in one shot now that the user
        // has stopped speaking; the overlay stays up for the result.
        const QByteArray clip = m_clipRecorder->takeWav();
        if (!clip.isEmpty()) {
            qWarning() << "Dictation clip ready:" << clip.size()
                       << "bytes — posting for transcription";
            setTranscribing(true);
            m_transcriber->transcribe(clip);
        } else {
            qWarning() << "Dictation produced no clip (no usable audio was captured)";
            setTranscribing(false);
        }
    }

    m_recording = recording;
    Q_EMIT recordingChanged();
}

void PhoenixController::setTranscribing(bool transcribing)
{
    if (m_transcribing == transcribing) {
        return;
    }
    m_transcribing = transcribing;
    Q_EMIT transcribingChanged();
}

void PhoenixController::setStatusMessage(const QString &message)
{
    if (m_statusMessage == message) {
        return;
    }
    m_statusMessage = message;
    Q_EMIT statusMessageChanged();
}

QString PhoenixController::recordShortcut() const
{
    // Report the live binding rather than the default: it stays truthful
    // after the user re-binds the hotkey, and reads empty when KGlobalAccel
    // could not register anything (e.g. no kglobalacceld on the session bus).
    const QList<QKeySequence> shortcuts = KGlobalAccel::self()->shortcut(m_toggleShortcut);
    for (const QKeySequence &shortcut : shortcuts) {
        if (!shortcut.isEmpty()) {
            return shortcut.toString(QKeySequence::NativeText);
        }
    }
    return QString();
}

QString PhoenixController::searchShortcut() const
{
    // The live binding of the session search, like the record hotkey.
    const QList<QKeySequence> shortcuts = KGlobalAccel::self()->shortcut(m_searchShortcut);
    for (const QKeySequence &shortcut : shortcuts) {
        if (!shortcut.isEmpty()) {
            return shortcut.toString(QKeySequence::NativeText);
        }
    }
    return QString();
}

QString PhoenixController::searchUser() const
{
    // The daemon files sessions per user: root-mode runs belong to
    // "root", everything else (user and web mode) to the local login
    // name. The search follows the mode the widget is in.
    if (m_mode == static_cast<int>(AgentMode::Mode::Root)) {
        return QStringLiteral("root");
    }
    return AgentBridge::localUserName();
}

void PhoenixController::showDictationOverlay()
{
    Q_EMIT dictationOverlayRequested();
}

void PhoenixController::toggleConversationPane()
{
    Q_EMIT conversationPaneRequested();
}

void PhoenixController::showSessionSearch()
{
    Q_EMIT sessionSearchRequested();
}

void PhoenixController::toggleSessionSearch()
{
    // The search button is a toggle: pressing it while the search is
    // up closes it again.
    if (m_sessionSearchOpen) {
        Q_EMIT sessionSearchCloseRequested();
    } else {
        Q_EMIT sessionSearchRequested();
    }
}

void PhoenixController::setSessionSearchOpen(bool open)
{
    if (m_sessionSearchOpen == open) {
        return;
    }
    m_sessionSearchOpen = open;
    Q_EMIT sessionSearchOpenChanged();
}

void PhoenixController::searchSessions(const QString &query)
{
    // The daemon does the matching: it knows the session names and can
    // answer with the sessions that still match the query.
    m_agent->requestSessionSearch(searchUser(), query);
}

void PhoenixController::openSession(const QString &sessionId)
{
    m_agent->requestSessionTranscript(searchUser(), sessionId);
}

void PhoenixController::openSessionResults(const QString &sessionId,
                                           const QString &mode,
                                           const QString &title)
{
    m_agent->openStoredSession(searchUser(), sessionId, mode, title);
}

void PhoenixController::submitText(const QString &text, bool newSession)
{
    if (text.isEmpty()) {
        return;
    }
    // The transcript is consumed: it is now a prompt. Both transcript
    // surfaces (the overlay's editor and the pane's follow-up field) are
    // cleared through partialTranscriptChanged, so the follow-up field is
    // empty — placeholder reading "Ask a follow-up…" — when the answer's
    // pane appears, instead of duplicating the prompt bubble with the
    // query that produced it. Clearing also voids the pending transcript
    // cleanup: the raw text was already submitted, so its cleaned variant
    // no longer has anything to replace (AgentBridge compares the source).
    m_partialTranscript.clear();
    Q_EMIT partialTranscriptChanged();

    // The query is generated and on its way; the pane no longer opens here.
    // It appears by itself when the daemon's answer arrives — see
    // AgentBridge::onRunFinished().
    //
    // The attachments travel with it and are consumed: they belonged to
    // this prompt, not to the next one.
    const QVariantList attachments = m_attachments->toPayload();
    m_attachments->clear();
    m_agent->sendPrompt(text, newSession, attachments);
}

bool PhoenixController::addAttachment(const QUrl &url)
{
    if (m_attachments->addFile(url)) {
        setStatusMessage(QString());
        return true;
    }

    // Tell the user why the file did not make it: an unreadable path, a
    // duplicate, or a file too large to inline.
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const QFileInfo info(path);
    QString reason;
    if (!info.exists() || !info.isFile()) {
        reason = QStringLiteral("no such file");
    } else if (!info.isReadable()) {
        reason = QStringLiteral("not readable");
    } else if (info.size() > PromptAttachments::MaxFileSize) {
        reason = QStringLiteral("larger than %1 MiB")
                     .arg(PromptAttachments::MaxFileSize / (1024 * 1024));
    } else {
        reason = QStringLiteral("already attached");
    }
    setStatusMessage(QStringLiteral("Cannot attach %1: %2").arg(info.fileName(), reason));
    return false;
}

void PhoenixController::removeAttachment(int index)
{
    m_attachments->remove(index);
}

void PhoenixController::startScreenshot()
{
    m_screenshotPicker->start();
}

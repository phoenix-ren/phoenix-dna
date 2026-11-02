// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
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
#include <QKeySequence>
#include <cstdio>

PhoenixController::PhoenixController(QObject *parent)
    : QObject(parent),
      m_waveform(new AudioRingBuffer(this)),
      m_agent(new AgentBridge(this)),
      m_capture(new AudioCapture(this)),
      m_clipRecorder(new ClipRecorder(this)),
      m_transcriber(new Transcriber(this))
{
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
    // the overlay explains that in its placeholder.
    connect(m_transcriber, &Transcriber::finished, this, [this](const QString &text) {
        setTranscribing(false);
        m_partialTranscript = text;
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
    m_toggleShortcut->setProperty("componentName", QStringLiteral("org.phoenixai.agent"));
    m_toggleShortcut->setProperty("componentDisplayName", QStringLiteral("Phoenix AI"));
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

void PhoenixController::showDictationOverlay()
{
    Q_EMIT dictationOverlayRequested();
}

void PhoenixController::toggleConversationPane()
{
    Q_EMIT conversationPaneRequested();
}

void PhoenixController::submitText(const QString &text)
{
    if (text.isEmpty()) {
        return;
    }
    // The query is generated and on its way; the pane no longer opens here.
    // It appears by itself when the daemon's answer arrives — see
    // AgentBridge::onRunFinished().
    m_agent->sendPrompt(text);
}

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>

#include "agentmode.h"

class QAction;
class AudioRingBuffer;
class AgentBridge;
class AudioCapture;
class ClipRecorder;
class Transcriber;

/*!
 * \brief The widget's controller, exposed to QML as a singleton.
 *
 * This is the bridge between the C++ side and the QML side. The applet itself
 * is created by Plasma, so the state that the QML needs lives here instead:
 * the capture state, the ring-buffered waveform and the agent connection.
 *
 * Registered under the applet's QML module, so QML reaches it as:
 * \code
 * import plasma.applet.org.phoenixdna.agent as Phoenix
 * Phoenix.PhoenixController.recording
 * \endcode
 */
class PhoenixController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    Q_PROPERTY(AudioRingBuffer *waveform READ waveform CONSTANT)
    Q_PROPERTY(AgentBridge *agent READ agent CONSTANT)
    Q_PROPERTY(QString partialTranscript READ partialTranscript NOTIFY partialTranscriptChanged)
    Q_PROPERTY(bool transcribing READ isTranscribing NOTIFY transcribingChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QString recordShortcut READ recordShortcut NOTIFY recordShortcutChanged)

public:
    explicit PhoenixController(QObject *parent = nullptr);
    ~PhoenixController() override;

    /*! \brief Creates the singleton. Called by the QML engine. */
    static PhoenixController *create(QQmlEngine *engine, QJSEngine *);

    int mode() const { return m_mode; }
    void setMode(int mode);

    bool isRecording() const { return m_recording; }
    AudioRingBuffer *waveform() const { return m_waveform; }
    AgentBridge *agent() const { return m_agent; }
    QString partialTranscript() const { return m_partialTranscript; }

    /*! \brief Whether the finished clip is with the transcription service. */
    bool isTranscribing() const { return m_transcribing; }

    /*! \brief The last non-fatal status line (e.g. a transcription error). */
    QString statusMessage() const { return m_statusMessage; }

    /*! \brief The bound global record hotkey ("Meta+Alt+Space" unless the
     *  user re-bound it in System Settings). Empty when none is bound. */
    QString recordShortcut() const;

    /*! \brief The icon representing the given mode. */
    Q_INVOKABLE QString modeIconName(int mode) const;

    /*! \brief A human-readable name for the given mode. */
    Q_INVOKABLE QString modeDisplayName(int mode) const;

    /*! \brief Toggles capture. Bound to the record button. */
    Q_INVOKABLE void toggleRecording();

    /*! \brief Shows the dictation overlay. */
    Q_INVOKABLE void showDictationOverlay();

    /*! \brief Shows or hides the conversation pane. */
    Q_INVOKABLE void toggleConversationPane();

    /*! \brief Sends the given text to the agent as if it had been dictated. */
    Q_INVOKABLE void submitText(const QString &text);

Q_SIGNALS:
    void modeChanged();
    void recordingChanged();
    void partialTranscriptChanged();
    void transcribingChanged();
    void statusMessageChanged();
    void recordShortcutChanged();

    /*! \brief Emitted when the dictation overlay should be shown. */
    void dictationOverlayRequested();

    /*! \brief Emitted when the conversation pane should be toggled. */
    void conversationPaneRequested();

private:
    void setRecording(bool recording);
    void setTranscribing(bool transcribing);
    void setStatusMessage(const QString &message);

    AudioRingBuffer *m_waveform = nullptr;
    AgentBridge *m_agent = nullptr;
    AudioCapture *m_capture = nullptr;
    ClipRecorder *m_clipRecorder = nullptr;
    Transcriber *m_transcriber = nullptr;
    QAction *m_toggleShortcut = nullptr;
    bool m_recording = false;
    bool m_transcribing = false;
    int m_mode = static_cast<int>(AgentMode::Mode::User);
    QString m_partialTranscript;
    QString m_statusMessage;
};

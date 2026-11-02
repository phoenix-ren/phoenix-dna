// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>

#include "agentmode.h"
#include "promptattachments.h"
#include "settingsstore.h"
#include "windowscreenshotpicker.h"

#include <QUrl>

class QAction;
class AudioRingBuffer;
class AgentBridge;
class AudioCapture;
class ClipRecorder;
class Transcriber;
class ShutterSound;

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
    Q_PROPERTY(QString searchShortcut READ searchShortcut NOTIFY searchShortcutChanged)
    Q_PROPERTY(QString promptShortcut READ promptShortcut NOTIFY promptShortcutChanged)

    /*! \brief Whether the session search is on screen.
     *
     * The panel's search button shows it as pressed while it is, so the
     * button is a toggle: pressing it opens the search, pressing it
     * again closes it. */
    Q_PROPERTY(bool sessionSearchOpen READ sessionSearchOpen NOTIFY sessionSearchOpenChanged)

    /*! \brief The user whose session history the search browses.
     *
     * "root" in root mode (the daemon files root sessions under their
     * own database), the local login name otherwise. */
    Q_PROPERTY(QString searchUser READ searchUser NOTIFY searchUserChanged)

    /*! \brief The files attached to the prompt that is being composed.
     *
     * Shared by every prompt surface (the dictation overlay, the results
     * pane's follow-up field), so a file added in one of them travels with
     * the prompt the next one submits. */
    Q_PROPERTY(PromptAttachments *attachments READ attachments CONSTANT)

    /*! \brief The picker that photographs a window of the desktop. */
    Q_PROPERTY(WindowScreenshotPicker *screenshotPicker READ screenshotPicker CONSTANT)

    /*! \brief The user-level settings: API keys and global shortcuts.
     *
     * What the settings window edits: the LLM and speech-to-text keys in
     * ~/.config/phoenix-dna/keys.json, and the re-binding of the three
     * system-wide hotkeys through KGlobalAccel. */
    Q_PROPERTY(SettingsStore *settings READ settings CONSTANT)

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

    /*! \brief The bound global session-search hotkey ("Meta+Alt+`" unless
     *  the user re-bound it in System Settings). Empty when none is
     *  bound. */
    QString searchShortcut() const;

    /*! \brief The bound global hotkey that opens the prompt editor
     *  ("Meta+Alt+Return" unless the user re-bound it in System
     *  Settings). Empty when none is bound. */
    QString promptShortcut() const;

    /*! \brief The user whose sessions the search lists. */
    QString searchUser() const;

    /*! \brief Whether the session search is on screen. */
    bool sessionSearchOpen() const { return m_sessionSearchOpen; }

    /*! \brief The files attached to the prompt being composed. */
    PromptAttachments *attachments() const { return m_attachments; }

    /*! \brief Attaches the file behind \a url to the prompt.
     *
     * Returns false when the file cannot be attached (not a local file, not
     * readable, already attached or too large); the reason is reported as a
     * status message. */
    Q_INVOKABLE bool addAttachment(const QUrl &url);

    /*! \brief Removes the attachment at \a index from the prompt. */
    Q_INVOKABLE void removeAttachment(int index);

    /*! \brief The picker that photographs a window of the desktop. */
    WindowScreenshotPicker *screenshotPicker() const { return m_screenshotPicker; }

    /*! \brief The user-level settings store of the settings window. */
    SettingsStore *settings() const { return m_settings; }

    /*! \brief Starts the window picker: the user clicks a window of the
     *  desktop and its picture is attached to the prompt.
     *
     * The same thing the global shortcut does; the buttons call it so the
     * feature is discoverable. */
    Q_INVOKABLE void startScreenshot();

    /*! \brief The icon representing the given mode. */
    Q_INVOKABLE QString modeIconName(int mode) const;

    /*! \brief A human-readable name for the given mode. */
    Q_INVOKABLE QString modeDisplayName(int mode) const;

    /*! \brief Toggles capture. Bound to the record button. */
    Q_INVOKABLE void toggleRecording();

    /*! \brief Shows the dictation overlay. */
    Q_INVOKABLE void showDictationOverlay();

    /*! \brief Opens the prompt editor: the query is typed, not dictated.
     *
     * The same overlay the microphone opens, but with nothing being
     * recorded: the editor waits for the keyboard and the query is sent
     * like a dictated one. This is what the panel's keyboard button and
     * the global hotkey do.
     *
     * A dictation in flight owns the overlay (the editor is locked while
     * the transcript arrives), so the request is ignored while the
     * microphone is open or the clip is being transcribed. */
    Q_INVOKABLE void showPromptEditor();

    /*! \brief Shows or hides the conversation pane. */
    Q_INVOKABLE void toggleConversationPane();

    /*! \brief Shows the session search overlay.
     *
     * The overlay lists the user's previous sessions on the left and the
     * selected session's queries and responses on the right. */
    Q_INVOKABLE void showSessionSearch();

    /*! \brief Opens the session search, or closes it when it is open.
     *
     * What the panel's search button (and its global hotkey) do. */
    Q_INVOKABLE void toggleSessionSearch();

    /*! \brief Remembers whether the session search is on screen.
     *
     * The shell tells the controller when the overlay opened or closed,
     * so the panel's search button can show it as pressed. */
    Q_INVOKABLE void setSessionSearchOpen(bool open);

    /*! \brief Searches the user's sessions for a query.
     *
     * The daemon matches the query fuzzily against the sessions' summary
     * names and answers with the sessions that still match; the search
     * overlay lists them. */
    Q_INVOKABLE void searchSessions(const QString &query);

    /*! \brief Shows the queries and responses of one session. */
    Q_INVOKABLE void openSession(const QString &sessionId);

    /*! \brief Opens a stored session in the results pane.
     *
     * The session's history is loaded into the conversation model and
     * the pane is opened on it. The widget follows that session from
     * then on, so a follow-on typed in the pane continues that very
     * conversation.
     *
     * \a mode is the session's mode ("root" or "user"), which
     * names the sender of its queries, and \a title the name the pane's
     * header shows. */
    Q_INVOKABLE void openSessionResults(const QString &sessionId,
                                        const QString &mode,
                                        const QString &title);

    /*! \brief Sends the given text to the agent.
     *
     * With \a newSession (record button on the desktop pane: the query
     * starts a fresh conversation) the daemon opens a new session and the
     * pane's history restarts; without it the query continues the
     * conversation on screen (follow-ons from the pane's input bar or its
     * record button). */
    Q_INVOKABLE void submitText(const QString &text, bool newSession);

Q_SIGNALS:
    void modeChanged();
    void recordingChanged();
    void partialTranscriptChanged();
    void transcribingChanged();
    void statusMessageChanged();
    void recordShortcutChanged();
    void searchShortcutChanged();
    void promptShortcutChanged();
    void searchUserChanged();
    void sessionSearchOpenChanged();

    /*! \brief Emitted when the dictation overlay should be shown. */
    void dictationOverlayRequested();

    /*! \brief Emitted when the prompt editor should be shown.
     *
     * The typed counterpart of dictationOverlayRequested: the query is
     * written instead of spoken, and no recording takes place. */
    void promptEditorRequested();

    /*! \brief Emitted when the conversation pane should be toggled. */
    void conversationPaneRequested();

    /*! \brief Emitted when the session search should be shown. */
    void sessionSearchRequested();

    /*! \brief Emitted when the session search should be closed. */
    void sessionSearchCloseRequested();

private:
    void setRecording(bool recording);
    void setTranscribing(bool transcribing);
    void setStatusMessage(const QString &message);

    AudioRingBuffer *m_waveform = nullptr;
    AgentBridge *m_agent = nullptr;
    AudioCapture *m_capture = nullptr;
    ClipRecorder *m_clipRecorder = nullptr;
    Transcriber *m_transcriber = nullptr;
    PromptAttachments *m_attachments = nullptr;
    WindowScreenshotPicker *m_screenshotPicker = nullptr;
    ShutterSound *m_shutterSound = nullptr;
    SettingsStore *m_settings = nullptr;
    QAction *m_toggleShortcut = nullptr;
    QAction *m_searchShortcut = nullptr;
    QAction *m_promptShortcut = nullptr;
    bool m_recording = false;
    bool m_transcribing = false;
    // Whether the session search is on screen: the panel's search
    // button is a toggle, and this is its state.
    bool m_sessionSearchOpen = false;
    int m_mode = static_cast<int>(AgentMode::Mode::User);
    QString m_partialTranscript;
    QString m_statusMessage;
};

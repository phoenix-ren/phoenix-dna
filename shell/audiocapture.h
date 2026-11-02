// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>

/*!
 * \brief Captures microphone audio by running a short-lived helper process.
 *
 * The plasmoid is a plugin loaded into plasmashell: anything that faults
 * inside this library takes down the whole desktop session. Capture therefore
 * happens in a separate pw-cat process (PipeWire ships it as part of
 * pipewire-utils), and this class only talks to it over pipes. If the helper
 * dies, the desktop is unaffected.
 *
 * pw-cat is started in raw passthrough mode. That mode does not allow the
 * sample format to be forced, so the negotiated format is read from the
 * helper's verbose output ("-v" logs a "raw: rate=… channels=… fmt=…" line)
 * and every frame is converted to signed 16-bit PCM before being published
 * through pcmReady().
 */
class AudioCapture : public QObject
{
    Q_OBJECT

public:
    explicit AudioCapture(QObject *parent = nullptr);
    ~AudioCapture() override;

    /*! \brief Whether the helper is currently capturing. */
    bool isActive() const { return m_active; }

    /*! \brief Starts capturing from the default microphone source. */
    bool start();

    /*! \brief Stops the capture helper. */
    void stop();

Q_SIGNALS:
    /*!
     * \brief Emitted for every block of captured audio.
     *
     * \a pcm is signed 16-bit little-endian, interleaved, \a channels wide.
     */
    void pcmReady(const QByteArray &pcm, int sampleRate, int channels);

    /*! \brief Emitted when the helper could not start or died unexpectedly. */
    void captureFailed(const QString &message);

private Q_SLOTS:
    void readStdout();
    void readStderr();
    void onFinished(int exitCode, QProcess::ExitStatus status);

private:
    void publish(const QByteArray &frames);
    bool negotiate(const QString &line);
    void shutdown(bool wait);

    QProcess m_process;
    bool m_active = false;

    // The format negotiated by the helper, read from its verbose output.
    QString m_format = QStringLiteral("s16");
    int m_sampleRate = 48000;
    int m_channels = 2;

    // Bytes of an incomplete frame carried over to the next read, so the
    // published buffer always contains whole frames.
    QByteArray m_carry;
};

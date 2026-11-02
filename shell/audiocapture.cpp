// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "audiocapture.h"

#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QtEndian>

namespace
{

constexpr int kMaxCarry = 8192;

int bytesPerSample(const QString &format)
{
    if (format == QLatin1String("s8") || format == QLatin1String("u8")) {
        return 1;
    }
    if (format == QLatin1String("s16")) {
        return 2;
    }
    if (format == QLatin1String("s24")) {
        return 3;
    }
    if (format == QLatin1String("s32") || format == QLatin1String("f32") ||
        format == QLatin1String("s24le32")) {
        return 4;
    }
    if (format == QLatin1String("f64")) {
        return 8;
    }
    return 0;
}

/*! \brief Decodes one little-endian sample in the given format, advancing \a p. */
qint16 decodeSample(const QString &format, const uchar *&p)
{
    qint16 value = 0;
    if (format == QLatin1String("s16")) {
        value = qFromLittleEndian<qint16>(p);
        p += 2;
    } else if (format == QLatin1String("s24")) {
        // The sign bit of the third byte lands in the int's sign position,
        // so the arithmetic right shift sign-extends correctly.
        const int raw = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
        value = static_cast<qint16>(raw >> 16);
        p += 3;
    } else if (format == QLatin1String("s24le32")) {
        value = static_cast<qint16>(qFromLittleEndian<qint32>(p) >> 16);
        p += 4;
    } else if (format == QLatin1String("s32")) {
        value = static_cast<qint16>(qFromLittleEndian<qint32>(p) >> 16);
        p += 4;
    } else if (format == QLatin1String("f32")) {
        const float f = qFromLittleEndian<float>(p);
        value = static_cast<qint16>(qBound(-1.0f, f, 1.0f) * 32767.0f);
        p += 4;
    } else if (format == QLatin1String("f64")) {
        const double d = qFromLittleEndian<double>(p);
        value = static_cast<qint16>(qBound(-1.0, d, 1.0) * 32767.0);
        p += 8;
    } else if (format == QLatin1String("s8")) {
        value = static_cast<qint16>(static_cast<int>(*p) << 8);
        p += 1;
    } else {
        // u8 and anything unknown: treat as silence, advance one byte.
        value = static_cast<qint16>((static_cast<int>(*p) - 128) << 8);
        p += 1;
    }
    return value;
}

} // namespace

AudioCapture::AudioCapture(QObject *parent) : QObject(parent)
{
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &AudioCapture::readStdout);
    connect(&m_process, &QProcess::readyReadStandardError, this, &AudioCapture::readStderr);
    connect(&m_process, &QProcess::finished, this, &AudioCapture::onFinished);
}

AudioCapture::~AudioCapture()
{
    shutdown(/*wait=*/true);
}

bool AudioCapture::start()
{
    if (m_active) {
        return true;
    }

    // A stop() issued moments ago is asynchronous: SIGTERM goes out but the
    // old helper may still be exiting. QProcess refuses to start a second
    // program while one is running, so reap the previous helper first.
    // pw-cat exits within a few ms of SIGTERM, so this wait returns at once.
    if (m_process.state() != QProcess::NotRunning) {
        shutdown(/*wait=*/true);
    }

    const QString program = QStandardPaths::findExecutable(QStringLiteral("pw-cat"));
    if (program.isEmpty()) {
        Q_EMIT captureFailed(QStringLiteral("pw-cat (pipewire-utils) is not installed"));
        return false;
    }

    // "pw-cat -v --record --raw -" captures the default microphone in its
    // raw, negotiated format. Conversion to 16-bit happens in publish().
    QStringList arguments{
        QStringLiteral("-v"),
        QStringLiteral("--record"),
        QStringLiteral("--raw"),
        QStringLiteral("-"),
    };

    // A specific source can be requested through the environment; by default
    // the user's default microphone is captured.
    const QString target = qEnvironmentVariable("PHOENIX_AUDIO_TARGET");
    if (!target.isEmpty()) {
        arguments << QStringLiteral("--target") << target;
    }

    // The negotiated format is unknown until pw-cat reports it on stderr;
    // readStdout() holds bytes in the carry buffer until then. Guessing s16
    // here would misparse the first chunk of an s32 source into garbage
    // (a tall spike at bucket 0).
    m_format.clear();
    m_sampleRate = 48000;
    m_channels = 2;
    m_carry.clear();

    m_process.setProgram(program);
    m_process.setArguments(arguments);
    m_process.start();
    if (!m_process.waitForStarted(2000)) {
        Q_EMIT captureFailed(
            QStringLiteral("Failed to start pw-cat: %1").arg(m_process.errorString()));
        return false;
    }

    m_active = true;
    return true;
}

void AudioCapture::stop()
{
    if (!m_active) {
        return;
    }
    m_active = false;

    // Ask politely first; pw-cat exits promptly on SIGTERM. The kill timer
    // only fires if it ignored the request, and nothing here blocks the
    // plasmashell UI thread.
    m_process.terminate();
    QTimer::singleShot(500, this, [this]() {
        if (m_process.state() != QProcess::NotRunning) {
            m_process.kill();
        }
    });
}

void AudioCapture::shutdown(bool wait)
{
    if (m_process.state() == QProcess::NotRunning) {
        return;
    }
    m_process.terminate();
    if (!wait || !m_process.waitForFinished(300)) {
        m_process.kill();
        if (wait) {
            m_process.waitForFinished(300);
        }
    }
}

void AudioCapture::readStdout()
{
    if (!m_active) {
        return;
    }
    m_carry += m_process.readAllStandardOutput();

    const int frameSize = bytesPerSample(m_format) * m_channels;
    if (frameSize <= 0) {
        return;
    }

    const int complete = m_carry.size() - (m_carry.size() % frameSize);
    if (complete <= 0) {
        return;
    }

    publish(m_carry.left(complete));
    m_carry = m_carry.mid(complete);
    if (m_carry.size() > kMaxCarry) {
        // Should be unreachable while frame sizes are sane; keep memory
        // bounded anyway.
        m_carry.clear();
    }
}

void AudioCapture::readStderr()
{
    // Must use readAllStandardError(): QProcess's plain QIODevice read API
    // (readLine/readAll) operates on the stdout channel, so reading "lines"
    // here would consume raw audio bytes instead of pw-cat's diagnostics,
    // and the format negotiation would never complete.
    const QList<QByteArray> lines = m_process.readAllStandardError().split('\n');
    for (const QByteArray &line : lines) {
        if (negotiate(QString::fromLocal8Bit(line))) {
            return;
        }
    }
}

bool AudioCapture::negotiate(const QString &line)
{
    // pw-cat -v logs e.g.:
    //   raw: rate=48000 channels=2 fmt=s16 samplesize=2 stride=4
    static const QRegularExpression pattern(
        QStringLiteral("raw: rate=(\\d+) channels=(\\d+) fmt=(\\S+)"));
    const auto match = pattern.match(line);
    if (!match.hasMatch()) {
        return false;
    }

    m_sampleRate = match.captured(1).toInt();
    m_channels = qMax(1, match.captured(2).toInt());
    const QString format = match.captured(3);
    if (bytesPerSample(format) == 0) {
        m_active = false;
        Q_EMIT captureFailed(QStringLiteral("Unsupported audio format '%1'").arg(format));
        shutdown(true);
        return true;
    }
    m_format = format;
    return true;
}

void AudioCapture::publish(const QByteArray &frames)
{
    if (m_format == QLatin1String("s16")) {
        // Already the representation the ring buffer expects.
        Q_EMIT pcmReady(frames, m_sampleRate, m_channels);
        return;
    }

    QByteArray pcm;
    pcm.resize(frames.size() / bytesPerSample(m_format) * static_cast<int>(sizeof(qint16)));
    auto *out = reinterpret_cast<qint16 *>(pcm.data());
    const auto *src = reinterpret_cast<const uchar *>(frames.constData());
    const auto *const end = src + frames.size();
    while (src < end) {
        *out++ = decodeSample(m_format, src);
    }
    Q_EMIT pcmReady(pcm, m_sampleRate, m_channels);
}

void AudioCapture::onFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_active) {
        m_active = false;
        if (status == QProcess::CrashExit) {
            Q_EMIT captureFailed(QStringLiteral("pw-cat crashed (exit code %1)").arg(exitCode));
        }
    }
}

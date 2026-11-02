// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

// A check of the screenshot path.
//
// It asks KWin for the picture of the active window over D-Bus, reads
// the picture out of the pipe exactly as the picker does and writes it
// to a PNG:
//
//   screenshottest [output.png]
//   screenshottest --uuid <uuid> [output.png]
//
// The second form photographs the window with the given handle instead
// of the active one.
//
// The point of the tool is the reading of the pipe. KWin answers its
// call *before* it writes the picture, and the picture is far bigger
// than the pipe: it arrives in many pieces over many turns of the
// event loop. A picture that is looked at before it is complete sends
// the decoder past the end of the buffer - which is what used to crash
// plasmashell.

#include "windowpicture.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QImage>
#include <QSocketNotifier>
#include <QTimer>
#include <QVariantMap>

#include <cerrno>
#include <cstdio>
#include <unistd.h>

namespace
{
//: KWin's screenshot service.
const QString kScreenshotService = QStringLiteral("org.kde.KWin.ScreenShot2");
const QString kScreenshotPath = QStringLiteral("/org/kde/KWin/ScreenShot2");
const QString kScreenshotInterface = QStringLiteral("org.kde.KWin.ScreenShot2");

//: How much is read from the pipe at a time.
const int kChunkSize = 64 * 1024;
} // namespace

class ScreenShotTest : public QObject
{
    Q_OBJECT

public:
    explicit ScreenShotTest(const QString &output, const QString &uuid)
        : QObject(nullptr), m_output(output)
    {
        QVariantMap options;
        options.insert(QStringLiteral("include-decoration"), false);
        options.insert(QStringLiteral("include-shadow"), false);

        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0) {
            qFatal("cannot create the pipe for the picture");
        }

        QDBusUnixFileDescriptor descriptor(fds[1]);
        ::close(fds[1]);

        // With a handle the window itself is photographed, without one
        // the window that is active.
        const bool byHandle = !uuid.isEmpty();
        QDBusMessage message =
            QDBusMessage::createMethodCall(kScreenshotService, kScreenshotPath, kScreenshotInterface,
                                           byHandle ? QStringLiteral("CaptureWindow")
                                                    : QStringLiteral("CaptureActiveWindow"));
        if (byHandle) {
            message << uuid;
        }
        message << options << QVariant::fromValue(descriptor);

        m_pipeFd = fds[0];
        m_notifier = new QSocketNotifier(m_pipeFd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, &ScreenShotTest::readPicture);

        auto *watcher =
            new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, &ScreenShotTest::onAnswer);

        // The picture must be there within a minute; KWin takes it in
        // the compositor.
        QTimer::singleShot(60000, this, [this] {
            qWarning() << "the picture did not arrive in time";
            QCoreApplication::exit(2);
        });
    }

private Q_SLOTS:
    void onAnswer(QDBusPendingCallWatcher *watcher)
    {
        watcher->deleteLater();

        const QDBusReply<QVariantMap> reply = *watcher;
        if (!reply.isValid()) {
            qWarning() << "KWin refused the screenshot:" << watcher->error().name()
                       << watcher->error().message();
            QCoreApplication::exit(3);
            return;
        }

        m_info = reply.value();
        printf("answer: width=%d height=%d stride=%d format=%u scale=%f\n",
               m_info.value(QStringLiteral("width")).toInt(),
               m_info.value(QStringLiteral("height")).toInt(),
               m_info.value(QStringLiteral("stride")).toInt(),
               m_info.value(QStringLiteral("format")).toUInt(),
               m_info.value(QStringLiteral("scale")).toDouble());
        printf("the answer asks for %lld bytes\n", (long long)WindowPicture::size(m_info));

        // The picture is written after the answer: it is not there yet.
        printf("the picture is complete when the answer arrives: %s\n",
               WindowPicture::isComplete(m_info, m_picture) ? "yes" : "no");
    }

    void readPicture()
    {
        char buffer[kChunkSize];
        const ssize_t count = ::read(m_pipeFd, buffer, sizeof(buffer));
        if (count > 0) {
            m_picture.append(buffer, int(count));
            return;
        }
        if (count < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                return;
            }
            qWarning() << "cannot read the picture from the pipe";
            QCoreApplication::exit(4);
            return;
        }

        // The end of the pipe: the picture is complete.
        if (m_notifier) {
            m_notifier->setEnabled(false);
            delete m_notifier;
            m_notifier = nullptr;
        }
        ::close(m_pipeFd);
        m_pipeFd = -1;

        printf("read %lld bytes\n", (long long)m_picture.size());

        QImage image;
        if (!WindowPicture::decode(m_info, m_picture, image)) {
            qWarning() << "the picture could not be decoded";
            QCoreApplication::exit(5);
            return;
        }

        printf("decoded: %dx%d\n", image.width(), image.height());
        if (!image.save(m_output, "PNG")) {
            qWarning() << "the picture could not be written to" << m_output;
            QCoreApplication::exit(6);
            return;
        }
        printf("written to %s\n", qPrintable(m_output));
        QCoreApplication::exit(0);
    }

private:
    QString m_output;
    QVariantMap m_info;
    QByteArray m_picture;
    QSocketNotifier *m_notifier = nullptr;
    int m_pipeFd = -1;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QString output;
    QString uuid;
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == QStringLiteral("--uuid") && i + 1 < argc) {
            uuid = QString::fromLocal8Bit(argv[++i]);
        } else if (output.isEmpty()) {
            output = argument;
        }
    }
    if (output.isEmpty()) {
        output = QStringLiteral("screenshottest.png");
    }

    ScreenShotTest test(output, uuid);
    return app.exec();
}

#include "screenshottest.moc"

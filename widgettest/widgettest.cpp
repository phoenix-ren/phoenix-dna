// SPDX-FileCopyrightText: 2026 Phoenix AI contributors
// SPDX-License-Identifier: BSD-2-Clause

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>
#include <QPluginLoader>
#include <QString>
#include <QTimer>
#include <cmath>
#include <cstdio>

#include "waveformbuffer.h"
#include "waveformitem.h"
#include "wavfeed.h"
#include "wavreader.h"

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("widgettest"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Testbench for the Phoenix AI panel widget, hosting the real applet "
        "plugin from the build tree, plus the buffered waveform component "
        "(design_docs/WAVEFORM_COMPONENT_2.md)."));
    parser.addHelpOption();
    QCommandLineOption speedOption(
        QStringLiteral("speed"),
        QStringLiteral("Waveform feed speed relative to realtime, e.g. 4 (default: 1)."),
        QStringLiteral("factor"));
    QCommandLineOption secondsOption(
        QStringLiteral("seconds"),
        QStringLiteral("Quit automatically after this many seconds of wall clock."),
        QStringLiteral("s"));
    QCommandLineOption grabOption(
        QStringLiteral("grab"),
        QStringLiteral("Before quitting (see --seconds), save the window to this .png."),
        QStringLiteral("path"));
    QCommandLineOption recordOption(
        QStringLiteral("record"),
        QStringLiteral("Toggle recording on at startup (the real pw-cat capture "
                       "path), as if the record button had been clicked. Pair with "
                       "PHOENIX_AUDIO_TARGET to choose a source."));
    QCommandLineOption stopAfterOption(
        QStringLiteral("stop-after"),
        QStringLiteral("With --record: toggle recording off again after this many "
                       "seconds (exercises the stop path)."),
        QStringLiteral("s"));
    parser.addOptions({speedOption, secondsOption, grabOption, recordOption, stopAfterOption});
    parser.addPositionalArgument(
        QStringLiteral("wav"),
        QStringLiteral("Optional .wav file: fed to the waveform as realtime "
                       "25 ms buffers on a feed thread, looping. Without it "
                       "only the applet itself is shown."));
    parser.process(app);

    qreal speed = 1.0;
    if (parser.isSet(speedOption)) {
        bool ok = false;
        speed = parser.value(speedOption).toDouble(&ok);
        if (!ok || speed <= 0.0 || speed > 100.0) {
            std::fprintf(stderr, "widgettest: --speed must be a number between 0 (exclusive) and 100\n");
            return 2;
        }
    }

    // Load the applet plugin straight from the build tree: this registers the
    // QML module (controller singleton, mode enum, both representations)
    // exactly as plasmashell does, without installing anything. The widget
    // therefore runs its real capture path — record button included.
    QPluginLoader loader(QStringLiteral(PHOENIX_APPLET_PLUGIN));
    if (!loader.load()) {
        qWarning("cannot load applet plugin %s:\n  %s",
                 PHOENIX_APPLET_PLUGIN, qPrintable(loader.errorString()));
        return 1;
    }

    // The buffered waveform component: the buffer lives in C++ (the feed
    // thread appends to it), the item is registered for QML below.
    const QString wavPath = parser.positionalArguments().value(0);
    WavData wav;
    QString wavError;
    const bool feeding = !wavPath.isEmpty();
    if (feeding && !WavReader::read(wavPath, &wav, &wavError)) {
        std::fprintf(stderr, "widgettest: %s: %s\n", qPrintable(wavPath), qPrintable(wavError));
        return 1;
    }

    auto *waveBuffer = new WaveformBuffer(&app);
    WavFeedThread *feed = nullptr;
    if (feeding) {
        // Everything downstream of capture is 16-bit PCM, so the feed
        // converts the parsed file the way audiocapture would.
        QByteArray pcm;
        pcm.resize(int(wav.mono.size()) * 2);
        for (qsizetype i = 0; i < wav.mono.size(); ++i) {
            const double clamped = qBound(-1.0, wav.mono[i], 1.0);
            const qint16 sample = qint16(std::lround(clamped * 32767.0));
            pcm[int(i * 2)] = char(quint16(sample) & 0xff);
            pcm[int(i * 2 + 1)] = char(quint16(sample) >> 8);
        }
        waveBuffer->start();
        feed = new WavFeedThread(waveBuffer, std::move(pcm), wav.sampleRate, speed, &app);
    }

    qmlRegisterType<WaveformItem>("Phoenix.Waveform", 1, 0, "WaveformItem");

    QQuickView view;
    view.engine()->addImportPath(QStringLiteral("/usr/lib64/qt6/qml"));
    view.engine()->rootContext()->setContextProperty(QStringLiteral("waveBuffer"), waveBuffer);
    view.engine()->rootContext()->setContextProperty(QStringLiteral("waveFeedRunning"), feeding);
    view.engine()->rootContext()->setContextProperty(
        QStringLiteral("waveAutoRecord"), parser.isSet(recordOption));
    qreal stopAfter = 0.0;
    if (parser.isSet(stopAfterOption)) {
        bool ok = false;
        stopAfter = parser.value(stopAfterOption).toDouble(&ok);
        if (!ok || stopAfter <= 0.0) {
            std::fprintf(stderr, "widgettest: --stop-after must be a positive number\n");
            return 2;
        }
    }
    view.engine()->rootContext()->setContextProperty(
        QStringLiteral("waveAutoStopSeconds"), stopAfter);
    view.setTitle(QStringLiteral("Phoenix AI — widget testbench"));
    // The root Rectangle has no implicit size of its own: make the view size
    // the root object, or the content collapses to zero.
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.setSource(QUrl(QStringLiteral("qrc:/widgettest/qml/main.qml")));
    for (const auto &error : view.errors()) {
        fprintf(stderr, "QML error: %s\n", qPrintable(error.toString()));
    }
    if (view.status() == QQuickView::Error) {
        return 2;
    }

    // Panel-proportioned: 88 px hosts the applet alone; a fed waveform opens
    // a second row underneath it.
    view.resize(480, feeding ? 300 : 88);
    view.show();

    if (feed) {
        feed->start();
    }

    if (parser.isSet(secondsOption)) {
        bool ok = false;
        const qreal seconds = parser.value(secondsOption).toDouble(&ok);
        if (!ok || seconds <= 0.0) {
            std::fprintf(stderr, "widgettest: --seconds must be a positive number\n");
            return 2;
        }
        QTimer::singleShot(int(seconds * 1000), &app, [&view, &parser, grabOption]() {
            if (parser.isSet(grabOption)) {
                const QImage grabbed = view.grabWindow();
                if (!grabbed.save(parser.value(grabOption), "png")) {
                    std::fprintf(stderr, "widgettest: cannot save grab to %s\n",
                                 qPrintable(parser.value(grabOption)));
                }
            }
            QCoreApplication::quit();
        });
    }

    const int result = app.exec();

    if (feed) {
        feed->stopFeed();
        feed->wait();
    }
    return result;
}

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

// The rate of the conversation with the model — the moving average over
// the bytes that crossed the wire — checked against known traffic:
// a steady stream reads its own rate, a burst decays away over the
// window, the two directions are kept apart, and the peaks are the
// reference the bars are scaled against.
//
//   wiremetertest
//
// Exits non-zero when a check fails. (Plain printf, not qInfo: the
// categorized logging prints nothing in this environment.)

#include "wiremeter.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

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

// Waits \a ms, so the meter's clocks see the time pass - and lets the
// event loop run, so the meter's own tick fires (that is what ages the
// averages when no bytes arrive).
void wait(int ms)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ---- A steady stream reads its own rate -------------------------
    {
        WireMeter meter;
        meter.beginTurn(1);
        // 1 kB/s, in 100-byte packets every 100 ms. The window is three
        // seconds, so the average needs a few windows to come up to the
        // rate: it is an exponential approach, not a step.
        for (int i = 0; i < 120; ++i) {
            meter.addTraffic(1, 0, 100);
            wait(100);
        }
        const qreal rate = meter.receiveRate();
        check(rate > 900.0 && rate <= 1000.0, "a steady 1 kB/s stream reads about 1 kB/s");
        check(meter.receiveFraction() > 0.9 && meter.receiveFraction() <= 1.0,
              "the bar of a steady stream sits at its peak");
    }

    // ---- A burst decays away ----------------------------------------
    {
        WireMeter meter;
        meter.beginTurn(1);
        meter.addTraffic(1, 0, 10000);
        const qreal peak = meter.receiveRate();
        check(peak > 0.0, "a burst is metered");
        // The window is a time constant, so the burst fades over a few
        // of them: after five seconds it is a tenth of what it was.
        wait(5000);
        meter.addTraffic(1, 0, 0);
        check(meter.receiveRate() < peak * 0.2,
              "the rate of a burst decays away within the window");
        check(meter.receivePeak() >= peak, "the peak remembers the burst: it is the bar's scale");
    }

    // ---- The two directions are kept apart ---------------------------
    {
        WireMeter meter;
        meter.beginTurn(1);
        meter.addTraffic(1, 5000, 100);
        check(meter.sendRate() > meter.receiveRate() * 10,
              "a request that goes out does not show up as an answer");
        check(meter.sendPeak() > 0.0 && meter.receivePeak() > 0.0,
              "each direction keeps its own peak");
    }

    // ---- A new turn starts the count afresh -------------------------
    {
        WireMeter meter;
        meter.beginTurn(1);
        meter.addTraffic(1, 0, 1000);
        const qreal first = meter.receiveRate();
        check(first > 0.0, "the first turn is metered");
        meter.beginTurn(2);
        check(meter.receiveRate() == 0.0, "a new turn starts with no rate");
        check(meter.receivePeak() > 0.0, "the peak of the run is kept");
        check(meter.turn() == 2, "the meter knows its turn");
    }

    // ---- An idle meter says nothing ---------------------------------
    {
        WireMeter meter;
        check(!meter.isActive(), "a meter that metered nothing is inactive");
        meter.addTraffic(1, 10, 10);
        check(meter.isActive(), "traffic makes it active");
        meter.release();
        check(!meter.isActive(), "release() stops it");
    }

    return failures == 0 ? 0 : 1;
}

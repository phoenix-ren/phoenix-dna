// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

// The progress of a running transfer — the moving average over the
// bytes it moved, and the share of the thing that is here — checked
// against known traffic: a steady stream reads its own rate, a report
// of a transfer that never began is not metered, the counts drive the
// share, and a new transfer starts everything afresh.
//
//   transfermetertest
//
// Exits non-zero when a check fails. (Plain printf, not qInfo: the
// categorized logging prints nothing in this environment.)

#include "transfermeter.h"

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
// rate when no bytes arrive).
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

    // ---- A steady stream reads its own rate ---------------------------
    {
        TransferMeter meter;
        meter.begin(false, QStringLiteral("https://example.com/phoenix.iso"));
        // 1 kB/s, in 100-byte reports every 100 ms. The window is three
        // seconds, so the average needs a few windows to come up to the
        // rate: it is an exponential approach, not a step.
        for (int i = 1; i <= 120; ++i) {
            meter.addProgress(i * 100, 12000);
            wait(100);
        }
        check(meter.rate() > 900.0 && meter.rate() <= 1000.0,
              "a steady 1 kB/s stream reads about 1 kB/s");
        check(meter.rateFraction() > 0.9 && meter.rateFraction() <= 1.0,
              "the bar of a steady stream sits at its peak");
        check(meter.done() == quint64(12000), "the count is cumulative");
        check(meter.progress() == 1.0, "the whole thing is here");
        check(!meter.isUploading(), "the direction came with the beginning");
        check(meter.source() == QStringLiteral("https://example.com/phoenix.iso"),
              "the source came with the beginning too");
    }

    // ---- An unknown total keeps the share at nothing -------------------
    {
        TransferMeter meter;
        meter.begin(false, QStringLiteral("ftp://example.com/pub/thing"));
        check(!meter.hasTotal(), "no total, no promise");
        meter.addProgress(5000, 0);
        check(meter.hasTotal() == false && meter.progress() == 0.0,
              "a share needs a total: without one there is none");
        check(meter.done() == quint64(5000), "the count is still true");
    }

    // ---- A report of a transfer that never began is not metered --------
    {
        TransferMeter meter;
        meter.addProgress(1000, 2000);
        check(!meter.isActive(), "a report alone does not begin a transfer");
        check(meter.done() == quint64(0), "and it is not counted either");
    }

    // ---- A new transfer starts afresh ----------------------------------
    {
        TransferMeter meter;
        meter.begin(false, QStringLiteral("https://example.com/one"));
        meter.addProgress(4000, 4000);
        check(meter.progress() == 1.0, "the first transfer arrives whole");
        meter.begin(true, QStringLiteral("https://example.com/two"));
        check(meter.done() == quint64(0) && meter.total() == quint64(0),
              "the counts belong to the transfer that is running");
        check(meter.isUploading(), "the new direction came with the new transfer");
        check(meter.rate() == 0.0, "the rate starts over as well");
        check(meter.isActive(), "and the meter is on");
    }

    // ---- Release parks the meter ----------------------------------------
    {
        TransferMeter meter;
        meter.begin(false, QStringLiteral("magnet:?xt=urn:btih:deadbeef"));
        meter.addProgress(100, 0);
        check(meter.isActive(), "a magnet is metered like any download");
        meter.release();
        check(!meter.isActive(), "release() stops it");
        check(meter.done() == quint64(100),
              "the last counts stay for the fade-out");
    }

    return failures == 0 ? 0 : 1;
}

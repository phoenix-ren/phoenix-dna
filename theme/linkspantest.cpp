// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

/*!
 * \brief Offline check of the address-span detector (CTest target).
 *
 * The windows that show the agent's words — the results window and
 * the session search — turn every file, folder, page, mailbox and
 * phone number in them into a link the desktop opens. The detector
 * that decides what is an address lives in linkspan.h; this test
 * pins it to a table of sentences, so a changed matcher must say so
 * here first. Every case names the addresses it expects — the exact
 * characters of each span and the URL a click on it hands to the
 * desktop — and, just as much, the near misses a sentence full of
 * numbers, slashes and dots must not become. It exits non-zero and
 * names the first mismatches on failure.
 */

#include "linkspan.h"

#include <QDir>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <cstdio>

namespace {

struct Expected
{
    QString body;
    QString href;
};

struct Case
{
    QString input;
    QList<Expected> expected;
};

} // namespace

int main(int argc, char **argv)
{
    Q_UNUSED(argc)
    Q_UNUSED(argv)

    // A "~" expands to the home it stands for; the expected hrefs
    // below are written against the home of whoever runs the test.
    const QString home = QDir::homePath();

    const QList<Case> cases = {
        // Pages.
        {QStringLiteral("See https://example.com/x for more."),
         {{QStringLiteral("https://example.com/x"),
           QStringLiteral("https://example.com/x")}}},
        {QStringLiteral("Go to https://example.com."),
         {{QStringLiteral("https://example.com"),
           QStringLiteral("https://example.com")}}},
        {QStringLiteral("(https://example.com/a)"),
         {{QStringLiteral("https://example.com/a"),
           QStringLiteral("https://example.com/a")}}},
        {QStringLiteral("https://en.wikipedia.org/wiki/Foo_(bar)"),
         {{QStringLiteral("https://en.wikipedia.org/wiki/Foo_(bar)"),
           QStringLiteral("https://en.wikipedia.org/wiki/Foo_(bar)")}}},
        {QStringLiteral("https://example.com/x?y=1&z=2#frag"),
         {{QStringLiteral("https://example.com/x?y=1&z=2#frag"),
           QStringLiteral("https://example.com/x?y=1&z=2#frag")}}},
        {QStringLiteral("see https://example.com!"),
         {{QStringLiteral("https://example.com"),
           QStringLiteral("https://example.com")}}},
        {QStringLiteral("ftp://example.com/pub is the mirror"),
         {{QStringLiteral("ftp://example.com/pub"),
           QStringLiteral("ftp://example.com/pub")}}},
        // Bare www hosts.
        {QStringLiteral("visit www.kde.org or https://kde.org"),
         {{QStringLiteral("www.kde.org"), QStringLiteral("https://www.kde.org")},
          {QStringLiteral("https://kde.org"), QStringLiteral("https://kde.org")}}},
        {QStringLiteral("browse www.example"),
         {}},
        // Mailboxes.
        {QStringLiteral("mail alice@example.org tomorrow"),
         {{QStringLiteral("alice@example.org"),
           QStringLiteral("mailto:alice@example.org")}}},
        {QStringLiteral("<bob@example.com>"),
         {{QStringLiteral("bob@example.com"),
           QStringLiteral("mailto:bob@example.com")}}},
        {QStringLiteral("no link in foo@bar"),
         {}},
        // Phone numbers.
        {QStringLiteral("call +44 20 7946 0958 now"),
         {{QStringLiteral("+44 20 7946 0958"),
           QStringLiteral("tel:+442079460958")}}},
        {QStringLiteral("phone (555) 123-4567."),
         {{QStringLiteral("555) 123-4567"), QStringLiteral("tel:5551234567")}}},
        {QStringLiteral("dial 1-800-555-0199 now"),
         {{QStringLiteral("1-800-555-0199"), QStringLiteral("tel:18005550199")}}},
        {QStringLiteral("at 555 123 4567 or +1 (555) 123-4567"),
         {{QStringLiteral("555 123 4567"), QStringLiteral("tel:5551234567")},
          {QStringLiteral("+1 (555) 123-4567"),
           QStringLiteral("tel:+15551234567")}}},
        {QStringLiteral("the office is on 555 1234 today"),
         {{QStringLiteral("555 1234"), QStringLiteral("tel:5551234")}}},
        // The near misses: dates, versions, IPs, decimals, IDs.
        {QStringLiteral("on 2026-11-04"), {}},
        {QStringLiteral("versions 1.2.3.4 and 2.10"), {}},
        {QStringLiteral("IP 192.168.1.100 gateway"), {}},
        {QStringLiteral("case 555.123.4567 in the USA"), {}},
        {QStringLiteral("no numbers like 1234567 or 7654321"), {}},
        {QStringLiteral("numbers 4321-8765-99 are an order"), {}},
        {QStringLiteral("code 1234 5678 is not a number"), {}},
        {QStringLiteral("money $3.50 or 45%"), {}},
        // Files and folders.
        {QStringLiteral("saved to /tmp/out.png yesterday"),
         {{QStringLiteral("/tmp/out.png"), QStringLiteral("file:///tmp/out.png")}}},
        {QStringLiteral("it is in /home/user/notes."),
         {{QStringLiteral("/home/user/notes"),
           QStringLiteral("file:///home/user/notes")}}},
        {QStringLiteral("the log at /var/log/phoenix.log: line 3"),
         {{QStringLiteral("/var/log/phoenix.log"),
           QStringLiteral("file:///var/log/phoenix.log")}}},
        {QStringLiteral("under /usr there"), {{QStringLiteral("/usr"),
           QStringLiteral("file:///usr")}}},
        {QStringLiteral("or maybe /tmp/"), {{QStringLiteral("/tmp/"),
           QStringLiteral("file:///tmp/")}}},
        {QStringLiteral("the report ~/Documents/report.md was updated"),
         {{QStringLiteral("~/Documents/report.md"),
           QStringLiteral("file://") + home + QStringLiteral("/Documents/report.md")}}},
        // The near misses that look like paths.
        {QStringLiteral("sed 's/foo/bar/' changed it"), {}},
        {QStringLiteral("and/or either"), {}},
        {QStringLiteral("path ./relative and ../up"), {}},
        {QStringLiteral("/username/x is not /usr"), {{QStringLiteral("/usr"),
           QStringLiteral("file:///usr")}}},
        // A URL keeps the path inside it: one address, not two.
        {QStringLiteral("mirror at https://example.com/home/x has it"),
         {{QStringLiteral("https://example.com/home/x"),
           QStringLiteral("https://example.com/home/x")}}},
    };

    QString report;
    QTextStream out(&report);
    int failures = 0;
    for (const Case &testCase : cases) {
        const QList<LinkSpan> got = linkSpansIn(testCase.input);
        bool same = got.size() == testCase.expected.size();
        if (same) {
            for (int i = 0; i < got.size(); ++i) {
                const QString body = testCase.input.mid(
                    got.at(i).start, got.at(i).end - got.at(i).start);
                if (body != testCase.expected.at(i).body
                        || got.at(i).href != testCase.expected.at(i).href) {
                    same = false;
                    break;
                }
            }
        }
        if (!same) {
            ++failures;
            out << "span mismatch for input: " << testCase.input << "\n";
            out << "  expected " << testCase.expected.size()
                << " spans, got " << got.size() << "\n";
            for (const Expected &expected : testCase.expected) {
                out << "  want body=" << expected.body
                    << " href=" << expected.href << "\n";
            }
            for (const LinkSpan &span : got) {
                out << "  got  body=" << testCase.input.mid(
                           span.start, span.end - span.start)
                    << " href=" << span.href << "\n";
            }
        }
    }

    QTextStream stdio(stdout);
    stdio << report;
    stdio << (failures == 0
                  ? QStringLiteral("linkspantest: all cases pass\n")
                  : QStringLiteral("linkspantest: %1 failure(s)\n").arg(failures));
    return failures == 0 ? 0 : 1;
}

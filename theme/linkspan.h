// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*!
 * \brief The address-span detector the client-side linkifier shares
 * with its test (linkspantest.cpp).
 *
 * The agent's words name things — files and folders on the machine,
 * pages on the web, mailboxes, phone numbers — and the windows that
 * show those words turn every such address into a link the desktop
 * opens: a folder in the file manager, a page in the browser, an
 * address in the mail client, a number in the dialer. The detection
 * lives entirely on the client (the daemon is untouched): this header
 * is the detector, kept in a header of its own for the same reason
 * mathspan.h is — the test includes it directly.
 *
 * The detector is deliberately conservative. A bare "and/or" is not a
 * path, "2026-11-04" is not a phone number, "1.2.3" is not anything,
 * and a false link costs more than a missed one (a click that opens
 * the wrong thing teaches the user to trust none of them). So a path
 * must lead with one of the system's own roots or with "~", a number
 * without the international "+" must have the shape of one (it ends
 * in four digits, its groups are held apart by separators), and every
 * candidate sheds the punctuation of the sentence around it before it
 * becomes an address.
 */

#include <QDir>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <utility>

// One address found in a text: the half-open range [start, end) of
// UTF-16 offsets within that text, and the URL a click hands to the
// desktop.
struct LinkSpan
{
    int start = 0;
    int end = 0;
    QString href;
};

// The kinds of address, in the order their matchers run — the order
// is the priority when two kinds claim the same characters (a URL
// wins over the path inside it, an email over the path around it).
enum LinkKind
{
    LinkUrl,     // https://example.com/a, ftp://host/pub
    LinkWww,     // www.example.net — https:// is assumed
    LinkEmail,   // alice@example.org
    LinkPhone,   // +44 20 7946 0958, (555) 123-4567
    LinkPath,    // /home/alice/notes.txt, /etc/fstab, /tmp/
    LinkHome     // ~/Documents/report.md
};

// Every kind's matcher, in LinkKind order. Each is written to start
// and end on the address's own characters (lookbehinds for what a
// real address never hangs off), so the sentence's punctuation is
// trimmed, not carved, afterwards.
inline const QList<QRegularExpression> &linkSpanMatchers()
{
    static const QList<QRegularExpression> matchers = {
        // A URL with its scheme. The scheme is required: "example.com/x"
        // alone is prose too often to be a link. Quotes and brackets are
        // not URL characters; the closing ones that hug an address at the
        // end of a sentence are taken off by trimmedAddress().
        QRegularExpression(QStringLiteral(
            "\\b(?:https?|ftp)://[^\\s<>\"'`]+")),
        // A bare www host — https:// is assumed, the way browsers do.
        QRegularExpression(QStringLiteral("\\bwww\\.[^\\s<>\"'`]+")),
        // An email address: a local part, an @, and a domain with at
        // least one dot.
        QRegularExpression(QStringLiteral(
            "\\b[A-Za-z0-9][A-Za-z0-9._%+-]*@"
            "[A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)+")),
        // A phone number: digits with the separators phones use, tight
        // enough not to swallow the sentence around it
        // (plausiblePhone() does the judging).
        QRegularExpression(QStringLiteral(
            "(?<![\\w.+-])\\+?\\d[\\d\\s().-]{5,}\\d")),
        // An absolute path under one of the system's own roots. The
        // roots are what keeps this precise: "s/foo/bar/" never leads
        // with one, and a path inside a URL is blocked twice over (the
        // host's letter breaks the lookbehind, and the URL's own span
        // was claimed first).
        QRegularExpression(QStringLiteral(
            "(?<![\\w:~./-])/(?:bin|boot|data|dev|etc|home|lib|lib32|"
            "lib64|libx32|media|mnt|opt|proc|root|run|sbin|srv|sys|tmp|"
            "usr|var)(?:/[A-Za-z0-9_.@+-]+)*/?")),
        // The user's own home. The tilde expands when the href is
        // built; everything after it must look like a path.
        QRegularExpression(QStringLiteral(
            "(?<!\\w)~/(?:[A-Za-z0-9_.@+-]+/)*[A-Za-z0-9_.@+-]*/?")),
    };
    return matchers;
}

// The trailing punctuation of the sentence around an address: the
// period that ends it, the bracket that closed the aside it stood in.
// A closing bracket that has its opener inside the address stays —
// "https://en.wikipedia.org/wiki/Foo_(bar)" is one address.
inline QString trimmedAddress(QString candidate)
{
    static const QString sentenceEnd = QStringLiteral(".,;:!?`");
    while (!candidate.isEmpty()) {
        const QChar last = candidate[candidate.size() - 1];
        if (sentenceEnd.contains(last)
                || (last == u')' && !candidate.contains(u'('))
                || (last == u']' && !candidate.contains(u'['))) {
            candidate.chop(1);
            continue;
        }
        break;
    }
    return candidate;
}

// Whether a phone-shaped candidate is a phone number.
//
// With the international "+" any well-formed number passes: the
// prefix says what the digits mean. Without it, only the classic
// shapes do — the number ends in four digits, its groups are held
// apart by real separators, and a local number is seven digits long.
// The shape is what keeps the near misses out: an order number
// ("4321-8765-99"), a date ("2026-11-04") and a pair of four-digit
// groups ("1234 5678") have none of them.
inline bool plausiblePhone(const QString &candidate)
{
    int digits = 0;
    for (const QChar &ch : candidate) {
        if (ch.isDigit()) {
            ++digits;
        }
    }
    if (candidate.startsWith(QLatin1Char('+'))) {
        return digits >= 7 && digits <= 15;
    }
    if (digits < 7 || digits > 15) {
        return false;
    }
    // Dotted digit groups are versions, IPv4 addresses and decimals.
    if (candidate.contains(QLatin1Char('.'))) {
        return false;
    }
    // The groups of digits, as the separators stand them apart.
    QList<int> groups;
    int group = 0;
    for (const QChar &ch : candidate) {
        if (ch.isDigit()) {
            ++group;
            continue;
        }
        if (group > 0) {
            groups.append(group);
            group = 0;
        }
    }
    if (group > 0) {
        groups.append(group);
    }
    if (groups.size() < 2 || groups.last() != 4) {
        return false;
    }
    if (digits >= 10) {
        return true;
    }
    // A short number is a local one: three digits and four.
    return groups.size() == 2 && groups.first() == 3;
}

// The tel: form of a number: a leading "+" kept, everything that is
// not a digit dropped — the separators are for the eye.
inline QString phoneHref(const QString &candidate)
{
    QString href;
    if (candidate.startsWith(QLatin1Char('+'))) {
        href += QLatin1Char('+');
    }
    for (const QChar &ch : candidate) {
        if (ch.isDigit()) {
            href += ch;
        }
    }
    return QStringLiteral("tel:") + href;
}

// The file: form of a path: "~" expanded to the home it stands for,
// and everything percent-encoded the way a URL must be.
inline QString fileHref(const QString &path)
{
    QString expanded = path;
    if (expanded.startsWith(QLatin1String("~/"))) {
        expanded = QDir::homePath() + expanded.mid(1);
    }
    return QUrl::fromLocalFile(expanded).toString();
}

// Every address in \a text, in text order, with the URL a click on it
// hands to the desktop. Two kinds never claim the same characters:
// whichever kind runs first keeps the span (see LinkKind).
inline QList<LinkSpan> linkSpansIn(const QString &text)
{
    struct Accepted
    {
        int start;
        int end;
        int kind;
        QString body;
    };
    QList<Accepted> accepted;

    const QList<QRegularExpression> &matchers = linkSpanMatchers();
    for (int kind = 0; kind < matchers.size(); ++kind) {
        QRegularExpressionMatchIterator it =
            matchers.at(kind).globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            const QString body = trimmedAddress(match.captured(0));
            if (body.isEmpty()) {
                continue;
            }
            const int start = int(match.capturedStart());
            const int end = start + body.size();
            // The plausibility each kind judges for itself.
            if (kind == LinkWww && body.count(QLatin1Char('.')) < 2) {
                continue;
            }
            if (kind == LinkPhone && !plausiblePhone(body)) {
                continue;
            }
            bool overlaps = false;
            for (const Accepted &span : std::as_const(accepted)) {
                if (start < span.end && span.start < end) {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps) {
                accepted.append({start, end, kind, body});
            }
        }
    }

    QList<LinkSpan> spans;
    spans.reserve(accepted.size());
    for (const Accepted &span : std::as_const(accepted)) {
        QString href;
        switch (span.kind) {
        case LinkUrl:
            href = span.body;
            break;
        case LinkWww:
            href = QStringLiteral("https://") + span.body;
            break;
        case LinkEmail:
            href = QStringLiteral("mailto:") + span.body;
            break;
        case LinkPhone:
            href = phoneHref(span.body);
            break;
        case LinkPath:
        case LinkHome:
            href = fileHref(span.body);
            break;
        }
        spans.append({span.start, span.end, href});
    }
    std::sort(spans.begin(), spans.end(),
              [](const LinkSpan &a, const LinkSpan &b) {
                  return a.start < b.start;
              });
    return spans;
}

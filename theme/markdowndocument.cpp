// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "markdowndocument.h"

#include "linkspan.h"  // the address detector, shared with its test

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QUrl>

namespace {

// Tells the session's file manager (Dolphin, on KDE) to show \a url —
// the folder with the file selected in it — over the standard
// freedesktop interface every file manager implements.
//
// The call activates the file manager when it is not running yet (the
// session's service files know how to start it), so a file opens in
// Dolphin whether Dolphin was up or not. It is fired and forgotten —
// the GUI thread must not wait for a file manager to start — and the
// watcher is what answers for the case where no file manager answers
// at all: the file then opens with its own handler.
bool showInFileManager(const QUrl &url)
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return false;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("ShowItems"));
    call.setArguments({QStringList{url.toString()}, QString()});
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                     watcher, [url](QDBusPendingCallWatcher *self) {
        if (self->isError()) {
            QDesktopServices::openUrl(url);
        }
        self->deleteLater();
    });
    return true;
}

// Opens a file: URL the way a file listing's link opens: the folder
// itself in the file manager; a file in the file manager too,
// selected in its folder — where a file *is* is the useful thing to
// show, the point of the link is the place as much as the bytes —
// with the file's own handler as the fallback when no file manager
// answers; and a path that is gone in the deepest ancestor that is
// still there.
bool openLocalLink(const QUrl &url)
{
    // A file on another machine is not ours to open. (QUrl counts a
    // file: URL with a host as local all the same, so the host is
    // asked about itself.)
    if (!url.isLocalFile()
            || (!url.host().isEmpty()
                && url.host() != QLatin1String("localhost"))) {
        return false;
    }
    const QFileInfo info(url.toLocalFile());
    if (info.isDir()) {
        return QDesktopServices::openUrl(
            QUrl::fromLocalFile(info.absoluteFilePath()));
    }
    if (info.exists()) {
        if (showInFileManager(url)) {
            return true;
        }
        return QDesktopServices::openUrl(url);
    }
    // A path that is gone: the deepest ancestor that is still there.
    // The walk stops where there is no way up any more — QDir::cdUp()
    // is a no-op on some paths, and a loop that never ends is worse
    // than a link that opens nothing.
    QDir dir(info.absolutePath());
    while (!dir.exists()) {
        const QString here = dir.absolutePath();
        dir.cdUp();
        if (dir.absolutePath() == here) {
            break;
        }
    }
    if (!dir.exists()) {
        qWarning() << "MarkdownDocument: nothing to open in" << url;
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

} // namespace

void MarkdownDocument::spaceParagraphs(QQuickTextDocument *document,
                                       qreal spacing) const
{
    QTextDocument *doc = document ? document->textDocument() : nullptr;
    if (!doc) {
        return;
    }

    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (QTextBlock block = doc->begin(); block.isValid();
         block = block.next()) {
        // The gap between list items belongs to the list; margining
        // each item would tear the list apart.
        if (block.textList()) {
            continue;
        }
        const bool first = block == doc->begin();
        const bool last = !block.next().isValid();
        // A heading opens a section: it wants more room above it than
        // a paragraph does, and it binds to the text it introduces, so
        // it keeps less room below.
        const bool heading = block.blockFormat().headingLevel() > 0;
        // Qt does not collapse margins: the wanted gap is split across
        // the two blocks that meet, and the outermost edges stay at the
        // container's own padding.
        const qreal top = first ? 0 : (heading ? spacing : spacing / 2);
        const qreal bottom = last ? 0 : (heading ? spacing / 4 : spacing / 2);
        QTextBlockFormat format = block.blockFormat();
        // Idempotence: rewriting an unchanged format would still emit
        // contentsChange, and through the layout cascade that reaches
        // this handler again — the storm the journal filled with
        // stack overflows. A second polish must touch nothing.
        if (format.topMargin() == top && format.bottomMargin() == bottom) {
            continue;
        }
        format.setTopMargin(top);
        format.setBottomMargin(bottom);
        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);
    }
    cursor.endEditBlock();
}

void MarkdownDocument::linkify(QQuickTextDocument *document,
                               const QColor &linkColor) const
{
    QTextDocument *doc = document ? document->textDocument() : nullptr;
    if (!doc || !linkColor.isValid()) {
        return;
    }

    struct Candidate
    {
        int start;
        int end;
        QString href;   // empty for the Markdown's own links, whose
                        // href is never touched
    };
    QList<Candidate> candidates;
    QList<Candidate> retints;

    for (QTextBlock block = doc->begin(); block.isValid();
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat format = fragment.charFormat();
            // A formula image is not text; the Markdown's own links
            // keep the href they came with and only take the ink.
            if (format.isImageFormat()) {
                continue;
            }
            const QString text = fragment.text();
            if (text.isEmpty()) {
                continue;
            }
            const int start = fragment.position();
            const int end = start + text.size();
            if (format.isAnchor()) {
                if (format.foreground() != QBrush(linkColor)) {
                    retints.append({start, end, QString()});
                }
                continue;
            }
            const QList<LinkSpan> spans = linkSpansIn(text);
            for (const LinkSpan &span : spans) {
                candidates.append({start + span.start,
                                   start + span.end, span.href});
            }
        }
    }
    // The same idempotence contract as spaceParagraphs(): a second
    // pass that touches nothing, so the change cascade a format write
    // sets off cannot become a storm.
    if (candidates.isEmpty() && retints.isEmpty()) {
        return;
    }

    const QBrush ink(linkColor);
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    for (const Candidate &candidate : std::as_const(candidates)) {
        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorHref(candidate.href);
        format.setFontUnderline(true);
        format.setForeground(ink);
        cursor.setPosition(candidate.start);
        cursor.setPosition(candidate.end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    for (const Candidate &retint : std::as_const(retints)) {
        QTextCharFormat format;
        format.setForeground(ink);
        cursor.setPosition(retint.start);
        cursor.setPosition(retint.end, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(format);
    }
    cursor.endEditBlock();
}

bool MarkdownDocument::openLink(const QString &link) const
{
    const QUrl url(link);
    if (!url.isValid()) {
        return false;
    }
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("file")) {
        return openLocalLink(url);
    }
    // Only what the desktop has a handler for is ever handed to it.
    // The Markdown could have authored anything into a link, and a
    // scheme the desktop does not expect is not ours to try.
    static const QList<QString> opened = {
        QStringLiteral("http"),
        QStringLiteral("https"),
        QStringLiteral("ftp"),
        QStringLiteral("mailto"),
        QStringLiteral("tel"),
    };
    if (!opened.contains(scheme)) {
        qWarning() << "MarkdownDocument: refusing to open" << link;
        return false;
    }
    return QDesktopServices::openUrl(url);
}

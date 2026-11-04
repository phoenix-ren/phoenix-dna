// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QString>
#include <QQuickTextDocument>
#include <QtQml/qqmlregistration.h>

class QQuickTextDocument;

/*!
 * \brief Presentation helpers for the Markdown the agent writes.
 *
 * Reached from QML as \c MarkdownDocument. While an answer streams, its
 * LaTeX math is shown verbatim inside the native Markdown view; once
 * the run is finished, answers containing math are handed to
 * LatexRenderer, which compiles the whole answer to PDF via LaTeX and
 * replaces the view. What this class still owns is the one thing the
 * native view needs either way: Qt's rich-text engine renders Markdown
 * paragraphs with no vertical gap at all, which reads as one crammed
 * wall of text — spaceParagraphs() walks the rendered document and
 * gives every non-list block a margin, so paragraphs, headings, quotes
 * and code blocks breathe while list items stay tight (the gap between
 * items belongs to the list, not to each item). A heading gets more
 * room above it than a paragraph and keeps less below: it opens a
 * section and binds to the text it introduces.
 *
 * The addresses in the agent's words — files and folders, pages,
 * mailboxes, phone numbers — become links here as well:
 * linkify() walks the very same rendered document (after the Markdown
 * import, so the sentence's punctuation is known to be decoration, not
 * part of the address) and turns every one the detector in linkspan.h
 * finds into an anchor; the Markdown's own links join the app's link
 * ink. openLink() is what a click lands in: it hands the address to
 * the desktop — a folder in the file manager, a page in the browser,
 * an address in the mail client, a number in the dialer — and to
 * nothing else.
 */
class MarkdownDocument : public QObject
{
    Q_OBJECT
    QML_SINGLETON
    QML_NAMED_ELEMENT(MarkdownDocument)

public:
    /*! \brief \a spacing is the wanted gap between two neighbouring
     *  blocks (split across the bottom margin of one and the top margin
     *  of the next — Qt does not collapse margins); the first and last
     *  block keep the container's own padding, so they get no outer
     *  margin. */
    Q_INVOKABLE void spaceParagraphs(QQuickTextDocument *document,
                                     qreal spacing) const;

    /*! \brief Turns the bare addresses in \a document into links, in
     *  the app's link ink \a linkColor.
     *
     *  Runs on the rendered document (call it from onTextChanged,
     *  deferred like spaceParagraphs()), so it sees the paragraphs as
     *  they are shown: the Markdown's own links are anchors already
     *  and only take the colour, and every file, folder, page,
     *  mailbox and phone number the detector in linkspan.h finds
     *  becomes one — in prose, in the notes, and in the code fences of
     *  the tool log, where the agent's work names files most.
     *  Idempotent: a second pass with the same ink touches nothing,
     *  and one with another ink (a theme switch) re-tints. */
    Q_INVOKABLE void linkify(QQuickTextDocument *document,
                             const QColor &linkColor) const;

    /*! \brief Hands a clicked anchor to the desktop: a URL to the
     *  browser, a mailto: to the mail client, a tel: to the dialer, a
     *  folder to the file manager and a file to the file manager's
     *  own "show it, selected" — falling back to the file's handler
     *  when no file manager answers. Only those schemes are ever
     *  opened, whatever the Markdown authored; everything else is
     *  refused. Returns whether the desktop took it. */
    Q_INVOKABLE bool openLink(const QString &link) const;
};

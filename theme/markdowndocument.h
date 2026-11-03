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
};

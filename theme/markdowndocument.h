// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QString>
#include <QQuickTextDocument>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief Presentation helpers for the Markdown the agent writes.
 *
 * Reached from QML as \c MarkdownDocument. Two jobs:
 *
 * Paragraph spacing — Qt's rich-text engine renders Markdown paragraphs
 * with no vertical gap at all, which reads as one crammed wall of text.
 * spaceParagraphs() walks the rendered document and gives every
 * non-list block a margin, so paragraphs, headings, quotes and code
 * blocks breathe while list items stay tight (the gap between items
 * belongs to the list, not to each item).
 *
 * Math — the agent answers with LaTeX, and Qt renders it literally.
 * renderMath() rewrites the math spans of a Markdown text into rich
 * Unicode — superscripts, subscripts, Greek letters, operators,
 * integrals, sums — inside the fast native text view. This is
 * deliberately not a full equation layouter: no web view is embedded,
 * so theming, selection and scrolling keep working as they do.
 * \c $…$ and \c $$…$$ and \c \(…\) and \c \[…\] spans are transformed
 * (multi-line display spans included), unmistakable bare constructs
 * like \c \\alpha or \c x^2 come along in prose, and fenced and inline
 * code, plus text without any math markers, pass through untouched.
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

    /*! \brief The text with its LaTeX math spans rewritten into
     *  Unicode. Idempotent for text without math: a fast path returns
     *  it unchanged. */
    Q_INVOKABLE QString renderMath(const QString &text) const;
};

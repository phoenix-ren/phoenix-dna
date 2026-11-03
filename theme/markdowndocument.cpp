// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "markdowndocument.h"

#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>

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

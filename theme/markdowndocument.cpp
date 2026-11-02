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
        QTextBlockFormat format = block.blockFormat();
        // Qt does not collapse margins: the wanted gap is split across
        // the two blocks that meet, and the outermost edges stay at the
        // container's own padding.
        format.setTopMargin(first ? 0 : spacing / 2);
        format.setBottomMargin(last ? 0 : spacing / 2);
        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);
    }
    cursor.endEditBlock();
}

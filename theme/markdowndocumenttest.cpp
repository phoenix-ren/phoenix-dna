// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

/*!
 * \brief Offline check of the link layer of the rendered document
 * (CTest target).
 *
 * MarkdownDocument::linkify() is what turns the addresses in the
 * agent's words into links, and it works on the *rendered* document —
 * after the Markdown import, where the sentence's punctuation is
 * known to be decoration and not part of the address. This test drives
 * it over the very widget the windows show their answers in (a
 * TextEdit, the one inside Kirigami.SelectableLabel) and checks that:
 *
 *   * the bare addresses of a plain label become anchors with the
 *     right URLs — a file, a page, a mailbox, a number;
 *   * the Markdown's own links (Qt's importer autolinks bare URLs and
 *     addresses already) keep their href and take the app's link ink;
 *   * a second pass touches nothing (the idempotence the change
 *     cascade of the window depends on), and one with another ink only
 *     re-tints;
 *   * a link with a scheme the desktop has no business running is
 *     refused.
 */

#include "markdowndocument.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextStream>

namespace {

// The anchors of a document, in the order they stand in it.
QStringList anchorsIn(const QTextDocument *doc)
{
    QStringList hrefs;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.charFormat().isAnchor()) {
                hrefs.append(fragment.charFormat().anchorHref());
            }
        }
    }
    return hrefs;
}

// The ink the anchors of a document wear: every anchor's foreground,
// or an empty string when they differ.
QString inkIn(const QTextDocument *doc)
{
    QString ink;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.charFormat().isAnchor()) {
                continue;
            }
            const QString color = fragment.charFormat().foreground().color().name();
            if (ink.isEmpty()) {
                ink = color;
            } else if (ink != color) {
                return QStringLiteral("(mixed)");
            }
        }
    }
    return ink;
}

int failures = 0;

void check(bool condition, const QString &what)
{
    if (!condition) {
        ++failures;
        QTextStream(stderr) << "FAIL: " << what << "\n";
    }
}

// A TextEdit, as the labels are: the text, the format, and the
// document the Markdown was imported into.
class Editor
{
public:
    explicit Editor(QQmlEngine *engine)
    {
        component_ = new QQmlComponent(engine);
        component_->setData(
            QByteArrayLiteral(
                "import QtQuick\n"
                "TextEdit {\n"
                "    property string content: \"\"\n"
                "    property bool markdown: true\n"
                "    text: content\n"
                "    textFormat: markdown\n"
                "        ? TextEdit.MarkdownText : TextEdit.PlainText\n"
                "    property var doc: textDocument\n"
                "}\n"),
            QUrl());
        object_ = component_->create();
    }

    ~Editor()
    {
        delete object_;
        delete component_;
    }

    void set(const QString &text, bool markdown)
    {
        object_->setProperty("markdown", markdown);
        object_->setProperty("content", text);
    }

    QQuickTextDocument *document() const
    {
        return object_->property("doc").value<QQuickTextDocument *>();
    }

private:
    QQmlComponent *component_ = nullptr;
    QObject *object_ = nullptr;
};

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QQmlEngine engine;

    const MarkdownDocument markdown;
    const QColor ink("#3daee9");
    const QColor other("#1793d1");

    // A plain label — a dictated prompt: nothing in it is a link yet.
    {
        Editor editor(&engine);
        editor.set(QStringLiteral(
                       "mail bob@example.org, open https://example.com/a, "
                       "read /tmp/notes.txt, call +44 20 7946 0958"),
                   false);
        markdown.linkify(editor.document(), ink);

        const QStringList hrefs = anchorsIn(editor.document()->textDocument());
        check(hrefs == (QStringList{
                          QStringLiteral("mailto:bob@example.org"),
                          QStringLiteral("https://example.com/a"),
                          QStringLiteral("file:///tmp/notes.txt"),
                          QStringLiteral("tel:+442079460958")}),
              QStringLiteral("plain text anchors: %1").arg(hrefs.join(QStringLiteral(", "))));
        check(inkIn(editor.document()->textDocument())
                  == QStringLiteral("#3daee9"),
              QStringLiteral("plain text ink: %1")
                  .arg(inkIn(editor.document()->textDocument())));
    }

    // A Markdown label — what the answers are shown in. Qt's importer
    // autolinks the URL on its own; the file is the window's own find.
    {
        Editor editor(&engine);
        editor.set(QStringLiteral(
                       "See https://example.com/x for more.\n\n"
                       "The notes are in /home/user/notes.txt.\n\n"
                       "```bash\ncat /etc/fstab\n```\n"),
                   true);
        markdown.linkify(editor.document(), ink);

        const QStringList hrefs = anchorsIn(editor.document()->textDocument());
        check(hrefs.contains(QStringLiteral("https://example.com/x")),
              QStringLiteral("markdown keeps the importer's own link: %1")
                  .arg(hrefs.join(QStringLiteral(", "))));
        check(hrefs.contains(QStringLiteral("file:///home/user/notes.txt")),
              QStringLiteral("markdown finds the file: %1")
                  .arg(hrefs.join(QStringLiteral(", "))));
        check(hrefs.contains(QStringLiteral("file:///etc/fstab")),
              QStringLiteral("markdown finds the file in the code fence: %1")
                  .arg(hrefs.join(QStringLiteral(", "))));
        check(hrefs.size() == 3,
              QStringLiteral("markdown makes no other link: %1")
                  .arg(hrefs.join(QStringLiteral(", "))));
        // The Markdown's own link took the app's ink as well.
        check(inkIn(editor.document()->textDocument())
                  == QStringLiteral("#3daee9"),
              QStringLiteral("markdown ink: %1")
                  .arg(inkIn(editor.document()->textDocument())));
    }

    // A second pass touches nothing: the window's change cascade
    // depends on it.
    {
        Editor editor(&engine);
        editor.set(QStringLiteral("read /tmp/notes.txt"), false);
        markdown.linkify(editor.document(), ink);
        const int revision = editor.document()->textDocument()->revision();
        markdown.linkify(editor.document(), ink);
        check(editor.document()->textDocument()->revision() == revision,
              QStringLiteral("the second pass changes nothing"));
    }

    // A pass in another ink only re-tints.
    {
        Editor editor(&engine);
        editor.set(QStringLiteral("read /tmp/notes.txt"), false);
        markdown.linkify(editor.document(), ink);
        markdown.linkify(editor.document(), other);
        check(inkIn(editor.document()->textDocument())
                  == QStringLiteral("#1793d1"),
              QStringLiteral("the re-tint: %1")
                  .arg(inkIn(editor.document()->textDocument())));
        check(anchorsIn(editor.document()->textDocument())
                  == QStringList{QStringLiteral("file:///tmp/notes.txt")},
              QStringLiteral("the re-tint keeps the link: %1")
                  .arg(anchorsIn(editor.document()->textDocument())
                           .join(QStringLiteral(", "))));
    }

    // Only the schemes the desktop has a handler for are ever opened.
    check(!markdown.openLink(QStringLiteral("javascript:alert(1)")),
          QStringLiteral("a script link is refused"));
    check(!markdown.openLink(QStringLiteral("file://elsewhere/etc/passwd")),
          QStringLiteral("a file on another machine is refused"));

    QTextStream stdio(stdout);
    stdio << (failures == 0
                  ? QStringLiteral("markdowndocumenttest: all checks pass\n")
                  : QStringLiteral("markdowndocumenttest: %1 failure(s)\n")
                        .arg(failures));
    return failures == 0 ? 0 : 1;
}

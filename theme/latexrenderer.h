// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QQuickTextDocument>
#include <QtQml/qqmlregistration.h>

/*!
 * \brief Renders the agent's LaTeX formulas to images inside the native
 *  Markdown view.
 *
 * Reached from QML as \c LatexRenderer. The answer itself stays native
 * rich text — the app's font, size, margins, lists, scrolling and
 * selection, all untouched. Only the math spans ($…$, $$…$$, \(…\),
 * \[…\]) are compiled: one LaTeX run per finished answer, every formula
 * on its own page, the pages rendered through QtPdf and cropped to
 * their ink. Each cropped image becomes an alpha mask tinted with the
 * caller's ink colour — light-mode text colour or dark-mode text colour
 * — over a transparent background, so the formula sits on whatever the
 * card behind it looks like. A theme switch re-tints from the cached
 * PDF without recompiling.
 *
 * embedFormulas() is asynchronous (a compile takes its time); when the
 * formulas are in, embedded(requestId) fires and the given document
 * already shows them. Failures — no engine installed, a formula LaTeX
 * rejects, a timeout — emit failed(requestId, reason) and the document
 * keeps its verbatim Markdown.
 */
class LatexRenderer : public QObject
{
    Q_OBJECT
    /*! \brief Whether a TeX engine was found on the PATH. When false,
     *  the views keep the verbatim Markdown and never ask for math. */
    Q_PROPERTY(bool available READ isAvailable CONSTANT)
    QML_SINGLETON
    QML_NAMED_ELEMENT(LatexRenderer)

public:
    explicit LatexRenderer(QObject *parent = nullptr);

    bool isAvailable() const;

    /*! \brief Whether \a text looks like it carries LaTeX math:
     *  delimited spans ($…$, $$…$$, \(…\), \[…\]) or unmistakable
     *  commands with braces (\frac{…}, \mathcal{…}, \begin{…}). */
    Q_INVOKABLE bool containsLatex(const QString &text) const;

    /*! \brief Compiles the math spans of \a markdown and writes the
     *  result into \a document: each span replaced by its tinted image,
     *  paragraphs spaced by \a paragraphSpacing, images scaled from
     *  their compiled 11pt to \a fontPixelSize. The ink of the images
     *  is \a ink, so the caller themes them. Emits embedded(requestId)
     *  when done, failed(requestId, reason) when the compile fails —
     *  never synchronously (an emit inside the invokable does not
     *  reach every caller context). */
    Q_INVOKABLE void embedFormulas(int requestId,
                                   QQuickTextDocument *document,
                                   const QString &markdown,
                                   const QColor &ink,
                                   qreal fontPixelSize,
                                   qreal paragraphSpacing);

Q_SIGNALS:
    /*! \brief \a requestId finished: the document it was given shows
     *  the formulas. */
    void embedded(int requestId);

    /*! \brief \a requestId could not be compiled; \a reason carries
     *  the engine's first complaint when there is one. */
    void failed(int requestId, const QString &reason);

private:
    /*! \brief One pending embed: what to do when the compile lands. */
    struct Pending
    {
        int requestId = -1;
        QPointer<QQuickTextDocument> document;
        QString markdown;
        QColor ink;
        qreal fontPixelSize = 16;
        qreal paragraphSpacing = 8;
        QString pdfPath;
        QStringList spans;   // verbatim, delimiters included, in order
    };

    /*! \brief The detected engine executable, or empty. */
    QString engine() const;

    /*! \brief The PDF rasterizer (pdftoppm of poppler-utils), or
     *  empty — the compiled pages are turned into images with it. */
    QString rasterizer() const;

    /*! \brief The cache directory holding the .tex/.pdf files,
     *  created (and pruned of month-old files) on demand. */
    QString cacheDir() const;

    /*! \brief Runs the engine on \a texPath, asynchronously; on
     *  success rasterize() picks up the pending request. */
    void compile(int requestId, const QString &texPath,
                 const QString &pdfPath);

    /*! \brief Turns the compiled \a pdfPath into one PNG per formula
     *  page (pdftoppm), asynchronously; on success the pixel work
     *  continues in a worker thread. */
    void rasterize(int requestId, const QString &pdfPath);

    /*! \brief The pixel half of the embed — loading, cropping and
     *  tinting the pages — off the GUI thread: an essay carries a
     *  hundred formulas, each page rasterized at three pixels per
     *  point, and scanning that on the main thread froze the shell
     *  for the duration. When the images are ready, embedImages()
     *  takes over back on the GUI thread. */
    void processPages(const Pending &pending, const QString &pageDir);

    /*! \brief The document half of the embed, on the GUI thread: the
     *  tinted images become resources, the Markdown is rewritten with
     *  their references, and the sizes are fitted to the answer's
     *  font. Only this touches the QTextDocument — documents are not
     *  thread-safe, and this part is O(text + images), not
     *  O(pixels). */
    void embedImages(const Pending &pending, const QList<QImage> &images);

    /*! \brief Report a failure for \a requestId — through the event
     *  loop, like every emit of this class. */
    void failLater(int requestId, const QString &reason);

    mutable QString m_engine;
    mutable QString m_rasterizer;
    mutable bool m_engineResolved = false;
    mutable bool m_rasterizerResolved = false;

    /*! \brief The requests whose pixel work is running on a worker;
     *  looked up when the images come back to the GUI thread. */
    QHash<int, Pending> m_pendingByImages;
    QHash<int, Pending> m_pending;
};

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
 * on its own page, the pages rendered through pdftoppm and cropped to
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
 *
 * Three details of the document's contract are worth knowing, because
 * they are not obvious and each of them broke the feature once:
 *
 *  * The images are registered under a scheme-qualified URL
 *    ("phoenixformula://0"). A bare name is resolved against the
 *    document's base URL — the QML file's own directory — and the
 *    lookup then misses, and the view tries to open the name as a
 *    file, once per formula, per relayout.
 *  * The images are registered *after* the Markdown is written: the
 *    import clears the document's resource table.
 *  * The Markdown importer does not carry an image's size into the
 *    character format; the size is set here, in logical pixels.
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
     *  the formulas that made it in (see embedFormulas for the
     *  timeout semantics). */
    void embedded(int requestId);

    /*! \brief One more formula of \a requestId came back from the
     *  pixel pool: \a done of \a total are ready. Fires once per
     *  formula, on the GUI thread — the ticks a progress bar runs on. */
    void progress(int requestId, int done, int total);

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
     *  page (pdftoppm), asynchronously; on success startPageJobs()
     *  fans the pages out. */
    void rasterize(int requestId, const QString &pdfPath);

    /*! \brief The pixel half of the embed, one pool job per formula:
     *  loading its page, cropping it to the ink, tinting it. The jobs
     *  run on the global pool (a thread per core) and deliver
     *  themselves back through onImageReady(); assembleJob() joins
     *  the results — early when every page lands, at the deadline
     *  with whatever is ready otherwise, the stragglers keeping their
     *  LaTeX as text. */
    void startPageJobs(const Pending &pending, const QString &pageDir);

    /*! \brief One pool job delivered its image (or its failure, as a
     *  null image); on the GUI thread, per delivery. */
    void onImageReady(int requestId, int index, const QImage &image);

    /*! \brief The join: whatever images are ready by now go into the
     *  document, the rest stay LaTeX. Idempotent — the deadline and
     *  the last delivery race, and only one may win. */
    void assembleJob(int requestId);

    /*! \brief The document half of the embed, on the GUI thread: the
     *  tinted images become resources, the Markdown is rewritten with
     *  their references (a formula whose image is null keeps its
     *  LaTeX verbatim), and the sizes are fitted to the answer's
     *  font. Only this touches the QTextDocument — documents are not
     *  thread-safe, and this part is O(text + images), not
     *  O(pixels).
     *
     *  The order matters: the Markdown is written first (the import
     *  clears the document's resource table), the images are
     *  registered after it, and the sizing and the spacing go into a
     *  single edit block. */
    void embedImages(const Pending &pending, const QList<QImage> &images);

    /*! \brief Report a failure for \a requestId — through the event
     *  loop, like every emit of this class. */
    void failLater(int requestId, const QString &reason);

    /*! \brief One in-flight pixel fan-out: the request it belongs to,
     *  the images that have come back so far (null = still pending or
     *  failed — the formula keeps its LaTeX), how many of the pool
     *  jobs have delivered, and how many were started at all (a
     *  rasterizer that came out short starts fewer jobs — the join
     *  counts those, not the formula count). */
    struct RenderJob
    {
        Pending pending;
        QList<QImage> images;
        int delivered = 0;
        int submitted = 0;
    };

    /*! \brief The newest request per document. A theme switch starts a
     *  second render of the same answer in the other ink; when the
     *  older render lands after the newer one, it must not overwrite
     *  it — the formula would keep the ink of the theme that is gone. */
    struct LatestRequest
    {
        QPointer<QQuickTextDocument> document;
        int requestId = -1;
    };

    mutable QString m_engine;
    mutable QString m_rasterizer;
    mutable bool m_engineResolved = false;
    mutable bool m_rasterizerResolved = false;

    /*! \brief The pixel fan-outs in flight, looked up as the pool
     *  jobs deliver. */
    QHash<int, RenderJob> m_jobs;
    QHash<int, Pending> m_pending;

    /*! \brief The newest request per document (see LatestRequest). */
    QHash<const QQuickTextDocument *, LatestRequest> m_latest;
};

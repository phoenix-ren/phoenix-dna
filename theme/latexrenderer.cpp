// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "latexrenderer.h"

#include "mathspan.h"  // shared with the speech-side detector

#include <QDebug>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <QQuickTextDocument>

namespace {

// --- the compiled page's geometry -------------------------------------

// The pages are rasterized at 216 dpi — three pixels per point.
constexpr qreal kPixelsPerPoint = 3.0;

// The formula starts this far from the page's left edge, and the
// marker that shows its baseline this far to the left of it, in the
// margin (see buildFormulaTex).
constexpr qreal kTextLeftPt = 20.0;
constexpr qreal kMarkerOffsetPt = 10.0;

// The strip of the page the marker is searched in: the margin in
// front of the formula.
constexpr qreal kMarkerStripPt = 15.0;


// One document, every formula on its own page: a generous page so no
// formula spills onto a second one, tiny margins, and each span kept
// in its own kind (inline math stays inline-style, display stays
// display-style — read off the span's own delimiters).
//
// The formula is typeset in the ink it is to appear in — the colour of
// the text around it — and the page itself stays transparent, so the
// rasterizer hands back the ink and its coverage and nothing else: the
// formula then sits on the answer's own background, with the edges
// blended into it, exactly like the text beside it.
//
// Every formula sits in a line of its own, and a small rule that
// hangs into the left margin marks the line's baseline: the renderer
// finds the mark in the rasterized page and knows where the formula
// stands — without it, the crop to the ink throws the baseline away
// and the formula floats above the text it belongs to.
QString buildFormulaTex(const QStringList &spans, const QColor &ink)
{
    QString body;
    body += QStringLiteral("\\documentclass[11pt]{article}\n");
    body += QStringLiteral(
        "\\IfFileExists{geometry.sty}{\\usepackage[paperwidth=24cm,"
        "paperheight=16cm,left=%1pt,right=1mm,top=1mm,bottom=1mm]"
        "{geometry}}{}\n").arg(kTextLeftPt);
    // The ink, in the colour it is to appear in. The page stays
    // transparent: the formula is drawn on the answer's background,
    // not on a sheet of paper.
    body += QStringLiteral("\\IfFileExists{color.sty}{\\usepackage{color}"
                           "\\color[rgb]{%1,%2,%3}}{}\n")
                .arg(ink.redF(), 0, 'f', 4)
                .arg(ink.greenF(), 0, 'f', 4)
                .arg(ink.blueF(), 0, 'f', 4);
    body += QStringLiteral("\\IfFileExists{amsmath.sty}{\\usepackage{amsmath}}{\n"
                           "\\providecommand{\\tfrac}[2]{\\frac{#1}{#2}}\n"
                           "\\providecommand{\\dfrac}[2]{\\frac{#1}{#2}}\n"
                           "\\providecommand{\\text}[1]{\\textrm{#1}}\n"
                           "\\providecommand{\\boldsymbol}[1]{\\mathbf{#1}}\n"
                           "}\n");
    body += QStringLiteral("\\IfFileExists{amssymb.sty}{\\usepackage{amssymb}}{}\n");
    body += QString::fromUtf8(unicodeTablePreamble());
    body += QStringLiteral("\\pagestyle{empty}\\parindent0pt\n\\begin{document}\n");
    for (int i = 0; i < spans.size(); ++i) {
        // The span arrives with its delimiters; rebuild it as either
        // inline math or display math, dropping the delimiter style it
        // came in.
        const QString &span = spans.at(i);
        QString inner;
        bool isDisplay = false;
        if (span.startsWith(QLatin1String("$$"))) {
            inner = span.mid(2, span.size() - 4);
            isDisplay = true;
        } else if (span.startsWith(QLatin1String("\\["))) {
            inner = span.mid(2, span.size() - 4);
            isDisplay = true;
        } else if (span.startsWith(QLatin1String("\\("))) {
            inner = span.mid(2, span.size() - 4);
        } else {
            inner = span.mid(1, span.size() - 2);
        }
        if (i > 0) {
            body += QStringLiteral("\\newpage\n");
        }
        // The formula in the line's own flow, so that the line takes
        // the room the formula needs — a formula taller than the
        // page's top margin would otherwise stick out over the page's
        // edge and be cut off by the rasterizer.
        //
        // The mark of the baseline: a rule that hangs into the left
        // margin, its bottom edge on the line's baseline. It is out of
        // the way of the ink, and the crop never sees it.
        body += QStringLiteral(
                    "\\noindent\\llap{\\rule{0.6pt}{0.6pt}\\hspace{%1pt}}%2\n")
                    .arg(kMarkerOffsetPt)
                    .arg(isDisplay
                             ? QStringLiteral("$\\displaystyle %1$").arg(inner)
                             : QStringLiteral("$%1$").arg(inner));
    }
    body += QStringLiteral("\\end{document}\n");
    return body;
}

// The name a formula's image is registered under.
//
// A scheme-qualified URL, and that is the whole point of it: the
// document resolves a bare name against its own base URL (the QML file's
// directory), the lookup misses, and the view falls back to opening the
// name as a *file* — once per formula, per relayout. That file-I/O
// churn around broken images drowned the GUI thread and the field froze
// at 96%. An absolute URL is looked up under exactly the name that was
// registered.
QString formulaName(int index)
{
    return QStringLiteral("phoenixformula://%1").arg(index);
}

// The factor the rasterized pages are pre-scaled by: the images are
// kept at twice their logical size, so they stay crisp on a hidpi
// screen — and, more importantly, the document never sees the
// multi-megapixel page the rasterizer produced.
constexpr int kPreScale = 2;

// The markdown with every span replaced by an image reference — every
// span whose image is ready (\a ready, in span order): the ones that
// did not make the deadline keep their LaTeX as text. Display formulas
// get their own paragraph; inline ones flow in the text.
QString markdownWithImages(const QString &markdown, const QList<bool> &ready)
{
    QString result;
    result.reserve(markdown.size());
    qsizetype last = 0;
    int index = 0;
    QRegularExpressionMatchIterator it =
        mathSpanMatcher().globalMatch(markdown);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += markdown.mid(last, match.capturedStart() - last);
        const bool isReady = index < ready.size() && ready.at(index);
        if (isReady) {
            const bool display = match.capturedStart(1) >= 0
                || match.capturedStart(2) >= 0;
            const QString ref = QStringLiteral("![formula](%1)")
                                    .arg(formulaName(index));
            result += display
                ? QStringLiteral("\n\n%1\n\n").arg(ref)
                : ref;
        } else {
            // A straggler: the LaTeX stays, verbatim.
            result += match.captured(0);
        }
        // The text before the next span starts after this one: without
        // this, every span would repeat the whole text in front of it
        // (the answer, twice over).
        last = match.capturedEnd();
        ++index;
    }
    result += markdown.mid(last);
    return result;
}

// The rendered page cropped to its ink, as it comes out of the
// rasterizer: the ink in the colour it is to appear in, and its
// coverage in the alpha — the page is transparent, so there is no
// paper to take away.
//
// \a baseline receives the y of the formula's baseline inside the
// cropped image (-1 when the marker was not found). The baseline is
// what the text around the formula stands on: the crop to the ink
// alone cannot say where it was, and a formula without it floats
// above the line it belongs to.
QImage formulaImage(const QImage &page, const QColor &ink, int *baseline)
{
    // The baseline marker: a small rule in the left margin, its bottom
    // edge on the formula's baseline.
    const int markerStrip = qRound(kMarkerStripPt * kPixelsPerPoint);
    const int textLeft = qRound(kTextLeftPt * kPixelsPerPoint);
    int baselineRow = -1;
    for (int y = 0; y < page.height() && baselineRow < 0; ++y) {
        for (int x = 0; x < markerStrip && x < page.width(); ++x) {
            if (qAlpha(page.pixel(x, y)) > 0) {
                // The mark's bottom edge is the baseline: the first
                // row below it is where the text stands.
                baselineRow = y + 1;
                break;
            }
        }
    }

    int left = page.width(), top = page.height(), right = -1, bottom = -1;
    for (int y = 0; y < page.height(); ++y) {
        for (int x = textLeft; x < page.width(); ++x) {
            // The page is transparent: the ink is where the alpha is.
            if (qAlpha(page.pixel(x, y)) > 0) {
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
        }
    }
    if (right < 0) {
        if (baseline) {
            *baseline = -1;
        }
        return {};
    }
    QImage out = page.copy(left, top, right - left + 1, bottom - top + 1)
                     .convertToFormat(QImage::Format_ARGB32);
    if (baseline) {
        // The baseline's place inside the cropped image.
        *baseline = (baselineRow >= 0) ? baselineRow - top : -1;
    }
    Q_UNUSED(ink);
    return out;
}

// The image with transparent room below it, up to \a height: the room
// a formula's baseline needs under it, so that the box the text
// renderer places can put the baseline where the text's baseline is.
QImage paddedImage(const QImage &image, int height)
{
    if (height <= image.height()) {
        return image;
    }
    QImage out(image.width(), height, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.drawImage(0, 0, image);
    painter.end();
    return out;
}

} // namespace

LatexRenderer::LatexRenderer(QObject *parent) : QObject(parent) {}

bool LatexRenderer::isAvailable() const
{
    // Both halves of the pipeline: the engine that typesets the
    // formulas and the rasterizer that turns the pages into images.
    return !engine().isEmpty() && !rasterizer().isEmpty();
}

QString LatexRenderer::engine() const
{
    if (!m_engineResolved) {
        // Xe/LuaLaTeX first: the models' Unicode goes in natively.
        for (const char *candidate : {"xelatex", "lualatex", "pdflatex"}) {
            const QString name = QString::fromLatin1(candidate);
            QString path = QStandardPaths::findExecutable(name);
            if (path.isEmpty()) {
                // plasmashell can run with a trimmed PATH: probe the
                // ordinary install locations before giving up.
                for (const char *dir :
                     {"/usr/bin", "/usr/local/bin", "/opt/bin"}) {
                    const QString candidatePath =
                        QDir(QString::fromLatin1(dir)).filePath(name);
                    if (QFileInfo::exists(candidatePath)) {
                        path = candidatePath;
                        break;
                    }
                }
            }
            if (!path.isEmpty()) {
                m_engine = path;
                break;
            }
        }
        if (m_engine.isEmpty()) {
            qWarning() << "LatexRenderer: no TeX engine (xelatex/lualatex/"
                          "pdflatex) found — answers keep LaTeX verbatim;"
                          "install one, e.g. dnf install texlive-scheme-basic";
        }
        m_engineResolved = true;
    }
    return m_engine;
}

QString LatexRenderer::rasterizer() const
{
    if (!m_rasterizerResolved) {
        // pdftocairo, not pdftoppm: it keeps the page transparent, so
        // the rasterized page carries the ink's coverage in its alpha
        // and the formula can be drawn on the answer's own background.
        m_rasterizer = QStandardPaths::findExecutable(
            QStringLiteral("pdftocairo"));
        if (m_rasterizer.isEmpty()) {
            qWarning() << "LatexRenderer: pdftocairo (poppler-utils) not "
                          "found — answers keep LaTeX verbatim.";
        }
        m_rasterizerResolved = true;
    }
    return m_rasterizer;
}

QString LatexRenderer::cacheDir() const
{
    static QString dir;
    static bool pruned = false;
    if (dir.isEmpty()) {
        dir = QStandardPaths::writableLocation(
                  QStandardPaths::CacheLocation)
            + QStringLiteral("/latex");
    }
    QDir().mkpath(dir);
    if (!pruned) {
        // Compiled formulas of days past are of no value; keep the
        // cache from growing forever.
        const QFileInfoList entries = QDir(dir).entryInfoList(
            {QStringLiteral("*.tex"), QStringLiteral("*.pdf"),
             QStringLiteral("*.log"), QStringLiteral("*.aux")},
            QDir::Files);
        const QDateTime cutoff =
            QDateTime::currentDateTimeUtc().addDays(-30);
        for (const QFileInfo &entry : entries) {
            if (entry.lastModified() < cutoff) {
                QFile::remove(entry.absoluteFilePath());
            }
        }
        pruned = true;
    }
    return dir;
}

bool LatexRenderer::containsLatex(const QString &text) const
{
    if (!text.contains(QLatin1Char('$'))
        && !text.contains(QLatin1Char('\\'))) {
        return false;
    }
    // Delimited spans, or an unmistakable command with braces
    // ("\frac{...}", "\mathcal{...}", "\begin{...}").
    static const QRegularExpression bracedCommand(
        QStringLiteral("\\\\[a-zA-Z]+\\s*\\{"));
    return bracedCommand.match(text).hasMatch()
        || mathSpanMatcher().match(text).hasMatch();
}

int LatexRenderer::countFormulas(const QString &text) const
{
    if (!text.contains(QLatin1Char('$'))
        && !text.contains(QLatin1Char('\\'))) {
        return 0;
    }
    // The delimited spans — the very ones embedFormulas() compiles. A
    // braced command without delimiters is left alone by the
    // renderer, so it is not counted here either.
    int count = 0;
    QRegularExpressionMatchIterator it = mathSpanMatcher().globalMatch(text);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

void LatexRenderer::embedFormulas(int requestId,
                                  QQuickTextDocument *document,
                                  const QString &markdown,
                                  const QColor &ink,
                                  qreal fontPixelSize,
                                  qreal paragraphSpacing)
{
    Pending pending;
    pending.requestId = requestId;
    pending.document = document;
    pending.markdown = markdown;
    pending.ink = ink;
    pending.fontPixelSize = fontPixelSize;
    pending.paragraphSpacing = paragraphSpacing;

    // This request owns the document from now on: a render that is
    // already on its way for the same document must not overwrite this
    // one's result (a theme switch starts a second render in the other
    // ink).
    if (document) {
        m_latest.insert(document, LatestRequest{document, requestId});
    }

    // Collect the spans, keeping their kind.
    QRegularExpressionMatchIterator it =
        mathSpanMatcher().globalMatch(markdown);
    while (it.hasNext()) {
        pending.spans.append(it.next().captured(0));
    }
    if (pending.spans.isEmpty()) {
        // Nothing to embed: done, and nothing to fail.
        QMetaObject::invokeMethod(this, [this, requestId] {
            Q_EMIT embedded(requestId);
        }, Qt::QueuedConnection);
        return;
    }
    if (!document) {
        // Formulas without a view to land in: the caller went away
        // between the binding and the call.
        failLater(requestId,
                  QStringLiteral("No document to embed the formulas in."));
        return;
    }

    const QString engineBin = engine();
    if (engineBin.isEmpty()) {
        failLater(requestId,
                  QStringLiteral("No TeX engine found — install one (e.g. "
                                 "dnf install texlive-scheme-basic) to "
                                 "render math."));
        return;
    }
    if (rasterizer().isEmpty()) {
        failLater(requestId,
                  QStringLiteral("pdftocairo not found — install "
                                 "poppler-utils to render math."));
        return;
    }

    // Content-addressed cache over the span list: the same formulas
    // compile once, whatever text surrounds them. The ink is part of
    // the key — the formula is typeset in the colour it is to appear
    // in — and so is the version: the compiled page changed (the
    // formulas carry their baseline marker now), and the old pages are
    // not interchangeable.
    QString spanKey = QStringLiteral("ink-1:");
    spanKey += QStringLiteral("%1,%2,%3;")
                   .arg(ink.red()).arg(ink.green()).arg(ink.blue());
    for (const QString &span : pending.spans) {
        spanKey += span + QChar::Nbsp;
    }
    const QString dir = cacheDir();
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(spanKey.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex()
            .left(24));
    const QString texPath = dir + QLatin1Char('/') + hash
        + QLatin1String(".tex");
    const QString pdfPath = dir + QLatin1Char('/') + hash + QLatin1String(".pdf");
    const QString pageDir = dir + QLatin1Char('/') + hash
        + QLatin1String(".pages");
    pending.pdfPath = pdfPath;

    // Already rasterized? Straight to the embed — still through the
    // event loop (see the header).
    const QFileInfoList readyPages = QDir(pageDir).entryInfoList(
        {QStringLiteral("page-*.png")}, QDir::Files, QDir::Name);
    if (QFileInfo::exists(pdfPath) && readyPages.size() >= pending.spans.size()) {
        m_pending.insert(requestId, pending);
        const QString pages = pageDir;
        QMetaObject::invokeMethod(this, [this, requestId, pages] {
            const auto it = m_pending.constFind(requestId);
            if (it != m_pending.constEnd()) {
                startPageJobs(it.value(), pages);
                m_pending.erase(it);
            }
        }, Qt::QueuedConnection);
        return;
    }

    if (QFileInfo::exists(pdfPath)) {
        // Compiled before, but the pages are not rasterized (or the
        // formulas changed count): rasterize what is there.
        m_pending.insert(requestId, pending);
        rasterize(requestId, pdfPath);
        return;
    }

    QFile texFile(texPath);
    if (!texFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || texFile.write(buildFormulaTex(pending.spans, ink).toUtf8()) < 0) {
        failLater(requestId, texFile.errorString());
        return;
    }
    texFile.close();

    m_pending.insert(requestId, pending);
    compile(requestId, texPath, pdfPath);
}

void LatexRenderer::failLater(int requestId, const QString &reason)
{
    QMetaObject::invokeMethod(this, [this, requestId, reason] {
        Q_EMIT failed(requestId, reason);
    }, Qt::QueuedConnection);
}

void LatexRenderer::rasterize(int requestId, const QString &pdfPath)
{
    const QString pageDir =
        pdfPath.left(pdfPath.size() - 4) + QLatin1String(".pages");
    QDir().mkpath(pageDir);
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this,
            [this, requestId, pageDir, process](int exitCode,
                                                QProcess::ExitStatus status) {
                process->deleteLater();
                const auto it = m_pending.constFind(requestId);
                if (it == m_pending.constEnd()) {
                    return;
                }
                if (status == QProcess::NormalExit && exitCode == 0) {
                    startPageJobs(it.value(), pageDir);
                    m_pending.erase(it);
                    return;
                }
                m_pending.erase(it);
                failLater(requestId,
                          QStringLiteral(
                              "Rasterizing the formulas failed (pdftocairo "
                              "exit %1).")
                              .arg(exitCode));
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, requestId, process](QProcess::ProcessError) {
                process->deleteLater();
                m_pending.remove(requestId);
                failLater(requestId,
                          QStringLiteral(
                              "pdftocairo could not be started."));
            });
    // Rasterizing many pages takes their own time; a fixed short cap
    // would cut long essays off.
    QTimer::singleShot(60000, process, [this, requestId, process] {
        if (process->state() == QProcess::NotRunning) {
            return;
        }
        process->kill();
        process->deleteLater();
        m_pending.remove(requestId);
        failLater(requestId,
                  QStringLiteral("Rasterizing timed out."));
    });
    // 216 dpi = three pixels per point: crisp on hidpi, cropped to the
    // ink right after. The page stays transparent (-transp): the
    // rasterized page carries the ink's coverage in its alpha, and the
    // formula is drawn on the answer's own background.
    process->setWorkingDirectory(QFileInfo(pdfPath).absolutePath());
    process->start(rasterizer(),
                   {QStringLiteral("-png"),
                    QStringLiteral("-transp"),
                    QStringLiteral("-r"), QStringLiteral("216"),
                    QFileInfo(pdfPath).fileName(),
                    pageDir + QLatin1String("/page")});
}

void LatexRenderer::startPageJobs(const Pending &pending,
                                  const QString &pageDir)
{
    const QFileInfoList pages = QDir(pageDir).entryInfoList(
        {QStringLiteral("page-*.png")}, QDir::Files, QDir::Name);
    RenderJob job;
    job.pending = pending;
    job.images.resize(pending.spans.size());
    job.baselines.resize(pending.spans.size());
    // The jobs actually started: one per formula, but only as many as
    // there are pages. The join counts these, not the formula count —
    // a rasterizer that came out short must not hold the whole answer
    // hostage until the deadline.
    job.submitted = qMin(pending.spans.size(), pages.size());
    const int jobId = pending.requestId;
    if (job.submitted == 0) {
        failLater(jobId,
                  QStringLiteral("No formula pages came out of the render."));
        return;
    }
    m_jobs.insert(jobId, job);

    // The pages were rasterized at 216 dpi — three pixels per point —
    // and the formulas were compiled at 11pt (≈14.67px at 96dpi); the
    // answer is set at fontPixelSize logical pixels. Image pixels per
    // logical pixel:
    const qreal pixelPerLogical =
        3 * (72.0 / 96.0) / (pending.fontPixelSize / 14.67);

    // One pool job per formula: load the page, crop it to its ink,
    // tint it, scale it down to the size the document will show it at.
    // The pool is a thread per core, so an essay's formulas process in
    // parallel instead of down one worker.
    for (int i = 0; i < job.submitted; ++i) {
        const QString path = pages.at(i).absoluteFilePath();
        const QColor ink = pending.ink;
        QThreadPool::globalInstance()->start([this, jobId, i, path, ink,
                                              pixelPerLogical] {
            const QImage page(path);
            int baseline = -1;
            QImage image = page.isNull()
                    ? QImage()
                    : formulaImage(page, ink, &baseline);
            if (!image.isNull()) {
                // Pre-scale to twice the logical size: the document
                // must never see the multi-megapixel page (its layout
                // would drown in it long before the format pass got to
                // shrink it).
                const int targetHeight =
                    qRound(image.height() / pixelPerLogical) * kPreScale;
                if (targetHeight > 0) {
                    image = image.scaledToHeight(targetHeight,
                                                 Qt::SmoothTransformation);
                }
                if (baseline >= 0) {
                    // The baseline comes along with the scale.
                    baseline = qRound(baseline / pixelPerLogical) * kPreScale;
                }
            }
            // Delivery is queued to the GUI thread; if the deadline
            // assembled the job before this lands, the lookup below
            // finds nothing and the straggler is dropped.
            QMetaObject::invokeMethod(
                this,
                [this, jobId, i, image, baseline] {
                    onImageReady(jobId, i, image, baseline);
                },
                Qt::QueuedConnection);
        });
    }

    // The join deadline — the non-blocking stand-in for waiting on
    // the futures: like the compile budget it grows with the formula
    // count, and whatever is ready when it fires is what goes in.
    const int pixelBudget =
        qMin(180000, 30000 + 500 * pending.spans.size());
    QTimer::singleShot(pixelBudget, this, [this, jobId] {
        assembleJob(jobId);
    });
}

void LatexRenderer::onImageReady(int requestId, int index,
                                 const QImage &image, int baseline)
{
    auto it = m_jobs.find(requestId);
    if (it == m_jobs.end()) {
        return;   // deadline won the race; the job is already in
    }
    RenderJob &job = it.value();
    if (index >= 0 && index < job.images.size()) {
        job.images[index] = image;
        job.baselines[index] = baseline;
    }
    ++job.delivered;
    Q_EMIT progress(requestId, job.delivered, job.submitted);
    if (job.delivered >= job.submitted) {
        assembleJob(requestId);
    }
}

void LatexRenderer::assembleJob(int requestId)
{
    auto it = m_jobs.find(requestId);
    if (it == m_jobs.end()) {
        return;   // already assembled (the deadline and the last
                  // delivery race; only one may win)
    }
    RenderJob job = it.value();
    m_jobs.erase(it);

    int ready = 0;
    for (const QImage &image : std::as_const(job.images)) {
        if (!image.isNull()) {
            ++ready;
        }
    }
    if (ready == 0) {
        failLater(requestId,
                  job.submitted == 0
                      ? QStringLiteral("No formula pages came out of the "
                                       "render.")
                      : QStringLiteral("Rendering the formulas timed out."));
        return;
    }
    // A straggler keeps its LaTeX as text: embedImages only swaps the
    // formulas whose image arrived.
    embedImages(job.pending, job.images, job.baselines);
}

void LatexRenderer::embedImages(const Pending &pending,
                                const QList<QImage> &images,
                                const QList<int> &baselines)
{
    QTextDocument *doc = pending.document
        ? pending.document->textDocument()
        : nullptr;
    if (!doc) {
        // The view went away while the pipeline ran — nothing to do.
        return;
    }
    if (images.size() < pending.spans.size()) {
        failLater(pending.requestId,
                  QStringLiteral("Formula pages did not come out as "
                                 "expected."));
        return;
    }

    // A newer request for the same document owns it now (a theme switch
    // starts a second render in the other ink): this result is stale
    // and must not overwrite the newer one.
    if (pending.document) {
        const auto latest = m_latest.constFind(pending.document.data());
        if (latest != m_latest.constEnd()
            && latest->requestId != pending.requestId) {
            return;
        }
        m_latest.remove(pending.document.data());
    }

    // The stragglers keep their LaTeX: only the formulas whose image
    // arrived are swapped.
    QList<bool> ready;
    ready.reserve(images.size());
    for (const QImage &image : images) {
        ready.append(!image.isNull());
    }

    // The text's descent: how far the text reaches below its own
    // baseline. The text renderer places an image's box with its
    // bottom on the baseline plus this descent (its "align on the
    // baseline" case), so a box that reaches exactly one text descent
    // below the *formula's* baseline puts the two baselines on top of
    // each other — and the room for it is transparent padding under
    // the ink.
    const QFontMetricsF textMetrics(doc->defaultFont());
    const qreal textDescent = textMetrics.descent();

    // The images with that room under them.
    QList<QImage> prepared;
    prepared.reserve(images.size());
    for (int i = 0; i < images.size(); ++i) {
        const QImage &image = images.at(i);
        const int baseline = baselines.value(i, -1);
        if (image.isNull() || baseline < 0) {
            // No baseline to place: the image keeps its ink as it is.
            prepared.append(image);
            continue;
        }
        // How far the ink reaches below the formula's baseline, in
        // logical pixels.
        const qreal below =
            (image.height() - baseline) / qreal(kPreScale);
        if (below >= textDescent) {
            // The formula reaches deeper than the text does (a
            // fraction's denominator): its ink stays, and it sits a
            // little high — the renderer has no placement that would
            // do better.
            prepared.append(image);
            continue;
        }
        const int wanted = baseline + qRound(textDescent * kPreScale);
        prepared.append(paddedImage(image, wanted));
    }

    // The size of each image, in logical pixels: the pool scaled the
    // pages down to twice the size they are shown at, so the pixels are
    // halved here.
    QHash<QString, QSize> sizes;
    int embeddedCount = 0;
    for (int i = 0; i < pending.spans.size(); ++i) {
        const QImage &image = prepared.value(i);
        if (image.isNull()) {
            continue;
        }
        sizes.insert(formulaName(i),
                     QSize(image.width() / qreal(kPreScale),
                          image.height() / qreal(kPreScale)));
        ++embeddedCount;
    }

    // The tinted images become the document's resources — before the
    // Markdown as well as after it.
    //
    // Before: the import makes the view lay the document out, and the
    // layout looks every image up; an image it cannot find is reported
    // as an unknown protocol (once per formula, in the journal).
    //
    // After: the import clears the document's resource table
    // (QTextDocument::clear() does), so a resource registered before it
    // is simply gone when the layout looks the image up again.
    const auto registerImages = [&] {
        for (int i = 0; i < pending.spans.size(); ++i) {
            const QImage &image = prepared.value(i);
            if (image.isNull()) {
                continue;
            }
            doc->addResource(QTextDocument::ImageResource,
                             QUrl(formulaName(i)), image);
        }
    };
    registerImages();

    // The Markdown lands: the importer writes the image references into
    // the document.
    doc->setMarkdown(markdownWithImages(pending.markdown, ready));

    // ... and the images are back in the table.
    registerImages();

    if (embeddedCount == 0) {
        failLater(pending.requestId,
                  QStringLiteral("No formulas survived rendering."));
        return;
    }

    // One edit block, one walk over the blocks: the images are sized,
    // the display formulas are centred, and the paragraphs are spaced
    // as the native view spaces them. Every setCharFormat and every
    // setBlockFormat outside an edit block relayouts the whole
    // document — with a formula per line that is O(N²) relayouts, and
    // the field froze on it.
    //
    // The width the formulas are centred in: the page the document is
    // laid out on, less its margins.
    const qreal available = doc->pageSize().width()
        - 2 * doc->documentMargin();
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    bool first = true;
    for (QTextBlock block = doc->begin(); block.isValid();
         block = block.next(), first = false) {
        bool sawImage = false;
        bool textBeside = false;
        int imageWidth = 0;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.charFormat().isImageFormat()) {
                const QTextImageFormat imageFormat =
                    fragment.charFormat().toImageFormat();
                const QSize size =
                    sizes.value(imageFormat.name(), QSize());
                if (size.isValid() && size.width() > 0 && size.height() > 0) {
                    QTextImageFormat sized = imageFormat;
                    sized.setWidth(size.width());
                    sized.setHeight(size.height());
                    // The baseline of the formula, not the bottom of
                    // its ink: the box is placed on the text's
                    // baseline, and the transparent room under the ink
                    // carries the formula's own baseline down to it.
                    // The font is the text's own, so that the
                    // renderer measures the descent the same way.
                    sized.setFont(doc->defaultFont());
                    sized.setProperty(QTextFormat::TextVerticalAlignment,
                                      int(QTextCharFormat::AlignBaseline));
                    cursor.setPosition(fragment.position());
                    cursor.setPosition(fragment.position()
                                           + fragment.length(),
                                       QTextCursor::KeepAnchor);
                    cursor.setCharFormat(sized);
                    sawImage = true;
                    imageWidth = qMax(imageWidth, size.width());
                }
            } else if (!fragment.text().trimmed().isEmpty()) {
                textBeside = true;
            }
        }

        QTextBlockFormat format = block.blockFormat();
        bool changed = false;
        if (sawImage && !textBeside) {
            // A display formula, alone in its paragraph: centred.
            //
            // The alignment centres the *text* of a paragraph; an image
            // alone in a paragraph is drawn at the paragraph's left edge
            // by the Quick text renderer, so the centring is done with
            // an indent as well. (The indent follows the width the
            // answer had when it was rendered; a later resize moves the
            // text around it, not the formula.)
            format.setAlignment(Qt::AlignHCenter);
            const qreal indent =
                qMax<qreal>(0, (available - imageWidth) / 2);
            if (format.leftMargin() != indent) {
                format.setLeftMargin(indent);
            }
            changed = true;
        }
        if (!block.textList()) {
            // The same paragraph spacing the native view applies, so
            // the answer's rhythm survives the formula pass: a heading
            // opens its section with more room above and keeps less
            // below, just as it does there.
            const bool last = !block.next().isValid();
            const bool heading = format.headingLevel() > 0;
            const qreal top = first ? 0
                                    : (heading ? pending.paragraphSpacing
                                               : pending.paragraphSpacing / 2);
            const qreal bottom = last ? 0
                                      : (heading ? pending.paragraphSpacing / 4
                                                 : pending.paragraphSpacing / 2);
            if (format.topMargin() != top || format.bottomMargin() != bottom) {
                format.setTopMargin(top);
                format.setBottomMargin(bottom);
                changed = true;
            }
        }
        if (changed) {
            cursor.setPosition(block.position());
            cursor.setBlockFormat(format);
        }
    }
    cursor.endEditBlock();

    QMetaObject::invokeMethod(this, [this, pending] {
        Q_EMIT embedded(pending.requestId);
    }, Qt::QueuedConnection);
}

void LatexRenderer::compile(int requestId, const QString &texPath,
                            const QString &pdfPath)
{
    const QString engineBin = engine();
    auto *process = new QProcess(this);
    // One failure report per run, whichever of the three signals brings
    // the news (start failure, kill, or a nonzero exit).
    auto *done = new bool(false);
    connect(process, &QObject::destroyed, this, [done] { delete done; });
    connect(process, &QProcess::finished, this,
            [this, requestId, texPath, pdfPath, process, done](
                int exitCode, QProcess::ExitStatus status) {
                if (*done) {
                    return;
                }
                *done = true;
                process->deleteLater();
                if (status == QProcess::NormalExit && exitCode == 0
                    && QFileInfo::exists(pdfPath)) {
                    // Typeset; now the pages become images.
                    rasterize(requestId, pdfPath);
                    return;
                }
                // The engine's first complaint from the log, when there
                // is one worth showing.
                QString reason =
                    QStringLiteral("LaTeX failed (exit %1)").arg(exitCode);
                QFile logFile(texPath.left(texPath.size() - 4)
                              + QLatin1String(".log"));
                if (logFile.open(QIODevice::ReadOnly)) {
                    const QStringList logLines = QString::fromUtf8(
                        logFile.readAll())
                        .split(QLatin1Char('\n'));
                    for (const QString &line : logLines) {
                        if (line.startsWith(QLatin1Char('!'))) {
                            reason = line.mid(1).trimmed();
                            break;
                        }
                    }
                }
                qWarning() << "LatexRenderer: compile failed for" << texPath
                           << "-" << reason;
                m_pending.remove(requestId);
                failLater(requestId, reason);
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, requestId, process, done](QProcess::ProcessError error) {
                if (*done) {
                    return;
                }
                *done = true;
                process->deleteLater();
                m_pending.remove(requestId);
                failLater(requestId,
                          error == QProcess::FailedToStart
                              ? QStringLiteral(
                                    "The TeX engine could not be started.")
                              : process->errorString());
            });
    // A stuck compile must not haunt the process table forever — but
    // an essay's formulas are many pages, and the engine needs a
    // moment per page: the cap grows with the formula count.
    const int compileBudget = qMin(
        180000, 30000 + 400 * m_pending.value(requestId).spans.size());
    QTimer::singleShot(compileBudget, process, [this, requestId, process, done] {
        if (*done) {
            return;
        }
        *done = true;
        process->kill();
        process->deleteLater();
        m_pending.remove(requestId);
        failLater(requestId,
                  QStringLiteral("LaTeX timed out after 30 s"));
    });
    process->setWorkingDirectory(QFileInfo(texPath).absolutePath());
    process->start(engineBin,
                   {QStringLiteral("-interaction=batchmode"),
                    QStringLiteral("-halt-on-error"),
                    QFileInfo(texPath).fileName()});
}

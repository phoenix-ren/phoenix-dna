// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "latexrenderer.h"

#include <QDebug>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTimer>
#include <QUrl>

#include <QQuickTextDocument>

namespace {

// --- math-span detection (shared by containsLatex and the embedder) ------

// All span kinds in one alternation, display delimiters first: a "$$"
// can never be misread as two "$" spans. The inline "$…$" must hug its
// content, so "$5 and $10" stays currency.
const QRegularExpression &mathSpanMatcher()
{
    static const QRegularExpression matcher(QStringLiteral(
        "\\\\\\[([\\s\\S]*?)\\\\\\]"
        "|\\$\\$([\\s\\S]*?)\\$\\$"
        "|\\\\\\((.+?)\\\\\\)"
        "|\\$(\\S(?:[^$\\n]*\\S)?)\\$"));
    return matcher;
}

// The Unicode symbols the models write (∫, α, → …) mapped to their math
// commands — kept in the compile so a formula mixing Unicode and LaTeX
// still typesets. Gated and inert when the package is missing.
const char *unicodeTablePreamble()
{
    return R"TEX(\IfFileExists{newunicodechar.sty}{\usepackage{newunicodechar}}{}
\providecommand{\newunicodechar}[2]{}
\newunicodechar{×}{\ensuremath{\times}}
\newunicodechar{⋅}{\ensuremath{\cdot}}
\newunicodechar{·}{\ensuremath{\cdot}}
\newunicodechar{≤}{\ensuremath{\leq}}
\newunicodechar{≥}{\ensuremath{\geq}}
\newunicodechar{≠}{\ensuremath{\neq}}
\newunicodechar{≈}{\ensuremath{\approx}}
\newunicodechar{≡}{\ensuremath{\equiv}}
\newunicodechar{∝}{\ensuremath{\propto}}
\newunicodechar{∈}{\ensuremath{\in}}
\newunicodechar{∉}{\ensuremath{\notin}}
\newunicodechar{∞}{\ensuremath{\infty}}
\newunicodechar{∂}{\ensuremath{\partial}}
\newunicodechar{∇}{\ensuremath{\nabla}}
\newunicodechar{√}{\ensuremath{\sqrt}}
\newunicodechar{∫}{\ensuremath{\int}}
\newunicodechar{∮}{\ensuremath{\oint}}
\newunicodechar{∑}{\ensuremath{\sum}}
\newunicodechar{∏}{\ensuremath{\prod}}
\newunicodechar{→}{\ensuremath{\rightarrow}}
\newunicodechar{←}{\ensuremath{\leftarrow}}
\newunicodechar{↔}{\ensuremath{\leftrightarrow}}
\newunicodechar{⇒}{\ensuremath{\Rightarrow}}
\newunicodechar{⇐}{\ensuremath{\Leftarrow}}
\newunicodechar{⇔}{\ensuremath{\Leftrightarrow}}
\newunicodechar{⟶}{\ensuremath{\longrightarrow}}
\newunicodechar{↦}{\ensuremath{\mapsto}}
\newunicodechar{ℏ}{\ensuremath{\hbar}}
\newunicodechar{ℓ}{\ensuremath{\ell}}
\newunicodechar{ℝ}{\ensuremath{\mathbb{R}}}
\newunicodechar{ℕ}{\ensuremath{\mathbb{N}}}
\newunicodechar{ℤ}{\ensuremath{\mathbb{Z}}}
\newunicodechar{ℚ}{\ensuremath{\mathbb{Q}}}
\newunicodechar{ℂ}{\ensuremath{\mathbb{C}}}
\newunicodechar{′}{\ensuremath{\prime}}
\newunicodechar{″}{\ensuremath{\prime\prime}}
\newunicodechar{α}{\ensuremath{\alpha}}
\newunicodechar{β}{\ensuremath{\beta}}
\newunicodechar{γ}{\ensuremath{\gamma}}
\newunicodechar{δ}{\ensuremath{\delta}}
\newunicodechar{ε}{\ensuremath{\varepsilon}}
\newunicodechar{θ}{\ensuremath{\theta}}
\newunicodechar{λ}{\ensuremath{\lambda}}
\newunicodechar{μ}{\ensuremath{\mu}}
\newunicodechar{π}{\ensuremath{\pi}}
\newunicodechar{ρ}{\ensuremath{\rho}}
\newunicodechar{σ}{\ensuremath{\sigma}}
\newunicodechar{τ}{\ensuremath{\tau}}
\newunicodechar{φ}{\ensuremath{\varphi}}
\newunicodechar{ω}{\ensuremath{\omega}}
\newunicodechar{Δ}{\ensuremath{\Delta}}
\newunicodechar{Ω}{\ensuremath{\Omega}}
\newunicodechar{Γ}{\ensuremath{\Gamma}}
\newunicodechar{Σ}{\ensuremath{\Sigma}}
\newunicodechar{Θ}{\ensuremath{\Theta}}
)TEX";
}

// One document, every formula on its own page: a generous page so no
// formula spills onto a second one, tiny margins, and each span kept
// in its own kind (inline math stays inline-style, display stays
// display-style — read off the span's own delimiters).
QString buildFormulaTex(const QStringList &spans)
{
    QString body;
    body += QStringLiteral("\\documentclass[11pt]{article}\n");
    body += QStringLiteral(
        "\\IfFileExists{geometry.sty}{\\usepackage[paperwidth=24cm,"
        "paperheight=16cm,margin=1mm]{geometry}}{}\n");
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
        body += isDisplay ? QStringLiteral("\\[%1\\]\n").arg(inner)
                          : QStringLiteral("$%1$\n").arg(inner);
    }
    body += QStringLiteral("\\end{document}\n");
    return body;
}

// The markdown with every span replaced by an image reference. Display
// formulas get their own paragraph; inline ones flow in the text.
QString markdownWithImages(const QString &markdown)
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
        const bool display = match.capturedStart(1) >= 0
            || match.capturedStart(2) >= 0;
        const QString ref =
            QStringLiteral("![formula](phoenixformula-%1)").arg(index);
        result += display ? QStringLiteral("\n\n%1\n\n").arg(ref) : ref;
        ++index;
    }
    result += markdown.mid(last);
    return result;
}

// The rendered page cropped to its ink (anything darker than paper),
// as a tinted alpha mask: the formula's coverage becomes transparency,
// the colour is \a ink, the background vanishes.
QImage formulaImage(const QImage &page, const QColor &ink)
{
    int left = page.width(), top = page.height(), right = -1, bottom = -1;
    for (int y = 0; y < page.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(page.scanLine(y));
        for (int x = 0; x < page.width(); ++x) {
            const QRgb px = line[x];
            // LaTeX ink is black on white; anti-aliased edges are in
            // between, so anything clearly darker than paper is ink.
            if (qGray(px) < 200) {
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
        }
    }
    if (right < 0) {
        return {};
    }
    QImage out = page.copy(left, top, right - left + 1, bottom - top + 1)
                     .convertToFormat(QImage::Format_ARGB32);
    const QRgb inkRgb = qRgba(ink.red(), ink.green(), ink.blue(), 255);
    for (int y = 0; y < out.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            // Luminance → alpha: black ink is opaque, paper is gone.
            const int alpha = 255 - qGray(line[x]);
            line[x] = qRgba(qRed(inkRgb), qGreen(inkRgb), qBlue(inkRgb),
                            alpha);
        }
    }
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
        m_rasterizer = QStandardPaths::findExecutable(
            QStringLiteral("pdftoppm"));
        if (m_rasterizer.isEmpty()) {
            qWarning() << "LatexRenderer: pdftoppm (poppler-utils) not "
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
                  QStringLiteral("pdftoppm not found — install "
                                 "poppler-utils to render math."));
        return;
    }

    // Content-addressed cache over the span list: the same formulas
    // compile once, whatever text surrounds them.
    QString spanKey;
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
                finishEmbed(it.value(), pages);
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
        || texFile.write(buildFormulaTex(pending.spans).toUtf8()) < 0) {
        failLater(requestId, texFile.errorString());
        return;
    }
    texFile.close();

    m_pending.insert(requestId, pending);
    compile(requestId, texPath, pdfPath);
}

void LatexRenderer::finishEmbed(const Pending &pending,
                                const QString &pageDir)
{
    QTextDocument *doc = pending.document
        ? pending.document->textDocument()
        : nullptr;
    if (!doc) {
        // The view went away while the pipeline ran — nothing to do.
        return;
    }

    // One PNG per formula, in page order.
    const QFileInfoList pages = QDir(pageDir).entryInfoList(
        {QStringLiteral("page-*.png")}, QDir::Files, QDir::Name);
    if (pages.size() < pending.spans.size()) {
        failLater(pending.requestId,
                  QStringLiteral("Formula pages did not come out as "
                                 "expected."));
        return;
    }

    // Register every tinted image before the markdown lands, so the
    // importer finds them. The pages were rasterized at 216 dpi —
    // three pixels per point — and the formulas were compiled at 11pt
    // (≈14.67px at 96dpi); the answer is set at fontPixelSize logical
    // pixels. Image pixels per logical pixel:
    const qreal renderScale = 3;
    const qreal pixelPerLogical =
        renderScale * (72.0 / 96.0) / (pending.fontPixelSize / 14.67);
    int embeddedCount = 0;
    for (int i = 0; i < pending.spans.size(); ++i) {
        QImage page(pages.at(i).absoluteFilePath());
        if (page.isNull()) {
            continue;
        }
        const QImage image = formulaImage(page, pending.ink);
        if (image.isNull()) {
            continue;
        }
        doc->addResource(QTextDocument::ImageResource,
                         QUrl(QStringLiteral("phoenixformula-%1").arg(i)),
                         image);
        ++embeddedCount;
    }
    if (embeddedCount == 0) {
        failLater(pending.requestId,
                  QStringLiteral("No formulas survived rendering."));
        return;
    }

    doc->setMarkdown(markdownWithImages(pending.markdown));

    // Second pass: scale the images (the importer takes the raw pixel
    // size), and centre the display formulas' paragraphs.
    QTextCursor cursor(doc);
    for (QTextBlock block = doc->begin(); block.isValid();
         block = block.next()) {
        bool sawImage = false;
        bool textBeside = false;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.charFormat().isImageFormat()) {
                QTextImageFormat imageFormat =
                    fragment.charFormat().toImageFormat();
                const int width =
                    qRound(imageFormat.width() / pixelPerLogical);
                const int height =
                    qRound(imageFormat.height() / pixelPerLogical);
                if (width > 0 && height > 0) {
                    imageFormat.setWidth(width);
                    imageFormat.setHeight(height);
                    cursor.setPosition(fragment.position());
                    cursor.setPosition(fragment.position()
                                           + fragment.length(),
                                       QTextCursor::KeepAnchor);
                    cursor.setCharFormat(imageFormat);
                    sawImage = true;
                }
            } else if (!fragment.text().trimmed().isEmpty()) {
                textBeside = true;
            }
        }
        if (sawImage && !textBeside) {
            // A display formula, alone in its paragraph: centred.
            QTextBlockFormat centre = block.blockFormat();
            centre.setAlignment(Qt::AlignHCenter);
            cursor.setPosition(block.position());
            cursor.setBlockFormat(centre);
        }
    }

    // The same paragraph spacing the native view applies, so the
    // answer's rhythm survives the formula pass.
    for (QTextBlock block = doc->begin(); block.isValid();
         block = block.next()) {
        if (block.textList()) {
            continue;
        }
        const bool first = block == doc->begin();
        const bool last = !block.next().isValid();
        QTextBlockFormat format = block.blockFormat();
        const qreal top = first ? 0 : pending.paragraphSpacing / 2;
        const qreal bottom = last ? 0 : pending.paragraphSpacing / 2;
        if (format.topMargin() == top && format.bottomMargin() == bottom) {
            continue;
        }
        format.setTopMargin(top);
        format.setBottomMargin(bottom);
        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);
    }

    QMetaObject::invokeMethod(this, [this, pending] {
        Q_EMIT embedded(pending.requestId);
    }, Qt::QueuedConnection);
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
                    finishEmbed(it.value(), pageDir);
                    m_pending.erase(it);
                    return;
                }
                m_pending.erase(it);
                failLater(requestId,
                          QStringLiteral(
                              "Rasterizing the formulas failed (pdftoppm "
                              "exit %1).")
                              .arg(exitCode));
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, requestId, process](QProcess::ProcessError) {
                process->deleteLater();
                m_pending.remove(requestId);
                failLater(requestId,
                          QStringLiteral(
                              "pdftoppm could not be started."));
            });
    QTimer::singleShot(10000, process, [this, requestId, process] {
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
    // ink right after.
    process->setWorkingDirectory(QFileInfo(pdfPath).absolutePath());
    process->start(rasterizer(),
                   {QStringLiteral("-png"),
                    QStringLiteral("-r"), QStringLiteral("216"),
                    QFileInfo(pdfPath).fileName(),
                    pageDir + QLatin1String("/page")});
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
    // A stuck compile must not haunt the process table forever.
    QTimer::singleShot(30000, process, [this, requestId, process, done] {
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

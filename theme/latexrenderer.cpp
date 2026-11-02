// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "latexrenderer.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

namespace {

// --- math-span detection (shared by containsLatex and the converter) ------

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

// Replaces every match of \a re in \a text with what \a transform makes
// of it.
QString replaceMatches(
    const QString &text, const QRegularExpression &re,
    const std::function<QString(const QRegularExpressionMatch &)> &transform)
{
    QString result;
    result.reserve(text.size());
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += text.mid(last, match.capturedStart() - last);
        result += transform(match);
        last = match.capturedEnd();
    }
    result += text.mid(last);
    return result;
}

// --- LaTeX escaping ---------------------------------------------------------

// LaTeX specials → safe spellings. The backslash goes first (through a
// placeholder, so the backslashes the other replacements add are not
// re-escaped), and the two awkward ones get their text commands.
QString escapeLatexText(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\x07"));
    escaped.replace(QLatin1Char('&'), QStringLiteral("\\&"));
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    escaped.replace(QLatin1Char('#'), QStringLiteral("\\#"));
    escaped.replace(QLatin1Char('_'), QStringLiteral("\\_"));
    escaped.replace(QLatin1Char('{'), QStringLiteral("\\{"));
    escaped.replace(QLatin1Char('}'), QStringLiteral("\\}"));
    escaped.replace(QLatin1Char('~'), QStringLiteral("\\textasciitilde{}"));
    escaped.replace(QLatin1Char('^'), QStringLiteral("\\textasciicircum{}"));
    escaped.replace(QStringLiteral("\x07"),
                    QStringLiteral("\\textbackslash{}"));
    return escaped;
}

// A URL for \href: only the characters LaTeX would treat as syntax.
QString escapeLatexUrl(const QString &url)
{
    QString escaped = url;
    escaped.replace(QLatin1Char('%'), QStringLiteral("\\%"));
    escaped.replace(QLatin1Char('#'), QStringLiteral("\\#"));
    escaped.replace(QLatin1Char('&'), QStringLiteral("\\&"));
    return escaped;
}

// The body of one prose line, with its inline constructs converted.
// Math and code spans arrive masked (private-use sentinels) and pass
// through untouched; the caller restores them.
QString inlineToLatex(const QString &line)
{
    QString out = escapeLatexText(line);
    // Emphasis on the escaped text: the asterisks are not LaTeX
    // specials, so they survived the escaping above; bold first, so
    // its markers are gone when the italics pass runs.
    out.replace(QRegularExpression(QStringLiteral("\\*\\*([^*\n]+)\\*\\*")),
                QStringLiteral("\\textbf{\\1}"));
    out.replace(QRegularExpression(QStringLiteral("\\*([^*\n]+)\\*")),
                QStringLiteral("\\emph{\\1}"));
    // Links: [text](url) → \href{url}{text}.
    return replaceMatches(
        out,
        QRegularExpression(QStringLiteral("\\[([^\\]\\n]+)\\]\\(([^)\\n]+)\\)")),
        [](const QRegularExpressionMatch &m) {
            return QStringLiteral("\\href{%1}{%2}")
                .arg(escapeLatexUrl(m.captured(2)), m.captured(1));
        });
}

// One prose run (fenced code already split off): its math spans are
// passed through verbatim, its markdown structure (headings, lists,
// quotes, rules, paragraphs, emphasis, links, code) is converted, and
// everything else is escaped, so a stray special character cannot fail
// the compile.
QString transformProseBlock(const QString &block)
{
    // Mask code and math spans: private-use sentinels survive the
    // escaping and the structure passes, and the raw contents come
    // back at the end — code LaTeX-escaped in \texttt, math verbatim.
    QStringList codeSpans;
    QStringList mathSpans;
    QString masked;
    masked.reserve(block.size());
    qsizetype last = 0;
    QRegularExpressionMatchIterator it =
        QRegularExpression(QStringLiteral("`[^`\\n]+`")).globalMatch(block);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        masked += block.mid(last, match.capturedStart() - last);
        masked += QStringLiteral("\x01%1\x01").arg(codeSpans.size());
        codeSpans.append(match.captured(0).mid(1, match.capturedLength(0) - 2));
        last = match.capturedEnd();
    }
    it = mathSpanMatcher().globalMatch(block);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        masked += block.mid(last, match.capturedStart() - last);
        masked += QStringLiteral("\x02%1\x02").arg(mathSpans.size());
        mathSpans.append(match.captured(0));
        last = match.capturedEnd();
    }
    masked += block.mid(last);

    // Parse the masked lines into LaTeX blocks.
    QStringList out;
    QStringList paragraphLines;
    enum class Mode { None, Itemize, Enumerate, Quote };
    Mode mode = Mode::None;
    const auto closeList = [&]() {
        if (mode == Mode::Itemize) {
            out << QStringLiteral("\\end{itemize}");
        } else if (mode == Mode::Enumerate) {
            out << QStringLiteral("\\end{enumerate}");
        } else if (mode == Mode::Quote) {
            out << QStringLiteral("\\end{quote}");
        }
        mode = Mode::None;
    };
    const auto ensureBlank = [&]() {
        // LaTeX paragraphs are separated by blank lines.
        if (!out.isEmpty() && !out.last().isEmpty()) {
            out << QString();
        }
    };
    const auto flushParagraph = [&]() {
        if (paragraphLines.isEmpty()) {
            return;
        }
        // A single newline in LaTeX is a space: the soft-wrapped lines
        // of one markdown paragraph become one LaTeX paragraph.
        ensureBlank();
        out << inlineToLatex(paragraphLines.join(QLatin1Char(' ')));
        paragraphLines.clear();
    };

    static const QRegularExpression ruleLine(
        QStringLiteral("^([-*_=])\\1{2,}\\s*$"));
    static const QRegularExpression headingLine(
        QStringLiteral("^(#{1,4})\\s+(.*)$"));
    static const QRegularExpression bulletLine(
        QStringLiteral("^[-*+]\\s+(.+)$"));
    static const QRegularExpression numberedLine(
        QStringLiteral("^\\d{1,3}[.)]\\s+(.+)$"));
    static const QRegularExpression quoteLine(QStringLiteral("^>\\s?(.*)$"));

    for (const QString &line : std::as_const(masked).split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            flushParagraph();
            closeList();
            continue;
        }
        if (ruleLine.match(trimmed).hasMatch()) {
            flushParagraph();
            closeList();
            out << QStringLiteral(
                "\\noindent\\rule{\\linewidth}{0.4pt}");
            continue;
        }
        const auto heading = headingLine.match(trimmed);
        if (heading.hasMatch()) {
            flushParagraph();
            closeList();
            static const QStringList commands = {
                QStringLiteral("section"), QStringLiteral("subsection"),
                QStringLiteral("subsubsection"),
                QStringLiteral("paragraph"),
            };
            const int level = qBound(1, heading.captured(1).size(), 4);
            out << QStringLiteral("\\%1*{%2}")
                       .arg(commands.at(level - 1),
                            inlineToLatex(heading.captured(2)));
            continue;
        }
        const auto bullet = bulletLine.match(trimmed);
        if (bullet.hasMatch()) {
            flushParagraph();
            if (mode != Mode::Itemize) {
                closeList();
                ensureBlank();
                out << QStringLiteral("\\begin{itemize}");
                mode = Mode::Itemize;
            }
            out << QStringLiteral("\\item %1")
                       .arg(inlineToLatex(bullet.captured(1)));
            continue;
        }
        const auto numbered = numberedLine.match(trimmed);
        if (numbered.hasMatch()) {
            flushParagraph();
            if (mode != Mode::Enumerate) {
                closeList();
                ensureBlank();
                out << QStringLiteral("\\begin{enumerate}");
                mode = Mode::Enumerate;
            }
            out << QStringLiteral("\\item %1")
                       .arg(inlineToLatex(numbered.captured(1)));
            continue;
        }
        const auto quote = quoteLine.match(trimmed);
        if (quote.hasMatch()) {
            flushParagraph();
            if (mode != Mode::Quote) {
                closeList();
                ensureBlank();
                out << QStringLiteral("\\begin{quote}");
                mode = Mode::Quote;
            }
            out << inlineToLatex(quote.captured(1));
            continue;
        }
        if (mode != Mode::None) {
            closeList();
        }
        paragraphLines << trimmed;
    }
    flushParagraph();
    closeList();

    QString body = out.join(QLatin1Char('\n'));
    // Restore the masked spans: math verbatim (its delimiters included
    // — LaTeX understands all four kinds), code escaped in \texttt.
    for (int i = mathSpans.size() - 1; i >= 0; --i) {
        body.replace(QStringLiteral("\x02%1\x02").arg(i), mathSpans.at(i));
    }
    for (int i = codeSpans.size() - 1; i >= 0; --i) {
        body.replace(QStringLiteral("\x01%1\x01").arg(i),
                     QStringLiteral("\\texttt{%1}")
                         .arg(escapeLatexText(codeSpans.at(i))));
    }
    return body;
}

// The document preamble: geometry, math packages, hyperref for links,
// and a table mapping the Unicode symbols the models emit (∫, α, → …)
// to their math commands. Every optional package is gated with
// IfFileExists and gets a providecommand fallback, so a minimal TeX
// install still compiles — the mapping table simply degrades to a
// no-op and an uncovered symbol turns into a blank instead of a
// failed answer.
const char *documentPreamble()
{
    return R"TEX(\documentclass[11pt]{article}
\IfFileExists{geometry.sty}{\usepackage[a4paper,margin=1.8cm]{geometry}}{}
\IfFileExists{amsmath.sty}{\usepackage{amsmath}}{
% Without amsmath, the amsmath-only commands the models write have
% plain LaTeX equivalents good enough for these answers.
\providecommand{\tfrac}[2]{\frac{#1}{#2}}
\providecommand{\dfrac}[2]{\frac{#1}{#2}}
\providecommand{\text}[1]{\textrm{#1}}
\providecommand{\boldsymbol}[1]{\mathbf{#1}}
}
\IfFileExists{amssymb.sty}{\usepackage{amssymb}}{}
\IfFileExists{newunicodechar.sty}{\usepackage{newunicodechar}}{}
\providecommand{\newunicodechar}[2]{}
\IfFileExists{lmodern.sty}{\usepackage{lmodern}}{}
\IfFileExists{hyperref.sty}{\usepackage{hyperref}}{}
\providecommand{\href}[2]{#2}
\pagestyle{empty}
\setlength{\parindent}{0pt}
\setlength{\parskip}{0.55em}
\emergencystretch=3em
\newunicodechar{×}{\ensuremath{\times}}
\newunicodechar{÷}{\ensuremath{\div}}
\newunicodechar{±}{\ensuremath{\pm}}
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
\newunicodechar{∧}{\ensuremath{\land}}
\newunicodechar{∨}{\ensuremath{\lor}}
\newunicodechar{¬}{\ensuremath{\neg}}
\newunicodechar{∀}{\ensuremath{\forall}}
\newunicodechar{∃}{\ensuremath{\exists}}
\newunicodechar{∅}{\ensuremath{\varnothing}}
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
\newunicodechar{…}{\textellipsis}
\newunicodechar{–}{\textendash}
\newunicodechar{—}{\textemdash}
\newunicodechar{‘}{\textquoteleft}
\newunicodechar{’}{\textquoteright}
\newunicodechar{“}{\textquotedblleft}
\newunicodechar{”}{\textquotedblright}
\begin{document}
\begin{flushleft}
)TEX";
}

const char *documentPostamble()
{
    return "\n\\end{flushleft}\n\\end{document}\n";
}

} // namespace

LatexRenderer::LatexRenderer(QObject *parent) : QObject(parent) {}

bool LatexRenderer::isAvailable() const
{
    return !engine().isEmpty();
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
        // Compiled answers of days past are of no value; keep the
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

QString LatexRenderer::toLatexBody(const QString &markdown) const
{
    QStringList body;
    QStringList proseLines;
    QStringList fenceLines;
    bool inFence = false;
    const auto flushProse = [&]() {
        if (proseLines.isEmpty()) {
            return;
        }
        body << transformProseBlock(proseLines.join(QLatin1Char('\n')));
        proseLines.clear();
    };
    for (const QString &line : markdown.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("```"))
            || trimmed.startsWith(QStringLiteral("~~~"))) {
            if (!inFence) {
                flushProse();
                inFence = true;
                fenceLines.clear();
            } else {
                inFence = false;
                if (!fenceLines.isEmpty()) {
                    body << QStringLiteral("\\begin{verbatim}");
                    body << fenceLines;
                    body << QStringLiteral("\\end{verbatim}");
                }
                fenceLines.clear();
            }
            continue;
        }
        if (inFence) {
            fenceLines << line;
        } else {
            proseLines << line;
        }
    }
    flushProse();
    // An unclosed fence keeps its content verbatim, too.
    if (!fenceLines.isEmpty()) {
        body << QStringLiteral("\\begin{verbatim}");
        body << fenceLines;
        body << QStringLiteral("\\end{verbatim}");
    }
    return body.join(QLatin1Char('\n'));
}

void LatexRenderer::render(int requestId, const QString &markdown)
{
    const QString engineBin = engine();
    if (engineBin.isEmpty()) {
        // Delivered asynchronously: a synchronous emit from inside the
        // invokable never reaches QML handlers when the call was made
        // from some contexts (a Repeater delegate, observed in the
        // field) — every emit of this class goes through the event
        // loop for that reason.
        QMetaObject::invokeMethod(
            this, [this, requestId] {
                Q_EMIT failed(requestId,
                              QStringLiteral(
                                  "No TeX engine found — install one "
                                  "(e.g. dnf install texlive-scheme-basic) "
                                  "to render math."));
            },
            Qt::QueuedConnection);
        return;
    }

    // Content-addressed cache: the same answer compiles once, and a
    // reopened window finds its PDF already there.
    const QString dir = cacheDir();
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(markdown.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex()
            .left(24));
    const QString texPath = dir + QLatin1Char('/') + hash
        + QLatin1String(".tex");
    const QString pdfPath = dir + QLatin1Char('/') + hash
        + QLatin1String(".pdf");
    if (QFileInfo::exists(pdfPath)) {
        // Cache hit: same rule as above — the emit goes through the
        // event loop, never synchronously out of the invokable.
        QMetaObject::invokeMethod(
            this, [this, requestId, pdfPath] {
                Q_EMIT rendered(requestId, QUrl::fromLocalFile(pdfPath));
            },
            Qt::QueuedConnection);
        return;
    }

    QString document = QString::fromUtf8(documentPreamble());
    // pdflatex has no native UTF-8: declare the encoding (a symbol
    // outside the table still fails the compile → verbatim fallback).
    if (QFileInfo(engineBin).fileName().startsWith(
            QStringLiteral("pdflatex"))) {
        document.insert(document.indexOf(QLatin1Char('\n')),
                        QStringLiteral(
                            "\n\\usepackage[utf8]{inputenc}\n\\usepackage[T1]"
                            "{fontenc}"));
    }
    document += toLatexBody(markdown);
    document += QString::fromUtf8(documentPostamble());

    QFile texFile(texPath);
    if (!texFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || texFile.write(document.toUtf8()) < 0) {
        Q_EMIT failed(requestId, texFile.errorString());
        return;
    }
    texFile.close();

    compile(requestId, engineBin, texPath, pdfPath);
}

void LatexRenderer::compile(int requestId, const QString &engineBin,
                            const QString &texPath, const QString &pdfPath)
{
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
                    Q_EMIT rendered(requestId,
                                    QUrl::fromLocalFile(pdfPath));
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
                Q_EMIT failed(requestId, reason);
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, requestId, process, done](QProcess::ProcessError error) {
                if (*done) {
                    return;
                }
                *done = true;
                process->deleteLater();
                Q_EMIT failed(requestId,
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
        Q_EMIT failed(requestId,
                      QStringLiteral("LaTeX timed out after 30 s"));
    });
    process->setWorkingDirectory(QFileInfo(texPath).absolutePath());
    process->start(engineBin,
                   {QStringLiteral("-interaction=batchmode"),
                    QStringLiteral("-halt-on-error"),
                    QFileInfo(texPath).fileName()});
}

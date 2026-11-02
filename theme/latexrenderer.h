// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

class QProcess;

/*!
 * \brief Compiles the agent's finished answers (Markdown + LaTeX math)
 *  to PDF via a LaTeX toolchain.
 *
 * Reached from QML as \c LatexRenderer. While an answer streams, the
 * view shows its Markdown with the math verbatim; when the run
 * finishes, the view asks this class to render the whole answer. The
 * answer's Markdown is converted to a LaTeX document — the math spans
 * pass through verbatim, since LaTeX *is* what the model writes — and
 * a detected engine (xelatex, lualatex, pdflatex, in that order:
 * Xe/LuaLaTeX handle the models' Unicode natively) compiles it to PDF
 * in the cache directory. rendered() delivers the finished file, and
 * the view swaps itself for a PDF viewer. On failure (no engine
 * installed, a formula the model wrote that LaTeX rejects, a timeout)
 * failed() says so, and the view simply keeps the verbatim Markdown.
 *
 * Rendered PDFs are cached by content hash, so revisiting the same
 * answer is instant and repeated answers compile once.
 */
class LatexRenderer : public QObject
{
    Q_OBJECT
    /*! \brief Whether a TeX engine was found on the PATH. When false,
     *  the views keep the verbatim Markdown and never ask for a PDF. */
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

    /*! \brief The Markdown answer as the body of a LaTeX document:
     *  headings, lists, emphasis, quotes, rules and code fences
     *  converted, the math spans passed through verbatim, everything
     *  else escaped so a stray special character cannot fail the
     *  compile. Exposed for testing. */
    Q_INVOKABLE QString toLatexBody(const QString &markdown) const;

    /*! \brief Compiles \a markdown to PDF, asynchronously. Emits
     *  rendered() with \a requestId when the file is ready, or
     *  failed() when the engine is missing, the compile fails or the
     *  run exceeds thirty seconds. */
    Q_INVOKABLE void render(int requestId, const QString &markdown);

Q_SIGNALS:
    /*! \brief \a requestId finished: \a pdf holds the compiled file. */
    void rendered(int requestId, const QUrl &pdf);

    /*! \brief \a requestId could not be compiled; \a reason carries
     *  the engine's first complaint when there is one. */
    void failed(int requestId, const QString &reason);

private:
    /*! \brief The detected engine executable, or empty. */
    QString engine() const;

    /*! \brief The cache directory holding the .tex/.pdf files,
     *  created (and pruned of month-old files) on demand. */
    QString cacheDir() const;

    /*! \brief Runs the engine on \a texPath, asynchronously. */
    void compile(int requestId, const QString &engineBin,
                 const QString &dir, const QString &texPath,
                 const QString &pdfPath);

    mutable QString m_engine;
    mutable bool m_engineResolved = false;
};

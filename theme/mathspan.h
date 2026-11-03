// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/*!
 * \brief The math-span vocabulary shared by the renderer and the tests.
 *
 * These two definitions used to live in latexrenderer.cpp's anonymous
 * namespace. They are the *contract* between the window that renders an
 * answer's math and the daemon that speaks it
 * (agent/phoenix_dna/speech/mathspan.py is a line-by-line port of
 * mathSpanMatcher(), and its spoken table is the English direction of
 * unicodeTablePreamble()) — so they moved into a header both the
 * renderer and the fixture test (mathspantest.cpp) can include. The
 * shared fixture agent/tests/data/mathspans.json pins input, spans,
 * LaTeX command and spoken word for both sides, so the C++ detector and
 * the Python detector cannot drift apart.
 */

#include <QRegularExpression>
#include <QString>

// All span kinds in one alternation, display delimiters first: a "$$"
// can never be misread as two "$" spans. The inline "$…$" must hug its
// content, so "$5 and $10" stays currency.
inline const QRegularExpression &mathSpanMatcher()
{
    static const QRegularExpression matcher(QStringLiteral(
        "\\\\\\[([\\s\\S]*?)\\\\\\]"
        "|\\$\\$([\\s\\S]*?)\\$\\$"
        "|\\\\\\((.+?)\\\\\\)"
        "|\\$(\\S(?:[^$\\n]*\\S)?)\\$"));
    return matcher;
    return matcher;
}

// The Unicode symbols the models write (∫, α, → …) mapped to their math
// commands — kept in the compile so a formula mixing Unicode and LaTeX
// still typesets. Gated and inert when the package is missing.
inline const char *unicodeTablePreamble()
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

// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

#include "markdowndocument.h"

#include <QHash>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QVector>

#include <algorithm>
#include <functional>

namespace {

// --- Unicode script tables -------------------------------------------------

// Parallel key/value tables: the character and its scripted shape. A
// character missing from the keys has no scripted shape, and its whole
// group falls back to the "^(...)" spelling, which stays honest instead
// of mangling.

// Superscripts exist for every digit, most lower-case letters (no "q")
// and a subset of the upper-case ones, plus the signs.
QString superscriptChar(QChar c)
{
    static const QString keys = QStringLiteral(
        "0123456789"
        "abcdefghijklmnop" "rstuvwxyz"
        "ABDEGHIJKLMNOPRTUVW"
        "+-−=");
    static const QString values = QStringLiteral(
        "⁰¹²³⁴⁵⁶⁷⁸⁹"
        "ᵃᵇᶜᵈᵉᶠᵍʰⁱʲᵏˡᵐⁿᵒᵖ" "ʳˢᵗᵘᵛʷˣʸᶻ"
        "ᴬᴮᴰᴱᴳᴴᴵᴶᴷᴸᴹᴺᴼᴾᴿᵀᵁⱽᵂ"
        "⁺⁻⁻⁼");
    Q_ASSERT(keys.size() == values.size());
    const int index = keys.indexOf(c);
    return index < 0 ? QString() : QString(values.at(index));
}

// Subscripts: only digits and a handful of lower-case letters exist.
QString subscriptChar(QChar c)
{
    static const QString keys = QStringLiteral(
        "0123456789"
        "aehijklmnoprstuvx");
    static const QString values = QStringLiteral(
        "₀₁₂₃₄₅₆₇₈₉"
        "ₐₑₕᵢⱼₖₗₘₙₒₚᵣₛₜᵤᵥₓ");
    Q_ASSERT(keys.size() == values.size());
    const int index = keys.indexOf(c);
    return index < 0 ? QString() : QString(values.at(index));
}

// Removes one level of braces around \a text ("{x}" → "x"); text
// without an enclosing pair comes back unchanged.
QString unwrap(const QString &text)
{
    if (text.startsWith(QLatin1Char('{')) && text.endsWith(QLatin1Char('}'))) {
        return text.mid(1, text.size() - 2);
    }
    return text;
}

// \a in as superscript/subscript Unicode, or false when a character has
// no mapping.
bool mapScript(const QString &in, bool superscript, QString *out)
{
    QString result;
    result.reserve(in.size());
    for (const QChar c : in) {
        const QString mapped = superscript ? superscriptChar(c)
                                           : subscriptChar(c);
        if (mapped.isEmpty()) {
            return false;
        }
        result += mapped;
    }
    *out = result;
    return true;
}

// \a body as a script. In a math span an unmappable body falls back to
// the explicit "^(...)" spelling — terminal, because span contents are
// transformed exactly once — except pure brackets, which would only
// end up doubled ("x_(a)" must not become "x_(()a)"). In bare prose
// there is no such safety (the caret is ordinary punctuation there),
// so an unmappable script stays untouched.
QString scriptBody(const QString &body, bool superscript, bool allowFallback)
{
    const QString unwrapped = unwrap(body);
    QString mapped;
    if (mapScript(unwrapped, superscript, &mapped)) {
        return mapped;
    }
    if (!allowFallback) {
        return (superscript ? QStringLiteral("^") : QStringLiteral("_"))
            + body;
    }
    if (unwrapped.size() == 1
        && QStringLiteral("([{)]}").contains(unwrapped.at(0))) {
        return (superscript ? QStringLiteral("^") : QStringLiteral("_"))
            + unwrapped;
    }
    return superscript ? QStringLiteral("^(%1)").arg(unwrapped)
                       : QStringLiteral("_(%1)").arg(unwrapped);
}

// --- The LaTeX command table -----------------------------------------------

struct CommandMap
{
    // Command (with backslash) → replacement.
    QHash<QString, QString> replacements;
    // Named functions get a thin space before a following letter, so
    // "\sin x" reads "sin x" and not "sinx" ("\sin(x)" needs none).
    QSet<QString> functions;
    // One alternation of every key, longest first, so "\sinh" never
    // loses its "h" to "\sin" and "\subseteq" never becomes
    // "\subset" + "eq".
    QRegularExpression matcher;
};

const CommandMap &commandMap()
{
    static const CommandMap map = [] {
        CommandMap map;
        map.replacements = {
            // --- grouping scaffolding -----------------------------------
            {QStringLiteral("\\left"), QString()},
            {QStringLiteral("\\right"), QString()},
            {QStringLiteral("\\quad"), QStringLiteral("    ")},
            {QStringLiteral("\\qquad"), QStringLiteral("        ")},
            // --- dots ------------------------------------------------------
            {QStringLiteral("\\cdots"), QStringLiteral("⋯")},
            {QStringLiteral("\\vdots"), QStringLiteral("⋮")},
            {QStringLiteral("\\ddots"), QStringLiteral("⋱")},
            {QStringLiteral("\\ldots"), QStringLiteral("…")},
            {QStringLiteral("\\dots"), QStringLiteral("…")},
            // --- greek -----------------------------------------------------
            {QStringLiteral("\\alpha"), QStringLiteral("α")},
            {QStringLiteral("\\beta"), QStringLiteral("β")},
            {QStringLiteral("\\gamma"), QStringLiteral("γ")},
            {QStringLiteral("\\delta"), QStringLiteral("δ")},
            {QStringLiteral("\\epsilon"), QStringLiteral("ε")},
            {QStringLiteral("\\varepsilon"), QStringLiteral("ε")},
            {QStringLiteral("\\zeta"), QStringLiteral("ζ")},
            {QStringLiteral("\\eta"), QStringLiteral("η")},
            {QStringLiteral("\\theta"), QStringLiteral("θ")},
            {QStringLiteral("\\vartheta"), QStringLiteral("ϑ")},
            {QStringLiteral("\\iota"), QStringLiteral("ι")},
            {QStringLiteral("\\kappa"), QStringLiteral("κ")},
            {QStringLiteral("\\lambda"), QStringLiteral("λ")},
            {QStringLiteral("\\mu"), QStringLiteral("μ")},
            {QStringLiteral("\\nu"), QStringLiteral("ν")},
            {QStringLiteral("\\xi"), QStringLiteral("ξ")},
            {QStringLiteral("\\pi"), QStringLiteral("π")},
            {QStringLiteral("\\rho"), QStringLiteral("ρ")},
            {QStringLiteral("\\sigma"), QStringLiteral("σ")},
            {QStringLiteral("\\tau"), QStringLiteral("τ")},
            {QStringLiteral("\\upsilon"), QStringLiteral("υ")},
            {QStringLiteral("\\phi"), QStringLiteral("φ")},
            {QStringLiteral("\\varphi"), QStringLiteral("φ")},
            {QStringLiteral("\\chi"), QStringLiteral("χ")},
            {QStringLiteral("\\psi"), QStringLiteral("ψ")},
            {QStringLiteral("\\omega"), QStringLiteral("ω")},
            {QStringLiteral("\\Gamma"), QStringLiteral("Γ")},
            {QStringLiteral("\\Delta"), QStringLiteral("Δ")},
            {QStringLiteral("\\Theta"), QStringLiteral("Θ")},
            {QStringLiteral("\\Lambda"), QStringLiteral("Λ")},
            {QStringLiteral("\\Xi"), QStringLiteral("Ξ")},
            {QStringLiteral("\\Pi"), QStringLiteral("Π")},
            {QStringLiteral("\\Sigma"), QStringLiteral("Σ")},
            {QStringLiteral("\\Upsilon"), QStringLiteral("Υ")},
            {QStringLiteral("\\Phi"), QStringLiteral("Φ")},
            {QStringLiteral("\\Psi"), QStringLiteral("Ψ")},
            {QStringLiteral("\\Omega"), QStringLiteral("Ω")},
            {QStringLiteral("\\ell"), QStringLiteral("ℓ")},
            {QStringLiteral("\\hbar"), QStringLiteral("ℏ")},
            {QStringLiteral("\\hslash"), QStringLiteral("ℏ")},
            // --- big operators and calculus ---------------------------------
            {QStringLiteral("\\int"), QStringLiteral("∫")},
            {QStringLiteral("\\intop"), QStringLiteral("∫")},
            {QStringLiteral("\\oint"), QStringLiteral("∮")},
            {QStringLiteral("\\iint"), QStringLiteral("∫∫")},
            {QStringLiteral("\\iiint"), QStringLiteral("∫∫∫")},
            {QStringLiteral("\\sum"), QStringLiteral("∑")},
            {QStringLiteral("\\prod"), QStringLiteral("∏")},
            {QStringLiteral("\\bigcup"), QStringLiteral("⋃")},
            {QStringLiteral("\\bigcap"), QStringLiteral("⋂")},
            {QStringLiteral("\\bigoplus"), QStringLiteral("⨁")},
            {QStringLiteral("\\bigotimes"), QStringLiteral("⨂")},
            {QStringLiteral("\\bigvee"), QStringLiteral("⋁")},
            {QStringLiteral("\\bigwedge"), QStringLiteral("⋀")},
            {QStringLiteral("\\nabla"), QStringLiteral("∇")},
            {QStringLiteral("\\partial"), QStringLiteral("∂")},
            {QStringLiteral("\\infty"), QStringLiteral("∞")},
            {QStringLiteral("\\prime"), QStringLiteral("′")},
            // --- relations ---------------------------------------------------
            {QStringLiteral("\\leq"), QStringLiteral("≤")},
            {QStringLiteral("\\le"), QStringLiteral("≤")},
            {QStringLiteral("\\geq"), QStringLiteral("≥")},
            {QStringLiteral("\\ge"), QStringLiteral("≥")},
            {QStringLiteral("\\neq"), QStringLiteral("≠")},
            {QStringLiteral("\\ne"), QStringLiteral("≠")},
            {QStringLiteral("\\approx"), QStringLiteral("≈")},
            {QStringLiteral("\\equiv"), QStringLiteral("≡")},
            {QStringLiteral("\\sim"), QStringLiteral("∼")},
            {QStringLiteral("\\simeq"), QStringLiteral("≃")},
            {QStringLiteral("\\cong"), QStringLiteral("≅")},
            {QStringLiteral("\\propto"), QStringLiteral("∝")},
            {QStringLiteral("\\ll"), QStringLiteral("≪")},
            {QStringLiteral("\\gg"), QStringLiteral("≫")},
            {QStringLiteral("\\subset"), QStringLiteral("⊂")},
            {QStringLiteral("\\subseteq"), QStringLiteral("⊆")},
            {QStringLiteral("\\supset"), QStringLiteral("⊃")},
            {QStringLiteral("\\supseteq"), QStringLiteral("⊇")},
            {QStringLiteral("\\in"), QStringLiteral("∈")},
            {QStringLiteral("\\ni"), QStringLiteral("∋")},
            {QStringLiteral("\\notin"), QStringLiteral("∉")},
            {QStringLiteral("\\mid"), QStringLiteral("∣")},
            {QStringLiteral("\\nmid"), QStringLiteral("∤")},
            // --- operators ----------------------------------------------------
            {QStringLiteral("\\times"), QStringLiteral("×")},
            {QStringLiteral("\\div"), QStringLiteral("÷")},
            {QStringLiteral("\\cdot"), QStringLiteral("·")},
            {QStringLiteral("\\ast"), QStringLiteral("∗")},
            {QStringLiteral("\\star"), QStringLiteral("⋆")},
            {QStringLiteral("\\circ"), QStringLiteral("∘")},
            {QStringLiteral("\\bullet"), QStringLiteral("•")},
            {QStringLiteral("\\oplus"), QStringLiteral("⊕")},
            {QStringLiteral("\\ominus"), QStringLiteral("⊖")},
            {QStringLiteral("\\otimes"), QStringLiteral("⊗")},
            {QStringLiteral("\\odot"), QStringLiteral("⊙")},
            {QStringLiteral("\\uplus"), QStringLiteral("⊎")},
            {QStringLiteral("\\pm"), QStringLiteral("±")},
            {QStringLiteral("\\mp"), QStringLiteral("∓")},
            {QStringLiteral("\\dagger"), QStringLiteral("†")},
            {QStringLiteral("\\ddagger"), QStringLiteral("‡")},
            // --- arrows --------------------------------------------------------
            {QStringLiteral("\\to"), QStringLiteral("→")},
            {QStringLiteral("\\rightarrow"), QStringLiteral("→")},
            {QStringLiteral("\\longrightarrow"), QStringLiteral("⟶")},
            {QStringLiteral("\\leftarrow"), QStringLiteral("←")},
            {QStringLiteral("\\longleftarrow"), QStringLiteral("⟵")},
            {QStringLiteral("\\leftrightarrow"), QStringLiteral("↔")},
            {QStringLiteral("\\hookrightarrow"), QStringLiteral("↪")},
            {QStringLiteral("\\mapsto"), QStringLiteral("↦")},
            {QStringLiteral("\\Rightarrow"), QStringLiteral("⇒")},
            {QStringLiteral("\\Longrightarrow"), QStringLiteral("⟹")},
            {QStringLiteral("\\Leftarrow"), QStringLiteral("⇐")},
            {QStringLiteral("\\Leftrightarrow"), QStringLiteral("⇔")},
            {QStringLiteral("\\uparrow"), QStringLiteral("↑")},
            {QStringLiteral("\\downarrow"), QStringLiteral("↓")},
            {QStringLiteral("\\nearrow"), QStringLiteral("↗")},
            {QStringLiteral("\\searrow"), QStringLiteral("↘")},
            {QStringLiteral("\\swarrow"), QStringLiteral("↙")},
            {QStringLiteral("\\nwarrow"), QStringLiteral("↖")},
            {QStringLiteral("\\leadsto"), QStringLiteral("⇝")},
            // --- logic and sets --------------------------------------------------
            {QStringLiteral("\\forall"), QStringLiteral("∀")},
            {QStringLiteral("\\exists"), QStringLiteral("∃")},
            {QStringLiteral("\\nexists"), QStringLiteral("∄")},
            {QStringLiteral("\\neg"), QStringLiteral("¬")},
            {QStringLiteral("\\land"), QStringLiteral("∧")},
            {QStringLiteral("\\lor"), QStringLiteral("∨")},
            {QStringLiteral("\\wedge"), QStringLiteral("∧")},
            {QStringLiteral("\\vee"), QStringLiteral("∨")},
            {QStringLiteral("\\top"), QStringLiteral("⊤")},
            {QStringLiteral("\\bot"), QStringLiteral("⊥")},
            {QStringLiteral("\\emptyset"), QStringLiteral("∅")},
            {QStringLiteral("\\varnothing"), QStringLiteral("∅")},
            {QStringLiteral("\\angle"), QStringLiteral("∠")},
            {QStringLiteral("\\triangle"), QStringLiteral("△")},
            {QStringLiteral("\\square"), QStringLiteral("□")},
            {QStringLiteral("\\checkmark"), QStringLiteral("✓")},
        };
        map.functions = {
            QStringLiteral("\\sin"), QStringLiteral("\\cos"),
            QStringLiteral("\\tan"), QStringLiteral("\\sinh"),
            QStringLiteral("\\cosh"), QStringLiteral("\\tanh"),
            QStringLiteral("\\cot"), QStringLiteral("\\sec"),
            QStringLiteral("\\csc"), QStringLiteral("\\arcsin"),
            QStringLiteral("\\arccos"), QStringLiteral("\\arctan"),
            QStringLiteral("\\log"), QStringLiteral("\\ln"),
            QStringLiteral("\\exp"), QStringLiteral("\\det"),
            QStringLiteral("\\dim"), QStringLiteral("\\deg"),
            QStringLiteral("\\gcd"), QStringLiteral("\\hom"),
            QStringLiteral("\\ker"), QStringLiteral("\\arg"),
            QStringLiteral("\\liminf"), QStringLiteral("\\limsup"),
            QStringLiteral("\\min"), QStringLiteral("\\max"),
            QStringLiteral("\\sup"), QStringLiteral("\\inf"),
            QStringLiteral("\\Pr"), QStringLiteral("\\lim"),
            QStringLiteral("\\tr"),
        };
        QStringList keys = map.replacements.keys();
        std::sort(keys.begin(), keys.end(),
                  [](const QString &a, const QString &b) {
                      return a.size() > b.size();
                  });
        QStringList alternatives;
        alternatives.reserve(keys.size());
        for (const QString &key : keys) {
            alternatives.append(QRegularExpression::escape(key));
        }
        map.matcher = QRegularExpression(
            QLatin1Char('(') + alternatives.join(QLatin1Char('|'))
            + QLatin1Char(')'));
        return map;
    }();
    return map;
}

// --- replacement machinery --------------------------------------------------

// Replaces every match of \a re in \a text with what \a transform makes
// of it — the manual loop QString::replace(re, functor) would do, kept
// explicit so it does not depend on the Qt minor version.
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

// Replaces every \c \\command{body} occurrence with what \a transform
// makes of the body. One brace level per pass: an outer command around
// an inner one becomes matchable on the next pass (see the repeat loop
// in transformMathInner).
QString replaceBraced(const QString &text, const QString &command,
                      const std::function<QString(const QString &)> &transform)
{
    const QRegularExpression re(
        QRegularExpression::escape(command) + QStringLiteral("\\{([^{}]*)\\}"));
    return replaceMatches(text, re, [&transform](const QRegularExpressionMatch &m) {
        return transform(m.captured(1));
    });
}

QString accentBody(const QString &body, QChar combining)
{
    const QString unwrapped = unwrap(body);
    if (unwrapped.isEmpty()) {
        return unwrapped;
    }
    // Every character carries the accent, so a two-letter body keeps
    // the mark continuous.
    QString result;
    result.reserve(unwrapped.size() * 2);
    for (const QChar c : unwrapped) {
        result += c;
        result += combining;
    }
    return result;
}

QString tailAccentBody(const QString &body, QChar combining)
{
    const QString unwrapped = unwrap(body);
    if (unwrapped.isEmpty()) {
        return unwrapped;
    }
    return unwrapped.left(unwrapped.size() - 1) + unwrapped.right(1)
        + combining;
}

// \mathbb{C} → ℂ, unknown alphabets unwrap.
QString blackboardBody(const QString &body)
{
    static const QString keys = QStringLiteral("CNRZQHP");
    static const QString values = QStringLiteral("ℂℕℝℤℚℍℙ");
    const QString unwrapped = unwrap(body);
    if (unwrapped.size() == 1) {
        const int index = keys.indexOf(unwrapped.at(0));
        if (index >= 0) {
            return QString(values.at(index));
        }
    }
    return unwrapped;
}

// The LaTeX constructs, applied to \a text.
//
// In a math span (\a spanMode) everything goes: superscripts and
// subscripts with the "^(...)" fallback, since the span is transformed
// exactly once and the fallback is terminal there.
//
// In bare prose (!\a spanMode) only what cannot be misread: Greek
// letters and other known commands, unmistakable brace constructs, and
// superscripts — and only when the whole body maps, so "x^2" becomes
// "x²" while "a^q" and snake_case underscores stay as written. No
// brace removal, no "&" removal, no "\\" line breaks — file paths,
// markdown escapes and quoted text pass through.
QString transformMathInner(const QString &text, bool spanMode)
{
    QString result = text;

    // Degrees: the one superscript with a proper sign of its own.
    result.replace(QRegularExpression(QStringLiteral("\\^\\{?\\\\circ\\}?")),
                   QStringLiteral("°"));

    // LaTeX escapes of characters that are syntax further down: make
    // them literal before any pass can eat them ("\_" must not
    // subscript, "\{" must not group).
    result.replace(QRegularExpression(QStringLiteral("\\\\([#$%&_{}])")),
                   QStringLiteral("\\1"));

    // Spacing commands → spaces (thin, so "\sin\,x" stays airy).
    result.replace(QRegularExpression(QStringLiteral("\\\\[,;:!]")),
                   QString(QChar(0x2009)));
    result.replace(QStringLiteral("\\ "), QStringLiteral(" "));

    // Structural commands; repeated while anything changes, because a
    // command inside another one's braces only becomes matchable after
    // the inner one has been rewritten ("\frac{\frac{a}{b}}{c}").
    for (bool changed = true; changed;) {
        changed = false;
        const QString before = result;
        // \frac{a}{b} → a/b (\dfrac, \tfrac the same).
        result = replaceMatches(
            result,
            QRegularExpression(QStringLiteral(
                "\\\\[dt]?frac\\{([^{}]*)\\}\\{([^{}]*)\\}")),
            [](const QRegularExpressionMatch &m) {
                return m.captured(1) + QLatin1Char('/') + m.captured(2);
            });
        // \sqrt[n]{x} → √[n](x); \sqrt{x} → √(x).
        result = replaceMatches(
            result,
            QRegularExpression(QStringLiteral(
                "\\\\sqrt(?:\\[([^\\]]*)\\])?\\{([^{}]*)\\}")),
            [](const QRegularExpressionMatch &m) {
                return m.captured(1).isEmpty()
                    ? QStringLiteral("√(%1)").arg(m.captured(2))
                    : QStringLiteral("√[%1](%2)").arg(m.captured(1),
                                                      m.captured(2));
            });
        // Text-setting commands wrap words, not math: unwrap them.
        result = replaceBraced(result, QStringLiteral("\\text"), unwrap);
        result = replaceBraced(result, QStringLiteral("\\mathrm"), unwrap);
        result = replaceBraced(result, QStringLiteral("\\mathit"), unwrap);
        result = replaceBraced(result, QStringLiteral("\\mathbf"), unwrap);
        result = replaceBraced(result, QStringLiteral("\\mathsf"), unwrap);
        result = replaceBraced(result, QStringLiteral("\\mathtt"), unwrap);
        result = replaceBraced(result, QStringLiteral("\\textrm"), unwrap);
        result = replaceBraced(result, QStringLiteral("\\operatorname"),
                               unwrap);
        result = replaceBraced(result, QStringLiteral("\\mathbb"),
                               blackboardBody);
        result = replaceBraced(result, QStringLiteral("\\overline"),
                               [](const QString &b) {
                                   return accentBody(b, QChar(0x0305));
                               });
        result = replaceBraced(result, QStringLiteral("\\bar"),
                               [](const QString &b) {
                                   return accentBody(b, QChar(0x0305));
                               });
        result = replaceBraced(result, QStringLiteral("\\hat"),
                               [](const QString &b) {
                                   return tailAccentBody(b, QChar(0x0302));
                               });
        result = replaceBraced(result, QStringLiteral("\\vec"),
                               [](const QString &b) {
                                   return tailAccentBody(b, QChar(0x20D7));
                               });
        result = replaceBraced(result, QStringLiteral("\\dot"),
                               [](const QString &b) {
                                   return tailAccentBody(b, QChar(0x0307));
                               });
        result = replaceBraced(result, QStringLiteral("\\tilde"),
                               [](const QString &b) {
                                   return tailAccentBody(b, QChar(0x0303));
                               });
        changed = (result != before);
    }

    // The command table.
    const CommandMap &map = commandMap();
    const QString beforeCommands = result;
    result = replaceMatches(result, map.matcher,
                            [&beforeCommands, &map](const QRegularExpressionMatch &m) {
                                const QString command = m.captured(1);
                                QString out = map.replacements.value(command);
                                if (map.functions.contains(command)
                                    && m.capturedEnd() < beforeCommands.size()) {
                                    const QChar next =
                                        beforeCommands.at(m.capturedEnd());
                                    if (next.isLetterOrNumber()) {
                                        out += QChar(0x2009);
                                    }
                                }
                                return out;
                            });

    // Scripts, after the commands: a script body that was itself a
    // command ("x^\infty") is now mapped or handled per the mode. One
    // combined pass, so the "_(...)" fallback of one script is never
    // fed back into another script pass. In bare prose the underscore
    // is left alone: raw underscores in prose spell snake_case, not
    // subscripts.
    static const QRegularExpression scripts(QStringLiteral(
        "\\^\\{([^{}]*)\\}"   // ^{...}
        "|_\\{([^{}]*)\\}"    // _{...}
        "|\\^(\\S)"           // ^x
        "|_(\\S)"));          // _x
    result = replaceMatches(result, scripts,
                            [spanMode](const QRegularExpressionMatch &m) {
                                QString body;
                                bool superscript = true;
                                for (int group = 1; group <= 4; ++group) {
                                    if (!m.captured(group).isNull()) {
                                        body = m.captured(group);
                                        superscript = group == 1 || group == 3;
                                        break;
                                    }
                                }
                                if (!spanMode && !superscript) {
                                    return m.captured(0);
                                }
                                return scriptBody(body, superscript, spanMode);
                            });

    return result;
}

// One math span (delimiters already stripped) as plain Unicode text.
QString mathToUnicode(const QString &span)
{
    QString text = span;
    // A prime is an accent in math ("f'(x)"): typeset it. Done first,
    // before the general passes could mistake it for quoting.
    text.replace(QLatin1Char('\''), QString(QChar(0x2032)));
    // Display-math line breaks: a "\\" is a line break — handled before
    // the inner passes, whose "\\ " spacing rule must not eat its first
    // backslash.
    text.replace(QStringLiteral("\\\\"), QStringLiteral("\n"));
    text = transformMathInner(text, true);
    // An "&" aligns columns; remaining braces were pure grouping.
    text.remove(QLatin1Char('&'));
    text.remove(QLatin1Char('{'));
    text.remove(QLatin1Char('}'));
    return text;
}

// One prose segment (fenced code already split off): every math span
// and every gap between spans is transformed exactly once — a combined
// match over all span kinds, so the bare pass can never re-process a
// span's output (which would double the "^(...)" fallbacks).
QString transformProse(const QString &segment)
{
    QString text = segment;

    // Inline code spans are verbatim, not math: mask them (per line —
    // backticks across a blank line do not pair) and restore at the end.
    QStringList codeSpans;
    {
        QString masked;
        masked.reserve(text.size());
        qsizetype last = 0;
        const QRegularExpression codeSpan(QStringLiteral("`[^`\\n]+`"));
        QRegularExpressionMatchIterator it = codeSpan.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            masked += text.mid(last, match.capturedStart() - last);
            masked += QStringLiteral("\x01%1\x02").arg(codeSpans.size());
            codeSpans.append(match.captured());
            last = match.capturedEnd();
        }
        masked += text.mid(last);
        text = masked;
    }

    // All span kinds in one alternation, longest delimiters first: a
    // "$$" can never be misread as two "$" spans, and the engine's
    // leftmost-match rule keeps spans from overlapping.
    static const QRegularExpression spans(QStringLiteral(
        "\\\\\\[([\\s\\S]*?)\\\\\\]"          // \[ display \]
        "|\\$\\$([\\s\\S]*?)\\$\\$"           // $$ display $$
        "|\\\\\\((.+?)\\\\\\)"                // \( inline \)
        "|\\$(\\S(?:[^$\\n]*\\S)?)\\$"));     // $ inline $ — hugging its
                                              // content, so "$5 and $10"
                                              // stays currency.
    QString result;
    result.reserve(text.size());
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = spans.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        // The gap before the span: bare prose.
        result += transformMathInner(text.mid(last, match.capturedStart() - last),
                                     false);
        // The span's content, delimiters dropped.
        for (int group = 1; group <= 4; ++group) {
            if (!match.captured(group).isNull()) {
                result += mathToUnicode(match.captured(group));
                break;
            }
        }
        last = match.capturedEnd();
    }
    // The trailing gap: bare prose.
    result += transformMathInner(text.mid(last), false);

    for (int i = codeSpans.size() - 1; i >= 0; --i) {
        result.replace(QStringLiteral("\x01%1\x02").arg(i), codeSpans.at(i));
    }
    return result;
}

} // namespace

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

QString MarkdownDocument::renderMath(const QString &text) const
{
    // Fast path: no math markers at all.
    if (!text.contains(QLatin1Char('$')) && !text.contains(QLatin1Char('\\'))
        && !text.contains(QLatin1Char('^'))
        && !text.contains(QLatin1Char('_'))) {
        return text;
    }

    // Split the text into fenced-code and prose ranges; the code stays
    // verbatim, the prose is transformed. An unclosed fence keeps the
    // rest of the text verbatim, too.
    const QStringList lines = text.split(QLatin1Char('\n'));
    QVector<QPair<qsizetype, qsizetype>> proseRanges;
    qsizetype proseStart = 0;
    qsizetype position = 0;
    bool inFence = false;
    QString fenceMarker;
    for (const QString &line : lines) {
        const qsizetype lineLength = line.size() + 1; // the newline, too
        const QString trimmed = line.trimmed();
        const bool opensFence =
            trimmed.startsWith(QStringLiteral("```"))
            || trimmed.startsWith(QStringLiteral("~~~"));
        if (opensFence) {
            if (!inFence) {
                if (position > proseStart) {
                    proseRanges.append({proseStart, position});
                }
                inFence = true;
                fenceMarker = trimmed.left(3);
            } else if (trimmed.startsWith(fenceMarker)) {
                inFence = false;
                proseStart = position + lineLength;
            }
        }
        position += lineLength;
    }
    if (!inFence && position > proseStart) {
        proseRanges.append({proseStart, position});
    }

    QString result;
    result.reserve(text.size());
    qsizetype copied = 0;
    for (const auto &range : proseRanges) {
        result += text.mid(copied, range.first - copied);
        result += transformProse(text.mid(range.first, range.second - range.first));
        copied = range.second;
    }
    result += text.mid(copied);
    return result;
}

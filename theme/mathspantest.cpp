// SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
// SPDX-License-Identifier: BSD-2-Clause

/*!
 * \brief Offline check of the shared math-span fixture (CTest target).
 *
 * The daemon's speech pipeline (agent/phoenix_dna/speech/) detects the
 * same math spans this renderer embeds as images, and speaks the same
 * Unicode symbols this renderer maps to LaTeX commands. Both sides are
 * tested against one fixture — agent/tests/data/mathspans.json — so the
 * C++ detector in mathspan.h and the Python port in mathspan.py cannot
 * drift apart. This executable runs the C++ half: every fixture input's
 * expected spans must come out of mathSpanMatcher(), and every fixture
 * symbol must appear in unicodeTablePreamble() with the fixture's LaTeX
 * command. It exits non-zero and names the first mismatches on failure.
 */

#include "mathspan.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>
#include <QString>
#include <QTextStream>

#include <algorithm>
#include <cstdio>

namespace {

struct ExpectedSpan
{
    int start;
    int end;
    QString kind;
    QString body;
};

// The span kind named by which capturing group matched (1-based), in
// the order of the matcher's alternation — display kinds first.
QString kindForGroup(int group)
{
    switch (group) {
    case 1:
        return QStringLiteral("display_bracket");
    case 2:
        return QStringLiteral("display_dollar");
    case 3:
        return QStringLiteral("inline_paren");
    case 4:
        return QStringLiteral("inline_dollar");
    }
    return QString();
}

int checkSpans(const QJsonArray &cases, QTextStream &out)
{
    int failures = 0;
    for (const QJsonValue &value : cases) {
        const QJsonObject entry = value.toObject();
        const QString input = entry.value(QStringLiteral("input")).toString();
        const QJsonArray expected = entry.value(QStringLiteral("expected")).toArray();

        QList<ExpectedSpan> wanted;
        for (const QJsonValue &span : expected) {
            const QJsonObject object = span.toObject();
            wanted.append({object.value(QStringLiteral("start")).toInt(),
                           object.value(QStringLiteral("end")).toInt(),
                           object.value(QStringLiteral("kind")).toString(),
                           object.value(QStringLiteral("body")).toString()});
        }

        QList<ExpectedSpan> got;
        auto it = mathSpanMatcher().globalMatch(input);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            for (int group = 1; group <= 4; ++group) {
                // A non-participating group starts at -1; the display
                // groups participate even when their body is empty.
                if (match.capturedStart(group) >= 0) {
                    got.append({static_cast<int>(match.capturedStart()),
                                static_cast<int>(match.capturedEnd()),
                                kindForGroup(group), match.captured(group)});
                    break;
                }
            }
        }

        const bool same = wanted.size() == got.size()
            && std::equal(wanted.cbegin(), wanted.cend(), got.cbegin(), got.cend(),
                          [](const ExpectedSpan &a, const ExpectedSpan &b) {
                              return a.start == b.start && a.end == b.end
                                  && a.kind == b.kind && a.body == b.body;
                          });
        if (!same) {
            ++failures;
            out << "span mismatch for input: " << input << "\n";
            out << "  expected " << wanted.size() << " spans, got " << got.size() << "\n";
            for (const ExpectedSpan &span : wanted) {
                out << "  want [" << span.start << "," << span.end << ") " << span.kind
                    << " body=" << span.body << "\n";
            }
            for (const ExpectedSpan &span : got) {
                out << "  got  [" << span.start << "," << span.end << ") " << span.kind
                    << " body=" << span.body << "\n";
            }
        }
    }
    return failures;
}

// \newunicodechar{×}{\ensuremath{\times}} → "×" maps to "times". The
// command capture allows one nested brace pair so the blackboard letters
// (\mathbb{R}) come out whole.
int checkUnicode(const QJsonArray &table, QTextStream &out)
{
    const QString preamble = QString::fromUtf8(unicodeTablePreamble());
    const QRegularExpression line(
        QStringLiteral("\\\\newunicodechar\\{([^}]+)\\}\\{\\\\ensuremath\\{\\\\((?:[^{}]|\\{[^}]*\\})+)\\}\\}"));
    QHash<QString, QString> mapped;
    auto it = line.globalMatch(preamble);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        mapped.insert(match.captured(1), match.captured(2));
    }

    int failures = 0;
    for (const QJsonValue &value : table) {
        const QJsonObject entry = value.toObject();
        const QString symbol = entry.value(QStringLiteral("symbol")).toString();
        const QString latex = entry.value(QStringLiteral("latex")).toString();
        if (mapped.value(symbol) != latex) {
            ++failures;
            out << "unicode mismatch for " << symbol << ": preamble maps it to "
                << mapped.value(symbol) << ", fixture says " << latex << "\n";
        }
    }
    return failures;
}

} // namespace

int main(int argc, char **argv)
{
    // The fixture path is baked in by the build (MATHSPAN_FIXTURE); an
    // argument overrides it, which makes the binary runnable from
    // anywhere (and from a source checkout without a build).
    QString fixturePath = QStringLiteral(MATHSPAN_FIXTURE);
    if (argc > 1) {
        fixturePath = QString::fromUtf8(argv[1]);
    }

    QFile file(fixturePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QTextStream(stderr) << "cannot open fixture " << fixturePath << "\n";
        return 2;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        QTextStream(stderr) << "fixture is not a JSON object: " << fixturePath << "\n";
        return 2;
    }

    QString report;
    QTextStream out(&report);
    const QJsonObject root = document.object();
    const int failures = checkSpans(root.value(QStringLiteral("spans")).toArray(), out)
        + checkUnicode(root.value(QStringLiteral("unicode")).toArray(), out);

    QTextStream stdio(stdout);
    stdio << report;
    stdio << (failures == 0 ? QStringLiteral("mathspantest: all fixture cases pass\n")
                            : QStringLiteral("mathspantest: %1 failure(s)\n").arg(failures));
    return failures == 0 ? 0 : 1;
}

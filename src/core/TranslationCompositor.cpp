#include "core/TranslationCompositor.h"

#include "core/PerfLog.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <limits>
#include <vector>

namespace Visnip::Translate {

namespace {

constexpr int kPatchPaddingPx = 2;
constexpr int kMinFontPixelSize = 9;
constexpr int kMaxFontPixelSize = 72;
constexpr double kLineFillRatio = 0.78;      // fallback glyph height relative to line box
constexpr double kMaxHeightRatio = 1.25;     // box-height fallback ratio when ink is unknown
constexpr double kMaxInkHeightRatio = 1.3;   // lines with more disparate glyph heights don't
                                             // merge (headings are often only ~1.3x the body)
constexpr double kMaxVerticalGapRatio = 0.8; // broad pairwise gate before block context
constexpr double kMaxInitialGapRatio = 0.55; // first continuation must be visibly line-wrapped
constexpr double kMaxGapGrowthRatio = 1.6;   // a gap this much beyond the block's rhythm splits
constexpr double kMaxStackedOverlapRatio = 0.6; // unclip margins may overlap this much
constexpr double kMinHorizontalOverlap = 0.5;
constexpr int kStructureLookaheadLines = 4;
constexpr int kColorSampleCap = 4096;
constexpr int kInkLumaContrast = 48;         // luma delta that separates glyphs from background
constexpr int kMaxBackgroundLumaDifference = 24;
constexpr int kMinStyleContrastDifference = 32;
constexpr double kMinStyleContrastRatio = 1.2;
constexpr double kMinWrappedLineWidthInHeights = 7.0;
constexpr int kMinimumLexicalGraphemes = 2;
constexpr double kCjkEmPerInk = 1.0;         // CJK glyphs fill the em box
constexpr double kLatinEmPerInk = 1.26;      // Latin ink (cap..descender) sits inside the em

bool isCjk(QChar ch)
{
    const ushort u = ch.unicode();
    return (u >= 0x2E80 && u <= 0x9FFF)   // radicals, CJK ideographs, kana
        || (u >= 0xF900 && u <= 0xFAFF)   // compatibility ideographs
        || (u >= 0xFF00 && u <= 0xFF60);  // fullwidth forms
}

bool linesBelongTogether(const QRect& previous, const QRect& next, int previousInk, int nextInk)
{
    const int minHeight = qMin(previous.height(), next.height());
    if (minHeight <= 0) {
        return false;
    }
    // Same-size check. Measured glyph heights are the truthful signal: the
    // det boxes' unclip margins flatten font-size differences on short
    // lines, letting a heading share a box height with body text. Box
    // heights are only the fallback when ink could not be measured.
    if (previousInk >= 4 && nextInk >= 4) {
        const double inkRatio =
            static_cast<double>(qMax(previousInk, nextInk)) / qMin(previousInk, nextInk);
        if (inkRatio > kMaxInkHeightRatio) {
            return false;
        }
    } else {
        const int maxHeight = qMax(previous.height(), next.height());
        if (static_cast<double>(maxHeight) / minHeight > kMaxHeightRatio) {
            return false;
        }
    }

    // Must be stacked, not side by side. The detector's unclip margins can
    // make adjacent lines' boxes overlap vertically, so "stacked" tolerates
    // partial overlap: the next line's center must sit below the previous
    // line's center, and the overlap must stay well short of a shared row.
    const int previousCenterY = previous.top() + previous.height() / 2;
    const int nextCenterY = next.top() + next.height() / 2;
    if (nextCenterY <= previousCenterY) {
        return false;
    }
    const int verticalOverlap =
        qMin(previous.bottom(), next.bottom()) - qMax(previous.top(), next.top()) + 1;
    if (verticalOverlap > minHeight * kMaxStackedOverlapRatio) {
        return false;
    }
    const double averageHeight = (previous.height() + next.height()) / 2.0;
    const int gap = next.top() - previous.bottom();
    if (gap > averageHeight * kMaxVerticalGapRatio) {
        return false;
    }

    // A pure left indent marks a structural boundary: a flush heading above
    // an indented paragraph, a list item, a quote. Centered layouts pull
    // both edges in and right-aligned layouts keep the right edge, so only
    // reject when the right edge does not follow the left one.
    const int leftStep = next.left() - previous.left();
    const int indentThreshold = qMax(6, qRound(averageHeight * 0.7));
    if (leftStep > indentThreshold) {
        const bool rightAligned = qAbs(previous.right() - next.right()) <= indentThreshold;
        const bool centered = previous.right() - next.right() >= leftStep / 2;
        if (!rightAligned && !centered) {
            return false;
        }
    }

    // Horizontally aligned: ranges overlap by at least half of the narrower
    // line, or the left edges line up.
    const int overlap = qMin(previous.right(), next.right()) - qMax(previous.left(), next.left());
    const int narrower = qMin(previous.width(), next.width());
    if (narrower > 0 && overlap >= narrower * kMinHorizontalOverlap) {
        return true;
    }
    return qAbs(previous.left() - next.left()) <= averageHeight * 1.5;
}

bool lineStylesDiffer(const LineVisualStyle& previous, const LineVisualStyle& next)
{
    if (previous.backgroundLuma >= 0 && next.backgroundLuma >= 0
        && qAbs(previous.backgroundLuma - next.backgroundLuma)
            >= kMaxBackgroundLumaDifference) {
        return true;
    }
    if (!previous.foregroundMeasured || !next.foregroundMeasured) {
        return false;
    }
    const int weakerContrast = qMin(previous.contrast, next.contrast);
    const int strongerContrast = qMax(previous.contrast, next.contrast);
    return strongerContrast - weakerContrast >= kMinStyleContrastDifference
        && strongerContrast >= qRound(weakerContrast * kMinStyleContrastRatio);
}

bool blockAcceptsLine(const TextBlock& block,
                      const LineVisualStyle& blockLastStyle,
                      const QRect& lineBox,
                      const LineVisualStyle& lineStyle)
{
    const QRect& last = block.lineBoxes.last();
    bool belongs = linesBelongTogether(last,
                                       lineBox,
                                       blockLastStyle.inkHeight,
                                       lineStyle.inkHeight);
    if (!belongs && !lineStylesDiffer(blockLastStyle, lineStyle)) {
        // Links, underlines, and descenders can inflate the measured ink of a
        // long first line while its short wrapped tail has ordinary glyph ink.
        // Geometry remains decisive when both lines share the left margin, the
        // tail is clearly shorter, and their detector boxes touch/overlap.
        const int averageHeight = (last.height() + lineBox.height()) / 2;
        const int leftTolerance = qMax(3, averageHeight / 3);
        const int gap = lineBox.top() - last.bottom();
        belongs = last.width() >= averageHeight * 6
            && qAbs(lineBox.left() - last.left()) <= leftTolerance
            && lineBox.width() * 2 <= last.width() * 3
            && gap <= qMax(3, averageHeight / 5);
    }
    if (lineStylesDiffer(blockLastStyle, lineStyle) || !belongs) {
        return false;
    }
    if (block.lineBoxes.size() < 2) {
        // A standalone line has no established line rhythm. Its first
        // continuation needs both a close gap and evidence that at least one
        // line spans paragraph-like width. Short peer labels (menus, cards,
        // settings rows) must stay independent even when their left edges and
        // styles happen to match. A line whose leading icon was separated is
        // already known to be a navigation label and may accept its wrapped
        // icon-free tail.
        const double averageHeight = (last.height() + lineBox.height()) / 2.0;
        const bool hasWrapExtent = block.leadingIconSeparated
            || qMax(last.width(), lineBox.width())
                >= qRound(averageHeight * kMinWrappedLineWidthInHeights);
        if (!hasWrapExtent) {
            return false;
        }
        const int firstGap = lineBox.top() - last.bottom();
        const int minInk = qMin(blockLastStyle.inkHeight, lineStyle.inkHeight);
        const int spacingBase = minInk >= 4
            ? minInk
            : qMin(last.height(), lineBox.height());
        return firstGap <= qMax(3, qRound(spacingBase * kMaxInitialGapRatio));
    }
    // A block with an established line rhythm only accepts lines that keep
    // it: a clearly larger gap marks a paragraph or heading boundary even
    // when the absolute gap passes the height-based check.
    std::vector<int> gaps;
    gaps.reserve(static_cast<size_t>(block.lineBoxes.size()) - 1);
    for (int i = 1; i < block.lineBoxes.size(); ++i) {
        gaps.push_back(block.lineBoxes[i].top() - block.lineBoxes[i - 1].bottom());
    }
    const size_t mid = gaps.size() / 2;
    std::nth_element(gaps.begin(), gaps.begin() + static_cast<qptrdiff>(mid), gaps.end());
    const int typicalGap = gaps[mid];
    const int minHeight = qMin(last.height(), lineBox.height());
    const int newGap = lineBox.top() - last.bottom();
    return newGap <= qMax(typicalGap, 0) * kMaxGapGrowthRatio + qMax(3, minHeight / 3);
}

int lumaOf(QRgb rgb)
{
    return (qRed(rgb) * 299 + qGreen(rgb) * 587 + qBlue(rgb) * 114) / 1000;
}

// Span of rows within line whose sampled pixels contrast with
// backgroundLuma — the height the glyphs actually render at. Returns 0
// when nothing contrasts (blank or unmeasurable region).
int inkRowSpan(const QImage& source, const QRect& line, int backgroundLuma)
{
    const int xStep = qMax(1, line.width() / 64);
    const int sampledPerRow = line.width() / xStep;
    const int needed = qMax(2, sampledPerRow / 24);
    std::vector<bool> inkRows(static_cast<size_t>(line.height()), false);
    for (int y = line.top(); y <= line.bottom(); ++y) {
        int hits = 0;
        for (int x = line.left(); x <= line.right(); x += xStep) {
            if (qAbs(lumaOf(source.pixel(x, y)) - backgroundLuma) >= kInkLumaContrast) {
                if (++hits >= needed) {
                    break;
                }
            }
        }
        inkRows[static_cast<size_t>(y - line.top())] = hits >= needed;
    }

    int bestStart = -1;
    int bestEnd = -1;
    int runStart = -1;
    int lastInk = -1;
    int internalGap = 0;
    const int maximumInternalGap = 1;
    for (int row = 0; row < line.height(); ++row) {
        if (inkRows[static_cast<size_t>(row)]) {
            if (runStart < 0) {
                runStart = row;
            }
            lastInk = row;
            internalGap = 0;
            continue;
        }
        if (runStart >= 0 && ++internalGap > maximumInternalGap) {
            if (bestStart < 0 || lastInk - runStart > bestEnd - bestStart) {
                bestStart = runStart;
                bestEnd = lastInk;
            }
            runStart = -1;
            lastInk = -1;
            internalGap = 0;
        }
    }
    if (runStart >= 0
        && (bestStart < 0 || lastInk - runStart > bestEnd - bestStart)) {
        bestStart = runStart;
        bestEnd = lastInk;
    }
    return bestStart >= 0 ? bestEnd - bestStart + 1 : 0;
}

// Ink height of a single line box, measured against the box's own median
// luma (background pixels dominate a text line, so the median is the
// background).
int lineInkHeight(const QImage& source, const QRect& box)
{
    if (source.isNull()) {
        return 0;
    }
    const QRect line = box.intersected(source.rect());
    if (line.width() < 3 || line.height() < 3) {
        return 0;
    }
    const int xStep = qMax(1, line.width() / 64);
    const int yStep = qMax(1, line.height() / 16);
    std::vector<int> lumas;
    lumas.reserve(static_cast<size_t>((line.width() / xStep + 1))
                  * static_cast<size_t>(line.height() / yStep + 1));
    for (int y = line.top(); y <= line.bottom(); y += yStep) {
        for (int x = line.left(); x <= line.right(); x += xStep) {
            lumas.push_back(lumaOf(source.pixel(x, y)));
        }
    }
    if (lumas.empty()) {
        return 0;
    }
    const size_t mid = lumas.size() / 2;
    std::nth_element(lumas.begin(), lumas.begin() + static_cast<qptrdiff>(mid), lumas.end());
    return inkRowSpan(source, line, lumas[mid]);
}

struct HorizontalInkSpan {
    int left = -1;
    int right = -1;

    bool isValid() const { return left >= 0 && right >= left; }
};

HorizontalInkSpan lineHorizontalInkSpan(const QImage& source,
                                        const QRect& box,
                                        int backgroundLuma)
{
    HorizontalInkSpan span;
    if (source.isNull()) {
        return span;
    }
    const QRect line = box.intersected(source.rect());
    if (line.width() < 3 || line.height() < 3) {
        return span;
    }

    std::vector<bool> ink(static_cast<size_t>(line.width()), false);
    for (int x = line.left(); x <= line.right(); ++x) {
        for (int y = line.top(); y <= line.bottom(); ++y) {
            if (qAbs(lumaOf(source.pixel(x, y)) - backgroundLuma) >= kInkLumaContrast) {
                ink[static_cast<size_t>(x - line.left())] = true;
                break;
            }
        }
    }

    int first = 0;
    while (first < line.width() && !ink[static_cast<size_t>(first)]) {
        ++first;
    }
    int last = line.width() - 1;
    while (last >= first && !ink[static_cast<size_t>(last)]) {
        --last;
    }
    if (first > last) {
        return span;
    }

    // A detector box may begin inside a compact leading icon and then include
    // the adjacent label. Preserve that icon when a wide empty separator
    // clearly divides the small edge fragment from the main text ink.
    const int minimumGap = qMax(5, qRound(line.height() * 0.3));
    const int maximumArtifactWidth = qMax(8, qRound(line.height() * 0.7));
    const int edgeTolerance = qMax(2, line.height() / 8);
    for (int x = first; x <= last;) {
        if (ink[static_cast<size_t>(x)]) {
            ++x;
            continue;
        }
        const int gapStart = x;
        while (x <= last && !ink[static_cast<size_t>(x)]) {
            ++x;
        }
        const int gapWidth = x - gapStart;
        const int leadingWidth = gapStart - first;
        const int remainingWidth = last - x + 1;
        if (gapWidth >= minimumGap
            && first <= edgeTolerance
            && leadingWidth <= maximumArtifactWidth
            && remainingWidth >= qMax(line.height(), leadingWidth * 2)) {
            first = x;
        }
        break;
    }

    span.left = line.left() + first;
    span.right = line.left() + last;
    return span;
}

HorizontalInkSpan measuredHorizontalInkSpan(const QImage& source,
                                            const TextBlock& block,
                                            int backgroundLuma)
{
    HorizontalInkSpan result;
    for (const QRect& lineBox : block.lineBoxes) {
        const HorizontalInkSpan line = lineHorizontalInkSpan(source, lineBox, backgroundLuma);
        if (!line.isValid()) {
            continue;
        }
        result.left = result.isValid() ? qMin(result.left, line.left) : line.left;
        result.right = result.isValid() ? qMax(result.right, line.right) : line.right;
    }
    return result;
}

int availableBackgroundWidth(const QImage& source,
                             const QRect& box,
                             const QColor& background)
{
    if (source.isNull() || box.isEmpty() || box.right() >= source.width() - 1) {
        return 0;
    }
    const int maxExtension = qMin(qMax(24, box.height() * 4),
                                  source.width() - box.right() - 1);
    const int top = qMax(0, box.top() - kPatchPaddingPx);
    const int bottom = qMin(source.height() - 1,
                            box.bottom() + kPatchPaddingPx);
    const int backgroundLuma = lumaOf(background.rgb());
    int clear = 0;
    for (int x = box.right() + 1; x <= box.right() + maxExtension; ++x) {
        bool backgroundColumn = true;
        for (int y = top; y <= bottom; ++y) {
            if (qAbs(lumaOf(source.pixel(x, y)) - backgroundLuma)
                >= kInkLumaContrast) {
                backgroundColumn = false;
                break;
            }
        }
        if (!backgroundColumn) {
            break;
        }
        ++clear;
    }
    return clear;
}

bool beginsRepeatedLeftIndent(const QRect& previous,
                              const QVector<OcrTextLine>& lines,
                              int lineIndex,
                              const QImage& source,
                              const LineVisualStyle& lineStyle)
{
    const QRect& line = lines[lineIndex].box;
    const double averageHeight = (previous.height() + line.height()) / 2.0;
    const int indentThreshold = qMax(6, qRound(averageHeight * 0.7));
    if (line.left() - previous.left() <= indentThreshold) {
        return false;
    }

    // A standalone line carries no alignment information. Before treating a
    // narrower successor as centered/right-aligned, look ahead for evidence:
    // another stacked line sharing the successor's left margin establishes an
    // indented left-aligned run (typically body text below a flush title).
    const int scanStop = qMin(lines.size(), lineIndex + kStructureLookaheadLines + 1);
    for (int i = lineIndex + 1; i < scanStop; ++i) {
        const OcrTextLine& next = lines[i];
        if (next.text.trimmed().isEmpty()) {
            continue;
        }
        const int leftTolerance = qMax(3, qMin(line.height(), next.box.height()) / 3);
        if (qAbs(next.box.left() - line.left()) > leftTolerance) {
            continue;
        }
        const LineVisualStyle nextStyle = measureLineVisualStyle(source, next.box);
        if (!lineStylesDiffer(lineStyle, nextStyle)
            && linesBelongTogether(line,
                                   next.box,
                                   lineStyle.inkHeight,
                                   nextStyle.inkHeight)) {
            return true;
        }
    }
    return false;
}

// Height of a typical line of the block. Uses the line boxes, not the block
// height divided by the line count — the latter counts the inter-line gaps
// and inflates the font of multi-line paragraphs.
int medianLineHeight(const TextBlock& block)
{
    if (block.lineBoxes.isEmpty()) {
        return block.box.height();
    }
    std::vector<int> heights;
    heights.reserve(static_cast<size_t>(block.lineBoxes.size()));
    for (const QRect& line : block.lineBoxes) {
        heights.push_back(line.height());
    }
    const size_t mid = heights.size() / 2;
    std::nth_element(heights.begin(), heights.begin() + static_cast<qptrdiff>(mid), heights.end());
    return heights[mid];
}

// Median glyph height measured from the source pixels: for each line box,
// the span of rows containing pixels that contrast with the patch
// background. Unlike the raw box height this is free of the detector's
// unclip margin, so it tracks the size the original text actually renders
// at.
int measuredInkHeight(const QImage& source, const TextBlock& block, int backgroundLuma)
{
    if (source.isNull()) {
        return 0;
    }
    std::vector<int> heights;
    heights.reserve(static_cast<size_t>(block.lineBoxes.size()));
    const QRect bounds = source.rect();
    for (const QRect& lineBox : block.lineBoxes) {
        const QRect line = lineBox.intersected(bounds);
        if (line.width() < 3 || line.height() < 3) {
            continue;
        }
        const int span = inkRowSpan(source, line, backgroundLuma);
        if (span > 0) {
            heights.push_back(span);
        }
    }
    if (heights.empty()) {
        return 0;
    }
    const size_t mid = heights.size() / 2;
    std::nth_element(heights.begin(), heights.begin() + static_cast<qptrdiff>(mid), heights.end());
    return heights[mid];
}

bool containsCjk(const QString& text)
{
    for (const QChar ch : text) {
        if (isCjk(ch)) {
            return true;
        }
    }
    return false;
}

QColor medianColor(std::vector<QRgb>& samples)
{
    if (samples.empty()) {
        return QColor(Qt::white);
    }
    const size_t mid = samples.size() / 2;
    auto channelMedian = [&samples, mid](int (*channel)(QRgb)) {
        std::nth_element(samples.begin(), samples.begin() + static_cast<qptrdiff>(mid), samples.end(),
                         [channel](QRgb a, QRgb b) { return channel(a) < channel(b); });
        return channel(samples[mid]);
    };
    const int r = channelMedian(qRed);
    const int g = channelMedian(qGreen);
    const int b = channelMedian(qBlue);
    return QColor(r, g, b);
}

QColor dominantColor(const std::vector<QRgb>& samples)
{
    if (samples.empty()) {
        return QColor(Qt::black);
    }
    struct Bucket {
        int count = 0;
        qint64 red = 0;
        qint64 green = 0;
        qint64 blue = 0;
    };
    std::vector<Bucket> buckets(8 * 8 * 8);
    for (const QRgb rgb : samples) {
        const int index = (qRed(rgb) / 32) * 64
            + (qGreen(rgb) / 32) * 8
            + qBlue(rgb) / 32;
        Bucket& bucket = buckets[static_cast<size_t>(index)];
        ++bucket.count;
        bucket.red += qRed(rgb);
        bucket.green += qGreen(rgb);
        bucket.blue += qBlue(rgb);
    }
    const Bucket* dominant = &buckets.front();
    for (const Bucket& bucket : buckets) {
        if (bucket.count > dominant->count) {
            dominant = &bucket;
        }
    }
    return QColor(static_cast<int>(dominant->red / dominant->count),
                  static_cast<int>(dominant->green / dominant->count),
                  static_cast<int>(dominant->blue / dominant->count));
}

} // namespace

QString TextBlock::mergedText() const
{
    QString merged;
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        if (!merged.isEmpty()) {
            const bool joinDirectly = isCjk(merged.back()) && isCjk(trimmed.front());
            if (!joinDirectly) {
                merged += QLatin1Char(' ');
            }
        }
        merged += trimmed;
    }
    return Ocr::correctConfusableText(merged);
}

BlockTranslationDecision classifyBlockForTranslation(const TextBlock& block)
{
    BlockTranslationDecision decision;
    const QString text = block.mergedText().trimmed();
    if (text.isEmpty()) {
        decision.reason = QStringLiteral("empty-content");
        return decision;
    }

    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, text);
    boundaries.toStart();
    int start = 0;
    bool containsDenseScriptLetter = false;
    while (true) {
        const int end = boundaries.toNextBoundary();
        if (end < 0) {
            break;
        }
        bool containsLetter = false;
        const QList<uint> codePoints = text.mid(start, end - start).toUcs4();
        for (const uint codePoint : codePoints) {
            if (QChar::isLetter(codePoint)) {
                containsLetter = true;
                const QChar::Script script = QChar::script(
                    static_cast<char32_t>(codePoint));
                containsDenseScriptLetter = containsDenseScriptLetter
                    || script == QChar::Script_Han
                    || script == QChar::Script_Hiragana
                    || script == QChar::Script_Katakana
                    || script == QChar::Script_Hangul
                    || script == QChar::Script_Bopomofo;
                break;
            }
        }
        decision.lexicalGraphemes += containsLetter;
        start = end;
    }

    if (decision.lexicalGraphemes == 0) {
        decision.reason = QStringLiteral("no-linguistic-content");
        return decision;
    }
    if (decision.lexicalGraphemes < kMinimumLexicalGraphemes) {
        decision.reason = QStringLiteral("insufficient-linguistic-evidence");
        return decision;
    }
    if (decision.lexicalGraphemes == kMinimumLexicalGraphemes
        && !containsDenseScriptLetter) {
        decision.reason = QStringLiteral("ambiguous-short-alphabetic-token");
        return decision;
    }
    decision.translatable = true;
    decision.reason = QStringLiteral("linguistic-content");
    return decision;
}

QVector<TextBlock> makeLineTranslationUnits(const QVector<OcrTextLine>& lines)
{
    QVector<TextBlock> units;
    units.reserve(lines.size());
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const OcrTextLine& line = lines[lineIndex];
        if (line.text.trimmed().isEmpty() || line.box.isEmpty()) {
            continue;
        }
        TextBlock unit;
        unit.box = line.box;
        unit.lineBoxes.append(line.box);
        unit.sourceLineIndices.append(line.sourceIndex >= 0
                                          ? line.sourceIndex
                                          : lineIndex);
        unit.lines.append(line.text);
        unit.leadingIconSeparated = line.leadingIconSeparated;
        units.append(unit);
    }
    return units;
}

QVector<TextBlock> mergeLinesIntoBlocks(const QVector<OcrTextLine>& lines, const QImage& source)
{
    QVector<TextBlock> blocks;
    QVector<LineVisualStyle> blockLastStyles;
    for (int lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        const OcrTextLine& line = lines[lineIndex];
        if (line.text.trimmed().isEmpty()) {
            continue;
        }
        const LineVisualStyle style = measureLineVisualStyle(source, line.box);
        // Reading order interleaves side-by-side segments (labels, table and
        // multi-column text), so a paragraph's next line is not necessarily
        // in the most recently created block. Search every compatible active
        // predecessor; the geometry gate bounds the useful vertical range.
        int targetIndex = -1;
        int nearestVerticalDistance = std::numeric_limits<int>::max();
        // Every refined icon+label line is a new navigation item. A wrapped
        // continuation has no leading icon and can still join this new block.
        for (int b = line.leadingIconSeparated ? -1 : blocks.size() - 1;
             b >= 0;
             --b) {
            const TextBlock& candidate = blocks[b];
            if (candidate.lineBoxes.size() == 1
                && beginsRepeatedLeftIndent(candidate.lineBoxes.last(),
                                            lines,
                                            lineIndex,
                                            source,
                                            style)) {
                continue;
            }
            if (blockAcceptsLine(candidate, blockLastStyles[b], line.box, style)) {
                const int verticalDistance = line.box.center().y()
                    - candidate.lineBoxes.last().center().y();
                if (verticalDistance < nearestVerticalDistance) {
                    targetIndex = b;
                    nearestVerticalDistance = verticalDistance;
                }
            }
        }
        Perf::log(QStringLiteral("Translate.merge line=%1,%2 %3x%4 ink=%5 bg=%6 fg=%7 contrast=%8 -> %9")
                      .arg(line.box.x())
                      .arg(line.box.y())
                      .arg(line.box.width())
                      .arg(line.box.height())
                      .arg(style.inkHeight)
                      .arg(style.backgroundLuma)
                      .arg(style.foregroundLuma)
                      .arg(style.contrast)
                      .arg(targetIndex >= 0 ? QStringLiteral("block%1").arg(targetIndex)
                                            : QStringLiteral("new")));
        if (targetIndex >= 0) {
            TextBlock& target = blocks[targetIndex];
            target.box = target.box.united(line.box);
            target.lineBoxes.append(line.box);
            target.sourceLineIndices.append(line.sourceIndex >= 0
                                                ? line.sourceIndex
                                                : lineIndex);
            target.lines.append(line.text);
            target.leadingIconSeparated = target.leadingIconSeparated
                || line.leadingIconSeparated;
            blockLastStyles[targetIndex] = style;
            continue;
        }
        TextBlock block;
        block.box = line.box;
        block.lineBoxes.append(line.box);
        block.sourceLineIndices.append(line.sourceIndex >= 0
                                           ? line.sourceIndex
                                           : lineIndex);
        block.lines.append(line.text);
        block.leadingIconSeparated = line.leadingIconSeparated;
        blocks.append(block);
        blockLastStyles.append(style);
    }
    return blocks;
}

Qt::Alignment detectBlockAlignment(const TextBlock& block)
{
    if (block.lineBoxes.size() < 2) {
        return Qt::AlignLeft;
    }
    const int tolerance = qMax(3, medianLineHeight(block) / 2);
    const int boxCenter = block.box.left() + block.box.width() / 2;
    bool left = true;
    bool right = true;
    bool center = true;
    for (const QRect& line : block.lineBoxes) {
        left = left && qAbs(line.left() - block.box.left()) <= tolerance;
        right = right && qAbs(line.right() - block.box.right()) <= tolerance;
        const int lineCenter = line.left() + line.width() / 2;
        center = center && qAbs(lineCenter - boxCenter) <= tolerance;
    }
    // Left-aligned ragged-right text also matches "center" on near-full lines,
    // so left wins ties; center beats right for the same reason.
    if (left) {
        return Qt::AlignLeft;
    }
    if (center) {
        return Qt::AlignHCenter;
    }
    if (right) {
        return Qt::AlignRight;
    }
    return Qt::AlignLeft;
}

PatchStyle estimatePatchStyle(const QImage& source, const QRect& box)
{
    PatchStyle style;
    style.background = QColor(Qt::white);
    style.foreground = QColor(Qt::black);
    if (source.isNull() || box.isEmpty()) {
        return style;
    }
    const QRect bounds = source.rect();
    const QRect inner = box.intersected(bounds);
    if (inner.isEmpty()) {
        return style;
    }

    // Background: median of a ring just outside the box (falls back to the
    // box border where the box touches the image edge).
    const QRect outer = inner.adjusted(-3, -3, 3, 3).intersected(bounds);
    std::vector<QRgb> ring;
    ring.reserve(1024);
    const int ringStep = qMax(1, (outer.width() + outer.height()) * 2 / kColorSampleCap);
    int visited = 0;
    for (int y = outer.top(); y <= outer.bottom(); ++y) {
        const bool edgeRow = y < inner.top() || y > inner.bottom();
        for (int x = outer.left(); x <= outer.right(); ++x) {
            const bool insideBox = !edgeRow && x >= inner.left() && x <= inner.right();
            if (insideBox) {
                x = inner.right(); // skip the interior span
                continue;
            }
            if (visited++ % ringStep == 0) {
                ring.push_back(source.pixel(x, y));
            }
        }
    }
    if (ring.empty()) {
        for (int x = inner.left(); x <= inner.right(); ++x) {
            ring.push_back(source.pixel(x, inner.top()));
            ring.push_back(source.pixel(x, inner.bottom()));
        }
    }
    style.background = medianColor(ring);

    // Foreground: average the interior pixels that contrast most with the
    // background; they are dominated by the original glyphs.
    const int backgroundLuma = lumaOf(style.background.rgb());
    const qint64 interiorPixels = static_cast<qint64>(inner.width()) * inner.height();
    const int step = qMax<qint64>(1, interiorPixels / kColorSampleCap);
    std::vector<QRgb> interior;
    interior.reserve(kColorSampleCap);
    qint64 index = 0;
    for (int y = inner.top(); y <= inner.bottom(); ++y) {
        for (int x = inner.left(); x <= inner.right(); ++x) {
            if (index++ % step == 0) {
                interior.push_back(source.pixel(x, y));
            }
        }
    }
    std::vector<QRgb> contrasting;
    for (QRgb rgb : interior) {
        if (qAbs(lumaOf(rgb) - backgroundLuma) >= 48) {
            contrasting.push_back(rgb);
        }
    }
    if (contrasting.empty()) {
        style.foreground = backgroundLuma >= 128 ? QColor(32, 32, 36) : QColor(240, 240, 244);
        return style;
    }
    style.foregroundMeasured = true;
    // Select the most common glyph-color cluster. A small saturated hyperlink
    // (and its underline) must not recolor an otherwise muted paragraph.
    style.foreground = dominantColor(contrasting);
    return style;
}

LineVisualStyle measureLineVisualStyle(const QImage& source, const QRect& box)
{
    LineVisualStyle result;
    result.inkHeight = lineInkHeight(source, box);
    if (source.isNull() || box.intersected(source.rect()).isEmpty()) {
        return result;
    }
    const PatchStyle style = estimatePatchStyle(source, box);
    result.backgroundLuma = lumaOf(style.background.rgb());
    if (!style.foregroundMeasured) {
        return result;
    }
    result.foregroundLuma = lumaOf(style.foreground.rgb());
    result.contrast = qAbs(result.foregroundLuma - result.backgroundLuma);
    result.foregroundMeasured = true;
    return result;
}

QImage composeTranslatedImage(const QImage& source,
                              const QVector<TextBlock>& blocks,
                              const QStringList& translations,
                              const QString& fontFamily,
                              CompositionReport* report)
{
    if (report) {
        *report = {};
    }
    CompositionReport composition;
    if (source.isNull()) {
        return {};
    }
    if (translations.size() != blocks.size()) {
        Perf::log(QStringLiteral("Translate.compose countMismatch blocks=%1 translations=%2")
                      .arg(blocks.size())
                      .arg(translations.size()));
        return {};
    }
    QImage result = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (result.isNull()) {
        return {};
    }

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    // Pass 1: patch style and target font size per block. The target comes
    // from the measured ink height of the original glyphs; the raw box
    // height (inflated by the detector's unclip margin) is only a fallback.
    struct BlockLayout {
        PatchStyle style;
        HorizontalInkSpan horizontalInk;
        int lineHeight = 0;
        int inkHeight = 0;
        int targetSize = 0;
    };
    std::vector<BlockLayout> layouts(static_cast<size_t>(blocks.size()));
    for (int i = 0; i < blocks.size(); ++i) {
        const TextBlock& block = blocks.at(i);
        const QString translation = translations.at(i).trimmed();
        const bool invalidRegion = block.box.isEmpty()
            || !source.rect().contains(block.box)
            || block.lines.size() != 1
            || block.lineBoxes.size() > 1;
        if (translation.isEmpty() || invalidRegion) {
            Perf::log(QStringLiteral("Translate.compose invalidUnit block=%1 emptyTranslation=%2 invalidRegion=%3 lines=%4 lineBoxes=%5")
                          .arg(i)
                          .arg(translation.isEmpty())
                          .arg(invalidRegion)
                          .arg(block.lines.size())
                          .arg(block.lineBoxes.size()));
            return {};
        }
        if (translation == block.mergedText().trimmed()) {
            composition.equivalentIndices.append(i);
            continue;
        }
        BlockLayout& layout = layouts[static_cast<size_t>(i)];
        layout.style = estimatePatchStyle(source, block.box);
        const int backgroundLuma = lumaOf(layout.style.background.rgb());
        layout.horizontalInk = measuredHorizontalInkSpan(source, block, backgroundLuma);
        layout.lineHeight = medianLineHeight(block);
        layout.inkHeight = measuredInkHeight(source, block, backgroundLuma);
        const bool cjkTarget = containsCjk(translation);
        int target;
        if (layout.inkHeight >= 4 && layout.inkHeight <= layout.lineHeight) {
            target = qRound(layout.inkHeight * (cjkTarget ? kCjkEmPerInk : kLatinEmPerInk));
        } else {
            target = qRound(layout.lineHeight * kLineFillRatio);
        }
        layout.targetSize = qBound(kMinFontPixelSize, target, kMaxFontPixelSize);
    }

    // Pass 2: paint.
    for (int i = 0; i < blocks.size(); ++i) {
        const TextBlock& block = blocks.at(i);
        const QString translation = translations.at(i).trimmed();
        const BlockLayout& layout = layouts[static_cast<size_t>(i)];
        if (translation.isEmpty() || block.box.isEmpty() || layout.targetSize <= 0
            || translation == block.mergedText().trimmed()) {
            continue;
        }

        // The det box carries the unclip margin on every side, so it can
        // reach into neighboring artwork (leading icons, bullets). The
        // measured ink height tells how thick that margin is; pull the
        // patch back in by it so only the actual text area is covered.
        int inset = 0;
        if (layout.inkHeight >= 4 && layout.inkHeight <= layout.lineHeight) {
            inset = qMax(0, (layout.lineHeight - layout.inkHeight) / 2 - 2);
            inset = qMin(inset, qMin(block.box.width(), block.box.height()) / 4);
        }
        // A refined navigation box already begins after the icon separator.
        // Preserve that edge so the patch covers the first source glyph and the
        // translation starts at the original label column; other blocks still
        // use ink-based insets to avoid nearby artwork.
        int leftInset = block.leadingIconSeparated ? 0 : inset;
        int rightInset = inset;
        if (layout.horizontalInk.isValid()) {
            if (!block.leadingIconSeparated) {
                leftInset = qMax(leftInset,
                                 layout.horizontalInk.left - block.box.left());
            }
            rightInset = qMax(rightInset,
                              block.box.right() - layout.horizontalInk.right);
            const int maxHorizontalInset = qMax(0, block.box.width() - 1);
            leftInset = qBound(0, leftInset, maxHorizontalInset);
            rightInset = qBound(0, rightInset, maxHorizontalInset - leftInset);
        }
        QRect content = block.box.adjusted(leftInset, inset, -rightInset, -inset);

        // Fit the translation into the content area: start from the target
        // size and shrink until it wraps within the bounds.
        QFont font = fontFamily.isEmpty() ? QFont() : QFont(fontFamily);
        int pixelSize = layout.targetSize;
        const Qt::Alignment hAlign = detectBlockAlignment(block);
        const int flags = static_cast<int>(hAlign) | Qt::AlignVCenter | Qt::TextWordWrap;
        QRect needed;
        while (true) {
            font.setPixelSize(pixelSize);
            needed = QFontMetrics(font).boundingRect(content, flags, translation);
            if ((needed.width() <= content.width() + kPatchPaddingPx * 2
                 && needed.height() <= content.height() + kPatchPaddingPx * 2)
                || pixelSize <= kMinFontPixelSize) {
                break;
            }
            --pixelSize;
        }

        // A short source label can translate to a wider target (Bio -> 个人简历).
        // If the minimum font still wraps, use verified empty background to the
        // right rather than clipping a second line into the one-line patch.
        if (block.lineBoxes.size() == 1
            && translation.size() > block.mergedText().size()) {
            const int required = qMax(0,
                QFontMetrics(font).horizontalAdvance(translation)
                    - content.width() + kPatchPaddingPx * 2);
            if (required > 0) {
                const int available = availableBackgroundWidth(
                    source, block.box, layout.style.background);
                const int extension = qMin(available, required);
                if (extension > 0) {
                    content.setRight(content.right() + extension);
                    needed = QFontMetrics(font).boundingRect(
                        content, flags, translation);
                }
            }
        }

        if (needed.width() > content.width() + kPatchPaddingPx * 2
            || needed.height() > content.height() + kPatchPaddingPx * 2) {
            Perf::log(QStringLiteral("Translate.compose preservedUnfit block=%1 box=%2x%3 needed=%4x%5 font=%6 chars=%7")
                          .arg(i)
                          .arg(content.width())
                          .arg(content.height())
                          .arg(needed.width())
                          .arg(needed.height())
                          .arg(pixelSize)
                          .arg(translation.size()));
            composition.unfitIndices.append(i);
            continue;
        }

        QPainterPath path;
        const QRect patchRect = content
            .adjusted(-kPatchPaddingPx, -kPatchPaddingPx,
                      kPatchPaddingPx, kPatchPaddingPx)
            .intersected(result.rect());
        const qreal radius = qMin<qreal>(4.0, patchRect.height() / 4.0);
        path.addRoundedRect(patchRect, radius, radius);
        if (path.isEmpty()) {
            Perf::log(QStringLiteral("Translate.compose invalidPatch block=%1")
                          .arg(i));
            return {};
        }
        const QRect patch = path.boundingRect().toAlignedRect().intersected(result.rect());
        painter.fillPath(path, layout.style.background);

        painter.setFont(font);
        painter.setPen(layout.style.foreground);
        const int drawFlags = needed.height() <= patch.height()
            ? flags
            : (static_cast<int>(hAlign) | Qt::AlignTop | Qt::TextWordWrap);
        // Even at the minimum font a long translation can overflow the patch;
        // clip so it never paints over the surrounding image.
        painter.save();
        painter.setClipPath(path);
        painter.drawText(patch.adjusted(kPatchPaddingPx, 0, -kPatchPaddingPx, 0), drawFlags, translation);
        painter.restore();
        ++composition.appliedCount;

        Perf::log(QStringLiteral("Translate.compose block=%1 lines=%2 box=%3,%4 %5x%6 lineH=%7 ink=%8 inset=%9,%10,%11 font=%12->%13 align=%14 chars=%15")
                      .arg(i)
                      .arg(block.lines.size())
                      .arg(block.box.x())
                      .arg(block.box.y())
                      .arg(block.box.width())
                      .arg(block.box.height())
                      .arg(layout.lineHeight)
                      .arg(layout.inkHeight)
                      .arg(leftInset)
                      .arg(inset)
                      .arg(rightInset)
                      .arg(layout.targetSize)
                      .arg(pixelSize)
                      .arg(hAlign == Qt::AlignHCenter ? QStringLiteral("C")
                                                      : hAlign == Qt::AlignRight ? QStringLiteral("R")
                                                                                 : QStringLiteral("L"))
                      .arg(translation.size()));
    }
    painter.end();
    const int classifiedCount = composition.appliedCount
        + composition.equivalentIndices.size()
        + composition.unfitIndices.size();
    if (classifiedCount != blocks.size()) {
        Perf::log(QStringLiteral("Translate.compose incompleteReport classified=%1 blocks=%2")
                      .arg(classifiedCount)
                      .arg(blocks.size()));
        return {};
    }
    if (report) {
        *report = composition;
    }
    return result;
}

} // namespace Visnip::Translate

#pragma once

#include "core/OcrPostProcess.h"

#include <QColor>
#include <QImage>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtCore/qnamespace.h>

namespace Visnip::Translate {

// A visual block used either for non-destructive paragraph context analysis
// or as a one-line translation unit. Destructive composition accepts only the
// one-line form so grouping changes can never widen a patch across regions.
struct TextBlock {
    QRect box; // union of the line boxes, source-image pixels
    QVector<QRect> lineBoxes;
    QVector<int> sourceLineIndices;
    QStringList lines;
    // The OCR stage already moved this block's left edge past a leading icon.
    // Composition must not inset that edge a second time.
    bool leadingIconSeparated = false;

    // Joins the lines for translation: CJK-adjacent lines join directly,
    // everything else gets a space between lines.
    QString mergedText() const;
};

// Fail-closed decision used before a block is sent to the translation API.
// OCR confidence answers "which glyph is this?", not "is this region text?";
// therefore a block must contain enough Unicode letter graphemes to establish
// linguistic content. Numbers, punctuation, symbols, ambiguous one-glyph
// regions and two-letter alphabetic tokens are preserved in the source image
// instead of being repainted. Two CJK/Kana/Hangul graphemes carry enough
// linguistic evidence to remain eligible.
struct BlockTranslationDecision {
    bool translatable = false;
    int lexicalGraphemes = 0;
    QString reason;
};

BlockTranslationDecision classifyBlockForTranslation(const TextBlock& block);

// Creates stable one-line translation units. Layout grouping is deliberately
// separate: changing paragraph/title grouping must never change request/result
// identity or cause two source lines to share one destructive patch.
QVector<TextBlock> makeLineTranslationUnits(const QVector<OcrTextLine>& lines);

// Groups reading-ordered OCR lines into paragraphs. Lines merge only when
// they are vertically stacked, of similar size, and horizontally aligned;
// side-by-side segments (table columns, toolbar labels) stay separate, and
// a gap clearly beyond the block's line rhythm starts a new block (heading
// vs body, paragraph spacing). A line may continue any spatially compatible
// earlier block, so interleaved reading order (labels or a second column
// between paragraph lines) does not break a paragraph apart.
//
// When source (the image the boxes live in) is provided, "similar size" is
// judged by the measured ink height of each line — det box heights carry
// unclip margins that flatten font-size differences on short lines, so a
// heading can share a box height with body text. Without source the box
// heights are the only signal available.
QVector<TextBlock> mergeLinesIntoBlocks(const QVector<OcrTextLine>& lines,
                                        const QImage& source = QImage());

// Dominant horizontal alignment of the block's lines within its box, used to
// lay the translation out like the original. Single-line blocks report left
// (the box carries no alignment information).
Qt::Alignment detectBlockAlignment(const TextBlock& block);

struct PatchStyle {
    QColor background;
    QColor foreground;
    bool foregroundMeasured = false;
};

struct LineVisualStyle {
    int inkHeight = 0;
    int backgroundLuma = -1;
    int foregroundLuma = -1;
    int contrast = -1;
    bool foregroundMeasured = false;
};

// Estimates the cover color (median of the pixels ringing the box) and the
// text color (average of the box pixels contrasting most with the cover)
// for one block.
PatchStyle estimatePatchStyle(const QImage& source, const QRect& box);

// Returns the same pixel measurements used to identify visual boundaries
// while grouping OCR lines. Unmeasurable luminance values remain -1.
LineVisualStyle measureLineVisualStyle(const QImage& source, const QRect& box);

struct CompositionReport {
    int appliedCount = 0;
    QVector<int> equivalentIndices;
    QVector<int> unfitIndices;
};

// Returns a copy of source with every one-line translation unit covered by a
// background-colored patch carrying its translation. translations[i] belongs
// to blocks[i]; count mismatches, empty translations, multi-line units and
// invalid regions fail explicitly. fontFamily falls back to the application
// default when empty. Font size is measured independently for every source
// line so title/body hierarchy cannot be erased by cross-region clustering.
QImage composeTranslatedImage(const QImage& source,
                              const QVector<TextBlock>& blocks,
                              const QStringList& translations,
                              const QString& fontFamily = QString(),
                              CompositionReport* report = nullptr);

} // namespace Visnip::Translate

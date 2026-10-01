#pragma once

#include <QImage>
#include <QVector>

#include <optional>

namespace Visnip::LongCapture {

// Per-row projection of a frame: mean luma, edge energy, and two horizontal
// ink projections. The spatial projections disambiguate repeated rows without
// turning document alignment into a full image comparison.
struct RowSignature {
    QVector<float> luma;
    QVector<float> edge;
    QVector<float> spatialA;
    QVector<float> spatialB;

    int height() const { return luma.size(); }
    bool isEmpty() const { return luma.isEmpty(); }
    bool isValid() const
    {
        const bool spatialEmpty = spatialA.isEmpty() && spatialB.isEmpty();
        const bool spatialValid = spatialA.size() == luma.size() && spatialB.size() == luma.size();
        return !luma.isEmpty() && edge.size() == luma.size() && (spatialEmpty || spatialValid);
    }
    void clear()
    {
        luma.clear();
        edge.clear();
        spatialA.clear();
        spatialB.clear();
    }
};

RowSignature computeRowSignature(const QImage& image);

struct MatchResult {
    bool valid = false;
    int documentY = 0;     // document row of the frame's top edge
    double confidence = 0.0;
    double score = 255.0;  // weighted mean row difference at the best alignment (lower is better)
    int overlap = 0;       // rows shared with the document at the best alignment
    bool ambiguous = false;
};

// Locate `frame` inside the document signature. `docTopY` is the document row
// of doc.luma[0]. Candidate positions run over [minDocY, maxDocY] inclusive;
// positions overlapping the document by fewer than `minOverlap` rows are
// skipped. `coarseStep` > 1 uses bounded row sampling to choose a candidate,
// then evaluates the surrounding offsets at full resolution.
MatchResult matchSignatureInDocument(const RowSignature& doc,
                                     int docTopY,
                                     const RowSignature& frame,
                                     int minDocY,
                                     int maxDocY,
                                     int minOverlap,
                                     int coarseStep = 1,
                                     std::optional<int> preferredDocumentY = std::nullopt);

// Weighted mean difference of two aligned signature windows. 255 when the
// windows do not overlap. Cheap same-frame test: difference at offset 0.
double signatureDifference(const RowSignature& a, int ay,
                           const RowSignature& b, int by,
                           int height);

// Remaining time before scroll activity is considered quiet. A future
// activity timestamp is treated as current activity so clock/order skew cannot
// shorten the safety window.
int remainingQuietPeriodMs(qint64 nowMs,
                           qint64 lastWheelAtMs,
                           qint64 lastMovementAtMs,
                           int quietPeriodMs);

} // namespace Visnip::LongCapture

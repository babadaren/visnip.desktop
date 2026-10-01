#include "core/LongCaptureMatch.h"

#include <QtGlobal>

#include <cmath>

namespace Visnip::LongCapture {
namespace {

inline double rowPairDifference(const RowSignature& a, int ai,
                                const RowSignature& b, int bi,
                                double* weightOut)
{
    const double lumaDiff = qAbs(a.luma[ai] - b.luma[bi]);
    const double edgeDiff = qAbs(a.edge[ai] - b.edge[bi]);
    double spatialDiff = 0.0;
    if (!a.spatialA.isEmpty() && !b.spatialA.isEmpty()) {
        spatialDiff = (qAbs(a.spatialA[ai] - b.spatialA[bi])
                       + qAbs(a.spatialB[ai] - b.spatialB[bi]))
            * 0.5;
    }
    const double weight = 1.0 + qMin(6.0, (a.edge[ai] + b.edge[bi]) * 0.5);
    *weightOut = weight;
    return (lumaDiff + edgeDiff * 0.35 + spatialDiff * 0.75) * weight;
}

struct AlignmentScore {
    double score = 255.0;
    int overlap = 0;
    double meanWeight = 0.0;
};

AlignmentScore scoreAlignment(const RowSignature& doc, int docTopY,
                              const RowSignature& frame, int documentY,
                              int maxSamples = 0)
{
    AlignmentScore result;
    const int docHeight = doc.height();
    const int frameHeight = frame.height();
    const int overlapStart = qMax(documentY, docTopY);
    const int overlapEnd = qMin(documentY + frameHeight, docTopY + docHeight);
    const int overlap = overlapEnd - overlapStart;
    if (overlap <= 0) {
        return result;
    }
    const int docIndex = overlapStart - docTopY;
    const int frameIndex = overlapStart - documentY;
    const int rowStep = maxSamples > 0
        ? qMax(1, (overlap + maxSamples - 1) / maxSamples)
        : 1;
    double total = 0.0;
    double totalWeight = 0.0;
    int samples = 0;
    for (int i = 0; i < overlap; i += rowStep) {
        double weight = 0.0;
        total += rowPairDifference(doc, docIndex + i, frame, frameIndex + i, &weight);
        totalWeight += weight;
        ++samples;
    }
    result.score = totalWeight > 0.0 ? total / totalWeight : 255.0;
    result.overlap = overlap;
    result.meanWeight = samples > 0 ? totalWeight / samples : 0.0;
    return result;
}

} // namespace

RowSignature computeRowSignature(const QImage& image)
{
    RowSignature signature;
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return signature;
    }
    QImage source = image;
    if (source.format() != QImage::Format_ARGB32 && source.format() != QImage::Format_RGB32) {
        source = source.convertToFormat(QImage::Format_ARGB32);
    }
    const int width = source.width();
    const int height = source.height();
    const int step = qMax(1, width / 192);
    signature.luma.resize(height);
    signature.edge.resize(height);
    signature.spatialA.resize(height);
    signature.spatialB.resize(height);
    for (int y = 0; y < height; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(source.constScanLine(y));
        int samples = 0;
        double lumaTotal = 0.0;
        double edgeTotal = 0.0;
        double spatialATotal = 0.0;
        double spatialBTotal = 0.0;
        int previousGray = qGray(line[0]);
        for (int x = 0; x < width; x += step) {
            const int gray = qGray(line[x]);
            const double ink = 255.0 - gray;
            lumaTotal += gray;
            edgeTotal += qAbs(gray - previousGray);
            const int projectionIndex = x / step;
            spatialATotal += ink * ((projectionIndex & 1) == 0 ? 1.0 : -1.0);
            spatialBTotal += ink * ((projectionIndex % 5) < 2 ? 1.0 : -1.0);
            previousGray = gray;
            ++samples;
        }
        signature.luma[y] = samples > 0 ? static_cast<float>(lumaTotal / samples) : 0.0f;
        signature.edge[y] = samples > 0 ? static_cast<float>(edgeTotal / samples) : 0.0f;
        signature.spatialA[y] = samples > 0 ? static_cast<float>(spatialATotal / samples) : 0.0f;
        signature.spatialB[y] = samples > 0 ? static_cast<float>(spatialBTotal / samples) : 0.0f;
    }
    // Fold the vertical gradient of the luma profile into the edge channel:
    // rows where the profile changes are exactly the rows that pin down an
    // alignment, even when the content has few horizontal edges (e.g. bands
    // of solid color).
    for (int y = height - 1; y > 0; --y) {
        signature.edge[y] += qAbs(signature.luma[y] - signature.luma[y - 1]);
    }
    return signature;
}

MatchResult matchSignatureInDocument(const RowSignature& doc,
                                     int docTopY,
                                     const RowSignature& frame,
                                     int minDocY,
                                     int maxDocY,
                                     int minOverlap,
                                     int coarseStep,
                                     std::optional<int> preferredDocumentY)
{
    MatchResult result;
    if (!doc.isValid() || !frame.isValid() || minDocY > maxDocY) {
        return result;
    }
    const int step = qMax(1, coarseStep);
    minOverlap = qMax(1, minOverlap);

    struct CandidateScore {
        int documentY = 0;
        double score = 255.0;
    };

    constexpr qint64 kCandidateReserveLimit = 262144;
    QVector<CandidateScore> coarseCandidates;
    const qint64 candidateCount = static_cast<qint64>(maxDocY) - minDocY + 1;
    coarseCandidates.reserve(static_cast<qsizetype>(qMin(candidateCount, kCandidateReserveLimit)));
    double bestScore = 255.0;
    int bestY = 0;
    int bestOverlap = 0;
    double bestMeanWeight = 0.0;
    bool found = false;
    // Runner-up must be at least this far from the winner to count as a
    // distinct alternative; nearby positions always score similarly.
    constexpr int kDistinctDistance = 12;
    constexpr double kEquivalentScoreTolerance = 0.01;

    const auto candidateIsBetter = [&](double score, int documentY) {
        if (!found || score < bestScore - kEquivalentScoreTolerance) {
            return true;
        }
        if (!preferredDocumentY) {
            return score < bestScore;
        }
        if (qAbs(score - bestScore) > kEquivalentScoreTolerance) {
            return false;
        }
        return qAbs(documentY - *preferredDocumentY)
            < qAbs(bestY - *preferredDocumentY);
    };

    auto considerCoarse = [&](int documentY) {
        constexpr int kMaxCoarseSamples = 48;
        const AlignmentScore aligned = scoreAlignment(doc,
                                                       docTopY,
                                                       frame,
                                                       documentY,
                                                       step > 1 ? kMaxCoarseSamples : 0);
        if (aligned.overlap < minOverlap) {
            return;
        }
        coarseCandidates.append({documentY, aligned.score});
        if (candidateIsBetter(aligned.score, documentY)) {
            bestScore = aligned.score;
            bestOverlap = aligned.overlap;
            bestMeanWeight = aligned.meanWeight;
            bestY = documentY;
            found = true;
        }
    };

    // Every candidate position is visited row-by-row on purpose: content with
    // sharp per-row variation only scores well at the exact alignment, so a
    // strided scan would miss it entirely. "Coarse" refers to the subsampled
    // scoring (kMaxCoarseSamples rows per candidate); the refine pass rescoring
    // at full resolution compensates for subsampling error around the winner.
    for (qint64 documentY = minDocY; documentY <= maxDocY; ++documentY) {
        considerCoarse(static_cast<int>(documentY));
    }
    if (found && step > 1) {
        const int coarseBestY = bestY;
        const int refineStart = qMax(minDocY, coarseBestY - step + 1);
        const int refineEnd = qMin(maxDocY, coarseBestY + step - 1);
        found = false;
        bestScore = 255.0;
        for (int documentY = refineStart; documentY <= refineEnd; ++documentY) {
            const AlignmentScore aligned = scoreAlignment(doc, docTopY, frame, documentY);
            if (aligned.overlap < minOverlap) {
                continue;
            }
            if (candidateIsBetter(aligned.score, documentY)) {
                bestScore = aligned.score;
                bestOverlap = aligned.overlap;
                bestMeanWeight = aligned.meanWeight;
                bestY = documentY;
                found = true;
            }
        }
    }
    if (!found) {
        return result;
    }

    double secondScore = 255.0;
    for (const CandidateScore& candidate : coarseCandidates) {
        if (qAbs(candidate.documentY - bestY) >= kDistinctDistance) {
            secondScore = qMin(secondScore, candidate.score);
        }
    }
    bool hasEquivalentFullResolutionCandidate = false;
    for (const CandidateScore& candidate : coarseCandidates) {
        if (qAbs(candidate.documentY - bestY) < kDistinctDistance
            || candidate.score > bestScore + kEquivalentScoreTolerance) {
            continue;
        }
        const AlignmentScore fullResolution = scoreAlignment(doc,
                                                              docTopY,
                                                              frame,
                                                              candidate.documentY);
        if (fullResolution.score <= bestScore + kEquivalentScoreTolerance) {
            hasEquivalentFullResolutionCandidate = true;
            break;
        }
    }

    const double absolute = qBound(0.0, (40.0 - bestScore) / 40.0, 1.0);
    const double distinct = secondScore < 255.0
        ? qBound(0.0, (secondScore - bestScore) / qMax(6.0, secondScore), 1.0)
        : (bestScore <= 4.0 ? 0.6 : 0.0);
    // Featureless overlaps (weights stay near their 1.0 floor) match every
    // position equally well; scale confidence down so flat regions never
    // produce a confident offset.
    const double texture = qBound(0.0, (bestMeanWeight - 1.0) / 1.5, 1.0);
    // A tiny exact overlap is not sufficient evidence for a stitch. Evidence
    // ramps up after 10% of a frame and becomes complete around 38%, keeping
    // normal wheel steps usable without trusting edge-only coincidences.
    const double overlapRatio = static_cast<double>(bestOverlap) / qMax(1, frame.height());
    const double overlapSupport = qBound(0.0, (overlapRatio - 0.10) / 0.28, 1.0);
    result.ambiguous = hasEquivalentFullResolutionCandidate;
    result.valid = true;
    result.documentY = bestY;
    result.score = bestScore;
    result.overlap = bestOverlap;
    result.confidence = qBound(0.0,
                               (absolute * 0.45 + distinct * 0.55)
                                   * (0.25 + 0.75 * texture)
                                   * overlapSupport,
                               1.0);
    if (result.ambiguous) {
        // Exact periodic content has no observable absolute phase. Keep it
        // below the normal acceptance threshold instead of silently choosing
        // one of several equally valid document positions.
        result.confidence = qMin(result.confidence, 0.34);
    }
    return result;
}

double signatureDifference(const RowSignature& a, int ay,
                           const RowSignature& b, int by,
                           int height)
{
    if (!a.isValid() || !b.isValid()) {
        return 255.0;
    }
    const int overlap = qMin(qMin(a.height() - ay, b.height() - by), height);
    if (ay < 0 || by < 0 || overlap <= 0) {
        return 255.0;
    }
    double total = 0.0;
    double totalWeight = 0.0;
    for (int i = 0; i < overlap; ++i) {
        double weight = 0.0;
        total += rowPairDifference(a, ay + i, b, by + i, &weight);
        totalWeight += weight;
    }
    return totalWeight > 0.0 ? total / totalWeight : 255.0;
}

int remainingQuietPeriodMs(qint64 nowMs,
                           qint64 lastWheelAtMs,
                           qint64 lastMovementAtMs,
                           int quietPeriodMs)
{
    if (quietPeriodMs <= 0) {
        return 0;
    }
    const qint64 latestActivityMs = qMax(lastWheelAtMs, lastMovementAtMs);
    if (latestActivityMs < 0) {
        return 0;
    }
    const qint64 quietAgeMs = qMax<qint64>(0, nowMs - latestActivityMs);
    return static_cast<int>(qBound<qint64>(qint64{0},
                                          static_cast<qint64>(quietPeriodMs) - quietAgeMs,
                                          static_cast<qint64>(quietPeriodMs)));
}

} // namespace Visnip::LongCapture

#include "core/OcrPostProcess.h"

#include <QStringList>
#include <QTextBoundaryFinder>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Visnip::Ocr {

namespace {

struct MaskComponent {
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    qint64 pixels = 0;
    double probSum = 0.0;
};

} // namespace

QVector<QRect> extractDetBoxes(const float* prob,
                               int mapW,
                               int mapH,
                               double scaleToSourceX,
                               double scaleToSourceY,
                               const QSize& sourceSize,
                               const DetParams& params)
{
    if (!prob || mapW <= 0 || mapH <= 0 || sourceSize.isEmpty()) {
        return {};
    }

    // Two-pass connected-component labeling (8-connectivity) with union-find
    // over the binarized probability map.
    const int pixelCount = mapW * mapH;
    std::vector<int> labels(static_cast<size_t>(pixelCount), 0);
    std::vector<int> parent(1, 0); // parent[0] unused (background)

    auto findRoot = [&parent](int label) {
        while (parent[static_cast<size_t>(label)] != label) {
            parent[static_cast<size_t>(label)] =
                parent[static_cast<size_t>(parent[static_cast<size_t>(label)])];
            label = parent[static_cast<size_t>(label)];
        }
        return label;
    };
    auto unite = [&parent, &findRoot](int a, int b) {
        const int ra = findRoot(a);
        const int rb = findRoot(b);
        if (ra != rb) {
            parent[static_cast<size_t>(qMax(ra, rb))] = qMin(ra, rb);
        }
    };

    const float threshold = static_cast<float>(params.binaryThreshold);
    for (int y = 0; y < mapH; ++y) {
        for (int x = 0; x < mapW; ++x) {
            const int idx = y * mapW + x;
            if (prob[idx] <= threshold) {
                continue;
            }
            const int left = (x > 0) ? labels[static_cast<size_t>(idx - 1)] : 0;
            const int topLeft = (x > 0 && y > 0) ? labels[static_cast<size_t>(idx - mapW - 1)] : 0;
            const int top = (y > 0) ? labels[static_cast<size_t>(idx - mapW)] : 0;
            const int topRight = (x + 1 < mapW && y > 0) ? labels[static_cast<size_t>(idx - mapW + 1)] : 0;

            int label = 0;
            for (const int neighbor : { left, topLeft, top, topRight }) {
                if (neighbor == 0) {
                    continue;
                }
                if (label == 0) {
                    label = neighbor;
                } else if (neighbor != label) {
                    unite(label, neighbor);
                }
            }
            if (label == 0) {
                label = static_cast<int>(parent.size());
                parent.push_back(label);
            }
            labels[static_cast<size_t>(idx)] = label;
        }
    }

    std::vector<MaskComponent> components(parent.size());
    std::vector<bool> seen(parent.size(), false);
    for (int y = 0; y < mapH; ++y) {
        for (int x = 0; x < mapW; ++x) {
            const int idx = y * mapW + x;
            const int label = labels[static_cast<size_t>(idx)];
            if (label == 0) {
                continue;
            }
            const int root = findRoot(label);
            MaskComponent& c = components[static_cast<size_t>(root)];
            if (!seen[static_cast<size_t>(root)]) {
                seen[static_cast<size_t>(root)] = true;
                c.minX = c.maxX = x;
                c.minY = c.maxY = y;
            } else {
                c.minX = qMin(c.minX, x);
                c.maxX = qMax(c.maxX, x);
                c.minY = qMin(c.minY, y);
                c.maxY = qMax(c.maxY, y);
            }
            ++c.pixels;
            c.probSum += prob[idx];
        }
    }

    QVector<QRect> boxes;
    const QRect sourceBounds(QPoint(0, 0), sourceSize);
    for (size_t i = 1; i < components.size(); ++i) {
        if (!seen[i]) {
            continue;
        }
        const MaskComponent& c = components[i];
        const double score = c.probSum / static_cast<double>(c.pixels);
        if (score < params.boxScoreThreshold) {
            continue;
        }
        const int mapBoxW = c.maxX - c.minX + 1;
        const int mapBoxH = c.maxY - c.minY + 1;
        if (qMin(mapBoxW, mapBoxH) < params.minMapSidePx) {
            continue;
        }

        // DB shrinks text regions during training; compensate by expanding
        // each side by area * ratio / perimeter (the polygon-offset distance
        // for an axis-aligned rectangle).
        const double area = static_cast<double>(mapBoxW) * mapBoxH;
        const double perimeter = 2.0 * (mapBoxW + mapBoxH);
        const double offset = area * params.unclipRatio / perimeter;

        const double left = (c.minX - offset) * scaleToSourceX;
        const double top = (c.minY - offset) * scaleToSourceY;
        const double right = (c.maxX + 1 + offset) * scaleToSourceX;
        const double bottom = (c.maxY + 1 + offset) * scaleToSourceY;

        QRect box(QPoint(static_cast<int>(std::floor(left)), static_cast<int>(std::floor(top))),
                  QPoint(static_cast<int>(std::ceil(right)) - 1, static_cast<int>(std::ceil(bottom)) - 1));
        box = box.intersected(sourceBounds);
        if (box.width() < 3 || box.height() < 3) {
            continue;
        }
        boxes.append(box);
    }

    sortBoxesInReadingOrder(boxes);
    return boxes;
}

QString ctcGreedyDecode(const float* probs,
                        int timeSteps,
                        int numClasses,
                        const QVector<QString>& charset,
                        float* meanScore)
{
    if (meanScore) {
        *meanScore = 0.0f;
    }
    if (!probs || timeSteps <= 0 || numClasses <= 1 || charset.isEmpty()) {
        return {};
    }
    const int spaceClass = charset.size() + 1;

    QString text;
    double scoreSum = 0.0;
    int emitted = 0;
    int previousClass = 0;
    for (int t = 0; t < timeSteps; ++t) {
        const float* row = probs + static_cast<qsizetype>(t) * numClasses;
        int best = 0;
        float bestProb = row[0];
        for (int c = 1; c < numClasses; ++c) {
            if (row[c] > bestProb) {
                bestProb = row[c];
                best = c;
            }
        }
        if (best != 0 && best != previousClass) {
            if (best <= charset.size()) {
                text += charset.at(best - 1);
            } else if (best == spaceClass) {
                text += QLatin1Char(' ');
            } else {
                previousClass = best;
                continue; // class outside charset+space: ignore
            }
            scoreSum += bestProb;
            ++emitted;
        }
        previousClass = best;
    }

    if (meanScore && emitted > 0) {
        *meanScore = static_cast<float>(scoreSum / emitted);
    }
    return text;
}

QString reverseArabicPrediction(const QString& text)
{
    QStringList parts;
    QString latinRun;
    const auto isLatinRun = [](QChar character) {
        return (character >= QLatin1Char('a') && character <= QLatin1Char('z'))
            || (character >= QLatin1Char('A') && character <= QLatin1Char('Z'))
            || character.isDigit()
            || QStringLiteral(" :*./%+-").contains(character);
    };
    for (const QChar character : text) {
        if (isLatinRun(character)) {
            latinRun += character;
            continue;
        }
        if (!latinRun.isEmpty()) {
            parts.append(latinRun);
            latinRun.clear();
        }
        parts.append(QString(character));
    }
    if (!latinRun.isEmpty()) {
        parts.append(latinRun);
    }
    std::reverse(parts.begin(), parts.end());
    return parts.join(QString());
}

namespace {

int graphemeCount(const QString& text)
{
    const QString trimmed = text.trimmed();
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, trimmed);
    int count = 0;
    finder.toStart();
    while (finder.toNextBoundary() >= 0) {
        ++count;
    }
    return count;
}

bool isSameRow(const QRect& candidate, const QRect& peer)
{
    const int centerDifference = qAbs(candidate.center().y() - peer.center().y());
    return centerDifference * 100
        <= qMax(candidate.height(), peer.height()) * 35;
}

int horizontalGap(const QRect& candidate, const QRect& peer)
{
    if (candidate.center().x() <= peer.center().x()) {
        return peer.left() - candidate.right() - 1;
    }
    return candidate.left() - peer.right() - 1;
}

int lumaOf(QRgb rgb)
{
    return (qRed(rgb) * 299 + qGreen(rgb) * 587 + qBlue(rgb) * 114) / 1000;
}

struct ForegroundMask {
    std::vector<bool> pixels;
    int inkPixels = 0;
};

ForegroundMask foregroundMask(const QImage& source, const QRect& box)
{
    if (source.isNull() || !source.rect().contains(box)
        || box.width() < 3 || box.height() < 3) {
        return {};
    }

    std::vector<int> lumas;
    lumas.reserve(static_cast<size_t>(box.width() * box.height()));
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            lumas.push_back(lumaOf(source.pixel(x, y)));
        }
    }
    const size_t mid = lumas.size() / 2;
    std::nth_element(lumas.begin(),
                     lumas.begin() + static_cast<qptrdiff>(mid),
                     lumas.end());
    const int backgroundLuma = lumas[mid];

    ForegroundMask result;
    result.pixels.resize(lumas.size(), false);
    size_t offset = 0;
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x, ++offset) {
            if (qAbs(lumaOf(source.pixel(x, y)) - backgroundLuma) >= 48) {
                result.pixels[offset] = true;
                ++result.inkPixels;
            }
        }
    }
    const int area = box.width() * box.height();
    if (result.inkPixels < qMax(4, area / 30)
        || result.inkPixels * 4 > area * 3) {
        return {};
    }
    return result;
}

struct BinaryComponent {
    int pixels = 0;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

QVector<BinaryComponent> connectedComponents(const std::vector<bool>& pixels,
                                           int width,
                                           int height)
{
    QVector<BinaryComponent> components;
    if (width <= 0 || height <= 0
        || pixels.size() != static_cast<size_t>(width * height)) {
        return components;
    }

    std::vector<bool> visited(pixels.size(), false);
    std::vector<int> pending;
    for (int seed = 0; seed < static_cast<int>(pixels.size()); ++seed) {
        if (!pixels[static_cast<size_t>(seed)]
            || visited[static_cast<size_t>(seed)]) {
            continue;
        }
        const int seedX = seed % width;
        const int seedY = seed / width;
        BinaryComponent component { 0, seedX, seedY, seedX, seedY };
        pending.clear();
        pending.push_back(seed);
        visited[static_cast<size_t>(seed)] = true;
        while (!pending.empty()) {
            const int current = pending.back();
            pending.pop_back();
            const int x = current % width;
            const int y = current / width;
            ++component.pixels;
            component.left = qMin(component.left, x);
            component.top = qMin(component.top, y);
            component.right = qMax(component.right, x);
            component.bottom = qMax(component.bottom, y);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((dx == 0 && dy == 0)
                        || x + dx < 0 || x + dx >= width
                        || y + dy < 0 || y + dy >= height) {
                        continue;
                    }
                    const int neighbor = (y + dy) * width + x + dx;
                    if (pixels[static_cast<size_t>(neighbor)]
                        && !visited[static_cast<size_t>(neighbor)]) {
                        visited[static_cast<size_t>(neighbor)] = true;
                        pending.push_back(neighbor);
                    }
                }
            }
        }
        components.append(component);
    }
    return components;
}

bool looksLikeEnclosedCompactIcon(const QImage& source, const QRect& box)
{
    if (source.isNull() || !source.rect().contains(box)
        || box.width() < 12 || box.height() < 12
        || box.width() > 28 || box.height() > 28
        || qMin(box.width(), box.height()) * 4
            < qMax(box.width(), box.height()) * 3) {
        return false;
    }
    const ForegroundMask mask = foregroundMask(source, box);
    if (mask.inkPixels == 0) {
        return false;
    }

    const QVector<BinaryComponent> components = connectedComponents(
        mask.pixels, box.width(), box.height());
    if (components.size() < 3) {
        return false;
    }

    const auto outerIt = std::max_element(
        components.cbegin(), components.cend(),
        [](const BinaryComponent& first, const BinaryComponent& second) {
            return first.pixels < second.pixels;
        });
    const BinaryComponent& outer = *outerIt;
    const int outerWidth = outer.right - outer.left + 1;
    const int outerHeight = outer.bottom - outer.top + 1;
    if (outer.pixels * 2 < mask.inkPixels
        || outerWidth * 100 < box.width() * 70
        || outerHeight * 100 < box.height() * 70) {
        return false;
    }

    QVector<const BinaryComponent*> enclosed;
    for (const BinaryComponent& component : components) {
        if (&component == &outer || component.pixels < 2
            || component.left <= outer.left || component.right >= outer.right
            || component.top <= outer.top || component.bottom >= outer.bottom) {
            continue;
        }
        enclosed.append(&component);
    }
    if (enclosed.size() < 3) {
        return false;
    }

    const auto centerX = [](const BinaryComponent& component) {
        return (component.left + component.right) / 2;
    };
    const auto centerY = [](const BinaryComponent& component) {
        return (component.top + component.bottom) / 2;
    };
    for (int firstIndex = 0; firstIndex < enclosed.size(); ++firstIndex) {
        const BinaryComponent& first = *enclosed[firstIndex];
        const int firstWidth = first.right - first.left + 1;
        const int firstHeight = first.bottom - first.top + 1;
        for (int secondIndex = firstIndex + 1;
             secondIndex < enclosed.size(); ++secondIndex) {
            const BinaryComponent& second = *enclosed[secondIndex];
            const int secondWidth = second.right - second.left + 1;
            const int secondHeight = second.bottom - second.top + 1;
            if (qMin(first.pixels, second.pixels) * 2
                    < qMax(first.pixels, second.pixels)
                || qMax(firstWidth, secondWidth) * 3 > outerWidth
                || qMax(firstHeight, secondHeight) * 3 > outerHeight
                || qAbs(centerY(first) - centerY(second))
                    > qMax(2, outerHeight / 6)
                || qAbs(centerX(first) - centerX(second)) * 3 < outerWidth
                || qAbs(centerX(first) + centerX(second)
                        - outer.left - outer.right)
                    > qMax(2, outerWidth / 5)
                || qMax(centerY(first), centerY(second))
                    > outer.top + outerHeight * 2 / 3) {
                continue;
            }

            for (int lowerIndex = 0; lowerIndex < enclosed.size(); ++lowerIndex) {
                if (lowerIndex == firstIndex || lowerIndex == secondIndex) {
                    continue;
                }
                if (centerY(*enclosed[lowerIndex])
                    > qMax(centerY(first), centerY(second))) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool masksMatch(const ForegroundMask& first, const ForegroundMask& second)
{
    if (first.inkPixels == 0 || second.inkPixels == 0
        || first.pixels.size() != second.pixels.size()) {
        return false;
    }
    int intersection = 0;
    for (size_t index = 0; index < first.pixels.size(); ++index) {
        intersection += first.pixels[index] && second.pixels[index];
    }
    const int largerInkCount = qMax(first.inkPixels, second.inkPixels);
    return intersection * 100 >= largerInkCount * 72;
}

bool hasRepeatedCompactLeadingShape(
    int candidateIndex,
    int peerIndex,
    const QVector<OcrTextLine>& lines,
    const QVector<int>& graphemes,
    const QImage& source)
{
    if (source.isNull() || candidateIndex < 0 || peerIndex < 0
        || candidateIndex >= lines.size() || peerIndex >= lines.size()) {
        return false;
    }
    const QRect candidateBox = lines[candidateIndex].box;
    const QRect peerBox = lines[peerIndex].box;
    const ForegroundMask candidateMask = foregroundMask(source, candidateBox);
    if (candidateMask.inkPixels == 0) {
        return false;
    }

    const int relativeLeft = candidateBox.left() - peerBox.left();
    const int relativeTop = candidateBox.top() - peerBox.top();
    const int positionTolerance = qMax(2, candidateBox.height() / 5);
    int matchingRows = 0;
    for (int index = 0; index < lines.size(); ++index) {
        if (index == peerIndex || graphemes[index] < 3) {
            continue;
        }
        const QRect otherPeer = lines[index].box;
        const int heightTolerance = qMax(4, peerBox.height() / 3);
        const int leftTolerance = qMax(4, peerBox.height() / 3);
        if (qAbs(otherPeer.left() - peerBox.left()) > leftTolerance
            || qAbs(otherPeer.height() - peerBox.height()) > heightTolerance
            || qMin(otherPeer.width(), peerBox.width()) * 100
                < qMax(otherPeer.width(), peerBox.width()) * 65
            || qAbs(otherPeer.center().y() - peerBox.center().y())
                < qMax(otherPeer.height(), peerBox.height())) {
            continue;
        }

        const QPoint expectedTopLeft(otherPeer.left() + relativeLeft,
                                     otherPeer.top() + relativeTop);
        bool matched = false;
        for (int dy = -positionTolerance; dy <= positionTolerance && !matched; ++dy) {
            for (int dx = -positionTolerance; dx <= positionTolerance; ++dx) {
                const QRect comparisonBox(expectedTopLeft + QPoint(dx, dy),
                                          candidateBox.size());
                if (masksMatch(candidateMask,
                               foregroundMask(source, comparisonBox))) {
                    matched = true;
                    break;
                }
            }
        }
        if (matched && ++matchingRows >= 2) {
            return true;
        }
    }
    return false;
}

struct LeadingSplitCandidate {
    int textLeft = -1;
    bool valid = false;
};

LeadingSplitCandidate leadingSplitCandidate(const QRect& detectedBox,
                                             const QImage& source,
                                             bool compactLabel = false)
{
    const QRect box = detectedBox.intersected(source.rect());
    if (source.isNull() || box.height() < 12 || box.height() > 64
        || box.width() < qMax(24, box.height() * 2)) {
        return {};
    }

    std::vector<int> lumas;
    lumas.reserve(static_cast<size_t>(box.width() * box.height()));
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            lumas.push_back(lumaOf(source.pixel(x, y)));
        }
    }
    const size_t mid = lumas.size() / 2;
    std::nth_element(lumas.begin(),
                     lumas.begin() + static_cast<qptrdiff>(mid),
                     lumas.end());
    const int backgroundLuma = lumas[mid];

    std::vector<bool> ink(static_cast<size_t>(box.width()), false);
    for (int x = box.left(); x <= box.right(); ++x) {
        for (int y = box.top(); y <= box.bottom(); ++y) {
            if (qAbs(lumaOf(source.pixel(x, y)) - backgroundLuma) >= 48) {
                ink[static_cast<size_t>(x - box.left())] = true;
                break;
            }
        }
    }
    int first = 0;
    while (first < box.width() && !ink[static_cast<size_t>(first)]) {
        ++first;
    }
    int last = box.width() - 1;
    while (last >= first && !ink[static_cast<size_t>(last)]) {
        --last;
    }
    if (first > last) {
        return {};
    }

    // UI icon gutters are visibly wider than ordinary inter-word spacing.
    // Requiring at least 6px keeps repeated paragraph lines beginning with a
    // short word from looking like a menu column.
    const int minimumGap = compactLabel
        ? qMax(4, qRound(box.height() * 0.18))
        : qMax(6, qRound(box.height() * 0.25));
    const int minimumLeadingWidth = qMax(4, qRound(box.height() * 0.35));
    const int maximumLeadingWidth = compactLabel
        ? qMax(10, qRound(box.height() * 0.9))
        : qMax(8, qRound(box.height() * 0.8));
    const int edgeTolerance = qMax(4, qRound(box.height() * 0.5));
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
            && leadingWidth >= minimumLeadingWidth
            && leadingWidth <= maximumLeadingWidth
            && remainingWidth >= qMax(box.height(), leadingWidth * 2)) {
            return { box.left() + x, true };
        }
        if (leadingWidth > maximumLeadingWidth) {
            break;
        }
    }
    return {};
}

QString stripMergedLeadingIcon(const OcrTextLine& line, const QImage& source)
{
    const QString text = line.text.trimmed();
    if (line.leadingIconSeparated || source.isNull() || text.size() < 4
        || line.score >= 0.85f
        || line.box.width() > 80 || line.box.height() > 26
        || !text.front().isLetter() || text.front().script() == QChar::Script_Latin) {
        return {};
    }

    bool latinLabel = true;
    for (int index = 1; index < text.size(); ++index) {
        const QChar ch = text[index];
        if (!ch.isLetter() || ch.script() != QChar::Script_Latin) {
            latinLabel = false;
            break;
        }
    }
    if (!latinLabel) {
        return {};
    }

    const QRect box = line.box.intersected(source.rect());
    if (box.width() < 3 || box.height() < 3) {
        return {};
    }
    std::vector<int> lumas;
    lumas.reserve(static_cast<size_t>(box.width() * box.height()));
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            lumas.push_back(lumaOf(source.pixel(x, y)));
        }
    }
    const size_t mid = lumas.size() / 2;
    std::nth_element(lumas.begin(), lumas.begin() + static_cast<qptrdiff>(mid), lumas.end());
    const int backgroundLuma = lumas[mid];

    std::vector<bool> ink(static_cast<size_t>(box.width()), false);
    for (int x = box.left(); x <= box.right(); ++x) {
        for (int y = box.top(); y <= box.bottom(); ++y) {
            if (qAbs(lumaOf(source.pixel(x, y)) - backgroundLuma) >= 48) {
                ink[static_cast<size_t>(x - box.left())] = true;
                break;
            }
        }
    }
    int first = 0;
    while (first < box.width() && !ink[static_cast<size_t>(first)]) {
        ++first;
    }
    int last = box.width() - 1;
    while (last >= first && !ink[static_cast<size_t>(last)]) {
        --last;
    }
    if (first > last) {
        return {};
    }

    const int minimumGap = qMax(5, qRound(box.height() * 0.3));
    const int maximumIconWidth = qMax(8, qRound(box.height() * 0.75));
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
            && leadingWidth <= maximumIconWidth
            && remainingWidth >= qMax(box.height(), leadingWidth * 2)) {
            return text.sliced(1);
        }
        break;
    }
    return {};
}

} // namespace

QVector<QRect> recoverFragmentedLineBoxes(const QVector<QRect>& boxes,
                                          const QImage& source)
{
    if (source.isNull() || boxes.size() < 2) {
        return boxes;
    }

    QVector<QRect> recovered = boxes;
    QVector<bool> absorbed(boxes.size(), false);
    for (int anchorIndex = 0; anchorIndex < boxes.size(); ++anchorIndex) {
        if (absorbed[anchorIndex]) {
            continue;
        }
        const QRect anchor = boxes[anchorIndex].intersected(source.rect());
        if (anchor.height() < 18 || anchor.height() > 36
            || anchor.width() < anchor.height() * 8) {
            continue;
        }

        QVector<int> fragments;
        int fragmentRight = anchor.right();
        for (int index = 0; index < boxes.size(); ++index) {
            if (index == anchorIndex || absorbed[index]) {
                continue;
            }
            const QRect fragment = boxes[index].intersected(source.rect());
            const int overlapTop = qMax(anchor.top(), fragment.top());
            const int overlapBottom = qMin(anchor.bottom(), fragment.bottom());
            const int verticalOverlap = overlapBottom - overlapTop + 1;
            if (fragment.height() < 7
                || fragment.height() * 100 > anchor.height() * 70
                || verticalOverlap * 100 < fragment.height() * 80
                || fragment.left() < anchor.left() + anchor.width() / 2
                || fragment.left() > anchor.right() + anchor.height()
                || fragment.width() > anchor.width()) {
                continue;
            }
            fragments.append(index);
            fragmentRight = qMax(fragmentRight, fragment.right());
        }
        if (fragments.isEmpty()) {
            continue;
        }

        const QRect probe(anchor.left(),
                          anchor.top(),
                          qMin(source.width() - anchor.left(),
                               fragmentRight - anchor.left() + 1
                                   + anchor.height() * 8),
                          anchor.height());
        std::vector<int> lumas;
        lumas.reserve(static_cast<size_t>(probe.width() * probe.height()));
        for (int y = probe.top(); y <= probe.bottom(); ++y) {
            for (int x = probe.left(); x <= probe.right(); ++x) {
                lumas.push_back(lumaOf(source.pixel(x, y)));
            }
        }
        const size_t mid = lumas.size() / 2;
        std::nth_element(lumas.begin(),
                         lumas.begin() + static_cast<qptrdiff>(mid),
                         lumas.end());
        const int backgroundLuma = lumas[mid];

        std::vector<int> rowInk(static_cast<size_t>(anchor.height()), 0);
        for (int y = anchor.top(); y <= anchor.bottom(); ++y) {
            for (int x = anchor.left(); x <= anchor.right(); ++x) {
                rowInk[static_cast<size_t>(y - anchor.top())] +=
                    qAbs(lumaOf(source.pixel(x, y)) - backgroundLuma) >= 48;
            }
        }
        QVector<int> textRows;
        for (int row = 0; row < anchor.height(); ++row) {
            if (rowInk[static_cast<size_t>(row)] >= 3) {
                textRows.append(anchor.top() + row);
            }
        }
        if (textRows.size() < 5) {
            continue;
        }

        const int stopGap = qMax(12, anchor.height() / 2);
        int rightmostInk = anchor.right();
        int blankColumns = 0;
        for (int x = anchor.left(); x <= probe.right(); ++x) {
            bool columnHasInk = false;
            for (const int y : textRows) {
                if (qAbs(lumaOf(source.pixel(x, y)) - backgroundLuma) >= 48) {
                    columnHasInk = true;
                    break;
                }
            }
            if (columnHasInk) {
                rightmostInk = x;
                blankColumns = 0;
            } else if (x > anchor.right() && ++blankColumns >= stopGap) {
                break;
            }
        }
        if (rightmostInk <= anchor.right() + anchor.height()
            || rightmostInk < fragmentRight - anchor.height()) {
            continue;
        }

        recovered[anchorIndex].setRight(rightmostInk);
        for (int index = 0; index < boxes.size(); ++index) {
            if (index == anchorIndex || absorbed[index]) {
                continue;
            }
            const QRect fragment = boxes[index].intersected(source.rect());
            const int overlapTop = qMax(anchor.top(), fragment.top());
            const int overlapBottom = qMin(anchor.bottom(), fragment.bottom());
            const int verticalOverlap = overlapBottom - overlapTop + 1;
            if (fragment.height() >= 7
                && fragment.height() * 100 <= anchor.height() * 70
                && verticalOverlap * 100 >= fragment.height() * 80
                && fragment.left() >= anchor.left() + anchor.width() / 2
                && fragment.right() <= rightmostInk + stopGap) {
                absorbed[index] = true;
            }
        }
    }

    QVector<QRect> result;
    result.reserve(recovered.size());
    for (int index = 0; index < recovered.size(); ++index) {
        if (!absorbed[index]) {
            result.append(recovered[index]);
        }
    }
    sortBoxesInReadingOrder(result);
    return result;
}

QRect recognitionCropBox(const QRect& textBox, const QImage& source)
{
    const QRect box = textBox.intersected(source.rect());
    if (source.isNull() || box.height() < 18 || box.height() > 36) {
        return box;
    }

    std::vector<int> lumas;
    lumas.reserve(static_cast<size_t>(box.width() * box.height()));
    for (int y = box.top(); y <= box.bottom(); ++y) {
        for (int x = box.left(); x <= box.right(); ++x) {
            lumas.push_back(lumaOf(source.pixel(x, y)));
        }
    }
    const size_t mid = lumas.size() / 2;
    std::nth_element(lumas.begin(),
                     lumas.begin() + static_cast<qptrdiff>(mid),
                     lumas.end());
    const int backgroundLuma = lumas[mid];

    const int minimumRowInk = box.width() <= 32 ? 1 : 3;
    std::vector<bool> inkRows(static_cast<size_t>(box.height()), false);
    for (int y = box.top(); y <= box.bottom(); ++y) {
        int inkPixels = 0;
        for (int x = box.left(); x <= box.right(); ++x) {
            inkPixels += qAbs(lumaOf(source.pixel(x, y)) - backgroundLuma) >= 48;
        }
        inkRows[static_cast<size_t>(y - box.top())] =
            inkPixels >= minimumRowInk;
    }

    int bestStart = -1;
    int bestEnd = -1;
    int runStart = -1;
    int lastInk = -1;
    int internalGap = 0;
    for (int row = 0; row < box.height(); ++row) {
        if (inkRows[static_cast<size_t>(row)]) {
            if (runStart < 0) {
                runStart = row;
            }
            lastInk = row;
            internalGap = 0;
            continue;
        }
        if (runStart >= 0 && ++internalGap > 1) {
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
    if (bestStart < 0) {
        return box;
    }

    const int firstInkRow = box.top() + bestStart;
    const int lastInkRow = box.top() + bestEnd;
    const int inkHeight = bestEnd - bestStart + 1;
    if (inkHeight < 6 || inkHeight * 100 > box.height() * 70) {
        return box;
    }
    const int verticalPadding = qMax(2, inkHeight / 5);
    return QRect(box.left(),
                 qMax(box.top(), firstInkRow - verticalPadding),
                 box.width(),
                 qMin(box.bottom(), lastInkRow + verticalPadding)
                     - qMax(box.top(), firstInkRow - verticalPadding) + 1);
}

QString correctConfusableText(const QString& text)
{
    QString corrected = text;
    static const QStringList aiFollowingWords = {
        QStringLiteral("credit"),
        QStringLiteral("credits"),
        QStringLiteral("model"),
        QStringLiteral("models"),
        QStringLiteral("usage"),
    };
    for (const QString& word : aiFollowingWords) {
        corrected.replace(QStringLiteral("Al %1").arg(word),
                          QStringLiteral("AI %1").arg(word));
        QString titleWord = word;
        titleWord[0] = titleWord[0].toUpper();
        corrected.replace(QStringLiteral("Al %1").arg(titleWord),
                          QStringLiteral("AI %1").arg(titleWord));
    }
    corrected.replace(QStringLiteral("profi le"), QStringLiteral("profile"));
    corrected.replace(QStringLiteral("Profi le"), QStringLiteral("Profile"));
    if (corrected == QStringLiteral("0ct")) {
        corrected = QStringLiteral("Oct");
    }
    return corrected;
}

QVector<RefinedOcrBox> refineRecognitionBoxes(const QVector<QRect>& boxes,
                                              const QImage& source)
{
    QVector<RefinedOcrBox> result;
    result.reserve(boxes.size());
    for (const QRect& box : boxes) {
        result.append({ box, box, false, {} });
    }
    if (source.isNull() || boxes.size() < 3) {
        return result;
    }

    struct Candidate {
        int index = -1;
        int detectedLeft = 0;
        int textLeft = 0;
        int height = 0;
    };
    QVector<Candidate> candidates;
    candidates.reserve(boxes.size());
    for (int index = 0; index < boxes.size(); ++index) {
        const LeadingSplitCandidate split = leadingSplitCandidate(boxes[index], source);
        if (!split.valid) {
            continue;
        }
        candidates.append({ index,
                            boxes[index].left(),
                            split.textLeft,
                            boxes[index].height() });
    }

    QVector<bool> separated(boxes.size(), false);
    for (int seedIndex = 0; seedIndex < candidates.size(); ++seedIndex) {
        const Candidate& seed = candidates[seedIndex];
        QVector<int> cluster;
        int minCenterY = std::numeric_limits<int>::max();
        int maxCenterY = std::numeric_limits<int>::min();
        for (int candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
            const Candidate& candidate = candidates[candidateIndex];
            const int height = qMin(seed.height, candidate.height);
            const int detectedTolerance = qMax(5, qRound(height * 0.35));
            const int textTolerance = qMax(4, qRound(height * 0.25));
            if (qAbs(candidate.detectedLeft - seed.detectedLeft) > detectedTolerance
                || qAbs(candidate.textLeft - seed.textLeft) > textTolerance) {
                continue;
            }
            cluster.append(candidateIndex);
            minCenterY = qMin(minCenterY, boxes[candidate.index].center().y());
            maxCenterY = qMax(maxCenterY, boxes[candidate.index].center().y());
        }
        if (cluster.size() < 3
            || maxCenterY - minCenterY < qMax(24, seed.height * 2)) {
            continue;
        }
        for (const int candidateIndex : cluster) {
            const Candidate& candidate = candidates[candidateIndex];
            if (separated[candidate.index]) {
                continue;
            }
            const QRect detected = boxes[candidate.index];
            const int textLeft = qBound(detected.left(),
                                        candidate.textLeft,
                                        detected.right());
            const QRect textBox(QPoint(textLeft, detected.top()),
                                detected.bottomRight());
            if (textBox.width() < 4) {
                continue;
            }
            RefinedOcrBox& refined = result[candidate.index];
            refined.textBox = textBox;
            refined.leadingIconSeparated = true;
            refined.reason = QStringLiteral("repeated-leading-icon-column");
            separated[candidate.index] = true;
        }
    }
    return result;
}

bool refineIsolatedLeadingIcon(OcrTextLine* line, const QImage& source)
{
    if (!line || line->leadingIconSeparated || source.isNull()
        || !line->text.startsWith(QLatin1Char(' '))) {
        return false;
    }
    const QRect detected = line->detectedBox.isValid()
        ? line->detectedBox
        : line->box;
    if (detected.height() < 12 || detected.height() > 36
        || detected.width() < detected.height()
        || detected.width() > detected.height() * 4) {
        return false;
    }
    const LeadingSplitCandidate split = leadingSplitCandidate(detected, source, true);
    if (!split.valid || split.textLeft <= detected.left()
        || split.textLeft > detected.right() - 3) {
        return false;
    }

    line->detectedBox = detected;
    line->box = QRect(QPoint(split.textLeft, detected.top()),
                      detected.bottomRight());
    line->leadingIconSeparated = true;
    line->refinementReason = QStringLiteral("isolated-leading-icon-whitespace");
    return true;
}

QVector<OcrTextLine> filterTranslatableLines(
    const QVector<OcrTextLine>& lines,
    const QImage& source,
    QVector<RejectedTextLine>* rejected)
{
    if (rejected) {
        rejected->clear();
    }
    QVector<OcrTextLine> processed = lines;
    QVector<bool> keep(lines.size(), true);
    QVector<int> graphemes;
    graphemes.reserve(lines.size());
    for (int index = 0; index < lines.size(); ++index) {
        const OcrTextLine& line = lines[index];
        processed[index].sourceIndex = line.sourceIndex >= 0
            ? line.sourceIndex
            : index;
        const QString stripped = stripMergedLeadingIcon(line, source);
        if (!stripped.isEmpty()) {
            processed[index].text = stripped;
            if (rejected) {
                OcrTextLine icon = line;
                icon.text = line.text.left(1);
                rejected->append({ icon,
                                   QStringLiteral("merged-leading-icon-prefix"),
                                   index,
                                   index });
            }
        }
        graphemes.append(graphemeCount(processed[index].text));
    }

    for (int candidateIndex = 0; candidateIndex < lines.size(); ++candidateIndex) {
        const OcrTextLine& candidate = processed[candidateIndex];
        if (graphemes[candidateIndex] == 0 || graphemes[candidateIndex] > 2
            || candidate.box.width() > 28 || candidate.box.height() > 28
            || candidate.box.width() * candidate.box.height() > 784) {
            continue;
        }
        if (candidate.score < 0.85f
            && looksLikeEnclosedCompactIcon(source, candidate.box)) {
            keep[candidateIndex] = false;
            if (rejected) {
                rejected->append({ candidate,
                                   QStringLiteral("enclosed-compact-icon"),
                                   candidateIndex,
                                   -1 });
            }
            continue;
        }
        if (candidate.box.height() > 18
            || candidate.box.width() * candidate.box.height() > 450) {
            continue;
        }

        int peerIndex = -1;
        int bestGap = std::numeric_limits<int>::max();
        QString rejectionReason;
        for (int index = 0; index < lines.size(); ++index) {
            if (index == candidateIndex || graphemes[index] < 3) {
                continue;
            }
            const OcrTextLine& peer = processed[index];
            if (!isSameRow(candidate.box, peer.box)
                || candidate.box.width() * 10 > peer.box.width() * 7) {
                continue;
            }
            const int gap = horizontalGap(candidate.box, peer.box);
            const int allowedGap = qMax(4, peer.box.height() / 4);
            if (candidate.score < 0.85f
                && gap <= allowedGap && gap < bestGap) {
                bestGap = gap;
                peerIndex = index;
                rejectionReason =
                    QStringLiteral("compact-low-confidence-near-label");
                continue;
            }

            const int repeatedShapeGap = qMax(20, peer.box.height());
            if (candidate.box.right() < peer.box.left()
                && gap <= repeatedShapeGap && gap < bestGap
                && hasRepeatedCompactLeadingShape(candidateIndex,
                                                  index,
                                                  processed,
                                                  graphemes,
                                                  source)) {
                bestGap = gap;
                peerIndex = index;
                rejectionReason =
                    QStringLiteral("repeated-compact-icon-near-label");
            }
        }
        if (peerIndex < 0) {
            continue;
        }

        keep[candidateIndex] = false;
        if (rejected) {
            rejected->append({ candidate,
                               rejectionReason,
                               candidateIndex,
                               peerIndex });
        }
    }

    QVector<OcrTextLine> result;
    result.reserve(lines.size());
    for (int index = 0; index < lines.size(); ++index) {
        if (keep[index]) {
            result.append(processed[index]);
        }
    }
    return result;
}

void sortBoxesInReadingOrder(QVector<QRect>& boxes)
{
    // A tolerance-based "same visual row" comparator is not a strict weak
    // ordering, so group into rows explicitly instead of sorting once.
    std::sort(boxes.begin(), boxes.end(), [](const QRect& a, const QRect& b) {
        if (a.top() != b.top()) {
            return a.top() < b.top();
        }
        return a.left() < b.left();
    });

    QVector<QRect> ordered;
    ordered.reserve(boxes.size());
    int i = 0;
    while (i < boxes.size()) {
        int rowEnd = i + 1;
        const int rowCenter = boxes[i].center().y();
        while (rowEnd < boxes.size()) {
            const QRect& candidate = boxes[rowEnd];
            const int centerDifference = qAbs(candidate.center().y() - rowCenter);
            const int centerTolerance = qRound(
                qMin(boxes[i].height(), candidate.height()) * 0.35);
            if (centerDifference > qMax(2, centerTolerance)) {
                break;
            }
            ++rowEnd;
        }
        std::sort(boxes.begin() + i, boxes.begin() + rowEnd, [](const QRect& a, const QRect& b) {
            if (a.left() != b.left()) {
                return a.left() < b.left();
            }
            return a.top() < b.top();
        });
        for (; i < rowEnd; ++i) {
            ordered.append(boxes[i]);
        }
    }
    boxes = ordered;
}

} // namespace Visnip::Ocr

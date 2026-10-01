// Standalone equivalence + benchmark harness for the box-blur rewrite.
// Emulates the 32bpp QImage surface the real code operates on so the two
// implementations can be compared without a Qt build.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using QRgb = uint32_t;
using uchar = unsigned char;
using qsizetype = long long;

static inline int qAlpha(QRgb c) { return int((c >> 24) & 0xff); }
static inline int qRed(QRgb c) { return int((c >> 16) & 0xff); }
static inline int qGreen(QRgb c) { return int((c >> 8) & 0xff); }
static inline int qBlue(QRgb c) { return int(c & 0xff); }
static inline QRgb qRgba(int r, int g, int b, int a)
{
    return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}
template <typename T> static inline T qBound(T lo, T v, T hi) { return std::max(lo, std::min(v, hi)); }

// Minimal stand-in for QImage: 32bpp, stride padded like Qt (4-byte aligned,
// which for 32bpp is always width*4, but keep it explicit).
struct Image {
    int w = 0, h = 0;
    std::vector<uchar> data;
    Image() = default;
    Image(int width, int height) : w(width), h(height), data(size_t(width) * 4 * height, 0) {}
    bool isNull() const { return w <= 0 || h <= 0; }
    int width() const { return w; }
    int height() const { return h; }
    int depth() const { return 32; }
    qsizetype bytesPerLine() const { return qsizetype(w) * 4; }
    const uchar* constBits() const { return data.data(); }
    uchar* bits() { return data.data(); }
    // QImage::pixel()/setPixel() live in libQt6Gui: not inlinable from app code,
    // and each does a null check, a rect().contains() bounds check and a
    // depth/format dispatch. REALISTIC=1 models that; the default build models
    // the optimistic "as if fully inlined" lower bound.
#if REALISTIC
    __attribute__((noinline)) QRgb pixel(int x, int y) const
    {
        if (isNull()) return 0;
        if (x < 0 || y < 0 || x >= w || y >= h) return 0;
        switch (depth()) {
        case 32: break;
        default: return 0;
        }
        QRgb v;
        std::memcpy(&v, data.data() + bytesPerLine() * y + qsizetype(x) * 4, 4);
        return v;
    }
    __attribute__((noinline)) void setPixel(int x, int y, QRgb v)
    {
        if (isNull()) return;
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        if (detached_ == 0) detached_ = 1;   // stands in for the detach() check
        switch (depth()) {
        case 32: break;
        default: return;
        }
        std::memcpy(data.data() + bytesPerLine() * y + qsizetype(x) * 4, &v, 4);
    }
    mutable int detached_ = 0;
#else
    QRgb pixel(int x, int y) const
    {
        QRgb v;
        std::memcpy(&v, data.data() + bytesPerLine() * y + qsizetype(x) * 4, 4);
        return v;
    }
    void setPixel(int x, int y, QRgb v)
    {
        std::memcpy(data.data() + bytesPerLine() * y + qsizetype(x) * 4, &v, 4);
    }
#endif
};

// ---------------------------------------------------------------------------
// OLD implementation (verbatim from AnnotationModel.cpp before the change)
// ---------------------------------------------------------------------------
namespace old_impl {
void blurHorizontal(const Image& source, Image& target, int radius)
{
    const int w = source.width();
    const int h = source.height();
    const int span = radius * 2 + 1;
    for (int y = 0; y < h; ++y) {
        int64_t a = 0, r = 0, g = 0, b = 0;
        for (int dx = -radius; dx <= radius; ++dx) {
            const QRgb px = source.pixel(qBound(0, dx, w - 1), y);
            a += qAlpha(px); r += qRed(px); g += qGreen(px); b += qBlue(px);
        }
        for (int x = 0; x < w; ++x) {
            target.setPixel(x, y, qRgba(int(r / span), int(g / span), int(b / span), int(a / span)));
            const QRgb removePx = source.pixel(qBound(0, x - radius, w - 1), y);
            const QRgb addPx = source.pixel(qBound(0, x + radius + 1, w - 1), y);
            a += qAlpha(addPx) - qAlpha(removePx);
            r += qRed(addPx) - qRed(removePx);
            g += qGreen(addPx) - qGreen(removePx);
            b += qBlue(addPx) - qBlue(removePx);
        }
    }
}

void blurVertical(const Image& source, Image& target, int radius)
{
    const int w = source.width();
    const int h = source.height();
    const int span = radius * 2 + 1;
    for (int x = 0; x < w; ++x) {
        int64_t a = 0, r = 0, g = 0, b = 0;
        for (int dy = -radius; dy <= radius; ++dy) {
            const QRgb px = source.pixel(x, qBound(0, dy, h - 1));
            a += qAlpha(px); r += qRed(px); g += qGreen(px); b += qBlue(px);
        }
        for (int y = 0; y < h; ++y) {
            target.setPixel(x, y, qRgba(int(r / span), int(g / span), int(b / span), int(a / span)));
            const QRgb removePx = source.pixel(x, qBound(0, y - radius, h - 1));
            const QRgb addPx = source.pixel(x, qBound(0, y + radius + 1, h - 1));
            a += qAlpha(addPx) - qAlpha(removePx);
            r += qRed(addPx) - qRed(removePx);
            g += qGreen(addPx) - qGreen(removePx);
            b += qBlue(addPx) - qBlue(removePx);
        }
    }
}

void boxBlur(Image& image, int radius)
{
    if (image.isNull() || radius <= 0) return;
    Image temp(image.width(), image.height());
    Image out(image.width(), image.height());
    blurHorizontal(image, temp, radius);
    blurVertical(temp, out, radius);
    image = out;
}

void applyGaussianBlur(Image& work, int boxRadius)
{
    for (int i = 0; i < 3; ++i) boxBlur(work, boxRadius);
}
} // namespace old_impl

// ---------------------------------------------------------------------------
// NEW implementation (verbatim from the patched AnnotationModel.cpp)
// ---------------------------------------------------------------------------
namespace new_impl {
void blurHorizontal(const Image& source, Image& target, int radius)
{
    const int w = source.width();
    const int h = source.height();
    if (w <= 0 || h <= 0 || radius <= 0) return;
    const int span = radius * 2 + 1;

    const uchar* const srcBase = source.constBits();
    const qsizetype srcStride = source.bytesPerLine();
    uchar* const dstBase = target.bits();
    const qsizetype dstStride = target.bytesPerLine();

    for (int y = 0; y < h; ++y) {
        const QRgb* const src = reinterpret_cast<const QRgb*>(srcBase + srcStride * y);
        QRgb* const dst = reinterpret_cast<QRgb*>(dstBase + dstStride * y);
        int a = 0, r = 0, g = 0, b = 0;
        for (int dx = -radius; dx <= radius; ++dx) {
            const QRgb px = src[qBound(0, dx, w - 1)];
            a += qAlpha(px); r += qRed(px); g += qGreen(px); b += qBlue(px);
        }
        for (int x = 0; x < w; ++x) {
            dst[x] = qRgba(r / span, g / span, b / span, a / span);
            const QRgb removePx = src[qBound(0, x - radius, w - 1)];
            const QRgb addPx = src[qBound(0, x + radius + 1, w - 1)];
            a += qAlpha(addPx) - qAlpha(removePx);
            r += qRed(addPx) - qRed(removePx);
            g += qGreen(addPx) - qGreen(removePx);
            b += qBlue(addPx) - qBlue(removePx);
        }
    }
}

void blurVertical(const Image& source, Image& target, int radius)
{
    const int w = source.width();
    const int h = source.height();
    if (w <= 0 || h <= 0 || radius <= 0) return;
    const int span = radius * 2 + 1;

    const uchar* const srcBase = source.constBits();
    const qsizetype srcStride = source.bytesPerLine();
    uchar* const dstBase = target.bits();
    const qsizetype dstStride = target.bytesPerLine();

    const auto sourceRow = [&](int y) {
        return reinterpret_cast<const QRgb*>(srcBase + srcStride * qBound(0, y, h - 1));
    };

    std::vector<int> accumulator(size_t(w) * 4, 0);
    int* const acc = accumulator.data();

    for (int dy = -radius; dy <= radius; ++dy) {
        const QRgb* const line = sourceRow(dy);
        for (int x = 0; x < w; ++x) {
            const QRgb px = line[x];
            int* const c = acc + x * 4;
            c[0] += qAlpha(px); c[1] += qRed(px); c[2] += qGreen(px); c[3] += qBlue(px);
        }
    }

    for (int y = 0; y < h; ++y) {
        QRgb* const dst = reinterpret_cast<QRgb*>(dstBase + dstStride * y);
        const QRgb* const removeLine = sourceRow(y - radius);
        const QRgb* const addLine = sourceRow(y + radius + 1);
        for (int x = 0; x < w; ++x) {
            int* const c = acc + x * 4;
            dst[x] = qRgba(c[1] / span, c[2] / span, c[3] / span, c[0] / span);
            const QRgb removePx = removeLine[x];
            const QRgb addPx = addLine[x];
            c[0] += qAlpha(addPx) - qAlpha(removePx);
            c[1] += qRed(addPx) - qRed(removePx);
            c[2] += qGreen(addPx) - qGreen(removePx);
            c[3] += qBlue(addPx) - qBlue(removePx);
        }
    }
}

bool boxBlur(Image& image, Image& scratch, int radius)
{
    if (image.isNull() || radius <= 0) return true;
    if (image.depth() != 32) return false;
    if (scratch.width() != image.width() || scratch.height() != image.height()) {
        scratch = Image(image.width(), image.height());
        if (scratch.isNull()) return false;
    }
    blurHorizontal(image, scratch, radius);
    blurVertical(scratch, image, radius);
    return true;
}

void applyGaussianBlur(Image& work, int boxRadius)
{
    Image scratch;
    for (int i = 0; i < 3; ++i) {
        if (!boxBlur(work, scratch, boxRadius)) return;
    }
}
} // namespace new_impl

// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// applyMosaic: OLD (QColor-per-pixel emulated) vs NEW (scanline + fill_n)
// ---------------------------------------------------------------------------
struct Rect { int x, y, w, h; int left() const {return x;} int top() const {return y;}
    int right() const {return x + w - 1;} int bottom() const {return y + h - 1;} };

namespace old_impl {
void applyMosaic(Image& image, Rect rect, int blockSize)
{
    blockSize = qBound(3, blockSize, 64);
    for (int y = rect.top(); y <= rect.bottom(); y += blockSize) {
        for (int x = rect.left(); x <= rect.right(); x += blockSize) {
            const int bl = x, bt = y;
            const int br = std::min(x + blockSize - 1, rect.right());
            const int bb = std::min(y + blockSize - 1, rect.bottom());
            int64_t r = 0, g = 0, b = 0, count = 0;
            for (int by = bt; by <= bb; ++by) {
                const QRgb* line = reinterpret_cast<const QRgb*>(image.constBits() + image.bytesPerLine() * by);
                for (int bx = bl; bx <= br; ++bx) {
                    // QColor::fromRgb(rgb).red() == qRed(rgb) etc.; alpha dropped
                    r += qRed(line[bx]); g += qGreen(line[bx]); b += qBlue(line[bx]);
                    ++count;
                }
            }
            if (count <= 0) continue;
            const QRgb avg = 0xff000000u | (uint32_t(r / count) << 16) | (uint32_t(g / count) << 8) | uint32_t(b / count);
            for (int by = bt; by <= bb; ++by) {
                QRgb* line = reinterpret_cast<QRgb*>(image.bits() + image.bytesPerLine() * by);
                for (int bx = bl; bx <= br; ++bx) line[bx] = avg;
            }
        }
    }
}
} // namespace old_impl

namespace new_impl {
void applyMosaic(Image& image, Rect rect, int blockSize)
{
    blockSize = qBound(3, blockSize, 64);
    uchar* const base = image.bits();
    const qsizetype stride = image.bytesPerLine();
    for (int y = rect.top(); y <= rect.bottom(); y += blockSize) {
        for (int x = rect.left(); x <= rect.right(); x += blockSize) {
            const int left = x;
            const int width = std::min(x + blockSize - 1, rect.right()) - x + 1;
            const int bt = y, bb = std::min(y + blockSize - 1, rect.bottom());
            int r = 0, g = 0, b = 0, count = 0;
            for (int by = bt; by <= bb; ++by) {
                const QRgb* line = reinterpret_cast<const QRgb*>(base + stride * by) + left;
                for (int i = 0; i < width; ++i) {
                    const QRgb px = line[i];
                    r += qRed(px); g += qGreen(px); b += qBlue(px);
                }
                count += width;
            }
            if (count <= 0) continue;
            const QRgb avg = 0xff000000u | (uint32_t(r / count) << 16) | (uint32_t(g / count) << 8) | uint32_t(b / count);
            for (int by = bt; by <= bb; ++by) {
                QRgb* line = reinterpret_cast<QRgb*>(base + stride * by) + left;
                std::fill_n(line, width, avg);
            }
        }
    }
}
} // namespace new_impl

static Image makeNoise(int w, int h, uint32_t seed)
{
    Image img(w, h);
    std::mt19937 rng(seed);
    auto* p = reinterpret_cast<QRgb*>(img.bits());
    for (size_t i = 0; i < size_t(w) * h; ++i) p[i] = rng();
    return img;
}

static bool compare(const Image& a, const Image& b, const char* label)
{
    if (a.width() != b.width() || a.height() != b.height()) {
        std::printf("  %-28s SIZE MISMATCH\n", label);
        return false;
    }
    const auto* pa = reinterpret_cast<const QRgb*>(a.constBits());
    const auto* pb = reinterpret_cast<const QRgb*>(b.constBits());
    for (size_t i = 0; i < size_t(a.width()) * a.height(); ++i) {
        if (pa[i] != pb[i]) {
            std::printf("  %-28s DIFF at px %zu: old=%08x new=%08x\n", label, i, pa[i], pb[i]);
            return false;
        }
    }
    std::printf("  %-28s identical (%d x %d)\n", label, a.width(), a.height());
    return true;
}

int main()
{
    bool ok = true;

    std::printf("== equivalence ==\n");
    // Cover odd/even sizes, degenerate strips, radius >= dimension, and the
    // radius range the app actually produces (radius 1..24 -> boxRadius 1..12).
    const int sizes[][2] = {{1,1},{1,17},{17,1},{2,3},{7,7},{31,29},{64,48},{129,97},{256,181}};
    const int radii[] = {1, 2, 3, 5, 12, 40};
    for (auto& s : sizes) {
        for (int r : radii) {
            Image base = makeNoise(s[0], s[1], uint32_t(s[0] * 7919 + s[1] * 104729 + r));
            Image a = base, b = base;
            old_impl::applyGaussianBlur(a, r);
            new_impl::applyGaussianBlur(b, r);
            char label[64];
            std::snprintf(label, sizeof(label), "%dx%d r=%d", s[0], s[1], r);
            ok &= compare(a, b, label);
        }
    }

    std::printf("\n== mosaic equivalence ==\n");
    for (int block : {3, 5, 7, 16, 64}) {
        Image a = makeNoise(163, 121, uint32_t(block) * 31u);
        Image b = a;
        Rect roi{7, 5, 150, 110};
        old_impl::applyMosaic(a, roi, block);
        new_impl::applyMosaic(b, roi, block);
        char label[64];
        std::snprintf(label, sizeof(label), "mosaic block=%d", block);
        ok &= compare(a, b, label);
    }

    std::printf("\n== benchmark (3-pass gaussian, boxRadius=12) ==\n");
    for (auto& s : (const int[][2]){{430, 330}, {800, 600}, {1200, 800}}) {
        Image base = makeNoise(s[0], s[1], 42);
        Image a = base, b = base;

        auto t0 = std::chrono::steady_clock::now();
        old_impl::applyGaussianBlur(a, 12);
        auto t1 = std::chrono::steady_clock::now();
        new_impl::applyGaussianBlur(b, 12);
        auto t2 = std::chrono::steady_clock::now();

        const double oldMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double newMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
        std::printf("  %4dx%-4d  old=%8.2f ms   new=%7.2f ms   speedup=%5.1fx   %s\n",
                    s[0], s[1], oldMs, newMs, oldMs / newMs,
                    std::memcmp(a.constBits(), b.constBits(), a.data.size()) == 0 ? "identical" : "DIFF!");
        ok &= (std::memcmp(a.constBits(), b.constBits(), a.data.size()) == 0);
    }

    std::printf("\n%s\n", ok ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
    return ok ? 0 : 1;
}

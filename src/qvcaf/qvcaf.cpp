// SPDX-License-Identifier: MIT
// Multi-scale PatchMatch content-aware fill. Port of open-ps patchmatch-inpaint.ts.
#include "qvcaf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace qvcaf {
namespace {

constexpr int ALPHA_SAMPLE_MIN = 12;

struct Level {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgb;       // n*3
    std::vector<uint8_t> alpha;     // n
    std::vector<uint8_t> hole;      // n, 0/1
    std::vector<uint8_t> sampleable;// n, 0/1
    std::vector<int32_t> holeList;
    int holeCount = 0;
};

struct Quality {
    int radius;
    int pmIters;
    int searchCap;
};

struct ValidField {
    std::vector<uint8_t> valid;
    std::vector<int32_t> list;
    int count = 0;
    int radius = 0;
};

// Ring context for patchDistance weights (single-threaded Phase 1).
const uint16_t *g_ringDist = nullptr;
int g_ringTrusted = 0;

inline uint8_t clampByte(float value)
{
    if (value < 0.f)
        return 0;
    if (value > 255.f)
        return 255;
    return static_cast<uint8_t>(value + 0.5f);
}

inline float clampUnit(float value)
{
    if (value < 0.f)
        return 0.f;
    if (value > 255.f)
        return 255.f;
    return value;
}


struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    float next()
    {
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s) / 4294967296.f;
    }
};

int countHoles(const uint8_t *mask, int n)
{
    int count = 0;
    for (int i = 0; i < n; ++i) {
        if (mask[i])
            ++count;
    }
    return count;
}

Quality qualityFor(int holeCount)
{
    if (holeCount < 8000)
        return {4, 3, 72};
    if (holeCount < 40000)
        return {4, 3, 64};
    if (holeCount < 120000)
        return {3, 2, 40};
    return {2, 2, 28};
}

std::vector<uint16_t> distanceToKnown(const uint8_t *hole, int w, int h)
{
    const int n = w * h;
    std::vector<uint16_t> dist(n);
    for (int i = 0; i < n; ++i)
        dist[i] = hole[i] ? 65535 : 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (dist[i] == 0)
                continue;
            uint16_t best = dist[i];
            if (x > 0 && dist[i - 1] + 1 < best)
                best = dist[i - 1] + 1;
            if (y > 0 && dist[i - w] + 1 < best)
                best = dist[i - w] + 1;
            dist[i] = best;
        }
    }
    for (int y = h - 1; y >= 0; --y) {
        for (int x = w - 1; x >= 0; --x) {
            const int i = y * w + x;
            if (dist[i] == 0)
                continue;
            uint16_t best = dist[i];
            if (x + 1 < w && dist[i + 1] + 1 < best)
                best = dist[i + 1] + 1;
            if (y + 1 < h && dist[i + w] + 1 < best)
                best = dist[i + w] + 1;
            dist[i] = best;
        }
    }
    return dist;
}

Level buildLevel(const uint8_t *rgba, const uint8_t *mask, int w, int h)
{
    Level level;
    level.w = w;
    level.h = h;
    const int n = w * h;
    level.rgb.resize(n * 3);
    level.alpha.resize(n);
    level.hole.resize(n);
    int opaque = 0;
    for (int i = 0, p = 0; i < n; ++i, p += 4) {
        level.rgb[i * 3] = rgba[p];
        level.rgb[i * 3 + 1] = rgba[p + 1];
        level.rgb[i * 3 + 2] = rgba[p + 2];
        level.alpha[i] = rgba[p + 3];
        level.hole[i] = mask[i] ? 1 : 0;
        if (!level.hole[i] && level.alpha[i] > ALPHA_SAMPLE_MIN)
            ++opaque;
    }
    const bool useAlpha = opaque > 32;
    level.sampleable.resize(n);
    int hc = 0;
    for (int i = 0; i < n; ++i) {
        if (level.hole[i])
            ++hc;
    }
    level.holeList.resize(hc);
    level.holeCount = 0;
    for (int i = 0; i < n; ++i) {
        level.sampleable[i] = (!level.hole[i] && (!useAlpha || level.alpha[i] > ALPHA_SAMPLE_MIN)) ? 1 : 0;
        if (level.hole[i])
            level.holeList[level.holeCount++] = i;
    }
    return level;
}

Level downsampleLevel(const Level &level)
{
    const int w = level.w;
    const int h = level.h;
    const int nw = std::max(1, w >> 1);
    const int nh = std::max(1, h >> 1);
    const int n = nw * nh;
    Level out;
    out.w = nw;
    out.h = nh;
    out.rgb.assign(n * 3, 0);
    out.alpha.assign(n, 0);
    out.hole.assign(n, 0);
    out.sampleable.assign(n, 0);
    int hc = 0;
    for (int y = 0; y < nh; ++y) {
        for (int x = 0; x < nw; ++x) {
            int r = 0, g = 0, b = 0, a = 0, samples = 0, holeAny = 0, sampleAny = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const int yy = y * 2 + dy;
                if (yy >= h)
                    continue;
                for (int dx = 0; dx < 2; ++dx) {
                    const int xx = x * 2 + dx;
                    if (xx >= w)
                        continue;
                    const int i = yy * w + xx;
                    const int c = i * 3;
                    r += level.rgb[c];
                    g += level.rgb[c + 1];
                    b += level.rgb[c + 2];
                    a += level.alpha[i];
                    ++samples;
                    if (level.hole[i])
                        holeAny = 1;
                    if (level.sampleable[i])
                        sampleAny = 1;
                }
            }
            const int oi = y * nw + x;
            const float inv = samples > 0 ? 1.f / samples : 0.f;
            out.rgb[oi * 3] = static_cast<uint8_t>(r * inv + 0.5f);
            out.rgb[oi * 3 + 1] = static_cast<uint8_t>(g * inv + 0.5f);
            out.rgb[oi * 3 + 2] = static_cast<uint8_t>(b * inv + 0.5f);
            out.alpha[oi] = static_cast<uint8_t>(a * inv + 0.5f);
            out.hole[oi] = static_cast<uint8_t>(holeAny);
            out.sampleable[oi] = holeAny ? 0 : static_cast<uint8_t>(sampleAny);
            if (holeAny)
                ++hc;
        }
    }
    out.holeList.resize(hc);
    out.holeCount = 0;
    for (int i = 0; i < n; ++i) {
        if (out.hole[i])
            out.holeList[out.holeCount++] = i;
    }
    return out;
}

std::vector<Level> buildPyramid(const Level &finest, int radius)
{
    std::vector<Level> levels;
    levels.push_back(finest);
    while (static_cast<int>(levels.size()) < 6) {
        const Level &current = levels.back();
        const int maxSide = std::max(current.w, current.h);
        if (maxSide <= std::max(40, radius * 6) || current.holeCount < 48)
            break;
        Level next = downsampleLevel(current);
        if (next.w < 8 || next.h < 8 || next.holeCount == 0)
            break;
        levels.push_back(std::move(next));
    }
    return levels;
}

void downsampleRgba(const uint8_t *rgba, const uint8_t *mask, int w, int h,
                    std::vector<uint8_t> &outRgba, std::vector<uint8_t> &outMask, int &nw, int &nh)
{
    nw = std::max(1, w >> 1);
    nh = std::max(1, h >> 1);
    outRgba.assign(nw * nh * 4, 0);
    outMask.assign(nw * nh, 0);
    for (int y = 0; y < nh; ++y) {
        for (int x = 0; x < nw; ++x) {
            int r = 0, g = 0, b = 0, a = 0, samples = 0, holeAny = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const int yy = y * 2 + dy;
                if (yy >= h)
                    continue;
                for (int dx = 0; dx < 2; ++dx) {
                    const int xx = x * 2 + dx;
                    if (xx >= w)
                        continue;
                    const int i = yy * w + xx;
                    const int p = i * 4;
                    r += rgba[p];
                    g += rgba[p + 1];
                    b += rgba[p + 2];
                    a += rgba[p + 3];
                    ++samples;
                    if (mask[i])
                        holeAny = 1;
                }
            }
            const int oi = (y * nw + x) * 4;
            const float inv = samples > 0 ? 1.f / samples : 0.f;
            outRgba[oi] = static_cast<uint8_t>(r * inv + 0.5f);
            outRgba[oi + 1] = static_cast<uint8_t>(g * inv + 0.5f);
            outRgba[oi + 2] = static_cast<uint8_t>(b * inv + 0.5f);
            outRgba[oi + 3] = static_cast<uint8_t>(a * inv + 0.5f);
            outMask[y * nw + x] = static_cast<uint8_t>(holeAny);
        }
    }
}

int fitRadius(int radius, int w, int h)
{
    int r = radius;
    while (r > 1 && (w <= r * 2 + 2 || h <= r * 2 + 2))
        --r;
    return r;
}

ValidField buildValidAt(const Level &level, int r)
{
    ValidField field;
    const int w = level.w;
    const int h = level.h;
    const int stride = w + 1;
    std::vector<uint32_t> integ((w + 1) * (h + 1), 0);
    for (int y = 0; y < h; ++y) {
        uint32_t row = 0;
        const int yOff = y * w;
        const int outRow = (y + 1) * stride;
        const int prevRow = y * stride;
        for (int x = 0; x < w; ++x) {
            row += level.sampleable[yOff + x] ? 0u : 1u;
            integ[outRow + x + 1] = integ[prevRow + x + 1] + row;
        }
    }
    field.valid.assign(w * h, 0);
    field.count = 0;
    for (int y = r; y < h - r; ++y) {
        for (int x = r; x < w - r; ++x) {
            const int x0 = x - r;
            const int y0 = y - r;
            const int x1 = x + r + 1;
            const int y1 = y + r + 1;
            const uint32_t blocked = integ[y1 * stride + x1] - integ[y0 * stride + x1]
                                     - integ[y1 * stride + x0] + integ[y0 * stride + x0];
            if (blocked == 0) {
                field.valid[y * w + x] = 1;
                ++field.count;
            }
        }
    }
    field.list.resize(field.count);
    int cursor = 0;
    for (int i = 0; i < w * h; ++i) {
        if (field.valid[i])
            field.list[cursor++] = i;
    }
    field.radius = r;
    return field;
}

bool buildValid(const Level &level, int radius, ValidField &out)
{
    int r = radius;
    while (r >= 1) {
        out = buildValidAt(level, r);
        if (out.count >= 4)
            return true;
        --r;
    }
    return false;
}

void splitRgb(const uint8_t *rgb, uint8_t *r, uint8_t *g, uint8_t *b, int n)
{
    for (int i = 0, c = 0; i < n; ++i, c += 3) {
        r[i] = rgb[c];
        g[i] = rgb[c + 1];
        b[i] = rgb[c + 2];
    }
}

std::vector<uint8_t> interleaveRgb(const uint8_t *r, const uint8_t *g, const uint8_t *b, int n)
{
    std::vector<uint8_t> rgb(n * 3);
    for (int i = 0, c = 0; i < n; ++i, c += 3) {
        rgb[c] = r[i];
        rgb[c + 1] = g[i];
        rgb[c + 2] = b[i];
    }
    return rgb;
}

int paintUnfilled(int nx, int ny, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca,
                  uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a,
                  const uint8_t *hole, uint8_t *filled, int32_t *qx, int32_t *qy, int qe,
                  int w, int h)
{
    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
        return qe;
    const int ni = ny * w + nx;
    if (!hole[ni] || filled[ni])
        return qe;
    r[ni] = cr;
    g[ni] = cg;
    b[ni] = cb;
    a[ni] = ca;
    filled[ni] = 1;
    qx[qe] = nx;
    qy[qe] = ny;
    return qe + 1;
}

void diffuseFromBoundary(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a,
                         const uint8_t *srcA, const uint8_t *hole, int w, int h)
{
    const int n = w * h;
    std::vector<uint8_t> filled(n, 0);
    std::vector<int32_t> qx(n), qy(n);
    int qe = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (!hole[i]) {
                filled[i] = 1;
                a[i] = srcA[i];
                continue;
            }
            int sr = 0, sg = 0, sb = 0, sa = 0, samples = 0;
            if (x > 0 && !hole[i - 1]) {
                sr += r[i - 1]; sg += g[i - 1]; sb += b[i - 1]; sa += srcA[i - 1]; ++samples;
            }
            if (x + 1 < w && !hole[i + 1]) {
                sr += r[i + 1]; sg += g[i + 1]; sb += b[i + 1]; sa += srcA[i + 1]; ++samples;
            }
            if (y > 0 && !hole[i - w]) {
                sr += r[i - w]; sg += g[i - w]; sb += b[i - w]; sa += srcA[i - w]; ++samples;
            }
            if (y + 1 < h && !hole[i + w]) {
                sr += r[i + w]; sg += g[i + w]; sb += b[i + w]; sa += srcA[i + w]; ++samples;
            }
            if (samples > 0) {
                const float inv = 1.f / samples;
                r[i] = static_cast<uint8_t>(sr * inv + 0.5f);
                g[i] = static_cast<uint8_t>(sg * inv + 0.5f);
                b[i] = static_cast<uint8_t>(sb * inv + 0.5f);
                a[i] = static_cast<uint8_t>(sa * inv + 0.5f);
                filled[i] = 1;
                qx[qe] = x;
                qy[qe] = y;
                ++qe;
            }
        }
    }
    int qs = 0;
    while (qs < qe) {
        const int x = qx[qs];
        const int y = qy[qs];
        ++qs;
        const int i = y * w + x;
        const uint8_t cr = r[i], cg = g[i], cb = b[i], ca = a[i];
        qe = paintUnfilled(x - 1, y, cr, cg, cb, ca, r, g, b, a, hole, filled.data(), qx.data(), qy.data(), qe, w, h);
        qe = paintUnfilled(x + 1, y, cr, cg, cb, ca, r, g, b, a, hole, filled.data(), qx.data(), qy.data(), qe, w, h);
        qe = paintUnfilled(x, y - 1, cr, cg, cb, ca, r, g, b, a, hole, filled.data(), qx.data(), qy.data(), qe, w, h);
        qe = paintUnfilled(x, y + 1, cr, cg, cb, ca, r, g, b, a, hole, filled.data(), qx.data(), qy.data(), qe, w, h);
    }
}

void sampleBilinear(const uint8_t *rgb, const uint8_t *alpha, int w, int h, float x, float y,
                    uint8_t out[4])
{
    x = std::max(0.f, std::min(static_cast<float>(w - 1), x));
    y = std::max(0.f, std::min(static_cast<float>(h - 1), y));
    const int x0 = static_cast<int>(x);
    const int y0 = static_cast<int>(y);
    const int x1 = x0 + 1 < w ? x0 + 1 : x0;
    const int y1 = y0 + 1 < h ? y0 + 1 : y0;
    const float tx = x - x0;
    const float ty = y - y0;
    const int i00 = y0 * w + x0;
    const int i10 = y0 * w + x1;
    const int i01 = y1 * w + x0;
    const int i11 = y1 * w + x1;
    const int c00 = i00 * 3, c10 = i10 * 3, c01 = i01 * 3, c11 = i11 * 3;
    const float w00 = (1 - tx) * (1 - ty);
    const float w10 = tx * (1 - ty);
    const float w01 = (1 - tx) * ty;
    const float w11 = tx * ty;
    out[0] = static_cast<uint8_t>(rgb[c00] * w00 + rgb[c10] * w10 + rgb[c01] * w01 + rgb[c11] * w11 + 0.5f);
    out[1] = static_cast<uint8_t>(rgb[c00 + 1] * w00 + rgb[c10 + 1] * w10 + rgb[c01 + 1] * w01 + rgb[c11 + 1] * w11 + 0.5f);
    out[2] = static_cast<uint8_t>(rgb[c00 + 2] * w00 + rgb[c10 + 2] * w10 + rgb[c01 + 2] * w01 + rgb[c11 + 2] * w11 + 0.5f);
    out[3] = static_cast<uint8_t>(alpha[i00] * w00 + alpha[i10] * w10 + alpha[i01] * w01 + alpha[i11] * w11 + 0.5f);
}

void upsampleColors(const uint8_t *coarseRgb, const uint8_t *coarseA, int cw, int ch,
                    uint8_t *dstR, uint8_t *dstG, uint8_t *dstB, uint8_t *dstA,
                    const uint8_t *fineRgb, const uint8_t *fineA, const uint8_t *hole, int w, int h)
{
    splitRgb(fineRgb, dstR, dstG, dstB, w * h);
    std::memcpy(dstA, fineA, w * h);
    for (int y = 0; y < h; ++y) {
        const float fy = (y + 0.5f) * (static_cast<float>(ch) / h) - 0.5f;
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (!hole[i])
                continue;
            const float fx = (x + 0.5f) * (static_cast<float>(cw) / w) - 0.5f;
            uint8_t sample[4];
            sampleBilinear(coarseRgb, coarseA, cw, ch, fx, fy, sample);
            dstR[i] = sample[0];
            dstG[i] = sample[1];
            dstB[i] = sample[2];
            dstA[i] = sample[3];
        }
    }
}

int snapValid(int sx, int sy, const uint8_t *valid, int w, int h, int r)
{
    if (sx >= r && sy >= r && sx < w - r && sy < h - r && valid[sy * w + sx])
        return sy * w + sx;
    for (int rad = 1; rad <= 12; ++rad) {
        for (int dy = -rad; dy <= rad; ++dy) {
            const int y = sy + dy;
            if (y < r || y >= h - r)
                continue;
            for (int dx = -rad; dx <= rad; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != rad)
                    continue;
                const int x = sx + dx;
                if (x < r || x >= w - r)
                    continue;
                const int i = y * w + x;
                if (valid[i])
                    return i;
            }
        }
    }
    return -1;
}

void seedNnfRandom(int32_t *nnx, int32_t *nny, const int32_t *holeList, int holeCount,
                   const int32_t *validList, int validCount, int w, Rng &rnd)
{
    for (int k = 0; k < holeCount; ++k) {
        const int i = holeList[k];
        const int pick = validList[static_cast<int>(rnd.next() * validCount)];
        nnx[i] = pick % w;
        nny[i] = pick / w;
    }
}

void seedNnfFromCoarse(int32_t *nnx, int32_t *nny,
                       const int32_t *coarseNx, const int32_t *coarseNy, int cw, int ch,
                       const uint8_t *hole, const uint8_t *valid, int w, int h, int r,
                       const int32_t *validList, int validCount, Rng &rnd)
{
    for (int y = 0; y < h; ++y) {
        const int cy = y >> 1;
        const int cyClamped = cy < ch ? cy : ch - 1;
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (!hole[i])
                continue;
            const int cx = x >> 1;
            const int cxClamped = cx < cw ? cx : cw - 1;
            const int ci = cyClamped * cw + cxClamped;
            int sx = coarseNx[ci];
            int sy = coarseNy[ci];
            if (sx >= 0 && sy >= 0) {
                sx = sx * 2 + (x & 1);
                sy = sy * 2 + (y & 1);
                const int snapped = snapValid(sx, sy, valid, w, h, r);
                if (snapped >= 0) {
                    nnx[i] = snapped % w;
                    nny[i] = snapped / w;
                    continue;
                }
            }
            const int pick = validList[static_cast<int>(rnd.next() * validCount)];
            nnx[i] = pick % w;
            nny[i] = pick / w;
        }
    }
}

float patchDistance(const uint8_t *dstR, const uint8_t *dstG, const uint8_t *dstB, const uint8_t *srcRgb,
                    const uint8_t *hole, const uint8_t *sampleable,
                    int w, int h, int x, int y, int sx, int sy, int r, float best)
{
    float sum = 0.f;
    float weightSum = 0.f;
    const int size = r * 2 + 1;
    int ty = y - r;
    int sy0 = sy - r;
    for (int row = 0; row < size; ++row) {
        if (ty < 0 || sy0 < 0 || ty >= h || sy0 >= h) {
            ++ty;
            ++sy0;
            continue;
        }
        int di = ty * w + (x - r);
        int si = (sy0 * w + (sx - r)) * 3;
        const int rowEnd = di + size;
        const int rowStartLimit = ty * w;
        const int rowEndLimit = rowStartLimit + w;
        if (di < rowStartLimit) {
            const int skip = rowStartLimit - di;
            di += skip;
            si += skip * 3;
        }
        const int maxDi = rowEnd < rowEndLimit ? rowEnd : rowEndLimit;
        while (di < maxDi) {
            if (hole[di] || sampleable[di]) {
                const int dr = static_cast<int>(dstR[di]) - static_cast<int>(srcRgb[si]);
                const int dg = static_cast<int>(dstG[di]) - static_cast<int>(srcRgb[si + 1]);
                const int db = static_cast<int>(dstB[di]) - static_cast<int>(srcRgb[si + 2]);
                const float weight = !hole[di] ? 4.f
                    : (g_ringDist && g_ringDist[di] < static_cast<uint16_t>(g_ringTrusted) ? 3.f : 1.f);
                sum += (dr * dr + dg * dg + db * db) * weight;
                weightSum += weight;
                if (weightSum > 0.f && sum >= best * weightSum)
                    return sum / weightSum;
            }
            ++di;
            si += 3;
        }
        ++ty;
        ++sy0;
    }
    return weightSum > 0.f ? sum / weightSum : 1e12f;
}

void rescore(float *score, const int32_t *nnx, const int32_t *nny,
             const uint8_t *dstR, const uint8_t *dstG, const uint8_t *dstB, const uint8_t *srcRgb,
             const uint8_t *hole, const uint8_t *sampleable,
             const int32_t *holeList, int holeCount,
             int w, int h, int r, const uint8_t *valid)
{
    for (int k = 0; k < holeCount; ++k) {
        const int i = holeList[k];
        const int sx = nnx[i];
        const int sy = nny[i];
        const int y = i / w;
        const int x = i - y * w;
        if (sx < r || sy < r || sx >= w - r || sy >= h - r || !valid[sy * w + sx]) {
            score[i] = 1e12f;
            continue;
        }
        score[i] = patchDistance(dstR, dstG, dstB, srcRgb, hole, sampleable, w, h, x, y, sx, sy, r, 1e12f);
    }
}

void tryMatch(int x, int y, int sx, int sy, int i,
              int32_t *nnx, int32_t *nny, float *score,
              const uint8_t *dstR, const uint8_t *dstG, const uint8_t *dstB, const uint8_t *srcRgb,
              const uint8_t *hole, const uint8_t *sampleable, const uint8_t *valid,
              int w, int h, int r)
{
    if (sx < r || sy < r || sx >= w - r || sy >= h - r)
        return;
    if (!valid[sy * w + sx])
        return;
    if (nnx[i] == sx && nny[i] == sy)
        return;
    const float best = score[i];
    const float dist = patchDistance(dstR, dstG, dstB, srcRgb, hole, sampleable, w, h, x, y, sx, sy, r, best);
    if (dist < best) {
        score[i] = dist;
        nnx[i] = sx;
        nny[i] = sy;
    }
}

void propagateAndSearch(bool forward,
                        int32_t *nnx, int32_t *nny, float *score,
                        const uint8_t *dstR, const uint8_t *dstG, const uint8_t *dstB, const uint8_t *srcRgb,
                        const uint8_t *hole, const uint8_t *sampleable,
                        const int32_t *holeList, int holeCount,
                        const uint8_t *valid, const int32_t *validList, int validCount,
                        int w, int h, int r, int searchRadius, bool allowGlobal, Rng &rnd)
{
    const int xStep = forward ? 1 : -1;
    const int yStep = forward ? 1 : -1;
    const int start = forward ? 0 : holeCount - 1;
    const int end = forward ? holeCount : -1;
    for (int k = start; k != end; k += yStep) {
        const int i = holeList[k];
        const int y = i / w;
        const int x = i - y * w;
        const int px = x - xStep;
        if (px >= 0 && px < w) {
            const int ni = y * w + px;
            if (hole[ni] && nnx[ni] >= 0)
                tryMatch(x, y, nnx[ni] + xStep, nny[ni], i, nnx, nny, score, dstR, dstG, dstB, srcRgb, hole, sampleable, valid, w, h, r);
        }
        const int py = y - yStep;
        if (py >= 0 && py < h) {
            const int ni = py * w + x;
            if (hole[ni] && nnx[ni] >= 0)
                tryMatch(x, y, nnx[ni], nny[ni] + yStep, i, nnx, nny, score, dstR, dstG, dstB, srcRgb, hole, sampleable, valid, w, h, r);
        }
        int rad = searchRadius;
        int guard = 0;
        while (rad >= 1 && guard < 7) {
            const int ox = static_cast<int>(rnd.next() * (rad * 2 + 1)) - rad;
            const int oy = static_cast<int>(rnd.next() * (rad * 2 + 1)) - rad;
            tryMatch(x, y, nnx[i] + ox, nny[i] + oy, i, nnx, nny, score, dstR, dstG, dstB, srcRgb, hole, sampleable, valid, w, h, r);
            rad >>= 1;
            ++guard;
        }
        if (allowGlobal && validCount > 0) {
            const int pick = validList[static_cast<int>(rnd.next() * validCount)];
            tryMatch(x, y, pick % w, pick / w, i, nnx, nny, score, dstR, dstG, dstB, srcRgb, hole, sampleable, valid, w, h, r);
        }
    }
}

void vote(uint8_t *dstR, uint8_t *dstG, uint8_t *dstB, uint8_t *dstA,
          const uint8_t *srcRgb, const uint8_t *srcA, const uint8_t *hole,
          const int32_t *nnx, const int32_t *nny, const float *score,
          const int32_t *holeList, int holeCount,
          int w, int h, int r)
{
    const int n = w * h;
    std::vector<float> accR(n, 0), accG(n, 0), accB(n, 0), accA(n, 0), accW(n, 0);
    for (int k = 0; k < holeCount; ++k) {
        const int i = holeList[k];
        const int sx = nnx[i];
        const int sy = nny[i];
        if (sx < 0 || sy < 0)
            continue;
        const int y = i / w;
        const int x = i - y * w;
        const float match = 1.f / (1.f + score[i] / 180.f);
        for (int oy = -r; oy <= r; ++oy) {
            const int ty = y + oy;
            const int qy = sy + oy;
            if (ty < 0 || qy < 0 || ty >= h || qy >= h)
                continue;
            for (int ox = -r; ox <= r; ++ox) {
                const int tx = x + ox;
                const int qx = sx + ox;
                if (tx < 0 || qx < 0 || tx >= w || qx >= w)
                    continue;
                const int ti = ty * w + tx;
                if (!hole[ti])
                    continue;
                const float spatial = 1.f / (1.f + ox * ox + oy * oy);
                const float weight = match * spatial;
                const int qi = qy * w + qx;
                const int qc = qi * 3;
                accR[ti] += srcRgb[qc] * weight;
                accG[ti] += srcRgb[qc + 1] * weight;
                accB[ti] += srcRgb[qc + 2] * weight;
                accA[ti] += srcA[qi] * weight;
                accW[ti] += weight;
            }
        }
    }
    for (int k = 0; k < holeCount; ++k) {
        const int i = holeList[k];
        const float weight = accW[i];
        if (weight <= 1e-6f)
            continue;
        const float inv = 1.f / weight;
        dstR[i] = clampByte(accR[i] * inv);
        dstG[i] = clampByte(accG[i] * inv);
        dstB[i] = clampByte(accB[i] * inv);
        dstA[i] = clampByte(accA[i] * inv);
    }
}

void featherBoundary(uint8_t *dstR, uint8_t *dstG, uint8_t *dstB, uint8_t *dstA,
                     const uint8_t *srcRgb, const uint8_t *srcA, const uint8_t *hole, int w, int h)
{
    const int n = w * h;
    std::vector<uint16_t> dist(n);
    for (int i = 0; i < n; ++i)
        dist[i] = hole[i] ? 65535 : 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (dist[i] == 0)
                continue;
            uint16_t m = dist[i];
            if (x > 0 && dist[i - 1] + 1 < m)
                m = dist[i - 1] + 1;
            if (y > 0 && dist[i - w] + 1 < m)
                m = dist[i - w] + 1;
            dist[i] = m;
        }
    }
    for (int y = h - 1; y >= 0; --y) {
        for (int x = w - 1; x >= 0; --x) {
            const int i = y * w + x;
            if (dist[i] == 0)
                continue;
            uint16_t m = dist[i];
            if (x + 1 < w && dist[i + 1] + 1 < m)
                m = dist[i + 1] + 1;
            if (y + 1 < h && dist[i + w] + 1 < m)
                m = dist[i + w] + 1;
            dist[i] = m > 8 ? 8 : m;
        }
    }
    for (int band = 1; band <= 2; ++band) {
        const float blend = band == 1 ? 0.42f : 0.18f;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int i = y * w + x;
                if (dist[i] != band)
                    continue;
                int samples = 0, sr = 0, sg = 0, sb = 0, sa = 0;
                for (int oy = -1; oy <= 1; ++oy) {
                    const int ny = y + oy;
                    if (ny < 0 || ny >= h)
                        continue;
                    for (int ox = -1; ox <= 1; ++ox) {
                        const int nx = x + ox;
                        if (nx < 0 || nx >= w || (ox == 0 && oy == 0))
                            continue;
                        const int ni = ny * w + nx;
                        if (dist[ni] >= band)
                            continue;
                        const int c = ni * 3;
                        const int pr = dist[ni] == 0 ? srcRgb[c] : dstR[ni];
                        const int pg = dist[ni] == 0 ? srcRgb[c + 1] : dstG[ni];
                        const int pb = dist[ni] == 0 ? srcRgb[c + 2] : dstB[ni];
                        const int pa = dist[ni] == 0 ? srcA[ni] : dstA[ni];
                        sr += pr; sg += pg; sb += pb; sa += pa;
                        ++samples;
                    }
                }
                if (samples == 0)
                    continue;
                const float inv = 1.f / samples;
                dstR[i] = clampByte(dstR[i] * (1.f - blend) + sr * inv * blend);
                dstG[i] = clampByte(dstG[i] * (1.f - blend) + sg * inv * blend);
                dstB[i] = clampByte(dstB[i] * (1.f - blend) + sb * inv * blend);
                dstA[i] = clampByte(dstA[i] * (1.f - blend) + sa * inv * blend);
            }
        }
    }
}

void harmonizePlanes(uint8_t *dstR, uint8_t *dstG, uint8_t *dstB, uint8_t *dstA,
                     const uint8_t *srcRgb, const uint8_t *srcA, const uint8_t *hole,
                     const int32_t *holeList, int holeCount, int w, int h, bool feather)
{
    const int n = w * h;
    for (int i = 0; i < n; ++i) {
        if (hole[i])
            continue;
        const int c = i * 3;
        dstR[i] = srcRgb[c];
        dstG[i] = srcRgb[c + 1];
        dstB[i] = srcRgb[c + 2];
        dstA[i] = srcA[i];
    }
    std::vector<float> gR(n), gG(n), gB(n), gA(n), r(n), g(n), b(n), a(n);
    for (int i = 0; i < n; ++i) {
        gR[i] = dstR[i]; gG[i] = dstG[i]; gB[i] = dstB[i]; gA[i] = dstA[i];
        r[i] = dstR[i]; g[i] = dstG[i]; b[i] = dstB[i]; a[i] = dstA[i];
    }
    int minX = w, maxX = 0, minY = h, maxY = 0;
    for (int k = 0; k < holeCount; ++k) {
        const int i = holeList[k];
        const int y = i / w;
        const int x = i - y * w;
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    const int diameter = std::max(std::max(maxX - minX, maxY - minY), 1);
    const int iters = std::min(56, std::max(14, static_cast<int>(std::round(diameter * 0.45))));

    for (int iter = 0; iter < iters; ++iter) {
        const bool forward = (iter & 1) == 0;
        const int start = forward ? 0 : holeCount - 1;
        const int end = forward ? holeCount : -1;
        const int step = forward ? 1 : -1;
        for (int k = start; k != end; k += step) {
            const int i = holeList[k];
            const int y = i / w;
            const int x = i - y * w;
            int nn = 0;
            float sr = 0, sg = 0, sb = 0, sa = 0;
            float gr = 0, gg = 0, gb = 0, ga = 0;
            if (x > 0) {
                const int j = i - 1;
                sr += r[j]; sg += g[j]; sb += b[j]; sa += a[j];
                gr += gR[j]; gg += gG[j]; gb += gB[j]; ga += gA[j]; ++nn;
            }
            if (x + 1 < w) {
                const int j = i + 1;
                sr += r[j]; sg += g[j]; sb += b[j]; sa += a[j];
                gr += gR[j]; gg += gG[j]; gb += gB[j]; ga += gA[j]; ++nn;
            }
            if (y > 0) {
                const int j = i - w;
                sr += r[j]; sg += g[j]; sb += b[j]; sa += a[j];
                gr += gR[j]; gg += gG[j]; gb += gB[j]; ga += gA[j]; ++nn;
            }
            if (y + 1 < h) {
                const int j = i + w;
                sr += r[j]; sg += g[j]; sb += b[j]; sa += a[j];
                gr += gR[j]; gg += gG[j]; gb += gB[j]; ga += gA[j]; ++nn;
            }
            if (nn == 0)
                continue;
            const float inv = 1.f / nn;
            r[i] = clampUnit(sr * inv + gR[i] - gr * inv);
            g[i] = clampUnit(sg * inv + gG[i] - gg * inv);
            b[i] = clampUnit(sb * inv + gB[i] - gb * inv);
            a[i] = clampUnit(sa * inv + gA[i] - ga * inv);
        }
    }
    for (int k = 0; k < holeCount; ++k) {
        const int i = holeList[k];
        dstR[i] = static_cast<uint8_t>(r[i] + 0.5f);
        dstG[i] = static_cast<uint8_t>(g[i] + 0.5f);
        dstB[i] = static_cast<uint8_t>(b[i] + 0.5f);
        dstA[i] = static_cast<uint8_t>(a[i] + 0.5f);
    }
    if (feather)
        featherBoundary(dstR, dstG, dstB, dstA, srcRgb, srcA, hole, w, h);
}

void packRgba(const uint8_t *original, const uint8_t *r, const uint8_t *g, const uint8_t *b,
              const uint8_t *a, const uint8_t *hole, int w, int h, std::vector<uint8_t> &out)
{
    const int n = w * h;
    out.resize(n * 4);
    for (int i = 0, p = 0; i < n; ++i, p += 4) {
        if (!hole[i]) {
            out[p] = original[p];
            out[p + 1] = original[p + 1];
            out[p + 2] = original[p + 2];
            out[p + 3] = original[p + 3];
        } else {
            out[p] = r[i];
            out[p + 1] = g[i];
            out[p + 2] = b[i];
            out[p + 3] = a[i];
        }
    }
}

void upsampleHoles(const uint8_t *coarse, int cw, int ch,
                   const uint8_t *fine, const uint8_t *fineMask, int fw, int fh,
                   std::vector<uint8_t> &out)
{
    out.assign(fine, fine + fw * fh * 4);
    std::vector<uint8_t> coarseRgb(cw * ch * 3), coarseA(cw * ch);
    for (int i = 0, p = 0; i < cw * ch; ++i, p += 4) {
        coarseRgb[i * 3] = coarse[p];
        coarseRgb[i * 3 + 1] = coarse[p + 1];
        coarseRgb[i * 3 + 2] = coarse[p + 2];
        coarseA[i] = coarse[p + 3];
    }
    for (int y = 0; y < fh; ++y) {
        const float fy = (y + 0.5f) * (static_cast<float>(ch) / fh) - 0.5f;
        for (int x = 0; x < fw; ++x) {
            const int i = y * fw + x;
            if (!fineMask[i])
                continue;
            const float fx = (x + 0.5f) * (static_cast<float>(cw) / fw) - 0.5f;
            uint8_t sample[4];
            sampleBilinear(coarseRgb.data(), coarseA.data(), cw, ch, fx, fy, sample);
            const int p = i * 4;
            out[p] = sample[0];
            out[p + 1] = sample[1];
            out[p + 2] = sample[2];
            out[p + 3] = sample[3];
        }
    }
}

void harmonizeImage(const uint8_t *original, const uint8_t *filled, const uint8_t *mask,
                    int w, int h, std::vector<uint8_t> &out)
{
    Level level = buildLevel(filled, mask, w, h);
    Level src = buildLevel(original, mask, w, h);
    std::vector<uint8_t> dstR(w * h), dstG(w * h), dstB(w * h), dstA = level.alpha;
    splitRgb(level.rgb.data(), dstR.data(), dstG.data(), dstB.data(), w * h);
    harmonizePlanes(dstR.data(), dstG.data(), dstB.data(), dstA.data(),
                    src.rgb.data(), src.alpha.data(), level.hole.data(),
                    level.holeList.data(), level.holeCount, w, h, true);
    packRgba(original, dstR.data(), dstG.data(), dstB.data(), dstA.data(), level.hole.data(), w, h, out);
}

FillError fillFull(const uint8_t *rgba, const uint8_t *mask, int width, int height, int holeCount,
                   const FillOptions &options, std::vector<uint8_t> &outRgba)
{
    g_ringDist = nullptr;
    g_ringTrusted = 0;
    const Quality quality = qualityFor(holeCount);
    Level finest = buildLevel(rgba, mask, width, height);
    if (finest.holeCount == 0)
        return FillError::Empty;
    std::vector<Level> levels = buildPyramid(finest, quality.radius);
    Rng rnd(static_cast<uint32_t>((width * 374761393) ^ (height * 668265263) ^ holeCount));

    std::vector<uint8_t> coarseDstR, coarseDstA;
    std::vector<int32_t> coarseNx, coarseNy;
    int coarseW = 0, coarseH = 0;
    bool haveCoarse = false;

    const int levelCount = static_cast<int>(levels.size());
    for (int li = levelCount - 1; li >= 0; --li) {
        Level &level = levels[li];
        const bool isFinest = li == 0;
        const bool isCoarsest = li == levelCount - 1;
        ValidField field;
        if (!buildValid(level, fitRadius(quality.radius, level.w, level.h), field) || field.count < 4) {
            haveCoarse = false;
            if (isFinest)
                return FillError::NoContext;
            continue;
        }
        const int r = field.radius;
        const int w = level.w;
        const int h = level.h;
        const int n = w * h;
        std::vector<uint8_t> dstR(n), dstG(n), dstB(n), dstA(n);
        splitRgb(level.rgb.data(), dstR.data(), dstG.data(), dstB.data(), n);

        const bool alignedCoarse = haveCoarse
            && std::abs(coarseW - (w >> 1)) <= 1
            && std::abs(coarseH - (h >> 1)) <= 1;

        if (!alignedCoarse) {
            diffuseFromBoundary(dstR.data(), dstG.data(), dstB.data(), dstA.data(),
                                level.alpha.data(), level.hole.data(), w, h);
        } else {
            upsampleColors(coarseDstR.data(), coarseDstA.data(), coarseW, coarseH,
                           dstR.data(), dstG.data(), dstB.data(), dstA.data(),
                           level.rgb.data(), level.alpha.data(), level.hole.data(), w, h);
        }

        std::vector<int32_t> nnx(n, -1), nny(n, -1);
        if (alignedCoarse) {
            seedNnfFromCoarse(nnx.data(), nny.data(), coarseNx.data(), coarseNy.data(), coarseW, coarseH,
                              level.hole.data(), field.valid.data(), w, h, r,
                              field.list.data(), field.count, rnd);
        } else {
            seedNnfRandom(nnx.data(), nny.data(), level.holeList.data(), level.holeCount,
                          field.list.data(), field.count, w, rnd);
        }

        std::vector<float> score(n, 0.f);
        const int pmIters = isFinest ? quality.pmIters : std::min(3, quality.pmIters);
        const int searchRadius = isCoarsest
            ? std::max(w, h)
            : std::min(std::max(w, h), isFinest ? quality.searchCap : quality.searchCap * 2);
        std::vector<uint16_t> holeDist = distanceToKnown(level.hole.data(), w, h);
        int maxDist = 1;
        for (int k = 0; k < level.holeCount; ++k) {
            const int distance = holeDist[level.holeList[k]];
            if (distance > maxDist && distance < 60000)
                maxDist = distance;
        }
        const int ringStep = std::max(r, 3);
        const int ringCount = (maxDist + ringStep - 1) / ringStep;
        for (int ring = 0; ring < ringCount; ++ring) {
            const int trustedBelow = ring * ringStep + 1;
            const int endDist = trustedBelow + ringStep;
            std::vector<int32_t> active(level.holeCount);
            int activeCount = 0;
            for (int k = 0; k < level.holeCount; ++k) {
                const int index = level.holeList[k];
                const int distance = holeDist[index];
                if (distance >= trustedBelow && distance < endDist)
                    active[activeCount++] = index;
            }
            if (activeCount == 0)
                continue;
            g_ringDist = holeDist.data();
            g_ringTrusted = trustedBelow;
            for (int iter = 0; iter < pmIters; ++iter) {
                rescore(score.data(), nnx.data(), nny.data(),
                        dstR.data(), dstG.data(), dstB.data(), level.rgb.data(),
                        level.hole.data(), level.sampleable.data(),
                        active.data(), activeCount, w, h, r, field.valid.data());
                propagateAndSearch(iter % 2 == 0,
                                   nnx.data(), nny.data(), score.data(),
                                   dstR.data(), dstG.data(), dstB.data(), level.rgb.data(),
                                   level.hole.data(), level.sampleable.data(),
                                   active.data(), activeCount,
                                   field.valid.data(), field.list.data(), field.count,
                                   w, h, r, searchRadius, isCoarsest, rnd);
            }
            vote(dstR.data(), dstG.data(), dstB.data(), dstA.data(),
                 level.rgb.data(), level.alpha.data(), level.hole.data(),
                 nnx.data(), nny.data(), score.data(),
                 active.data(), activeCount, w, h, r);
            if ((ring & 3) == 0 || ring == ringCount - 1) {
                const float doneLevels = (levelCount - 1 - li) + (ring + 1) / static_cast<float>(ringCount);
                if (options.onProgress)
                    options.onProgress(std::min(99, static_cast<int>(std::round((doneLevels / levelCount) * 92))));
            }
        }
        g_ringDist = nullptr;
        g_ringTrusted = 0;

        harmonizePlanes(dstR.data(), dstG.data(), dstB.data(), dstA.data(),
                        level.rgb.data(), level.alpha.data(), level.hole.data(),
                        level.holeList.data(), level.holeCount, w, h, isFinest);

        coarseDstR = interleaveRgb(dstR.data(), dstG.data(), dstB.data(), n);
        coarseDstA = dstA;
        coarseNx = nnx;
        coarseNy = nny;
        coarseW = w;
        coarseH = h;
        haveCoarse = true;

        if (isFinest) {
            if (options.onProgress)
                options.onProgress(100);
            // Use original rgba for known pixels — need original size buffer.
            // finest may be a copy; pack against input rgba.
            packRgba(rgba, dstR.data(), dstG.data(), dstB.data(), dstA.data(),
                     level.hole.data(), width, height, outRgba);
            return FillError::Ok;
        }
    }
    return FillError::NoContext;
}

FillError fillReduced(const uint8_t *rgba, const uint8_t *mask, int width, int height, int holeCount,
                      const FillOptions &options, std::vector<uint8_t> &outRgba)
{
    std::vector<uint8_t> curRgba(rgba, rgba + width * height * 4);
    std::vector<uint8_t> curMask(mask, mask + width * height);
    int cw = width, ch = height;
    int curHoles = holeCount;

    while ((curHoles > kHugeHole || cw * ch > kHugePixels) && cw > 48 && ch > 48) {
        std::vector<uint8_t> downRgba, downMask;
        int nw = 0, nh = 0;
        downsampleRgba(curRgba.data(), curMask.data(), cw, ch, downRgba, downMask, nw, nh);
        curRgba.swap(downRgba);
        curMask.swap(downMask);
        cw = nw;
        ch = nh;
        curHoles = countHoles(curMask.data(), cw * ch);
    }

    FillOptions nested = options;
    if (options.onProgress) {
        nested.onProgress = [&](int p) { options.onProgress(static_cast<int>(p * 0.75)); };
    }
    std::vector<uint8_t> filledSmall;
    const FillError err = fillFull(curRgba.data(), curMask.data(), cw, ch, curHoles, nested, filledSmall);
    if (err != FillError::Ok)
        return err;
    std::vector<uint8_t> up;
    upsampleHoles(filledSmall.data(), cw, ch, rgba, mask, width, height, up);
    if (options.onProgress)
        options.onProgress(90);
    harmonizeImage(rgba, up.data(), mask, width, height, outRgba);
    if (options.onProgress)
        options.onProgress(100);
    return FillError::Ok;
}

} // namespace

const char *errorString(FillError err)
{
    switch (err) {
    case FillError::Ok: return "ok";
    case FillError::Empty: return "empty";
    case FillError::NoContext: return "no-context";
    case FillError::Failed: return "failed";
    }
    return "failed";
}

FillError fill(const uint8_t *rgba, const uint8_t *mask, int width, int height,
               std::vector<uint8_t> &outRgba, const FillOptions &options)
{
    outRgba.clear();
    if (width <= 0 || height <= 0 || !rgba || !mask)
        return FillError::Failed;
    const int n = width * height;
    const int holeCount = countHoles(mask, n);
    if (holeCount == 0)
        return FillError::Empty;
    if (holeCount > kHugeHole || width * height > kHugePixels)
        return fillReduced(rgba, mask, width, height, holeCount, options, outRgba);
    return fillFull(rgba, mask, width, height, holeCount, options, outRgba);
}

} // namespace qvcaf

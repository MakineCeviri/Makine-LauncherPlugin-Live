// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "text_mask.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace live {

namespace {

constexpr int kCell = 4;          // cells of 4×4 half-resolution pixels (8×8 on screen)
constexpr int kCellMin = 3;       // a cell is "text" with at least this many marked pixels
constexpr int kOutlineRadius = 2;

int percentile(const std::array<int, 256>& hist, int total, double p)
{
    const int want = static_cast<int>(total * p);
    int seen = 0;
    for (int v = 0; v < 256; ++v) {
        seen += hist[v];
        if (seen > want) return v;
    }
    return 255;
}

} // namespace

TextMask TextMask::of(const uint8_t* bgra, int width, int height)
{
    TextMask m;
    const int w = width / 2, h = height / 2;
    if (!bgra || w < kCell || h < kCell) return m;
    // Half-resolution luma (a 2×2 average also softens JPEG / H.264 block noise)
    std::vector<uint8_t> y(static_cast<size_t>(w) * h);
    std::array<int, 256> hist{};
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) {
            int sum = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const uint8_t* p = bgra + (static_cast<size_t>(2 * r + dy) * width + 2 * c + dx) * 4;
                    sum += p[2] * 299 + p[1] * 587 + p[0] * 114;
                }
            const int v = sum / 4000;
            y[static_cast<size_t>(r) * w + c] = static_cast<uint8_t>(v);
            ++hist[v];
        }
    // Relative to this frame: the fill is in the upper half of its range, the outline darker by a share of it
    const int lo = percentile(hist, w * h, 0.01), hi = percentile(hist, w * h, 0.99);
    const int range = std::max(1, hi - lo);
    const int fillMin = lo + range / 2;
    const int contrast = std::max(40, range * 35 / 100);

    // Darkest pixel within the outline radius (separable min filter)
    std::vector<uint8_t> tmp(y.size()), dark(y.size());
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) {
            uint8_t v = 255;
            for (int k = std::max(0, c - kOutlineRadius); k <= std::min(w - 1, c + kOutlineRadius); ++k)
                v = std::min(v, y[static_cast<size_t>(r) * w + k]);
            tmp[static_cast<size_t>(r) * w + c] = v;
        }
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) {
            uint8_t v = 255;
            for (int k = std::max(0, r - kOutlineRadius); k <= std::min(h - 1, r + kOutlineRadius); ++k)
                v = std::min(v, tmp[static_cast<size_t>(k) * w + c]);
            dark[static_cast<size_t>(r) * w + c] = v;
        }

    m.m_w = w / kCell;
    m.m_h = h / kCell;
    m.m_bits.assign(static_cast<size_t>(m.m_w) * m.m_h, 0);
    std::vector<int> counts(m.m_bits.size(), 0);
    for (int r = 0; r < m.m_h * kCell; ++r)
        for (int c = 0; c < m.m_w * kCell; ++c) {
            const size_t i = static_cast<size_t>(r) * w + c;
            if (y[i] >= fillMin && y[i] - dark[i] >= contrast)
                ++counts[static_cast<size_t>(r / kCell) * m.m_w + c / kCell];
        }
    for (size_t i = 0; i < counts.size(); ++i)
        if (counts[i] >= kCellMin) {
            m.m_bits[i] = 1;
            ++m.m_count;
        }
    return m;
}

double TextMask::difference(const TextMask& o) const
{
    if (m_w != o.m_w || m_h != o.m_h) return 1.0;   // another size: a resized window, read it
    const int most = std::max(m_count, o.m_count);
    if (most == 0) return 0.0;
    int diff = 0;
    for (size_t i = 0; i < m_bits.size(); ++i) diff += m_bits[i] != o.m_bits[i];
    return static_cast<double>(diff) / most;
}

bool TextMask::sameText(const TextMask& o) const
{
    return !empty() && !o.empty() && difference(o) <= kSameText;
}

} // namespace live

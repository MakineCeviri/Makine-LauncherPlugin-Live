// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "finder_ocr.h"

#include <algorithm>
#include <cmath>

namespace live::finder {

namespace {
constexpr int kReadHead = 12;   // line heights read from the start of a line
}

std::vector<Observed> observe(OcrOrt& ocr, const uint8_t* bgra, int width, int height, std::string& error,
                              int recognize, const std::function<bool(const std::string&)>& own)
{
    if (!bgra || width <= 0 || height <= 0) return {};
    int sw = 0, sh = 0;
    const std::vector<uint8_t> small = downscale(bgra, width, height, kMaxWidth, sw, sh);
    const std::vector<OcrLine> lines = ocr.detect(small.data(), sw, sh, error);
    std::vector<Observed> obs;
    std::vector<std::pair<double, size_t>> order;
    for (const OcrLine& l : lines) {
        Observed o;
        o.box = {double(l.x0) / sw, double(l.y0) / sh, double(l.x1) / sw, double(l.y1) / sh};
        o.fill = fillScore(small.data(), sw, sh, l.x0, l.y0, l.x1, l.y1);
        order.push_back({likeness(o), obs.size()});
        obs.push_back(std::move(o));
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<bool> drop(obs.size(), false);
    for (int k = 0; k < recognize && k < static_cast<int>(order.size()) && order[static_cast<size_t>(k)].first >= 0.1; ++k) {
        Observed& o = obs[order[static_cast<size_t>(k)].second];
        // the line at full resolution, the recognizer alone (the detector already found it)
        const double padY = o.box.h() * 0.1, padX = 0.005;
        const int x0 = std::max(0, static_cast<int>(std::floor((o.box.x0 - padX) * width)));
        int x1 = std::min(width, static_cast<int>(std::ceil((o.box.x1 + padX) * width)));
        const int y0 = std::max(0, static_cast<int>(std::floor((o.box.y0 - padY) * height)));
        const int y1 = std::min(height, static_cast<int>(std::ceil((o.box.y1 + padY) * height)));
        if (x1 - x0 < 8 || y1 - y0 < 8) continue;
        // its head is enough (a speaker label, and which line it is); the recognizer's cost grows with the width
        x1 = std::min(x1, x0 + kReadHead * (y1 - y0));
        o.text = ocr.read(bgra, width, height, OcrLine{{}, x0, y0, x1, y1}, error);
        if (own && !o.text.empty() && own(o.text)) drop[order[static_cast<size_t>(k)].second] = true;
    }
    std::vector<Observed> kept;
    for (size_t i = 0; i < obs.size(); ++i)
        if (!drop[i]) kept.push_back(std::move(obs[i]));
    return kept;
}

} // namespace live::finder

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "subtitle_finder.h"

#include "garbage.h"
#include "text_mask.h"
#include "unitext.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace live::finder {

namespace {

constexpr size_t kMaxClusters = 12;
constexpr double kFoundScore = 0.45, kLockScore = 0.60;

struct Block {
    double top = 1, bottom = 0, x0 = 1, x1 = 0, lineH = 0, like = 0;
    int lines = 0;
    bool speaker = false;
    std::string sig;
    double cx() const { return (x0 + x1) / 2; }
    double cy() const { return (top + bottom) / 2; }
};

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

// Letters / words of a recognised line: subtitles are words, HUD text is often numbers ("$1,250", "100 %")
double textFactor(const std::string& s)
{
    if (s.empty()) return 0.8;
    const std::u32string u = text::fromUtf8(s);
    int letters = 0, other = 0, spaces = 0;
    for (char32_t c : u) {
        if (c == U' ') ++spaces;
        else if (text::strIsAlpha(std::u32string_view(&c, 1))) ++letters;
        else ++other;
    }
    const double ratio = letters + other ? double(letters) / (letters + other) : 0;
    if (ratio < 0.6) return 0.3;
    return spaces >= 1 ? 1.0 : 0.7;
}

std::string signature(const Block& b)
{
    if (!b.sig.empty()) return b.sig;
    return "#" + std::to_string(static_cast<int>(std::lround((b.x1 - b.x0) * 40))) + "x" + std::to_string(b.lines);
}

} // namespace

const char* stateName(State s)
{
    switch (s) {
    case State::Off: return "off";
    case State::Searching: return "searching";
    case State::Found: return "found";
    case State::Locked: return "locked";
    case State::Manual: return "manual";
    }
    return "off";
}

double likeness(const Observed& l)
{
    const Rect& b = l.box;
    if (b.w() <= 0 || b.h() <= 0) return 0;
    const double cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
    const double centre = std::max(0.05, 1 - std::abs(cx - 0.5) / 0.15);
    const double vert = cy >= 0.60 ? 1.0 : cy <= 0.22 ? 0.6 : 0.15;   // the lower third, or a top band
    const double width = 0.4 + 0.6 * clamp01(b.w() / 0.25);
    const double height = b.h() >= 0.022 && b.h() <= 0.09 ? 1.0 : 0.35;   // a subtitle's size, not a HUD label or a title
    const double fill = 0.5 + 0.5 * clamp01(l.fill / 0.25);
    return centre * vert * width * height * fill * textFactor(l.text);
}

void SubtitleFinder::reset()
{
    *this = SubtitleFinder();
}

void SubtitleFinder::restore(const Rect& band, double confidence)
{
    reset();
    m_band = band;
    m_confidence = confidence;
    m_state = State::Locked;
}

void SubtitleFinder::setManual(bool manual)
{
    if (manual) m_state = State::Manual;
    else if (m_state == State::Manual) reset();
}

bool SubtitleFinder::due(double now) const
{
    return (m_state == State::Searching || m_state == State::Found) && now - m_lastRun >= kIntervalS - 0.05;
}

std::optional<Rect> SubtitleFinder::band() const
{
    if (m_state == State::Found || m_state == State::Locked) return m_band;
    return std::nullopt;
}

bool SubtitleFinder::accept()
{
    if (m_state != State::Found || !m_band) return false;
    m_state = State::Locked;
    return true;
}

void SubtitleFinder::addSample(const std::vector<Observed>& lines, double now)
{
    if (m_state != State::Searching && m_state != State::Found) return;
    m_lastRun = now;
    if (m_firstAt < 0) m_firstAt = now;
    ++m_samples;

    // lines → blocks (the lines of one subtitle: stacked, centred over each other)
    std::vector<std::pair<double, const Observed*>> scored;
    for (const Observed& l : lines)
        if (const double v = likeness(l); v >= 0.03) scored.push_back({v, &l});
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.second->box.y0 < b.second->box.y0; });
    std::vector<Block> blocks;
    for (const auto& [like, l] : scored) {
        const Rect& r = l->box;
        const double cx = (r.x0 + r.x1) / 2;
        if (!blocks.empty()) {
            Block& b = blocks.back();
            const double lh = b.lineH / b.lines;
            if (r.y0 - b.bottom < 0.9 * lh && std::abs(cx - b.cx()) < 0.12 && r.h() < 1.8 * lh && r.h() > 0.55 * lh) {
                b.bottom = std::max(b.bottom, r.y1);
                b.x0 = std::min(b.x0, r.x0);
                b.x1 = std::max(b.x1, r.x1);
                b.lineH += r.h();
                ++b.lines;
                b.like = std::max(b.like, like);
                if (!l->text.empty()) b.sig += text::toUtf8(text::compactPoolKey(text::fromUtf8(l->text)));
                continue;
            }
        }
        Block b;
        b.top = r.y0;
        b.bottom = r.y1;
        b.x0 = r.x0;
        b.x1 = r.x1;
        b.lineH = r.h();
        b.lines = 1;
        b.like = like;
        b.speaker = !l->text.empty() && !text::splitSpeaker(l->text).speaker.empty();
        if (!l->text.empty()) b.sig = text::toUtf8(text::compactPoolKey(text::fromUtf8(l->text)));
        blocks.push_back(std::move(b));
    }
    for (Block& b : blocks) b.lineH /= b.lines;

    // blocks → candidate bands
    std::vector<bool> used(blocks.size(), false);
    for (Cluster& c : m_clusters) {
        int best = -1;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const Block& b = blocks[i];
            if (used[i] || b.cy() < c.top - c.lineH || b.cy() > c.bottom + c.lineH || std::abs(b.cx() - c.cx) > 0.2) continue;
            if (best < 0 || b.like > blocks[static_cast<size_t>(best)].like) best = static_cast<int>(i);
        }
        if (best < 0) {
            ++c.absent;
            if (c.wasPresent) ++c.transitions;
            c.wasPresent = false;
            continue;
        }
        used[static_cast<size_t>(best)] = true;
        const Block& b = blocks[static_cast<size_t>(best)];
        constexpr double a = 0.3;
        c.top += a * (b.top - c.top);
        c.bottom += a * (b.bottom - c.bottom);
        c.lineH += a * (b.lineH - c.lineH);
        c.cx += a * (b.cx() - c.cx);
        c.halfW = std::max(c.halfW, std::max(std::abs(b.x0 - 0.5), std::abs(b.x1 - 0.5)));
        if (!c.wasPresent) ++c.transitions;
        c.wasPresent = true;
        ++c.present;
        c.likeSum += b.like;
        if (b.speaker) ++c.speakers;
        c.anchors.push_back(b.cy() >= 0.5 ? b.bottom : b.top);   // a lower band grows up, a top band down
        if (c.anchors.size() > 16) c.anchors.erase(c.anchors.begin());
        const std::string sig = signature(b);
        if (sig != c.lastSig && std::find(c.signatures.begin(), c.signatures.end(), sig) == c.signatures.end()) {
            c.signatures.push_back(sig);
            if (c.signatures.size() > 8) c.signatures.erase(c.signatures.begin());
        }
        c.lastSig = sig;
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        if (used[i] || b.like < 0.15) continue;
        Cluster c;
        c.top = b.top;
        c.bottom = b.bottom;
        c.lineH = b.lineH;
        c.cx = b.cx();
        c.halfW = std::max(std::abs(b.x0 - 0.5), std::abs(b.x1 - 0.5));
        c.present = 1;
        c.wasPresent = true;
        c.likeSum = b.like;
        c.speakers = b.speaker ? 1 : 0;
        c.anchors.push_back(b.cy() >= 0.5 ? b.bottom : b.top);
        c.lastSig = signature(b);
        c.signatures.push_back(c.lastSig);
        c.firstAt = now;
        m_clusters.push_back(std::move(c));
    }
    if (m_clusters.size() > kMaxClusters) {
        std::sort(m_clusters.begin(), m_clusters.end(),
                  [&](const Cluster& x, const Cluster& y) { return score(x, now) > score(y, now); });
        m_clusters.resize(kMaxClusters);
    }

    const Cluster* best = nullptr;
    double bestScore = 0;
    for (const Cluster& c : m_clusters)
        if (const double s = score(c, now); s > bestScore) {
            bestScore = s;
            best = &c;
        }
    m_confidence = bestScore;
    if (!best || bestScore < kFoundScore || best->present < 3 || now - m_firstAt < kFoundS - 0.05) return;
    m_band = bandOf(*best);
    m_state = bestScore >= kLockScore && now - m_firstAt >= kLockS - 0.05 ? State::Locked : State::Found;
}

double SubtitleFinder::score(const Cluster& c, double) const
{
    if (c.present < 2) return 0;
    const double like = c.likeSum / c.present;
    // dynamics: its text changes, it comes and goes with the dialogue
    const double change = clamp01((static_cast<double>(c.signatures.size()) - 1) / 2);
    const double blink = clamp01((c.transitions - 1) / 2.0);   // the first appearance is no evidence
    const double dyn = std::max(change, 0.8 * blink);
    // a stable baseline
    double mean = 0, var = 0;
    for (double v : c.anchors) mean += v;
    mean /= static_cast<double>(c.anchors.size());
    for (double v : c.anchors) var += (v - mean) * (v - mean);
    const double sd = std::sqrt(var / static_cast<double>(c.anchors.size()));
    const double stable = clamp01(1 - sd / std::max(0.005, c.lineH));
    const double speaker = c.speakers ? 1.15 : 1.0;
    return clamp01(like * (0.3 + 0.7 * dyn) * (0.5 + 0.5 * stable) * speaker * 1.25);
}

Rect SubtitleFinder::bandOf(const Cluster& c) const
{
    const double lh = std::max(0.015, c.lineH);
    double top = c.top - 0.5 * lh, bottom = c.bottom + 0.5 * lh;
    const double need = 2.9 * lh;   // room for a two-line subtitle
    if (bottom - top < need) {
        if ((c.top + c.bottom) / 2 >= 0.5) top = bottom - need;
        else bottom = top + need;
    }
    const double half = std::max(c.halfW + 0.04, 0.30);
    Rect r{std::max(0.01, 0.5 - half), std::max(0.0, top), std::min(0.99, 0.5 + half), std::min(1.0, bottom)};
    return r;
}

std::vector<uint8_t> downscale(const uint8_t* bgra, int width, int height, int maxWidth, int& outW, int& outH)
{
    if (width <= maxWidth) {
        outW = width;
        outH = height;
        return std::vector<uint8_t>(bgra, bgra + static_cast<size_t>(width) * height * 4);
    }
    outW = maxWidth;
    outH = std::max(1, static_cast<int>(std::lround(double(height) * maxWidth / width)));
    std::vector<uint8_t> out(static_cast<size_t>(outW) * outH * 4);
    for (int y = 0; y < outH; ++y) {
        const int sy0 = static_cast<int>(static_cast<int64_t>(y) * height / outH);
        const int sy1 = std::max(sy0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * height / outH));
        for (int x = 0; x < outW; ++x) {
            const int sx0 = static_cast<int>(static_cast<int64_t>(x) * width / outW);
            const int sx1 = std::max(sx0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * width / outW));
            uint32_t s[4] = {0, 0, 0, 0};
            for (int yy = sy0; yy < sy1; ++yy) {
                const uint8_t* p = bgra + (static_cast<size_t>(yy) * width + sx0) * 4;
                for (int xx = sx0; xx < sx1; ++xx, p += 4)
                    for (int k = 0; k < 4; ++k) s[k] += p[k];
            }
            const uint32_t n = static_cast<uint32_t>((sy1 - sy0) * (sx1 - sx0));
            uint8_t* o = &out[(static_cast<size_t>(y) * outW + x) * 4];
            for (int k = 0; k < 4; ++k) o[k] = static_cast<uint8_t>(s[k] / n);
        }
    }
    return out;
}

double fillScore(const uint8_t* bgra, int width, int height, int x0, int y0, int x1, int y1)
{
    const int pad = std::max(2, (y1 - y0) / 5);
    x0 = std::max(0, x0 - pad);
    y0 = std::max(0, y0 - pad);
    x1 = std::min(width, x1 + pad);
    y1 = std::min(height, y1 + pad);
    const int w = x1 - x0, h = y1 - y0;
    if (w < 16 || h < 16) return 0;
    std::vector<uint8_t> crop(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        std::copy_n(bgra + (static_cast<size_t>(y0 + y) * width + x0) * 4, static_cast<size_t>(w) * 4,
                    &crop[static_cast<size_t>(y) * w * 4]);
    const TextMask m = TextMask::of(crop.data(), w, h);
    const int cells = (w / 2 / 4) * (h / 2 / 4);   // TextMask: 4×4 cells of half resolution
    return cells > 0 ? double(m.cells()) / cells : 0;
}

bool TempGrow::update(const std::vector<std::array<int, 4>>& boxes, int w, int h, double now)
{
    if (w <= 0 || h <= 0) return false;
    // the base region inside the grown one (px)
    const double bx0 = w * m_grow[0] / (1 + m_grow[0] + m_grow[1]), bx1 = w * (1 + m_grow[0]) / (1 + m_grow[0] + m_grow[1]);
    const double by0 = h * m_grow[2] / (1 + m_grow[2] + m_grow[3]), by1 = h * (1 + m_grow[2]) / (1 + m_grow[2] + m_grow[3]);
    const int edgeX = std::max(2, w / 100), edgeY = 1;
    std::array<bool, 4> touch{};
    for (const auto& b : boxes) {
        const int x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3];
        if (x0 < bx0) m_seen[0] = now;
        if (x1 > bx1) m_seen[1] = now;
        if (y0 < by0) m_seen[2] = now;
        if (y1 > by1) m_seen[3] = now;
        touch[0] = touch[0] || x0 <= edgeX;
        touch[1] = touch[1] || x1 >= w - edgeX;
        touch[2] = touch[2] || y0 <= edgeY;
        touch[3] = touch[3] || y1 >= h - edgeY;
    }
    bool grew = false;
    for (int s = 0; s < 4; ++s) {
        if (touch[static_cast<size_t>(s)] && m_grow[static_cast<size_t>(s)] < kMax - 1e-9) {
            m_grow[static_cast<size_t>(s)] = std::min(kMax, m_grow[static_cast<size_t>(s)] + kStep);
            m_seen[static_cast<size_t>(s)] = now;
            grew = true;
        } else if (m_grow[static_cast<size_t>(s)] > 0 && now - m_seen[static_cast<size_t>(s)] > kHoldS) {
            m_grow[static_cast<size_t>(s)] = 0;   // no text out there for a while: back to the band
        }
    }
    return grew;
}

} // namespace live::finder

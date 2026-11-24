// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Long translations on a screen that cannot hold them (Turkish runs ~20–30 % longer than the English
// it replaces). The renderer owns the first three steps, since only it has font metrics: (a) wrap up to
// maxLines, (b) shrink the font to a floor, (c) widen the band up to 90 % of the frame. The plugin owns
// the last one: (d) a line the renderer could not fit even then is split into chunks at sentence /
// clause / word boundaries and shown one after another, each for its reading time, while the source
// line stays on screen. The budget is counted in code points against a conservative glyph width, so a
// chunk the plugin sends always fits once the renderer has done (a)–(c).

#include <cstddef>
#include <string>
#include <vector>

namespace live::fit {

constexpr double kMinFontScale = 0.80;   // (b) the smallest font, as a share of style.sizePx
constexpr double kMaxWidthFrac = 0.90;   // (c) the widest band, as a share of the frame
constexpr double kCharsPerSecond = 17;   // Turkish reading speed for a chunk's time on screen
constexpr double kGlyphEm = 0.55;        // average advance of a mixed-case Latin glyph (Inter), conservative

struct Budget {
    int charsPerRow = 0;   // code points one row holds at the full font in the widest band
    int maxLines = 2;      // rows on screen (style.maxLines)
};

/// Budget of a band `frameWidthPx` wide (the frame the renderer may widen to 90 % of; logical px, as style.sizePx).
Budget budget(double frameWidthPx, double sizePx, int maxLines);
/// Budget of Makine TV: relSize = font height / screen height, a 16:9 screen, the TV's maxLines.
Budget tvBudget(double relSize, int maxLines);
/// The tighter of two budgets (the overlay and the TV both show the chunk).
Budget tighter(const Budget& a, const Budget& b);

/// Rows `text` takes ("\n" separates speaker lines, each starting on its own row).
int rows(const std::u32string& text, const Budget& b);
/// One chunk when it fits; else chunks that each fit, cut at the best boundary near an even split.
/// A speaker label ("Jason: ") is never cut and is repeated in front of each of its line's chunks.
std::vector<std::u32string> split(const std::u32string& text, const Budget& b);
/// A chunk's time on screen: its reading time at kCharsPerSecond, never under minShow.
double chunkSeconds(const std::u32string& chunk, double minShow);

/// Which chunk of the shown line is on screen (capture thread).
class ChunkPlayer {
public:
    void start(int id, std::vector<std::u32string> chunks, double minShow, double now);
    void clear();
    bool active() const { return !m_chunks.empty(); }
    int id() const { return m_id; }
    int count() const { return static_cast<int>(m_chunks.size()); }
    /// The chunk due at `now`; the last one stays.
    int at(double now) const;
    const std::u32string& chunk(int i) const { return m_chunks[static_cast<size_t>(i)]; }
    double total() const;   // all chunks' time on screen together

private:
    int m_id = 0;
    double m_start = 0;
    std::vector<std::u32string> m_chunks;
    std::vector<double> m_seconds;
};

} // namespace live::fit

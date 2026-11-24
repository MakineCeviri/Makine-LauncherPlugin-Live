// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Finding the subtitle band without the player drawing a region. Every kIntervalS the capture thread
// runs the text detector over the whole game window (or the console's video area) at ≤ kMaxWidth px
// and hands the lines here. Each line gets a subtitle-likeness (centred, low or high on the screen, a
// subtitle's size, a light fill with a dark outline, words rather than numbers, a "Name:" label);
// lines of one subtitle are grouped into a block, blocks at the same height into a candidate band, and
// the band accumulates evidence over time: it comes and goes with the dialogue, its text changes, its
// baseline stays put. HUD text (a minimap label, money, a mission title) is present all the time or
// never changes, so it never collects that evidence. searching → found (the band is read) → locked
// (the player accepted it, or the evidence stayed high for kLockS): a locked band is stored per game
// and the detector stops. Pure logic: time and pixels come from the caller.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace live::finder {

constexpr double kIntervalS = 2.0;   // one detector run per this many seconds, at most
constexpr int kMaxWidth = 640;       // the frame the detector sees is at most this wide (brief: ≤ 960; 640 halves the cost)
constexpr double kFoundS = 6.0;      // evidence over at least this long before a band is proposed
constexpr double kLockS = 16.0;      // … and this long (with high confidence) before it locks by itself

struct Rect {   // fractions of the searched area (0..1)
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    double w() const { return x1 - x0; }
    double h() const { return y1 - y0; }
};

struct Observed {   // one detected text line
    Rect box;
    double fill = 0;    // 0..1: share of its cells with a light fill next to a dark outline (TextMask)
    std::string text;   // "" when it was not recognised
};

enum class State { Off, Searching, Found, Locked, Manual };
const char* stateName(State s);

/// Subtitle-likeness of one line on its own (0..1).
double likeness(const Observed& line);

class SubtitleFinder {
public:
    void reset();                                        // searching from scratch
    void restore(const Rect& band, double confidence);   // a band stored for this game: locked
    void setManual(bool manual);                         // the player's own region overrides the finder
    /// One detector run: the lines it found (empty = no text anywhere).
    void addSample(const std::vector<Observed>& lines, double now);
    bool accept();   // found → locked; false when there is nothing to accept

    State state() const { return m_state; }
    /// The band to read (found / locked), fractions of the searched area.
    std::optional<Rect> band() const;
    double confidence() const { return m_confidence; }
    /// Should the detector run now (searching / found, kIntervalS since the last run)?
    bool due(double now) const;
    int samples() const { return m_samples; }

private:
    struct Cluster {
        double top = 0, bottom = 0, lineH = 0;   // running means of the block's extent and line height
        double cx = 0.5, halfW = 0;              // centre and the widest half width seen
        int present = 0, absent = 0, transitions = 0, speakers = 0, words = 0;
        bool wasPresent = false;
        double likeSum = 0;
        std::vector<double> anchors;             // the block's baseline per sample (stability)
        std::vector<std::string> signatures;     // distinct texts read
        double firstAt = 0;
    };
    double score(const Cluster& c, double now) const;
    Rect bandOf(const Cluster& c) const;

    State m_state = State::Searching;
    std::vector<Cluster> m_clusters;
    std::optional<Rect> m_band;
    double m_confidence = 0, m_firstAt = -1, m_lastRun = -1e9;
    int m_samples = 0;
};

/// Box-filter downscale of a top-down BGRA frame to at most maxWidth px wide (a copy when it is narrower).
std::vector<uint8_t> downscale(const uint8_t* bgra, int width, int height, int maxWidth, int& outW, int& outH);
/// The fill score of a line box (px) of a BGRA frame: TextMask cells over the box and a little around it.
double fillScore(const uint8_t* bgra, int width, int height, int x0, int y0, int x1, int y1);

/// Wide or long lines (temporary growth, beside the cut-line growth): a line running into an edge of the
/// region grows it that way by kStep (at most kMax per side, a share of the region's size); a side
/// with no text in its grown margin for kHoldS shrinks back.
class TempGrow {
public:
    static constexpr double kStep = 0.15, kMax = 0.30, kHoldS = 4.0;
    /// Lines in region px (w × h the region read). → true when a side grew (read again before using the text).
    bool update(const std::vector<std::array<int, 4>>& boxes, int w, int h, double now);
    void clear() { m_grow = {}; m_seen = {}; }
    const std::array<double, 4>& grow() const { return m_grow; }   // left, right, top, bottom

private:
    std::array<double, 4> m_grow{};
    std::array<double, 4> m_seen{};   // when text was last seen in each grown margin
};

} // namespace live::finder

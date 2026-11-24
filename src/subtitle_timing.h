// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Port of makine_live/core/subtitle_timing.py: which OCR reading is a new subtitle line,
// when it is settled enough to translate, and how long each translation stays on screen.
// Pure logic; time comes from the caller in seconds (monotonic). Tests: tests/cpp/test_subtitle.cpp
// (the cases of tests/test_subtitle_scheduler.py).

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace live {

double readingMin(const std::u32string& text, double base = 1.2, double perChar = 0.045, double cap = 7.0);

struct LineEvent {
    enum Kind { New, Gone, Dispatch } kind;
    int id;
    double t = 0;           // New: first seen; Gone: last seen
    std::u32string text;    // Dispatch: the text to translate
};

class LineTracker {
public:
    static constexpr double kSameLine = 0.80;
    static constexpr double kStableSim = 0.97;
    static constexpr int kStablePolls = 2;
    static constexpr double kMaxWait = 0.6;
    static constexpr double kGrowQuiet = 0.25;
    static constexpr double kMaxGrowWait = 3.0;
    static constexpr int kMissPolls = 2;
    static constexpr double kMissTime = 0.25;
    static constexpr double kRedispatchSim = 0.90;

    /// text: the reading that counts this round ("" = no valid line on screen).
    std::vector<LineEvent> update(const std::u32string& text, double now);
    /// Tracking paused (source hidden): the current line counts as gone.
    std::vector<LineEvent> reset();

    static bool sameLine(const std::u32string& a, const std::u32string& b);

private:
    struct Line {
        int id = 0;
        std::u32string text, key;
        double firstSeen = 0, lastSeen = 0;
        std::u32string prevKey;
        int stableN = 1;
        std::optional<std::u32string> dispatchedKey, dispatchedText;
        std::u32string bestText;
        double grewAt = -1.0;
        size_t maxLen = 0;
        bool partial = false;
    };
    static bool grows(const std::u32string& longest, size_t maxLen, const std::u32string& key);

    int m_nextId = 1;
    std::optional<Line> m_cur;
    int m_misses = 0;
};

struct Display {
    int id;
    std::u32string text;
    std::string kind;
};

class SubtitleScheduler {
public:
    double minBase = 1.2, perChar = 0.045, minCap = 7.0;
    double backlogLag = 2.0, waitOlder = 1.6, skipAfter = 3.0, dropAfter = 4.0, switchFloor = 0.8, forgetAfter = 10.0;

    void sourceNew(int id, double t);
    void sourceGone(int id, double lastSeen);
    void translationReady(int id, const std::u32string& text, const std::string& kind, double t);
    void translationFailed(int id);
    /// A line shown in chunks (fit.h) stays at least this long once its source is gone, unless a newer line waits.
    void holdFor(int id, double seconds);
    void clear();

    std::optional<Display> tick(double now);
    size_t size() const { return m_entries.size(); }

private:
    struct Entry {
        int id = 0;
        double firstSeen = 0;
        std::optional<double> lastSeen;   // empty: the source is still on screen
        std::optional<std::u32string> text;
        std::string kind = "api";
        std::optional<double> readyAt, shownAt;
        double hold = 0;
        bool visible() const { return !lastSeen; }
        double srcDuration() const { return lastSeen ? std::max(0.0, *lastSeen - firstSeen) : 0.0; }
        bool hasText() const { return text && !text->empty(); }
    };
    double endTime(const Entry& e) const;
    std::vector<const Entry*> waiting() const;
    Entry* pickNext(double now);
    void forgetStale(double now);

    std::map<int, Entry> m_entries;
    std::optional<int> m_current;
};

} // namespace live

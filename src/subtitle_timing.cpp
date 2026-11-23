// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "subtitle_timing.h"

#include "unitext.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace live {

namespace {

// Whitespace-free comparison key: OCR losing a space does not make a new line.
std::u32string compactKey(const std::u32string& text)
{
    std::u32string out;
    for (const char32_t c : text)
        if (!text::isSpace(c)) out += c;
    return text::lower(out);
}

double similarity(const std::u32string& a, const std::u32string& b)
{
    if (a == b) return 1.0;
    if (a.empty() || b.empty()) return 0.0;
    return text::sequenceRatio(a, b);
}

std::u32string dropLast2(const std::u32string& s)   // s[:-2]
{
    return s.size() > 2 ? s.substr(0, s.size() - 2) : std::u32string();
}

} // namespace

double readingMin(const std::u32string& text, double base, double perChar, double cap)
{
    return std::min(cap, base + perChar * static_cast<double>(text.size()));
}

// ── LineTracker ─────────────────────────────────────────────────────────────

bool LineTracker::grows(const std::u32string& longest, size_t maxLen, const std::u32string& key)
{
    // Typewriter effect: longer than any reading so far and a continuation of it (bar its
    // last two letters). A dot that comes and goes ("that" / "that.") is not growth.
    if (key.size() <= maxLen) return false;
    const std::u32string head = longest.size() > 4 ? dropLast2(longest) : longest;
    return text::startsWith(key, head);
}

bool LineTracker::sameLine(const std::u32string& a, const std::u32string& b)
{
    if (a == b) return true;
    const std::u32string& shortS = a.size() <= b.size() ? a : b;
    const std::u32string& longS = a.size() <= b.size() ? b : a;
    if (shortS.size() >= 4) {
        const size_t n = std::max<size_t>(4, static_cast<size_t>(static_cast<double>(shortS.size()) * 0.9));
        if (text::startsWith(longS, std::u32string_view(shortS).substr(0, n))) return true;
    }
    return similarity(a, b) >= kSameLine;
}

std::vector<LineEvent> LineTracker::update(const std::u32string& rawText, double now)
{
    std::vector<LineEvent> events;
    const std::u32string text = text::strip(rawText);
    if (text.empty()) {
        ++m_misses;
        if (m_cur && m_misses >= kMissPolls && now - m_cur->lastSeen >= kMissTime) {
            events.push_back({LineEvent::Gone, m_cur->id, m_cur->lastSeen, {}});
            m_cur.reset();
        }
        return events;
    }

    m_misses = 0;
    const std::u32string key = compactKey(text);
    if (m_cur && sameLine(m_cur->key, key)) {
        Line& c = *m_cur;
        // Growing text is not "stable": letter by letter, successive readings of a long
        // line are >97% alike and half lines were sent.
        if (grows(c.key, c.maxLen, key)) {
            c.stableN = 1;
            c.grewAt = now;
        } else {
            c.stableN = similarity(c.prevKey, key) >= kStableSim ? c.stableN + 1 : 1;
        }
        c.prevKey = key;
        c.maxLen = std::max(c.maxLen, key.size());
        if (static_cast<double>(key.size()) >= static_cast<double>(c.key.size()) * 0.9) {
            c.text = text;
            c.key = key;
        }
        const std::u32string bestKey = compactKey(c.bestText);
        const double kl = static_cast<double>(key.size()), bl = static_cast<double>(bestKey.size());
        if (kl > bl * 1.05 + 1 || (key.size() > bestKey.size() && text::startsWith(key, dropLast2(bestKey)))
            || (kl >= bl * 0.95 && text::count(text, U' ') > text::count(c.bestText, U' ')))
            c.bestText = text;
        c.lastSeen = now;
    } else {
        if (m_cur) events.push_back({LineEvent::Gone, m_cur->id, m_cur->lastSeen, {}});
        Line l;
        l.id = m_nextId++;
        l.text = text;
        l.key = key;
        l.firstSeen = l.lastSeen = now;
        l.prevKey = key;
        l.bestText = text;
        l.maxLen = key.size();
        m_cur = std::move(l);
        events.push_back({LineEvent::New, m_cur->id, now, {}});
    }

    Line& c = *m_cur;
    const double age = now - c.firstSeen;
    const bool settled = c.stableN >= kStablePolls || (age >= kMaxWait && now - c.grewAt >= kGrowQuiet);
    if (settled || age >= kMaxGrowWait) {
        const std::u32string out = c.bestText.empty() ? c.text : c.bestText;
        if (!c.dispatchedKey) {
            c.dispatchedKey = c.key;
            c.dispatchedText = out;
            c.partial = !settled;
            events.push_back({LineEvent::Dispatch, c.id, now, out});
        } else if (c.key != *c.dispatchedKey && out != *c.dispatchedText
                   && ((c.partial && settled) || similarity(c.key, *c.dispatchedKey) < kRedispatchSim
                       || static_cast<double>(c.key.size()) > static_cast<double>(c.dispatchedKey->size()) * 1.1 + 3)) {
            c.dispatchedKey = c.key;
            c.dispatchedText = out;
            c.partial = c.partial && !settled;
            events.push_back({LineEvent::Dispatch, c.id, now, out});
        }
    }
    return events;
}

std::vector<LineEvent> LineTracker::reset()
{
    std::vector<LineEvent> events;
    if (m_cur) {
        events.push_back({LineEvent::Gone, m_cur->id, m_cur->lastSeen, {}});
        m_cur.reset();
    }
    m_misses = 0;
    return events;
}

// ── SubtitleScheduler ───────────────────────────────────────────────────────

void SubtitleScheduler::sourceNew(int id, double t)
{
    Entry e;
    e.id = id;
    e.firstSeen = t;
    m_entries[id] = std::move(e);
}

void SubtitleScheduler::sourceGone(int id, double lastSeen)
{
    const auto it = m_entries.find(id);
    if (it != m_entries.end() && !it->second.lastSeen) it->second.lastSeen = lastSeen;
}

void SubtitleScheduler::translationReady(int id, const std::u32string& text, const std::string& kind, double t)
{
    const auto it = m_entries.find(id);
    if (it == m_entries.end() || text.empty()) return;
    it->second.text = text;
    it->second.kind = kind;
    if (!it->second.readyAt) it->second.readyAt = t;
}

void SubtitleScheduler::translationFailed(int id)
{
    const auto it = m_entries.find(id);
    if (it != m_entries.end() && !it->second.shownAt && !it->second.text) m_entries.erase(it);
}

void SubtitleScheduler::clear()
{
    m_entries.clear();
    m_current.reset();
}

std::vector<const SubtitleScheduler::Entry*> SubtitleScheduler::waiting() const
{
    std::vector<const Entry*> out;   // not shown yet, translation ready; oldest first
    for (const auto& [id, e] : m_entries)
        if (!e.shownAt && e.hasText()) out.push_back(&e);
    return out;
}

double SubtitleScheduler::endTime(const Entry& e) const
{
    if (e.visible()) return std::numeric_limits<double>::infinity();
    const double src = e.srcDuration();
    const Entry* next = nullptr;
    for (const Entry* w : waiting())
        if (w->id > e.id) {
            next = w;
            break;
        }
    // Nothing queued: at least long enough to read
    if (!next) return *e.shownAt + std::max(src, readingMin(*e.text, minBase, perChar, minCap));
    // A newer line is queued: move at the game's pace; if the next one would fall more than
    // backlogLag behind the game, shorten by the difference
    double base = src;
    const double lag = *e.shownAt + src - next->firstSeen;
    if (lag > backlogLag) base = src - (lag - backlogLag);
    return *e.shownAt + std::max(switchFloor, base);
}

void SubtitleScheduler::forgetStale(double now)
{
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        const Entry& e = it->second;
        if (!e.shownAt && !e.hasText() && e.lastSeen && now - *e.lastSeen > forgetAfter)
            it = m_entries.erase(it);
        else
            ++it;
    }
}

std::optional<Display> SubtitleScheduler::tick(double now)
{
    if (m_entries.size() > 8) forgetStale(now);
    if (m_current) {
        const auto it = m_entries.find(*m_current);
        if (it == m_entries.end() || now >= endTime(it->second)) {
            if (it != m_entries.end()) m_entries.erase(it);
            m_current.reset();
        } else {
            return Display{it->second.id, *it->second.text, it->second.kind};
        }
    }
    Entry* next = pickNext(now);
    if (!next) return std::nullopt;
    next->shownAt = now;
    m_current = next->id;
    const Display shown{next->id, *next->text, next->kind};
    // Older lines never shown are stale now
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->first < shown.id && !it->second.shownAt)
            it = m_entries.erase(it);
        else
            ++it;
    }
    return shown;
}

SubtitleScheduler::Entry* SubtitleScheduler::pickNext(double now)
{
    std::vector<Entry*> ordered, ready;
    for (auto& [id, e] : m_entries)
        if (!e.shownAt) {
            ordered.push_back(&e);
            if (e.hasText()) ready.push_back(&e);
        }
    if (ready.empty()) return nullptr;
    // An older line's translation is about to arrive: wait briefly (keeps the order)
    for (Entry* e : ordered) {
        if (e->id >= ready.front()->id) break;
        if (!e->hasText()) {
            if (now - e->firstSeen < waitOlder) return nullptr;
            break;
        }
    }
    // Skip a line whose source left long ago: after skipAfter if a newer one is ready, else dropAfter
    while (!ready.empty()) {
        Entry* head = ready.front();
        const std::optional<double> late = head->visible() ? std::nullopt : std::optional(now - *head->lastSeen);
        if (late && (*late > dropAfter || (ready.size() > 1 && *late > skipAfter))) {
            const int id = head->id;
            ready.erase(ready.begin());
            m_entries.erase(id);
        } else {
            break;
        }
    }
    return ready.empty() ? nullptr : ready.front();
}

} // namespace live

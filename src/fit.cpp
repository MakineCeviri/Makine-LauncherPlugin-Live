// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "fit.h"

#include "garbage.h"
#include "unitext.h"

#include <algorithm>
#include <cmath>

namespace live::fit {

namespace {

std::vector<std::u32string> speakerLines(const std::u32string& text)
{
    std::vector<std::u32string> out;
    for (size_t b = 0; b <= text.size();) {
        const size_t e = std::min(text.find(U'\n', b), text.size());
        if (e > b) out.push_back(text.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

// Rows one speaker line takes when wrapped at word boundaries (as Text.Wrap does).
int lineRows(std::u32string_view line, int perRow)
{
    if (perRow <= 0) return 1 << 20;
    int rowsUsed = 1, used = 0;
    for (size_t i = 0; i < line.size();) {
        while (i < line.size() && line[i] == U' ') ++i;
        size_t e = i;
        while (e < line.size() && line[e] != U' ') ++e;
        int word = static_cast<int>(e - i);
        if (word == 0) break;
        const int need = used ? used + 1 + word : word;
        if (need <= perRow) {
            used = need;
        } else if (!used) {   // a word longer than a row breaks anywhere (WrapAnywhere fallback)
            rowsUsed += (word - 1) / perRow;
            used = word - ((word - 1) / perRow) * perRow;
        } else {
            ++rowsUsed;
            used = 0;
            continue;   // the word starts the next row
        }
        i = e;
    }
    return rowsUsed;
}

bool sentenceEnd(char32_t c) { return c == U'.' || c == U'!' || c == U'?' || c == U'…'; }
bool clauseEnd(char32_t c) { return c == U',' || c == U';' || c == U':' || c == U'—' || c == U'–'; }

std::u32string trim(std::u32string s) { return text::strip(s); }

// One speaker line → pieces that each fit in b.maxLines rows with the label in front.
std::vector<std::u32string> splitLine(const std::u32string& line, const Budget& b)
{
    const auto fits = [&](const std::u32string& s) { return lineRows(s, b.charsPerRow) <= b.maxLines; };
    if (fits(line)) return {line};
    // the label stays whole and goes in front of every piece
    std::u32string prefix, body = line;
    const text::LineParts parts =text::splitSpeaker(text::toUtf8(line));
    if (!parts.speaker.empty()) {
        const std::u32string bare = text::fromUtf8(parts.text);
        if (!bare.empty() && bare.size() < line.size() && line.compare(line.size() - bare.size(), bare.size(), bare) == 0) {
            prefix = line.substr(0, line.size() - bare.size());
            body = bare;
        }
    }
    const int prefixLen = static_cast<int>(prefix.size());
    const int maxChars = std::max(8, static_cast<int>(b.charsPerRow * b.maxLines * 0.9) - prefixLen);
    std::vector<std::u32string> out;
    std::u32string rest = body;
    while (!rest.empty()) {
        if (fits(prefix + rest)) {
            out.push_back(prefix + rest);
            break;
        }
        const int len = static_cast<int>(rest.size());
        const int n = std::max(2, (len + maxChars - 1) / maxChars);
        const double ideal = double(len) / n;
        int best = -1;
        double bestScore = -1e9;
        for (int i = std::max(1, static_cast<int>(ideal * 0.5)); i < len; ++i) {
            // cut before rest[i]: rest[i] is a space; rest[i-1] says what kind of boundary it is
            if (rest[static_cast<size_t>(i)] != U' ') continue;
            const char32_t c = rest[static_cast<size_t>(i - 1)];
            const double kind = sentenceEnd(c) ? 3 : clauseEnd(c) ? 2 : 1;
            const std::u32string piece = trim(rest.substr(0, static_cast<size_t>(i)));
            if (!fits(prefix + piece)) break;   // longer cuts only get longer
            const double score = kind - 2.0 * std::abs(i - ideal) / ideal;
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        if (best < 0) {   // no space early enough: the longest head that fits (one long word)
            best = 1;
            while (best < len && fits(prefix + rest.substr(0, static_cast<size_t>(best + 1)))) ++best;
        }
        out.push_back(prefix + trim(rest.substr(0, static_cast<size_t>(best))));
        rest = trim(rest.substr(static_cast<size_t>(best)));
    }
    return out;
}

} // namespace

Budget budget(double frameWidthPx, double sizePx, int maxLines)
{
    // SubtitleBlock: padX 22 px at 28 px (grows with the font), a 14 px gutter on each side
    const double font = std::max(8.0, sizePx * kMinFontScale);
    const double inner = frameWidthPx * kMaxWidthFrac - 2 * 22 * std::max(1.0, font / 28) - 2 * 14;
    return {std::max(8, static_cast<int>(inner / (kGlyphEm * font))), std::max(1, maxLines)};
}

Budget tvBudget(double relSize, int maxLines)
{
    // a 16:9 screen in units of its height; the band keeps the safe area and one font height of padding
    const double rel = std::clamp(relSize, 0.02, 0.2);
    const double inner = 16.0 / 9.0 * kMaxWidthFrac - 2 * rel;
    return {std::max(8, static_cast<int>(inner / (kGlyphEm * rel))), std::max(1, maxLines)};
}

Budget tighter(const Budget& a, const Budget& b)
{
    return {std::min(a.charsPerRow, b.charsPerRow), std::min(a.maxLines, b.maxLines)};
}

int rows(const std::u32string& text, const Budget& b)
{
    int n = 0;
    for (const auto& line : speakerLines(text)) n += lineRows(line, b.charsPerRow);
    return n;
}

std::vector<std::u32string> split(const std::u32string& text, const Budget& b)
{
    if (b.charsPerRow <= 0 || rows(text, b) <= b.maxLines) return {text};
    std::vector<std::u32string> pieces;   // each speaker line's pieces, in order
    for (const auto& line : speakerLines(text))
        for (auto& p : splitLine(line, b)) pieces.push_back(std::move(p));
    // pack: consecutive pieces share a chunk while their rows still fit (the renderer keeps the last maxLines lines)
    std::vector<std::u32string> chunks;
    for (const auto& p : pieces) {
        if (!chunks.empty()) {
            const std::u32string joined = chunks.back() + U"\n" + p;
            if (rows(joined, b) <= b.maxLines) {
                chunks.back() = joined;
                continue;
            }
        }
        chunks.push_back(p);
    }
    return chunks;
}

double chunkSeconds(const std::u32string& chunk, double minShow)
{
    return std::max(minShow, double(chunk.size()) / kCharsPerSecond);
}

void ChunkPlayer::start(int id, std::vector<std::u32string> chunks, double minShow, double now)
{
    m_id = id;
    m_start = now;
    m_chunks = std::move(chunks);
    m_seconds.clear();
    for (const auto& c : m_chunks) m_seconds.push_back(chunkSeconds(c, minShow));
}

void ChunkPlayer::clear()
{
    m_chunks.clear();
    m_seconds.clear();
    m_id = 0;
}

int ChunkPlayer::at(double now) const
{
    double end = m_start;
    for (size_t i = 0; i + 1 < m_seconds.size(); ++i) {
        end += m_seconds[i];
        if (now < end) return static_cast<int>(i);
    }
    return std::max(0, count() - 1);
}

double ChunkPlayer::total() const
{
    double s = 0;
    for (double v : m_seconds) s += v;
    return s;
}

} // namespace live::fit

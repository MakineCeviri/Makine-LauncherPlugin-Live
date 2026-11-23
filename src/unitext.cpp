// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "unitext.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace live::text {

namespace {

struct CharRun {
    char32_t start;
    uint16_t flags;
};
struct LowerPair {
    char32_t from;
    char32_t to;
};

#include "unicode_data.inc"

enum : uint16_t {
    fAlpha = 1 << 0,
    fDecimal = 1 << 1,
    fDigit = 1 << 2,
    fNumeric = 1 << 3,
    fLower = 1 << 4,
    fUpper = 1 << 5,
    fTitle = 1 << 6,
    fSpace = 1 << 7,
    fNumber = 1 << 8,
    fCased = 1 << 9,
    fCaseIgnorable = 1 << 10,
};

uint16_t flagsOf(char32_t c)
{
    if (c < 0x80) {
        // ASCII fast path, same values as the table
        static const auto ascii = [] {
            std::array<uint16_t, 128> t{};
            for (char32_t i = 0; i < 128; ++i) {
                const auto it = std::upper_bound(std::begin(kRuns), std::end(kRuns), i,
                                                 [](char32_t v, const CharRun& r) { return v < r.start; });
                t[i] = std::prev(it)->flags;
            }
            return t;
        }();
        return ascii[c];
    }
    const auto it = std::upper_bound(std::begin(kRuns), std::end(kRuns), c,
                                     [](char32_t v, const CharRun& r) { return v < r.start; });
    return it == std::begin(kRuns) ? 0 : std::prev(it)->flags;
}

char32_t simpleLower(char32_t c)
{
    if (c < 0x80) return (c >= U'A' && c <= U'Z') ? c + 32 : c;
    const auto it = std::lower_bound(std::begin(kLower), std::end(kLower), c,
                                     [](const LowerPair& p, char32_t v) { return p.from < v; });
    return it != std::end(kLower) && it->from == c ? it->to : c;
}

void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace

std::u32string fromUtf8(std::string_view s)
{
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        int len = 0;
        char32_t cp = 0;
        if (b < 0x80) { cp = b; len = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; len = 4; }
        bool ok = len > 0 && i + len <= s.size();
        for (int k = 1; ok && k < len; ++k) {
            const auto c = static_cast<unsigned char>(s[i + k]);
            if ((c & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (c & 0x3F);
        }
        if (!ok) {
            out += U'�';
            ++i;
            continue;
        }
        out += cp;
        i += len;
    }
    return out;
}

std::string toUtf8(std::u32string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char32_t c : text) appendUtf8(out, c);
    return out;
}

bool isAlpha(char32_t c) { return flagsOf(c) & fAlpha; }
bool isDecimal(char32_t c) { return flagsOf(c) & fDecimal; }
bool isDigit(char32_t c) { return flagsOf(c) & fDigit; }
bool isNumeric(char32_t c) { return flagsOf(c) & fNumeric; }
bool isLower(char32_t c) { return flagsOf(c) & fLower; }
bool isUpper(char32_t c) { return flagsOf(c) & fUpper; }
bool isTitle(char32_t c) { return flagsOf(c) & fTitle; }
bool isSpace(char32_t c) { return flagsOf(c) & fSpace; }
bool isNumber(char32_t c) { return flagsOf(c) & fNumber; }

std::u32string lower(std::u32string_view s)
{
    std::u32string out;
    out.reserve(s.size() + 2);
    for (size_t i = 0; i < s.size(); ++i) {
        const char32_t c = s[i];
        if (c == 0x130) {   // the only multi-character lowercase mapping
            out += U'i';
            out += U'̇';
        } else if (c == 0x3A3) {   // CPython handle_capital_sigma
            size_t j = i;
            char32_t p = 0;
            bool found = false;
            while (j > 0) {
                p = s[--j];
                if (!(flagsOf(p) & fCaseIgnorable)) { found = true; break; }
            }
            bool finalSigma = found && (flagsOf(p) & fCased);
            if (finalSigma && i + 1 < s.size()) {
                size_t k = i + 1;
                for (; k < s.size(); ++k)
                    if (!(flagsOf(s[k]) & fCaseIgnorable)) break;
                finalSigma = k == s.size() || !(flagsOf(s[k]) & fCased);
            }
            out += finalSigma ? U'ς' : U'σ';
        } else {
            out += simpleLower(c);
        }
    }
    return out;
}

std::u32string strip(std::u32string_view s)
{
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return std::u32string(s.substr(b, e - b));
}

std::u32string strip(std::u32string_view s, std::u32string_view chars)
{
    size_t b = 0, e = s.size();
    while (b < e && chars.find(s[b]) != std::u32string_view::npos) ++b;
    while (e > b && chars.find(s[e - 1]) != std::u32string_view::npos) --e;
    return std::u32string(s.substr(b, e - b));
}

std::vector<std::u32string> split(std::u32string_view s)
{
    std::vector<std::u32string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isSpace(s[i])) ++i;
        const size_t b = i;
        while (i < s.size() && !isSpace(s[i])) ++i;
        if (i > b) out.emplace_back(s.substr(b, i - b));
    }
    return out;
}

std::vector<std::u32string> split(std::u32string_view s, char32_t sep)
{
    std::vector<std::u32string> out;
    size_t b = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(b, i - b));
            b = i + 1;
        }
    }
    return out;
}

std::u32string join(const std::vector<std::u32string>& parts, std::u32string_view sep)
{
    std::u32string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

bool strIsAlpha(std::u32string_view s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), isAlpha);
}

bool strIsUpper(std::u32string_view s)
{
    bool cased = false;
    for (const char32_t c : s) {
        if (isLower(c) || isTitle(c)) return false;
        if (isUpper(c)) cased = true;
    }
    return cased;
}

size_t count(std::u32string_view s, char32_t c)
{
    return static_cast<size_t>(std::count(s.begin(), s.end(), c));
}

bool startsWith(std::u32string_view s, std::u32string_view prefix)
{
    return s.substr(0, prefix.size()) == prefix;
}

std::u32string asciiLower(std::u32string_view s)
{
    std::u32string out(s);
    for (auto& c : out)
        if (c >= U'A' && c <= U'Z') c += 32;
    return out;
}

// difflib.SequenceMatcher with isjunk=None and autojunk=False: no junk and no
// "popular" elements, so b2j holds every element of b. ratio() only needs the
// total size of the matching blocks, which does not depend on their order.
double sequenceRatio(std::u32string_view a, std::u32string_view b)
{
    const size_t la = a.size(), lb = b.size();
    if (la + lb == 0) return 1.0;
    std::unordered_map<char32_t, std::vector<int>> b2j;
    for (size_t j = 0; j < lb; ++j) b2j[b[j]].push_back(static_cast<int>(j));

    const auto findLongest = [&](int alo, int ahi, int blo, int bhi, int& besti, int& bestj) {
        besti = alo;
        bestj = blo;
        int bestsize = 0;
        // As difflib: prev maps j -> length of the match ending at a[i-1], b[j].
        std::unordered_map<int, int> prev, cur;
        for (int i = alo; i < ahi; ++i) {
            cur.clear();
            const auto it = b2j.find(a[i]);
            if (it != b2j.end()) {
                for (const int j : it->second) {
                    if (j < blo) continue;
                    if (j >= bhi) break;
                    const auto p = prev.find(j - 1);
                    const int k = (p == prev.end() ? 0 : p->second) + 1;
                    cur[j] = k;
                    if (k > bestsize) {
                        besti = i - k + 1;
                        bestj = j - k + 1;
                        bestsize = k;
                    }
                }
            }
            std::swap(prev, cur);
        }
        while (besti > alo && bestj > blo && a[besti - 1] == b[bestj - 1]) {
            --besti;
            --bestj;
            ++bestsize;
        }
        while (besti + bestsize < ahi && bestj + bestsize < bhi && a[besti + bestsize] == b[bestj + bestsize])
            ++bestsize;
        return bestsize;
    };

    struct Span {
        int alo, ahi, blo, bhi;
    };
    std::vector<Span> queue{{0, static_cast<int>(la), 0, static_cast<int>(lb)}};
    size_t matches = 0;
    while (!queue.empty()) {
        const Span s = queue.back();
        queue.pop_back();
        int i = 0, j = 0;
        const int k = findLongest(s.alo, s.ahi, s.blo, s.bhi, i, j);
        if (!k) continue;
        matches += static_cast<size_t>(k);
        if (s.alo < i && s.blo < j) queue.push_back({s.alo, i, s.blo, j});
        if (i + k < s.ahi && j + k < s.bhi) queue.push_back({i + k, s.ahi, j + k, s.bhi});
    }
    return 2.0 * static_cast<double>(matches) / static_cast<double>(la + lb);
}

} // namespace live::text

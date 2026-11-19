// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "text_fix.h"

#include <cctype>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace live {

// A piece of a split must be at least this common (rank = position in the frequency list).
static constexpr int kPieceRank = 20000;
// Stricter limit when the token looks like a proper noun ("Kamski"), so names are not chopped up.
static constexpr int kNamePieceRank = 3000;
// Contractions sit at the end of the list but are very common in dialogue.
static constexpr int kContractionRank = 300;
// Tokens longer than this are not segmented (cost/benefit; OCR noise).
static constexpr size_t kMaxCoreLen = 48;

static bool isAsciiAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
static bool isLower(char c) { return c >= 'a' && c <= 'z'; }
static bool isCoreChar(char c) { return isAsciiAlpha(c) || c == '\''; }
static bool isSplitPunct(char c) { return c == '.' || c == '!' || c == '?' || c == ',' || c == ';' || c == ':'; }

static std::string lower(const std::string& s)
{
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool SpacingFixer::load(const std::wstring& wordListPath)
{
    m_rank.clear();
    m_maxLen = 0;
    std::ifstream in{std::filesystem::path(wordListPath)};
    if (!in) return false;
    std::string word;
    int index = 0;
    while (std::getline(in, word)) {
        if (!word.empty() && word.back() == '\r') word.pop_back();
        if (word.empty()) continue;
        int r = word.find('\'') != std::string::npos ? std::min(index, kContractionRank) : index;
        m_rank.emplace(word, r);
        m_maxLen = std::max(m_maxLen, word.size());
        ++index;
    }
    return !m_rank.empty();
}

int SpacingFixer::rank(const std::string& lowerWord) const
{
    auto it = m_rank.find(lowerWord);
    return it == m_rank.end() ? -1 : it->second;
}

// Dynamic-programming segmentation of `core` (letters/apostrophes) into common words.
// Cost of a word = log((rank + 1) * log(N)) — the usual Zipf-based word cost.
bool SpacingFixer::splitCore(const std::string& core, const std::string& original, std::string& out) const
{
    const size_t n = core.size();
    if (n < 3 || n > kMaxCoreLen) return false;

    const bool nameLike = isUpper(original[0]);
    const int limit = nameLike ? kNamePieceRank : kPieceRank;
    const double logN = std::log(static_cast<double>(m_rank.size()));
    const double inf = std::numeric_limits<double>::infinity();

    std::vector<double> best(n + 1, inf);
    std::vector<size_t> back(n + 1, 0);
    best[0] = 0;
    for (size_t i = 1; i <= n; ++i) {
        for (size_t k = 1; k <= std::min(m_maxLen, i); ++k) {
            if (best[i - k] == inf) continue;
            const std::string piece = core.substr(i - k, k);
            const int r = rank(piece);
            if (r < 0 || r >= limit) continue;
            // Single letters only for "a"/"I" or an uppercase letter in the original ("Xto" → "X to")
            if (k == 1 && piece != "a" && piece != "i" && !isUpper(original[i - k])) continue;
            const double c = best[i - k] + std::log((r + 1.0) * logN);
            if (c < best[i]) { best[i] = c; back[i] = k; }
        }
    }
    if (best[n] == inf) return false;

    std::vector<size_t> lengths;
    for (size_t i = n; i > 0; i -= back[i]) lengths.insert(lengths.begin(), back[i]);
    if (lengths.size() < 2) return false;
    // Two single letters in a row is almost always noise ("a i")
    for (size_t j = 1; j < lengths.size(); ++j)
        if (lengths[j] == 1 && lengths[j - 1] == 1) return false;

    out.clear();
    size_t pos = 0;
    for (size_t j = 0; j < lengths.size(); ++j) {
        if (j) out += ' ';
        out += original.substr(pos, lengths[j]);
        pos += lengths[j];
    }
    return true;
}

std::string SpacingFixer::fixToken(const std::string& token) const
{
    // 1) "Shaun.I" / "here,before" → separate at sentence punctuation between two letters
    //    (but keep abbreviations like "U.S." / "e.g.").
    for (size_t i = 1; i + 1 < token.size(); ++i) {
        if (!isSplitPunct(token[i]) || !isAsciiAlpha(token[i - 1]) || !isAsciiAlpha(token[i + 1])) continue;
        const bool leftSingle = i < 2 || !isAsciiAlpha(token[i - 2]);
        const bool rightSingle = i + 2 >= token.size() || !isAsciiAlpha(token[i + 2]);
        if (token[i] == '.' && leftSingle && rightSingle) continue;  // abbreviation
        return fixToken(token.substr(0, i + 1)) + ' ' + fixToken(token.substr(i + 1));
    }

    // 2) Split off leading/trailing punctuation; only pure letter/apostrophe cores are segmented.
    size_t b = 0, e = token.size();
    while (b < e && !isCoreChar(token[b])) ++b;
    while (e > b && !isCoreChar(token[e - 1])) --e;
    while (b < e && token[b] == '\'') ++b;          // quotes, not contractions
    while (e > b && token[e - 1] == '\'') --e;
    if (e - b < 3) return token;
    const std::string core = token.substr(b, e - b);
    for (char c : core)
        if (!isCoreChar(c)) return token;

    const std::string lc = lower(core);
    if (rank(lc) >= 0) return token;  // known word (names, rare words, game terms in the list)

    // 3) camelCase boundary inside an unknown token: "PressXto" → "Press" + "Xto"
    for (size_t i = 1; i < core.size(); ++i) {
        if (isLower(core[i - 1]) && isUpper(core[i])) {
            const std::string left = core.substr(0, i), right = core.substr(i);
            return token.substr(0, b) + fixToken(left) + ' ' + fixToken(right) + token.substr(e);
        }
    }

    // 4) Dictionary segmentation of the lowercase core
    std::string split;
    if (!splitCore(lc, core, split)) return token;
    return token.substr(0, b) + split + token.substr(e);
}

static bool isCjkCodePoint(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF)      // CJK radicals, kana, CJK symbols, ideographs
        || (cp >= 0xAC00 && cp <= 0xD7AF)      // Hangul syllables
        || (cp >= 0xF900 && cp <= 0xFAFF)      // CJK compatibility ideographs
        || (cp >= 0xFF00 && cp <= 0xFFEF)      // fullwidth forms
        || (cp >= 0x20000 && cp <= 0x2FA1F);   // CJK extensions
}

static bool containsCjk(const std::string& s)
{
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) { cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu); n = 4; }
        else if (c >= 0xE0 && i + 2 < s.size()) { cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu); n = 3; }
        else if (c >= 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu); n = 2; }
        if (isCjkCodePoint(cp)) return true;
        i += n;
    }
    return false;
}

bool isLatinScriptLanguage(const std::string& lang)
{
    static const char* const kLatin[] = {"en", "de", "fr", "es", "it", "pt", "tr", "pl", "nl", "sv", "da", "no", "fi", "cs", "hu", "ro"};
    for (const char* l : kLatin)
        if (lang == l) return true;
    return false;
}

std::string dropForeignScriptTokens(const std::string& text)
{
    std::string out;
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        const std::string line = text.substr(lineStart, lineEnd - lineStart);
        std::string kept;
        size_t p = 0;
        while (p < line.size()) {
            size_t q = line.find(' ', p);
            if (q == std::string::npos) q = line.size();
            const std::string tok = line.substr(p, q - p);
            if (!tok.empty() && !containsCjk(tok)) {
                if (!kept.empty()) kept += ' ';
                kept += tok;
            }
            p = q + 1;
        }
        if (!kept.empty()) {
            if (!out.empty()) out += '\n';
            out += kept;
        }
        if (lineEnd == text.size()) break;
        lineStart = lineEnd + 1;
    }
    return out;
}

std::string SpacingFixer::fix(const std::string& text) const
{
    if (!loaded()) return text;
    std::string out;
    out.reserve(text.size() + 16);
    std::string token;
    auto flush = [&] {
        if (!token.empty()) { out += fixToken(token); token.clear(); }
    };
    for (char c : text) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { flush(); out += c; }
        else token += c;
    }
    flush();
    return out;
}

} // namespace live

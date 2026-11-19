// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

#include <string>
#include <unordered_map>

namespace live {

// For Latin-script source languages: removes tokens containing CJK / fullwidth characters, which the Chinese
// recognition model produces from background noise ("园图B"). Empty lines are dropped.
std::string dropForeignScriptTokens(const std::string& text);

// Source languages written in the Latin alphabet (CJK in their OCR output is always noise).
bool isLatinScriptLanguage(const std::string& lang);

/**
 * Restores word spaces that OCR dropped in English text ("Iwas goingto findyou" → "I was going to find you").
 *
 * Uses a frequency-ordered word list (resources/en_words.txt, most common first). A token is left alone when it is a
 * known word (character names, rare words); an unknown token is split only when every piece is a COMMON word, so
 * names and game terms are not chopped up. Also separates "Shaun.I" → "Shaun. I" and "PressXto" → "Press X to".
 */
class SpacingFixer {
public:
    bool load(const std::wstring& wordListPath);
    bool loaded() const { return !m_rank.empty(); }

    std::string fix(const std::string& text) const;

private:
    std::string fixToken(const std::string& token) const;
    bool splitCore(const std::string& core, const std::string& original, std::string& out) const;
    int rank(const std::string& lowerWord) const;

    std::unordered_map<std::string, int> m_rank;
    size_t m_maxLen = 0;
};

} // namespace live

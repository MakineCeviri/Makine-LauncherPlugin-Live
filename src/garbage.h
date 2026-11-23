// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// The text rules every OCR line passes before it may reach the shared pool. Ports of
// makine_live/net/pool_client.py (normalize_dialogue_text, compact_pool_key) and
// makine_live/core/garbage.py (garbage_reason = the server's rule, filter_ocr), checked
// against spec/normalization_vectors.json and spec/garbage_vectors.json (tests/cpp/).
// Garbage reaching the vote-based pool is the worst failure this plugin can have: change
// these only together with the Python harness, the vectors and the server.

#include <string>
#include <string_view>
#include <unordered_map>

namespace live::text {

/// Spec v2: trim, lowercase, collapse blanks, strip leading quotes/dashes and trailing punctuation.
std::u32string normalizeDialogue(std::u32string_view text);
std::string normalizeDialogue(std::string_view utf8);

/// Pool key: the letters and digits of the normalized text (the server's dialogue_key).
std::u32string compactPoolKey(std::u32string_view text);
std::string compactPoolKey(std::string_view utf8);

/// game_library.slugify: the server's game slug ("Baldur's Gate 3" → "baldurs-gate-3").
std::string slugify(std::string_view name);
/// The pool slug of a game the launcher names: slugify, except the harness's pinned slugs.
std::string gameSlug(std::string_view name);

class Garbage {
public:
    /// Contents of resources/en_words.txt (frequency order). Without it every English
    /// line scores as meaningless, so callers must treat a failed load as fatal.
    bool setWords(std::string_view fileContent);
    bool loaded() const { return !m_rank.empty(); }

    /// garbage_reason: why the line is not dialogue (Turkish, as the server says it), or "".
    std::string reason(std::string_view source, std::string_view translation = {},
                       std::string_view sourceLang = "en") const;

    /// filter_ocr: OCR boxes ("\n"-separated) filtered line by line and joined into one
    /// sentence; "" when nothing left is dialogue (HUD, noise, what the server would refuse).
    std::string filterOcr(std::string_view raw, std::string_view sourceLang = "en") const;

private:
    std::u32string reasonOf(std::u32string_view source, std::u32string_view translation,
                            std::string_view sourceLang) const;
    bool splitsIntoWords(std::u32string_view lc) const;
    double englishScore(std::u32string_view text, int& count) const;
    int rank(std::u32string_view word) const;

    std::unordered_map<std::u32string, int> m_rank;   // word -> first index in en_words.txt
};

} // namespace live::text

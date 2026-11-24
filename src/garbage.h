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
#include <unordered_set>
#include <vector>

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

// ── subtitle shape (the server's src/live/text.ts; spec/speaker_vectors.json, line_issue_vectors.json,
// clipped_edge_vectors.json): speaker label, language note, clipped and merged readings ──

struct LineParts {
    std::string speaker;   // "Jason", "" when there is none
    std::string note;      // Turkish language names ("İspanyolca"), "" when there is none
    std::string text;      // the line itself: what is keyed and translated
};

/// "In Spanish" / "(speaking Italian)" / "Unknown Language" → its Turkish name, else "".
std::string languageNote(std::string_view label);
/// "Jason", "Dre'Quan", "Staff Member", "GAETANO", "Gang Member 2": a speaker label of up to three words.
bool isSpeakerLabel(std::string_view label);
/// "Mia: In Spanish: Oh, shit!" → {Mia, İspanyolca, "Oh, shit!"}; a leading "[groans]" goes too.
LineParts splitSpeaker(std::string_view source);
/// What is shown in front of a translation: "Jason: ", "Mia: (İspanyolca) ", "(İspanyolca) ".
std::string linePrefix(const std::string& speaker, const std::string& note);

struct ReadLine {
    std::string line;                  // keyed, looked up and sent: the line without its label, or the whole text
    std::string speaker, note, prefix; // prefix: shown again in front of the translation, untranslated
    std::vector<std::string> issues;   // lineIssues of the line
    bool hard = false;                 // clipped_left / clipped_right / multi_speaker
};

struct OcrBox {   // one OCR line and its box in the captured region (px)
    std::string text;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

struct EdgeVerdict {
    bool send = true;                 // false: a line was cut by the region, read again once it has grown
    std::vector<std::string> cut;     // "left" | "right" | "top" | "bottom"
    std::vector<std::string> widen;   // the sides to grow
    std::vector<std::string> lines;   // the reading split at each speaker
};

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

    /// A word of English (or a dropped-g "fuckin'"): the server's isEnglishWord.
    bool isEnglishWord(std::string_view token) const;
    /// clipped_left · clipped_right · multi_speaker · trailing_name · merged_tail (server lineIssues).
    std::vector<std::string> lineIssues(std::string_view text) const;
    /// One reading as the pool takes it (server readLine): the label comes off only when the rest is still a line.
    ReadLine readLine(std::string_view text, std::string_view sourceLang = "en") const;
    /// A block of several speakers split at every "[.!?…] Name:" boundary.
    std::vector<std::string> speakerLines(std::string_view text) const;
    /// Did the capture region cut a line (clipped_edge_vectors.json)? width / height: the region in px.
    EdgeVerdict edgeCheck(const std::vector<OcrBox>& boxes, int width, int height) const;

private:
    std::u32string reasonOf(std::u32string_view source, std::u32string_view translation,
                            std::string_view sourceLang) const;
    bool splitsIntoWords(std::u32string_view lc) const;
    double englishScore(std::u32string_view text, int& count) const;
    int rank(std::u32string_view word) const;
    bool isWordToken(std::u32string_view token) const;

    std::unordered_map<std::u32string, int> m_rank;   // word -> first index in en_words.txt
    std::unordered_set<std::u32string> m_all;          // every word of en_words.txt (the server's dictionary)
};

} // namespace live::text

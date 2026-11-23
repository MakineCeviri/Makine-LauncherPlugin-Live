// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// Line-for-line port of makine_live/core/garbage.py and the normalization of
// makine_live/net/pool_client.py. No <regex> (broken in MinGW 13.1): every Python
// pattern is spelled out by hand next to its original.

#include "garbage.h"

#include "unitext.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace live::text {

namespace {

using U = std::u32string;
using UV = std::u32string_view;

constexpr UV kWs = U" \t\n\r\f\v ";
constexpr UV kLeading = U"\"'`“”‘’«»([-–—… \t\n\r\f\v ";
constexpr UV kTrailing = U".,!?:;\"'`~_-–—…“”‘’«»)] \t\n\r\f\v ";

bool in(UV set, char32_t c) { return set.find(c) != UV::npos; }

// ── A tiny anchored regex for _INTERJECTION: literals, ( ) | * + ? ─────────────

struct Rx {
    enum Kind { Char, Seq, Alt, Rep } kind = Char;
    char32_t c = 0;
    int min = 0, max = -1;
    std::vector<Rx> kids;
};

class RxParser {
public:
    explicit RxParser(UV p) : m_p(p) {}
    Rx parse() { return alt(); }

private:
    Rx alt()
    {
        Rx node;
        node.kind = Rx::Alt;
        node.kids.push_back(seq());
        while (m_i < m_p.size() && m_p[m_i] == U'|') {
            ++m_i;
            node.kids.push_back(seq());
        }
        return node;
    }
    Rx seq()
    {
        Rx node;
        node.kind = Rx::Seq;
        while (m_i < m_p.size() && m_p[m_i] != U'|' && m_p[m_i] != U')') {
            Rx atom;
            if (m_p[m_i] == U'(') {
                ++m_i;
                atom = alt();
                ++m_i;   // ')'
            } else {
                atom.kind = Rx::Char;
                atom.c = m_p[m_i++];
            }
            if (m_i < m_p.size() && (m_p[m_i] == U'*' || m_p[m_i] == U'+' || m_p[m_i] == U'?')) {
                Rx rep;
                rep.kind = Rx::Rep;
                rep.min = m_p[m_i] == U'+' ? 1 : 0;
                rep.max = m_p[m_i] == U'?' ? 1 : -1;
                rep.kids.push_back(std::move(atom));
                atom = std::move(rep);
                ++m_i;
            }
            node.kids.push_back(std::move(atom));
        }
        return node;
    }
    UV m_p;
    size_t m_i = 0;
};

using Cont = std::function<bool(size_t)>;

bool rxMatch(const Rx& n, UV s, size_t i, const Cont& k);

bool rxSeq(const Rx& n, size_t idx, UV s, size_t i, const Cont& k)
{
    if (idx == n.kids.size()) return k(i);
    return rxMatch(n.kids[idx], s, i, [&](size_t j) { return rxSeq(n, idx + 1, s, j, k); });
}

bool rxRep(const Rx& n, int count, UV s, size_t i, const Cont& k)
{
    if ((n.max < 0 || count < n.max)
        && rxMatch(n.kids[0], s, i, [&](size_t j) { return j != i && rxRep(n, count + 1, s, j, k); }))
        return true;
    return count >= n.min && k(i);
}

bool rxMatch(const Rx& n, UV s, size_t i, const Cont& k)
{
    switch (n.kind) {
    case Rx::Char: return i < s.size() && s[i] == n.c && k(i + 1);
    case Rx::Seq: return rxSeq(n, 0, s, i, k);
    case Rx::Alt:
        for (const Rx& kid : n.kids)
            if (rxMatch(kid, s, i, k)) return true;
        return false;
    case Rx::Rep: return rxRep(n, 0, s, i, k);
    }
    return false;
}

// _INTERJECTION (re.I): the whole token is a sound, not a word.
bool isInterjection(UV t)
{
    static const Rx rx = RxParser(
        U"h*m+h*m*|h+m+p+h*|u+h+|u+m+|uh-?huh|a+h+|o+h+|w+h?o+a+h*|s+h+|p+s+t+|a?r+g+h+|e+r+m*|u+g+h+|h+u+h+"
        U"|o+w+|o+p+s+|y+e+a+h+|y+e+p+|n+o+p+e+|ok(ay)?|a+h+a+|wo+w+|he+y+|ha+(ha+)*|he+(he+)*|pf+t*|tsk|brr+|gr+")
                               .parse();
    const U lc = asciiLower(t);
    return rxMatch(rx, lc, 0, [&](size_t j) { return j == lc.size(); });
}

const std::unordered_set<U>& splitShort()
{
    static const std::unordered_set<U> s{U"i", U"a", U"to", U"of", U"in", U"is", U"it", U"on", U"at", U"be",
                                         U"do", U"go", U"he", U"me", U"my", U"no", U"so", U"up", U"us", U"we",
                                         U"an", U"as", U"by", U"if", U"or", U"oh", U"ok", U"am"};
    return s;
}

const std::unordered_set<U>& realShort()
{
    static const std::unordered_set<U> s = [] {
        std::unordered_set<U> r = splitShort();
        for (UV w : {U"hi", U"ya", U"yo", U"ye", U"ah", U"eh", U"uh", U"hm", U"mm", U"ha", U"ow", U"aw", U"em",
                     U"ma", U"pa", U"mr", U"ms", U"dr", U"st", U"tv", U"pc", U"ex", U"th", U"ta"})
            r.emplace(w);
        return r;
    }();
    return s;
}

const std::unordered_set<U>& systemLines()
{
    static const std::unordered_set<U> s{
        U"start", U"search", U"taskbar", U"widgets", U"weather", U"battery", U"microsoft", U"google chrome",
        U"discord", U"steam", U"başlat", U"ara", U"görev çubuğu", U"hava durumu", U"pil", U"duraklat", U"kilit",
        U"alan seç", U"taşıma", U"canlı çeviri", U"havuz", U"f8", U"f9", U"f10"};
    return s;
}

bool isGlyph(UV t)
{
    for (UV g : {U"a", U"ya", U"y", U"x", U"b", U"o", U"e", U"ii"})
        if (t == g) return true;
    return false;
}

const std::set<std::string>& latinSourceLangs()
{
    static const std::set<std::string> s{"en", "de", "fr", "es", "it", "pt", "tr", "pl",
                                         "nl", "sv", "da", "no", "fi", "cs", "hu", "ro"};
    return s;
}

// _CJK
bool isCjk(char32_t c)
{
    return (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0x3040 && c <= 0x30FF)
           || (c >= 0x3130 && c <= 0x318F) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF)
           || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF)
           || (c >= 0x20000 && c <= 0x2FA1F);
}

bool isAsciiLetter(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z'); }

bool ieq(UV s, size_t at, UV lowerWord)
{
    if (at + lowerWord.size() > s.size()) return false;
    for (size_t k = 0; k < lowerWord.size(); ++k) {
        char32_t c = s[at + k];
        if (c >= U'A' && c <= U'Z') c += 32;
        if (c != lowerWord[k]) return false;
    }
    return true;
}

// _ADDRESS: https?://|www\.|[\w.+-]+@[\w-]+\.\w|\b[a-z]:[\\/]|\.(exe|dll|png|jpe?g|txt|zip|php|html?)\b  (re.I)
bool hasAddress(UV s)
{
    const auto word = [&](size_t i) { return i < s.size() && isWord(s[i]); };
    for (size_t i = 0; i < s.size(); ++i) {
        if (ieq(s, i, U"http://") || ieq(s, i, U"https://") || ieq(s, i, U"www.")) return true;
        if (s[i] == U'@' && i > 0) {
            const char32_t p = s[i - 1];
            if (isWord(p) || p == U'.' || p == U'+' || p == U'-') {
                size_t j = i + 1;
                while (j < s.size() && (isWord(s[j]) || s[j] == U'-')) ++j;
                if (j > i + 1 && j < s.size() && s[j] == U'.' && word(j + 1)) return true;
            }
        }
        if (isAsciiLetter(s[i]) && (i == 0 || !isWord(s[i - 1])) && i + 2 < s.size() && s[i + 1] == U':'
            && (s[i + 2] == U'\\' || s[i + 2] == U'/'))
            return true;
        if (s[i] == U'.') {
            for (UV ext : {U"exe", U"dll", U"png", U"jpeg", U"jpg", U"txt", U"zip", U"php", U"html", U"htm"})
                if (ieq(s, i + 1, ext) && !word(i + 1 + ext.size())) return true;
        }
    }
    return false;
}

// re.findall(r"[^\W\d_]+", s)
std::vector<U> letterRuns(UV s)
{
    std::vector<U> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && !isWordLetter(s[i])) ++i;
        const size_t b = i;
        while (i < s.size() && isWordLetter(s[i])) ++i;
        if (i > b) out.emplace_back(s.substr(b, i - b));
    }
    return out;
}

// len(re.findall(r"[^\W\d_]+(?:'[^\W\d_]+)?", s))
size_t wordCount(UV s)
{
    size_t n = 0, i = 0;
    while (i < s.size()) {
        if (!isWordLetter(s[i])) {
            ++i;
            continue;
        }
        while (i < s.size() && isWordLetter(s[i])) ++i;
        if (i + 1 < s.size() && s[i] == U'\'' && isWordLetter(s[i + 1])) {
            ++i;
            while (i < s.size() && isWordLetter(s[i])) ++i;
        }
        ++n;
    }
    return n;
}

U onlyAsciiLower(UV t)   // re.sub(r"[^a-z]", "", t)
{
    U out;
    for (const char32_t c : t)
        if (c >= U'a' && c <= U'z') out += c;
    return out;
}

bool hasLower(UV s)   // _has_lower
{
    return std::count_if(s.begin(), s.end(), isLower) >= 2;
}

const U kTooShort = U"çok kısa";
const U kForeign = U"yabancı alfabe (OCR gürültüsü)";
const U kNoLetters = U"harf yok";
const U kLineNoise = U"çizgi gürültüsü";
const U kRepeated = U"tekrarlayan harf";
const U kAddress = U"adres veya dosya adı";
const U kSymbols = U"sembol veya rakam ağırlıklı";
const U kFragments = U"parçalı OCR gürültüsü";
const U kNonsenseLetters = U"anlamsız harf dizisi";
const U kNonsenseWords = U"anlamsız kelimeler";
const U kNonsenseWord = U"anlamsız kelime";
const U kUntranslated = U"çevrilmemiş";

// _LINE_DROP: a single OCR box that fails for one of these is dropped. "çok kısa" and
// "anlamsız kelime" (1–2 words) are not: a subtitle can split across two boxes.
bool dropsLine(const U& reason)
{
    for (const U* r : {&kForeign, &kNoLetters, &kLineNoise, &kRepeated, &kAddress, &kSymbols, &kFragments,
                       &kNonsenseLetters, &kNonsenseWords})
        if (reason == *r) return true;
    return false;
}

// _short_noise: non-words of one or two letters are more than 40% of the line.
bool shortNoise(UV line)
{
    const auto toks = letterRuns(line);
    if (toks.size() < 4) return false;
    size_t junk = 0;
    for (const U& t : toks) {
        const U lc = lower(t);
        if (lc.size() <= 2 && !realShort().count(lc)) ++junk;
    }
    return static_cast<double>(junk) / static_cast<double>(toks.size()) > 0.4;
}

// _strip_glyphs: leading button glyphs ("Ya Ya …", "a a a …") and an all-caps speaker name.
U stripGlyphs(UV line)
{
    std::vector<U> toks = split(line, U' ');
    size_t i = 0;
    while (i < toks.size() && toks[i].size() <= 2 && strIsAlpha(toks[i])) ++i;
    std::vector<U> run;
    for (size_t k = 0; k < i; ++k) run.push_back(lower(toks[k]));
    bool glyph = false;
    if (!run.empty()) {
        const std::set<U> distinct(run.begin(), run.end());
        glyph = distinct.size() == 1 && (isGlyph(run[0]) || !realShort().count(run[0]));
    }
    if (i >= 2) {
        const auto notReal = std::count_if(run.begin(), run.end(), [](const U& t) { return !realShort().count(t); });
        if (glyph || static_cast<double>(notReal) > static_cast<double>(run.size()) / 2)
            toks.erase(toks.begin(), toks.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (toks.size() >= 2 && strIsAlpha(toks[0]) && strIsUpper(toks[0]) && toks[0].size() >= 3 && toks[1].size() >= 2
        && toks[1][0] >= U'A' && toks[1][0] <= U'Z' && toks[1][1] >= U'a' && toks[1][1] <= U'z')
        toks.erase(toks.begin());
    return join(toks, U" ");
}

// _is_hud: all caps, at most six words, no sentence end.
bool isHud(UV text)
{
    if (hasLower(text)) return false;
    const size_t words = wordCount(text);
    if (words == 0 || words > 6) return false;
    for (const char32_t c : text)
        if (c == U'.' || c == U'!' || c == U'?' || c == U'…') return false;
    return true;
}

// is_valid_dialogue_text
bool isValidDialogue(UV text)
{
    const U cleaned = join(split(text), U" ");
    if (cleaned.size() < 3) return false;
    if (cleaned.find(U' ') == U::npos && cleaned.size() > 28) return false;
    size_t nonSpace = 0, letters = 0;
    for (const char32_t c : cleaned) {
        if (isSpace(c)) continue;
        ++nonSpace;
        if (isAlpha(c)) ++letters;
    }
    if (nonSpace == 0 || letters < 2 || static_cast<double>(letters) / static_cast<double>(nonSpace) < 0.60)
        return false;
    return !systemLines().count(strip(lower(cleaned), U" .:"));
}

} // namespace

// ── normalization ───────────────────────────────────────────────────────────

std::u32string normalizeDialogue(std::u32string_view text)
{
    const U s = lower(strip(text, kWs));
    U collapsed;
    collapsed.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (in(kWs, s[i])) {
            collapsed += U' ';
            while (i < s.size() && in(kWs, s[i])) ++i;
        } else {
            collapsed += s[i++];
        }
    }
    size_t b = 0, e = collapsed.size();
    while (b < e && in(kLeading, collapsed[b])) ++b;
    while (e > b && in(kTrailing, collapsed[e - 1])) --e;
    return strip(UV(collapsed).substr(b, e - b), kWs);
}

std::string normalizeDialogue(std::string_view utf8)
{
    return toUtf8(normalizeDialogue(fromUtf8(utf8)));
}

std::u32string compactPoolKey(std::u32string_view text)
{
    U out;
    for (const char32_t c : normalizeDialogue(text))
        if (isAlpha(c) || isNumber(c)) out += c;
    return out;
}

std::string compactPoolKey(std::string_view utf8)
{
    return toUtf8(compactPoolKey(fromUtf8(utf8)));
}

std::string slugify(std::string_view name)
{
    // re.sub(r"[^a-z0-9]+", "-", re.sub(r"['’`]", "", text.lower())).strip("-")[:100] or "game"
    const U lc = lower(fromUtf8(name));
    std::string slug;
    bool dash = false;
    for (const char32_t c : lc) {
        if (c == U'\'' || c == U'’' || c == U'`') continue;
        if ((c >= U'a' && c <= U'z') || (c >= U'0' && c <= U'9')) {
            if (dash && !slug.empty()) slug += '-';
            dash = false;
            slug += static_cast<char>(c);
        } else {
            dash = true;
        }
    }
    if (slug.size() > 100) slug.resize(100);
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    return slug.empty() ? "game" : slug;
}

std::string gameSlug(std::string_view name)
{
    // The harness pins these slugs (game_detector.KNOWN_GAMES); the server already has pools under them
    static const std::pair<const char*, const char*> pins[] = {
        {"clair-obscur-expedition-33", "expedition-33"},
        {"grand-theft-auto-v", "gta-5"},
        {"s-t-a-l-k-e-r-2-heart-of-chornobyl", "stalker-2"},
        {"skyrim-special-edition", "skyrim"},
        {"the-elder-scrolls-v-skyrim", "skyrim"},
        {"the-last-of-us-part-i", "the-last-of-us"},
        {"the-witcher-3-wild-hunt", "witcher-3"},
    };
    const std::string slug = slugify(name);
    for (const auto& [from, to] : pins)
        if (slug == from) return to;
    return slug;
}

// ── Garbage ─────────────────────────────────────────────────────────────────

bool Garbage::setWords(std::string_view content)
{
    m_rank.clear();
    const auto words = split(fromUtf8(content));
    for (size_t i = 0; i < words.size(); ++i)
        if (i < 30000 || words[i].find(U'\'') != U::npos) m_rank.emplace(words[i], static_cast<int>(i));
    return loaded();
}

int Garbage::rank(std::u32string_view word) const
{
    const auto it = m_rank.find(U(word));
    return it == m_rank.end() ? 1000000000 : it->second;
}

// _splits_into_words: "goingto" → going + to, every piece a common word.
bool Garbage::splitsIntoWords(std::u32string_view lc) const
{
    const size_t n = lc.size();
    if (n < 3 || n > 40) return false;
    for (const char32_t c : lc)
        if (c < U'a' || c > U'z') return false;
    std::vector<bool> ok(n + 1, false);
    ok[0] = true;
    for (size_t i = 1; i <= n; ++i) {
        for (size_t k = i > 20 ? i - 20 : 0; k < i; ++k) {
            if (ok[i]) break;
            if (!ok[k]) continue;
            const UV piece = lc.substr(k, i - k);
            if (piece.size() >= 3 ? rank(piece) < 10000 : splitShort().count(U(piece)) > 0) ok[i] = true;
        }
    }
    return ok[n];
}

// english_word_score: share of real English words; an unknown capitalised word counts 0.3 (a name).
double Garbage::englishScore(std::u32string_view text, int& count) const
{
    double score = 0.0;
    count = 0;
    size_t i = 0;
    while (i < text.size()) {   // [A-Za-z]+(?:'[A-Za-z]+)?
        if (!isAsciiLetter(text[i])) {
            ++i;
            continue;
        }
        const size_t b = i;
        while (i < text.size() && isAsciiLetter(text[i])) ++i;
        if (i + 1 < text.size() && text[i] == U'\'' && isAsciiLetter(text[i + 1])) {
            ++i;
            while (i < text.size() && isAsciiLetter(text[i])) ++i;
        }
        const UV tok = text.substr(b, i - b);
        if (tok.size() < 2) continue;
        ++count;
        const U lc = asciiLower(tok);
        if (lc.size() == 2) {
            if (realShort().count(lc) || isInterjection(lc)) score += 1;
            continue;
        }
        const size_t apos = lc.find(U'\'');
        if (rank(lc) < 1000000000 || (apos != U::npos && rank(UV(lc).substr(0, apos)) < 1000000000)
            || isInterjection(lc) || splitsIntoWords(lc)) {
            score += 1;
        } else if (tok.size() >= 3 && tok[0] >= U'A' && tok[0] <= U'Z'
                   && std::all_of(tok.begin() + 1, tok.end(), [](char32_t c) { return c >= U'a' && c <= U'z'; })) {
            score += 0.3;
        }
    }
    return count ? score / count : 1.0;
}

std::u32string Garbage::reasonOf(std::u32string_view source, std::u32string_view translation,
                                 std::string_view sourceLang) const
{
    const U src = strip(source);
    const U norm = normalizeDialogue(src);
    std::string lang(sourceLang);
    for (auto& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (latinSourceLangs().count(lang) && std::any_of(src.begin(), src.end(), isCjk)) return kForeign;
    if (norm.size() < 3) return kTooShort;
    const size_t letters = static_cast<size_t>(std::count_if(norm.begin(), norm.end(), isAlpha));
    if (letters < 2) return kNoLetters;

    // re.fullmatch(r"[il1|!\s.,]+", norm) and len(re.findall(r"[il1|]", norm)) >= 4
    {
        bool all = true;
        size_t strokes = 0;
        for (const char32_t c : norm) {
            if (c == U'i' || c == U'l' || c == U'1' || c == U'|') ++strokes;
            else if (!(c == U'!' || c == U'.' || c == U',' || isSpace(c))) all = false;
        }
        if (all && strokes >= 4) return kLineNoise;
    }

    // ws = words of re.split(r"[^\w]+|[\d_]+", norm) made only of letters
    std::vector<U> ws;
    {
        size_t i = 0;
        while (i < norm.size()) {
            while (i < norm.size() && !isWordLetter(norm[i])) ++i;
            const size_t b = i;
            while (i < norm.size() && isWordLetter(norm[i])) ++i;
            if (i > b) {
                U w = norm.substr(b, i - b);
                if (std::all_of(w.begin(), w.end(), isAlpha)) ws.push_back(std::move(w));
            }
        }
    }
    const bool allInterjections =
        !ws.empty() && std::all_of(ws.begin(), ws.end(), [](const U& w) { return isInterjection(w); });
    if (!allInterjections) {
        bool tripled = false;   // re.search(r"([^\W\d_])\1{2,}", norm)
        for (size_t i = 0; i + 2 < norm.size() && !tripled; ++i)
            tripled = isWordLetter(norm[i]) && norm[i + 1] == norm[i] && norm[i + 2] == norm[i];
        if (tripled) {
            std::set<char32_t> distinct;
            for (const char32_t c : norm)
                if (isAlpha(c)) distinct.insert(c);
            if (distinct.size() < 4) return kRepeated;
        }
    }
    if (hasAddress(src)) return kAddress;

    const size_t visible = static_cast<size_t>(std::count_if(norm.begin(), norm.end(), [](char32_t c) { return !isSpace(c); }));
    if (visible > 0 && static_cast<double>(letters) / static_cast<double>(visible) < 0.6) return kSymbols;

    std::vector<U> tokens;
    for (U& t : split(norm, U' '))
        if (!t.empty()) tokens.push_back(std::move(t));
    const size_t single = static_cast<size_t>(std::count_if(tokens.begin(), tokens.end(), [](const U& t) { return t.size() == 1; }));
    if (tokens.size() >= 3 && static_cast<double>(single) / static_cast<double>(tokens.size()) >= 0.6) return kFragments;

    if (!norm.empty() && std::all_of(norm.begin(), norm.end(), [](char32_t c) { return c >= 0x20 && c <= 0x7E; })) {
        size_t longCount = 0, noVowel = 0;
        for (const U& t : tokens) {
            const U letters4 = onlyAsciiLower(t);
            if (letters4.size() < 4) continue;
            ++longCount;
            const bool vowel = std::any_of(t.begin(), t.end(), [](char32_t c) {
                return c == U'a' || c == U'e' || c == U'i' || c == U'o' || c == U'u' || c == U'y';
            });
            if (!vowel && !isInterjection(letters4)) ++noVowel;
        }
        if (longCount && static_cast<double>(noVowel) / static_cast<double>(longCount) > 0.5) return kNonsenseLetters;
    }
    if (lang == "en") {
        int n = 0;
        const double score = englishScore(src, n);
        if (n >= 3 && score < 0.5) return kNonsenseWords;
        if (n >= 1 && n <= 2 && score == 0) return kNonsenseWord;
    }
    if (!translation.empty() && compactPoolKey(translation) == compactPoolKey(src)) return kUntranslated;
    return {};
}

std::string Garbage::reason(std::string_view source, std::string_view translation, std::string_view sourceLang) const
{
    return toUtf8(reasonOf(fromUtf8(source), fromUtf8(translation), sourceLang));
}

std::string Garbage::filterOcr(std::string_view raw, std::string_view sourceLang) const
{
    std::vector<U> lines;
    for (const U& part : split(fromUtf8(raw), U'\n')) {
        const U ln = join(split(part), U" ");
        // Noise check first, glyph strip second: the other way round can turn noise into plausible text
        if (ln.empty() || shortNoise(ln) || dropsLine(reasonOf(ln, {}, sourceLang))) continue;
        U kept = stripGlyphs(ln);
        if (!kept.empty()) lines.push_back(std::move(kept));
    }
    if (lines.size() > 1 && std::any_of(lines.begin(), lines.end(), [](const U& l) { return hasLower(l); })) {
        // HUD text next to the subtitle: short lines without lower case, digit-heavy lines
        std::vector<U> kept;
        for (U& ln : lines) {
            if (!hasLower(ln) && wordCount(ln) <= 4) continue;
            const auto digits = std::count_if(ln.begin(), ln.end(), isDigit);
            const auto letters = std::count_if(ln.begin(), ln.end(), isAlpha);
            if (static_cast<double>(digits) < 0.3 * static_cast<double>(std::max<std::ptrdiff_t>(1, digits + letters)))
                kept.push_back(std::move(ln));
        }
        lines = std::move(kept);
    }
    const U text = join(lines, U" ");
    if (text.empty() || isHud(text) || !isValidDialogue(text) || !reasonOf(text, {}, sourceLang).empty()) return {};
    return toUtf8(text);
}

} // namespace live::text

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// Port of Makine-WEB src/live/scene.ts; each regular expression is spelled out next to the original.

#include "scene.h"

#include "garbage.h"
#include "unitext.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace live::scene {

namespace {

using U = std::u32string;
using UV = std::u32string_view;
using json = nlohmann::json;

constexpr size_t kMaxSpeaker = 40;
constexpr size_t kMaxId = 64;

U cut(UV s, size_t n) { return U(s.substr(0, std::min(n, s.size()))); }

bool endsWith(UV s, UV tail) { return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail; }

std::string name(const json& v)
{
    return v.is_string() ? text::toUtf8(cut(text::fromUtf8(plain(v.get<std::string>())), kMaxSpeaker)) : std::string();
}

std::string idOf(const json& v)
{
    std::string s;
    if (v.is_string()) s = v.get<std::string>();
    else if (v.is_number_integer()) s = std::to_string(v.get<int64_t>());
    else if (v.is_number()) s = v.dump();
    else return {};
    return text::toUtf8(cut(text::fromUtf8(plain(s)), kMaxId));
}

} // namespace

std::string plain(std::string_view textUtf8)
{
    // control characters, U+007F-009F, line/paragraph separators, zero-width and direction marks -> ' ';
    // <markup> -> ' '; runs of blanks -> one
    U s = text::fromUtf8(textUtf8);
    for (char32_t& c : s)
        if (c <= 0x1f || (c >= 0x7f && c <= 0x9f) || c == 0x2028 || c == 0x2029 || (c >= 0x200b && c <= 0x200f)
            || (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069))
            c = U' ';
    U noTags;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == U'<') {
            const size_t close = s.find(U'>', i + 1);
            if (close != U::npos) {
                noTags += U' ';
                i = close;
                continue;
            }
        }
        noTags += s[i];
    }
    return text::toUtf8(text::join(text::split(noTags), U" "));
}

std::string speakerKey(std::string_view nameUtf8)
{
    return text::toUtf8(text::compactPoolKey(cut(text::fromUtf8(nameUtf8), kMaxSpeaker)));
}

bool isContextDependent(std::string_view lineUtf8)
{
    static const std::unordered_set<U> pronouns{
        U"i", U"me", U"my", U"mine", U"you", U"your", U"yours", U"he", U"him", U"his", U"she", U"her", U"hers", U"it",
        U"its", U"we", U"us", U"our", U"ours", U"they", U"them", U"their", U"theirs", U"this", U"that", U"these",
        U"those", U"who", U"what"};
    std::vector<U> words;
    for (const U& w : text::split(text::normalizeDialogue(text::fromUtf8(lineUtf8)), U' '))
        if (std::any_of(w.begin(), w.end(), [](char32_t c) { return text::isAlpha(c) || text::isNumber(c); }))
            words.push_back(w);
    if (words.empty()) return false;
    if (words.size() <= 3) return true;
    if (std::all_of(words.begin(), words.end(), [](const U& w) {
            U letters;
            for (const char32_t c : w)
                if (text::isAlpha(c)) letters += c;
            return pronouns.count(letters) > 0;
        }))
        return true;
    // /\?["”'’)]*$/ on the trimmed line
    const U t = text::strip(text::fromUtf8(lineUtf8));
    size_t e = t.size();
    while (e > 0 && (t[e - 1] == U'"' || t[e - 1] == U'”' || t[e - 1] == U'\'' || t[e - 1] == U'’' || t[e - 1] == U')')) --e;
    return e > 0 && t[e - 1] == U'?' && words.size() < 5;
}

std::string addressForm(std::string_view turkish)
{
    static const std::unordered_set<U> sizWords{U"siz", U"sizi", U"size", U"sizin", U"sizden", U"sizde", U"sizinle", U"sizce", U"sizler"};
    static const std::unordered_set<U> senWords{U"sen", U"seni", U"sana", U"senin", U"senden", U"sende", U"seninle", U"sence"};
    // "-siz" alone is "without" (sessiz) and "-sin" alone often the third person (gelsin): only long endings count
    static const UV sizEnd[] = {U"sınız", U"siniz", U"sunuz", U"sünüz", U"dınız", U"diniz", U"dunuz", U"dünüz",
                                U"tınız", U"tiniz", U"tunuz", U"tünüz"};
    static const UV senEnd[] = {U"mısın", U"misin", U"musun", U"müsün", U"yorsun", U"ırsın", U"irsin", U"ursun", U"ürsün",
                                U"arsın", U"ersin", U"acaksın", U"eceksin", U"malısın", U"melisin", U"abilirsin", U"ebilirsin"};
    // toLocaleLowerCase('tr-TR'): I → ı, İ → i
    U t;
    for (const char32_t c : text::fromUtf8(turkish)) {
        if (c == U'I') t += U'ı';
        else if (c == U'İ') t += U'i';
        else t += text::lower(U(1, c));
    }
    bool sen = false, siz = false;
    size_t i = 0;
    while (i < t.size()) {
        if (!text::isAlpha(t[i])) {
            ++i;
            continue;
        }
        size_t e = i;
        while (e < t.size() && text::isAlpha(t[e])) ++e;
        const UV w = UV(t).substr(i, e - i);
        i = e;
        const auto ends = [&](const auto& list) {
            return std::any_of(std::begin(list), std::end(list), [&](UV tail) { return endsWith(w, tail); });
        };
        if (sizWords.count(U(w)) || ends(sizEnd)) siz = true;
        else if (senWords.count(U(w)) || ends(senEnd)) sen = true;
    }
    return sen == siz ? std::string() : sen ? "sen" : "siz";
}

Scene readScene(const json& inp, const std::string& labelSpeaker, const text::Garbage& garbage)
{
    Scene s;
    const auto field = [&](const char* k) { return inp.is_object() && inp.contains(k) ? inp[k] : json(); };
    s.speaker = !labelSpeaker.empty() ? labelSpeaker : name(field("speaker"));
    s.sceneId = idOf(field("scene_id"));
    s.scriptLineId = idOf(field("script_line_id"));
    struct Line {
        std::string line, speaker, translated;
        bool fresh = false;
    };
    std::vector<Line> lines;
    const json context = field("context");
    if (context.is_array()) {
        const size_t from = context.size() > kMaxContext ? context.size() - kMaxContext : 0;
        for (size_t k = from; k < context.size(); ++k) {
            const json& o = context[k];
            if (!o.is_object() || !o.contains("text") || !o["text"].is_string()) continue;
            const std::string textPlain = plain(o["text"].get<std::string>());
            if (textPlain.empty() || text::fromUtf8(textPlain).size() > kMaxContextLine) continue;   // dropped, not cut
            const text::ReadLine r = garbage.readLine(textPlain);
            if (r.line.empty() || !garbage.reason(r.line).empty()) continue;
            // missing, negative, non-finite or older than 8 s: stale
            const double age = o.contains("age_ms") && o["age_ms"].is_number() && std::isfinite(o["age_ms"].get<double>())
                                   ? o["age_ms"].get<double>()
                                   : -1;
            const std::string itemScene = o.contains("scene_id") ? idOf(o["scene_id"]) : std::string();
            const bool sameScene = s.sceneId.empty() || itemScene.empty() || itemScene == s.sceneId;
            const std::string translated = o.contains("translated") && o["translated"].is_string()
                                               ? text::toUtf8(cut(text::fromUtf8(plain(o["translated"].get<std::string>())), kMaxContextLine))
                                               : std::string();
            lines.push_back({r.line, !r.speaker.empty() ? r.speaker : name(o.contains("speaker") ? o["speaker"] : json()),
                             translated, age >= 0 && age <= kContextFreshMs && sameScene});
        }
    }
    if (!s.speaker.empty())
        for (const Line& l : lines) {
            const std::string f = l.fresh && speakerKey(l.speaker) == speakerKey(s.speaker) ? addressForm(l.translated) : "";
            if (!f.empty()) s.lastForm = f;
        }
    if (!lines.empty() && lines.back().fresh) {
        s.prev = SceneLine{lines.back().line, lines.back().speaker};
        s.prevSpeaker = lines.back().speaker;
    }
    return s;
}

std::string contextText(const SceneLine& prev)
{
    return (prev.speaker.empty() ? std::string() : prev.speaker + ": ") + prev.line;
}

std::string listenerOf(const Scene& scene)
{
    const std::string& p = scene.prevSpeaker;
    return !p.empty() && speakerKey(p) != speakerKey(scene.speaker) ? p : std::string();
}

std::string ruleForm(const std::vector<Relation>& rules, const std::string& speaker, const std::string& listener)
{
    const std::string s = speakerKey(speaker);
    if (s.empty()) return {};
    const std::string l = speakerKey(listener);
    const Relation* hit = nullptr;
    if (!l.empty())
        for (const Relation& r : rules)
            if (r.speakerKey == s && r.listenerKey == l) {
                hit = &r;
                break;
            }
    if (!hit)
        for (const Relation& r : rules)
            if (r.speakerKey == s && r.listenerKey == "*") {
                hit = &r;
                break;
            }
    return hit && (hit->form == "sen" || hit->form == "siz") ? hit->form : std::string();
}

std::vector<std::string> variantSignals(const Scene& scene, const std::string& form)
{
    std::vector<std::string> out;
    if (!scene.scriptLineId.empty()) out.push_back("line:" + scene.scriptLineId);
    if (!scene.sceneId.empty()) out.push_back("scene:" + scene.sceneId);
    if (!form.empty()) out.push_back("f:" + form);
    return out;
}

} // namespace live::scene

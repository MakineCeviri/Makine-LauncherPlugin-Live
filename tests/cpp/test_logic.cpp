// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// C++ ports of the Python harness tests: the text rules must give the same answers as
// makine_live (and the server) or garbage reaches the shared pool.
//   live_logic_test <case> <repo root>
// Cases: normalization, garbage, ocr_filter, subtitle, console, local_pool (ctest runs each).

#include "console_mode.h"
#include "garbage.h"
#include "local_pool.h"
#include "subtitle_timing.h"
#include "unitext.h"
#include "winutil.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;
using namespace live;

namespace {

int g_fails = 0;
std::wstring g_root;

void check(bool cond, const std::string& label)
{
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", label.c_str());
    if (!cond) ++g_fails;
}

json loadJson(const wchar_t* rel)
{
    const auto text = win::readFile(g_root + rel);
    if (!text) {
        check(false, "cannot read " + win::wideToUtf8(rel));
        return json::array();
    }
    return json::parse(*text);
}

text::Garbage& garbage()
{
    static text::Garbage g = [] {
        text::Garbage x;
        const auto words = win::readFile(g_root + L"\\resources\\en_words.txt");
        check(words && x.setWords(*words), "en_words.txt loaded");
        return x;
    }();
    return g;
}

std::u32string U(const std::string& s) { return text::fromUtf8(s); }

// ── spec/normalization_vectors.json ─────────────────────────────────────────

void testNormalization()
{
    const json vectors = loadJson(L"\\spec\\normalization_vectors.json");
    check(vectors.size() >= 15, "at least 15 vectors");
    for (const json& v : vectors) {
        const std::string in = v["input"], expected = v["expected"];
        const std::string got = text::normalizeDialogue(in);
        check(got == expected, "normalize " + json(in).dump() + " -> " + json(got).dump());
        check(text::normalizeDialogue(expected) == expected, "idempotent " + json(expected).dump());
    }
    check(text::compactPoolKey(std::string("Shaun?I")) == text::compactPoolKey(std::string("Shaun. I")),
          "pool key ignores punctuation and spaces");
    check(text::compactPoolKey(std::string("Iwas going")) == "iwasgoing", "pool key = letters and digits");
    check(text::slugify("Grand Theft Auto VI") == console::kGameSlug, "console slug = slugify(GTA VI)");
    check(text::slugify("Baldur's Gate 3") == "baldurs-gate-3", "slugify drops apostrophes");
    check(text::gameSlug("Grand Theft Auto V") == "gta-5" && text::gameSlug("The Witcher 3: Wild Hunt") == "witcher-3"
              && text::gameSlug("Elden Ring") == "elden-ring",
          "game slugs pinned as the harness pins them");
}

// ── spec/garbage_vectors.json ───────────────────────────────────────────────

void testGarbage()
{
    const json vectors = loadJson(L"\\spec\\garbage_vectors.json");
    for (const json& v : vectors) {
        const std::string src = v["source"];
        const std::string reason =
            garbage().reason(src, v.value("translation", std::string()), v.value("source_lang", std::string("en")));
        const bool expected = v["garbage"];
        check(!reason.empty() == expected, json(src).dump() + " -> " + (reason.empty() ? "ok" : reason) + " ("
                                               + v.value("note", std::string()) + ")");
    }
}

// ── subtitle shape: the server's vectors (Makine-WEB tests/fixtures/live, copied to spec/) ──

void testSpeaker()
{
    for (const json& v : loadJson(L"\\spec\\speaker_vectors.json")) {
        const text::LineParts p = text::splitSpeaker(v["input"].get<std::string>());
        check(p.speaker == v["speaker"].get<std::string>() && p.note == v["note"].get<std::string>()
                  && p.text == v["text"].get<std::string>(),
              v["input"].dump() + " -> " + json({p.speaker, p.note, p.text}).dump());
    }
    // every speaker reads one line, one key; a label whose line alone is too short stays whole
    check(text::compactPoolKey(garbage().readLine("Jason: What? You okay?").line)
              == text::compactPoolKey(garbage().readLine("Lucia: What? You okay?").line),
          "one key for every speaker");
    check(garbage().readLine("Jason: No.").line == "Jason: No.", "a too short line keeps its label");
    check(garbage().readLine("Mia: In Spanish: Oh, shit!").prefix == "Mia: (İspanyolca) ", "label and note prefix");
    const auto lines = garbage().speakerLines("Cop: Stop right now, or I'll stop you! Cop: I want the engine off!");
    check(lines == std::vector<std::string>{"Cop: Stop right now, or I'll stop you!", "Cop: I want the engine off!"},
          "a two-speaker block splits at the second label");
}

void testLineIssues()
{
    for (const json& v : loadJson(L"\\spec\\line_issue_vectors.json")) {
        const auto got = garbage().lineIssues(text::splitSpeaker(v["text"].get<std::string>()).text);
        check(json(got) == v["issues"], v["text"].dump() + " -> " + json(got).dump());
    }
    check(garbage().readLine("ook what we have here!").hard, "a clipped line is hard");
    check(!garbage().readLine("Will do. Thanks, Kenton. Kenton").hard, "a trailing name is not");
    check(garbage().isEnglishWord("fuckin'") && !garbage().isEnglishWord("ook"), "isEnglishWord");
}

void testClippedEdge()
{
    const json doc = loadJson(L"\\spec\\clipped_edge_vectors.json");
    for (const json& c : doc["cases"]) {
        std::vector<text::OcrBox> boxes;
        for (const json& b : c["boxes"])
            boxes.push_back({b["text"].get<std::string>(), b["x0"].get<int>(), b["y0"].get<int>(), b["x1"].get<int>(),
                             b["y1"].get<int>()});
        const auto v = garbage().edgeCheck(boxes, c["region"]["width"].get<int>(), c["region"]["height"].get<int>());
        const json& e = c["expect"];
        bool ok = v.send == e["send"].get<bool>() && json(v.cut) == e["cut"] && json(v.widen) == e["widen"];
        if (e.contains("lines")) ok = ok && json(v.lines) == e["lines"];
        check(ok, c["name"].get<std::string>() + " -> send " + std::to_string(v.send) + " cut " + json(v.cut).dump()
                      + " lines " + json(v.lines).dump());
    }
}

// ── tests/test_garbage.py OcrFilter ─────────────────────────────────────────

void testOcrFilter()
{
    const auto kept = [](const std::string& raw, const std::string& expected) {
        const std::string got = garbage().filterOcr(raw);
        check(got == expected, "kept " + json(raw).dump() + " -> " + json(got).dump());
    };
    const auto dropped = [](const std::string& raw) {
        const std::string got = garbage().filterOcr(raw);
        check(got.empty(), "dropped " + json(raw).dump() + (got.empty() ? "" : " -> " + json(got).dump()));
    };
    // dialogue passes
    kept("Hey! Get out the damn road!", "Hey! Get out the damn road!");
    kept("Oh, shit. holy shit!\nWhat the actual fuck?", "Oh, shit. holy shit! What the actual fuck?");
    kept("Uh.. is that a shark?", "Uh.. is that a shark?");
    kept("Aaaah! Help me!", "Aaaah! Help me!");
    kept("I was going to\ngo.", "I was going to go.");
    // game HUD is not sent
    for (const char* raw : {"RES", "RES 0", "SECURITY TRACKER", "ACCESS SECURIT TRACKER", "STRENGTH RATING",
                            "VAPID JASON'S '70 G AN A DO", "ERTIBLE 3:06pm", "KR 0", "OT9"})
        dropped(raw);
    // HUD next to a subtitle is removed
    kept("VAPID JASON'S '70\nCome over here, silly!", "Come over here, silly!");
    kept("5:30 pm\nHey! Ah, c'mere, I got something for you.", "Hey! Ah, c'mere, I got something for you.");
    kept("VRAM Budget 4672 MB / 7003 MB\nYou must be busy with all of this.", "You must be busy with all of this.");
    // button glyphs and speaker names
    kept("Ya Ya You must be busy with it all.", "You must be busy with it all.");
    kept("a a a Don't let me keep you.", "Don't let me keep you.");
    kept("TOMIKO Listen to me carefully.", "Listen to me carefully.");
    kept("TOMIKO\nListen to me carefully.", "Listen to me carefully.");
    kept("It is unfortunate when a man dies.", "It is unfortunate when a man dies.");
    kept("Oh oh, we're in trouble now.", "Oh oh, we're in trouble now.");
    // OCR noise is not sent
    for (const char* raw : {"ee id n na do rs error a ra", "ikpuew oas on er ay non ue", "a os se ep on o a sr og ed",
                            "e e s e s a a se aai se", "wd 00:9 ee re e se", "Oyuna Devam Et",
                            "an no re ea a s na r areas", "o i ae ya one re e"})
        dropped(raw);
    // a noise line does not spoil the dialogue
    kept("ee id n na do rs error a ra\nWait, a bomb- what bomb?", "Wait, a bomb- what bomb?");
}

// ── tests/test_subtitle_scheduler.py ────────────────────────────────────────

double r3(double t) { return std::nearbyint(t * 1000.0) / 1000.0; }

using Timeline = std::vector<std::pair<double, std::optional<int>>>;

Timeline run(SubtitleScheduler& s, double t0, double t1, double step = 0.05)
{
    Timeline out;
    for (double t = t0; t <= t1 + 1e-9; t += step) {
        const auto d = s.tick(t);
        out.emplace_back(r3(t), d ? std::optional(d->id) : std::nullopt);
    }
    return out;
}

std::optional<std::pair<double, double>> shown(const Timeline& tl, int id)
{
    std::optional<std::pair<double, double>> r;
    for (const auto& [t, i] : tl)
        if (i && *i == id) r = r ? std::pair(r->first, t) : std::pair(t, t);
    return r;
}

std::vector<std::pair<double, LineEvent>> feed(LineTracker& tr, const std::vector<std::u32string>& seq)
{
    std::vector<std::pair<double, LineEvent>> events;
    double t = 0.0;
    for (const auto& text : seq) {
        for (auto& e : tr.update(text, t)) events.emplace_back(std::nearbyint(t * 100.0) / 100.0, e);
        t += 0.1;
    }
    return events;
}

int countKind(const std::vector<std::pair<double, LineEvent>>& ev, LineEvent::Kind k)
{
    int n = 0;
    for (const auto& [t, e] : ev) n += e.kind == k;
    return n;
}

std::vector<std::u32string> repeat(const std::u32string& s, int n) { return std::vector<std::u32string>(n, s); }

void testSubtitle()
{
    {   // late translation stays as long as the source was visible
        SubtitleScheduler s;
        s.sourceNew(1, 0.0);
        s.sourceGone(1, 4.0);
        s.translationReady(1, U("Seni bulacağım."), "api", 4.6);
        const auto iv = shown(run(s, 4.6, 12.0), 1);
        check(iv && std::abs(iv->first - 4.6) < 0.005 && iv->second - iv->first >= 4.0 - 0.06
                  && iv->second - iv->first < 4.2,
              "late translation stays as long as the source was visible");
    }
    {   // reading minimum for a short flash
        SubtitleScheduler s;
        s.sourceNew(1, 0.0);
        s.sourceGone(1, 0.3);
        const auto text = U("Jason'a seslenmek için X'e bas.");
        s.translationReady(1, text, "api", 0.9);
        const auto iv = shown(run(s, 0.9, 6.0), 1);
        check(iv && iv->second - iv->first >= readingMin(text) - 0.06, "reading minimum for a short flash");
    }
    {   // "En kısa gösterim" (style minShowMs → minBase): a one-word flash stays the chosen time
        for (const double minShow : {0.6, 3.0}) {
            SubtitleScheduler s;
            s.minBase = minShow;
            s.sourceNew(1, 0.0);
            s.sourceGone(1, 0.1);
            s.translationReady(1, U("Evet."), "api", 0.2);
            const auto iv = shown(run(s, 0.2, 8.0), 1);
            check(iv && iv->second - iv->first >= minShow - 0.06 && (minShow > 1 || iv->second - iv->first < 1.2),
                  "minShowMs " + std::to_string(static_cast<int>(minShow * 1000)) + " ms holds a short line that long");
        }
    }
    {   // stays while the source is visible
        SubtitleScheduler s;
        s.sourceNew(1, 0.0);
        s.translationReady(1, U("Uzun süre ekranda kalan satır."), "verified", 0.4);
        bool all = true;
        for (const auto& [t, i] : run(s, 0.4, 20.0)) all = all && i && *i == 1;
        check(all, "stays while the source is visible");
    }
    {   // next line waits for the current minimum
        SubtitleScheduler s;
        s.sourceNew(1, 0.0);
        s.sourceGone(1, 3.0);
        s.translationReady(1, U("Birinci satır."), "api", 1.0);
        s.sourceNew(2, 3.0);
        s.sourceGone(2, 5.0);
        s.translationReady(2, U("İkinci satır."), "api", 3.3);
        const auto tl = run(s, 1.0, 10.0);
        const auto a = shown(tl, 1), b = shown(tl, 2);
        check(a && b && a->second - a->first >= 3.0 - 0.06 && b->first >= a->second
                  && b->second - b->first >= 2.0 - 0.06,
              "next line waits for the current minimum");
    }
    {   // backlog catches up
        SubtitleScheduler s;
        for (int i = 0; i < 4; ++i) {
            const double t = i * 2.5;
            s.sourceNew(i + 1, t);
            s.sourceGone(i + 1, t + 2.5);
            s.translationReady(i + 1, U("Satır " + std::to_string(i + 1) + " çevirisi biraz uzun bir cümle."), "api",
                               t + 1.2);
        }
        const auto tl = run(s, 1.2, 25.0);
        bool all = true, floorOk = true;
        const double floor = std::max(1.0, 0.7 * readingMin(U("Satır 1 çevirisi biraz uzun bir cümle.")));
        double lastStart = 1e9;
        for (int i = 0; i < 4; ++i) {
            const auto iv = shown(tl, i + 1);
            all = all && iv;
            if (iv) floorOk = floorOk && iv->second - iv->first >= floor - 0.06;
            lastStart = iv ? iv->first : 1e9;
        }
        check(all && lastStart - (7.5 + 2.5) < 3.0 && floorOk, "backlog catches up");
    }
    {   // rapid burst stays in sync
        SubtitleScheduler s;
        for (int i = 0; i < 4; ++i) {
            const double t = 1.0 + i * 0.5;
            s.sourceNew(i + 1, t);
            s.sourceGone(i + 1, t + 0.5);
            s.translationReady(i + 1, U("Kısa satır " + std::to_string(i + 1) + ", cevap ver!"), "api",
                               t + 0.6 + 0.1 * i);
        }
        const auto last = shown(run(s, 1.6, 15.0), 4);
        check(last && last->first - 3.0 < 4.0, "rapid burst: newest line shown in time");
    }
    {   // stale line skipped when a newer one is ready
        SubtitleScheduler s;
        s.sourceNew(1, 0.0);
        s.sourceGone(1, 1.0);
        s.translationReady(1, U("A satırı."), "api", 0.5);
        s.tick(0.5);
        s.sourceNew(2, 1.0);
        s.sourceGone(2, 1.4);
        s.translationReady(2, U("B satırı."), "api", 1.5);
        s.sourceNew(3, 1.4);
        s.translationReady(3, U("C satırı."), "api", 1.6);
        check(shown(run(s, 0.5, 12.0), 3).has_value(), "stale line skipped, newest shown");
    }
    {   // older pending translation keeps the order
        SubtitleScheduler s;
        s.sourceNew(1, 0.0);
        s.sourceGone(1, 0.8);
        s.sourceNew(2, 0.8);
        s.translationReady(2, U("İkinci (havuzdan hızlı)."), "verified", 0.9);
        const bool waited = !s.tick(1.0);
        s.translationReady(1, U("Birinci (API)."), "api", 1.3);
        const auto d = s.tick(1.35);
        check(waited && d && d->id == 1, "older pending translation keeps the order");
    }
    {   // fast scene does not accumulate delay
        SubtitleScheduler s;
        const auto text = U("Hey, yeah! Use the fucking gun, Jason, shoot it before it gets us!");
        for (int i = 0; i < 12; ++i) {
            s.sourceNew(i + 1, i * 2.5);
            s.sourceGone(i + 1, i * 2.5 + 2.5);
        }
        Timeline tl;
        double t = 0.0;
        while (t <= 40.0) {
            for (int i = 0; i < 12; ++i)
                if (std::abs(t - (i * 2.5 + 1.0)) < 1e-6) s.translationReady(i + 1, text, "api", t);
            const auto d = s.tick(t);
            tl.emplace_back(r3(t), d ? std::optional(d->id) : std::nullopt);
            t = r3(t + 0.05);
        }
        bool ok = true;
        for (int i = 0; i < 12; ++i) {
            const auto iv = shown(tl, i + 1);
            ok = ok && iv && iv->first - i * 2.5 <= 1.0 + 0.11;
        }
        check(ok, "fast scene does not accumulate delay");
    }
    {
        SubtitleScheduler s;
        s.sourceNew(1, 0.0);
        s.sourceGone(1, 1.0);
        s.translationReady(1, U("Çok geç gelen çeviri."), "api", 5.5);
        check(!s.tick(5.5), "a very late translation is not shown");
    }
    {
        SubtitleScheduler s;
        for (int i = 0; i < 50; ++i) {
            s.sourceNew(i + 1, i * 0.1);
            s.sourceGone(i + 1, i * 0.1 + 0.05);
        }
        s.tick(30.0);
        check(s.size() <= 1, "untranslated entries are forgotten");
    }
    {
        SubtitleScheduler s;
        s.sourceNew(1, 0.0);
        s.sourceGone(1, 0.5);
        s.sourceNew(2, 0.5);
        s.translationReady(2, U("İkinci."), "api", 0.9);
        s.translationFailed(1);
        const auto d = s.tick(1.0);
        check(d && d->id == 2, "a failed translation does not block");
    }

    // LineTracker
    {
        LineTracker tr;
        const auto ev = feed(tr, repeat(U("I was going to find you, Shaun."), 20));
        check(ev.size() == 2 && ev[0].second.kind == LineEvent::New && ev[1].second.kind == LineEvent::Dispatch
                  && ev[1].first <= 0.15,
              "stable line dispatched once, after ~2 rounds");
    }
    {
        LineTracker tr;
        const std::vector<std::u32string> variants{
            U("I was going to find you, Shaun. I promise. We have to leave now."),
            U("Iwas going to find you, Shaun.I promise. We have to leave now."),
            U("I was going to find yon, Shaun. I promise. We have to leave now."),
            U("I was goingto find you, Shaun. I promise. We have to leave now")};
        std::vector<std::u32string> seq;
        for (int i = 0; i < 40; ++i) seq.push_back(variants[i % 4]);
        const auto ev = feed(tr, seq);
        check(countKind(ev, LineEvent::New) == 1 && countKind(ev, LineEvent::Gone) == 0
                  && countKind(ev, LineEvent::Dispatch) == 1,
              "jitter and glued words are the same line");
    }
    {
        LineTracker tr;
        auto seq = repeat(U("Where were you last night?"), 5);
        seq.push_back(U(""));
        for (int i = 0; i < 5; ++i) seq.push_back(U("Where were you last night?"));
        check(countKind(feed(tr, seq), LineEvent::Gone) == 0, "a brief OCR dropout is not gone");
    }
    {
        LineTracker tr;
        auto seq = repeat(U("Where were you last night?"), 5);
        for (int i = 0; i < 3; ++i) seq.push_back(U(""));
        const auto ev = feed(tr, seq);
        int gone = 0;
        double lastSeen = -1;
        for (const auto& [t, e] : ev)
            if (e.kind == LineEvent::Gone) {
                ++gone;
                lastSeen = e.t;
            }
        check(gone == 1 && std::abs(lastSeen - 0.4) < 0.005, "gone after two misses, at the last seen time");
    }
    {
        LineTracker tr;
        const std::u32string full = U("Deviant located. Proceeding to intercept.");
        std::vector<std::u32string> seq;
        for (size_t n = 6; n <= full.size(); n += 6) seq.push_back(full.substr(0, n));
        for (int i = 0; i < 4; ++i) seq.push_back(full);
        const auto ev = feed(tr, seq);
        std::u32string lastDisp;
        for (const auto& [t, e] : ev)
            if (e.kind == LineEvent::Dispatch) lastDisp = e.text;
        check(lastDisp == full && countKind(ev, LineEvent::New) == 1, "typewriter dispatches the final text");
    }
    {
        LineTracker tr;
        const std::u32string full = U("You gotta help him, Jason! He's bleeding out and we need to move right now.");
        std::vector<std::u32string> seq;
        for (size_t n = 20; n <= full.size(); n += 2) seq.push_back(full.substr(0, n));
        for (int i = 0; i < 4; ++i) seq.push_back(full);
        std::vector<std::u32string> disp;
        for (const auto& [t, e] : feed(tr, seq))
            if (e.kind == LineEvent::Dispatch) disp.push_back(e.text);
        check(disp.size() == 1 && disp[0] == full, "slow typewriter is sent once complete");
    }
    {
        LineTracker tr;
        const std::u32string full =
            U("I dunno if you ever paid for it, but somebody did, and now they want it back from us tonight.");
        std::vector<std::u32string> seq;
        for (size_t n = 10; n <= full.size(); n += 2) seq.push_back(full.substr(0, n));
        for (int i = 0; i < 4; ++i) seq.push_back(full);
        std::vector<std::u32string> disp;
        for (const auto& [t, e] : feed(tr, seq))
            if (e.kind == LineEvent::Dispatch) disp.push_back(e.text);
        check(disp.size() >= 2 && disp.back() == full, "very slow typewriter: half first, the rest when done");
    }
    {
        LineTracker tr;
        const auto a = U("Hang in there, Dobbs. Help is coming, I promise you that.");
        const auto b = U("Hang in there, Dobbs. Help is coming, I promise you that");
        auto seq = repeat(a, 3);
        for (int i = 0; i < 8; ++i) {
            seq.push_back(b);
            seq.push_back(a);
        }
        check(countKind(feed(tr, seq), LineEvent::Dispatch) == 1, "the same text is not sent twice");
    }
    {
        LineTracker tr;
        auto seq = repeat(U("Press X to call Jason."), 4);
        for (int i = 0; i < 4; ++i) seq.push_back(U("Ethan, where are you?"));
        std::vector<LineEvent::Kind> kinds;
        for (const auto& [t, e] : feed(tr, seq)) kinds.push_back(e.kind);
        check(kinds == std::vector{LineEvent::New, LineEvent::Dispatch, LineEvent::Gone, LineEvent::New,
                                   LineEvent::Dispatch},
              "a new line replaces the old");
    }
    check(std::abs(text::sequenceRatio(U("abcd"), U("bcde")) - 0.75) < 1e-12, "SequenceMatcher ratio abcd/bcde");
    check(std::abs(text::sequenceRatio(U("qabxcd"), U("abycdf")) - 2.0 * 4 / 12) < 1e-12,
          "SequenceMatcher ratio (difflib doc example)");
}

// ── tests/test_console_mode.py (geometry and matching) ──────────────────────

void testConsole()
{
    using namespace console;
    check(videoRect({0, 0, 1920, 1080}) == Rect{0, 0, 1920, 1080}, "video rect exact");
    check(videoRect({100, 50, 1100, 850}) == Rect{100, 169, 1100, 731}, "video rect: bars above and below");
    const Rect wide = videoRect({0, 0, 2560, 1080});
    check(wide.t == 0 && wide.b == 1080 && wide.r - wide.l == 1920 && wide.l == 320, "video rect: bars at the sides");
    const Rect video{200, 100, 1480, 820};
    const Box rect = regionToScreen(video, kDefaultRegion);
    check(rect == Box{354, 604, 973, 187}, "region -> screen");
    const Frac back = screenToRegion(video, rect);
    check(std::abs(back.x - kDefaultRegion.x) < 0.005 && std::abs(back.y - kDefaultRegion.y) < 0.005
              && std::abs(back.w - kDefaultRegion.w) < 0.005 && std::abs(back.h - kDefaultRegion.h) < 0.005,
          "screen -> region round trip");
    const Frac clipped = screenToRegion({0, 0, 1000, 500}, {-50, 400, 300, 200});
    check(clipped.x == 0.0 && clipped.y == 0.8 && clipped.w == 0.25 && clipped.h == 0.2, "region clipped to video");
    const Frac f{0.1, 0.8, 0.8, 0.15};
    check(regionToScreen({0, 0, 1280, 720}, f) == Box{128, 576, 1024, 108}
              && regionToScreen({640, 360, 2560, 1440}, f) == Box{832, 1224, 1536, 162},
          "region follows the window");
    check(validRegion(kDefaultRegion) && !validRegion({0.5, 0.5, 0.6, 0.1}) && !validRegion({0, 0, 0, 0}),
          "valid region");

    SourceApp s;
    check(matchSource("RemotePlay.exe", "PS Remote Play", {}, s) && s.key == "ps-remote-play", "PS Remote Play");
    check(matchSource("XboxPcApp.exe", "Xbox", {}, s) && s.console == "xbox", "Xbox app");
    check(matchSource("4KCaptureUtility.exe", "", {}, s) && s.console == "capture", "Elgato");
    check(matchSource("Asobi.exe", "Asobi", {}, s) && s.key == "asobi", "Asobi");
    const struct {
        const char *exe, *title, *key;
    } wider[] = {
        {"chiaki-ng.exe", "chiaki-ng", "chiaki"},
        {"PXPlay.exe", "PXPlay", "pxplay"},
        {"XboxPcApp.exe", "Xbox Cloud Gaming", "xbox-app"},
        {"RECentral.exe", "RECentral 4", "avermedia-recentral"},
        {"AVerMedia Streaming Center.exe", "Streaming Center", "avermedia-streaming-center"},
        {"Elgato Studio.exe", "Elgato Studio", "elgato-studio"},
    };
    for (const auto& w : wider)
        check(matchSource(w.exe, w.title, {}, s) && s.key == w.key, std::string("source ") + w.exe + " -> " + w.key);
    for (const char* exe : {"Moonlight.exe", "parsecd.exe", "GeForceNOW.exe", "vlc.exe", "SteamLink.exe"})
        check(!matchSource(exe, "dshow:// capture", {}, s), std::string("dropped for now: ") + exe);
    check(matchSource("AsobiRemotePlay.exe", "", {}, s) && s.console == "ps5", "Asobi under another exe name");
    check(isConsoleStream("RemotePlay.exe") && isConsoleStream("AsobiRemotePlay.exe") && isConsoleStream("PS Remote Play")
              && !isConsoleStream("Elden Ring") && !isConsoleStream("obs64.exe"),
          "console stream apps count as GTA VI");
    check(matchSource("obs64.exe", "Windowed Projector (Preview)", {}, s), "OBS projector");
    check(matchSource("obs64.exe", "Pencereli Projektör (Önizleme)", {}, s), "OBS projector (Turkish)");
    check(!matchSource("obs64.exe", "OBS 31.0.0 - Profile: Untitled", {}, s), "OBS main window is not a source");
    check(matchSource("ApplicationFrameHost.exe", "Kamera", {}, s), "Windows Camera");
    check(!matchSource("ApplicationFrameHost.exe", "Ayarlar", {}, s), "Settings app is not a source");
    for (const char* exe : {"chrome.exe", "zen.exe", "discord.exe", "spotify.exe", ""})
        check(!matchSource(exe, "PS Remote Play", {}, s), std::string("not a source: ") + exe);
    check(matchSource("PotPlayerMini64.exe", "x", {"potplayermini64.exe"}, s) && s.key == "custom"
              && s.console == "custom",
          "custom source");
}

// ── tests/test_local_pool.py ────────────────────────────────────────────────

json row(int64_t id, const std::string& src, const std::string& tr)
{
    return json::array({id, text::compactPoolKey(src), src, tr});
}

int lev(const std::u32string& a, const std::u32string& b)
{
    std::vector<int> prev(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        std::vector<int> cur{static_cast<int>(i)};
        for (size_t j = 1; j <= b.size(); ++j)
            cur.push_back(std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1])}));
        prev = cur;
    }
    return prev.back();
}

void testLocalPool()
{
    uint32_t seed = 7;
    const auto rnd = [&](uint32_t n) {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) % n;
    };
    bool ok = true;
    for (int k = 0; k < 300; ++k) {
        std::u32string a, b;
        for (uint32_t i = 0, n = rnd(15); i < n; ++i) a += static_cast<char32_t>(U'a' + rnd(5));
        for (uint32_t i = 0, n = rnd(15); i < n; ++i) b += static_cast<char32_t>(U'a' + rnd(5));
        const int d = lev(a, b);
        for (const int limit : {0, 2, 5, 20}) {
            const int got = boundedLevenshtein(a, b, limit);
            ok = ok && (d <= limit ? got == d : got == limit + 1);
        }
    }
    check(ok, "bounded Levenshtein equals the full one");
    check(keysSimilar(U("isanybodythereplease"), U("isanybodythereplaese")), "one-letter error is similar");
    check(!keysSimilar(U("yes"), U("yet")), "short keys never match");
    check(!keysSimilar(U("gotofloor3rightnow"), U("gotofloor4rightnow")), "different digits never match");

    GamePool p("heavy-rain", "Heavy Rain", "r1",
               json::array({row(1, "Is anybody there? Please help me.", "Orada kimse var mı? Lütfen yardım edin."),
                            row(2, "Press X to call Jason", "Jason'ı çağırmak için X'e bas"),
                            row(3, "Go to floor 3.", "3. kata çık."), json::array({4, "", "bozuk", "satır"})}),
               0);
    check(p.size() == 3, "broken row skipped");
    {   // a labelled row as the server syncs it: key and translation without the label, source as read
        GamePool gta("grand-theft-auto-vi", "Grand Theft Auto VI", "r1",
                     json::array({json::array({9, text::compactPoolKey(std::string("What? You okay?")),
                                               "Jason: What? You okay?", "Ne? İyi misin?"})}),
                     0);
        const text::ReadLine rl = garbage().readLine("Lucia: What? You okay?");
        const auto hit = gta.lookup(rl.line);
        check(hit && hit->exact && rl.prefix + hit->translation == "Lucia: Ne? İyi misin?",
              "another speaker's line hits the bare key and shows its own label once");
    }
    auto h = p.lookup("Is anybody there?Please help me");
    check(h && h->id == 1 && h->exact, "punctuation/space difference: exact key");
    h = p.lookup("Is anybody there? Plaese help me.");
    check(h && h->id == 1 && !h->exact, "OCR letter error: similar line");
    check(!p.lookup("Go to floor 4."), "different digit: no match");
    check(p.lookupExact("Is anybody there?Please help me") && !p.lookupExact("Is anybody there? Plaese help me."),
          "exact lookup (K12): punctuation yes, a similar line no");
    check(!p.lookup("Something never seen before in this game"), "unknown line: none");
    {
        json rows = json::array();
        for (int i = 0; i < 20000; ++i)
            rows.push_back(row(i, "Line number " + std::to_string(i) + " of the long dialogue tree here", "S"));
        const GamePool big("g", "", "", rows, 0);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 20; ++i) big.lookup("A sentence that is not in the pool at all, really " + std::to_string(i));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20;
        check(ms < 150, "20 000 lines, a line not in the pool: " + std::to_string(ms) + " ms");
        check(!big.lookup("A sentence that is not in the pool at all, really 3"), "not found is remembered");
    }

    const std::wstring dir = std::filesystem::temp_directory_path().wstring() + L"\\makine_pool_test_"
                             + std::to_wstring(GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    LocalPool lp(dir);
    check(!lp.get("heavy-rain") && !lp.lookup("heavy-rain", "Press X to call Jason"),
          "an unconfirmed game's pool is not used");
    lp.confirm("heavy-rain", "Heavy Rain");
    auto st = lp.applySync("heavy-rain", {{"rev", "r1"}, {"name", "Heavy Rain"},
                                          {"items", json::array({row(1, "Is anybody there? Please help me.", "Orada kimse var mı?"),
                                                                 row(2, "Press X to call Jason", "Jason'ı çağır")})}});
    check(st.added == 2 && st.changed == 0 && st.removed == 0 && st.total == 2 && !st.unchanged, "first download");
    check(lp.applySync("heavy-rain", {{"unchanged", true}, {"rev", "r1"}}).unchanged, "same revision: unchanged");
    st = lp.applySync("heavy-rain", {{"rev", "r2"},
                                     {"items", json::array({row(2, "Press X to call Jason", "Jason'ı çağırmak için X"),
                                                            row(5, "Shaun? Where are you?", "Shaun? Neredesin?")})}});
    check(st.added == 1 && st.changed == 1 && st.removed == 1 && st.total == 2, "approved / edited / removed lines");
    check(!lp.lookup("heavy-rain", "Is anybody there? Please help me."), "a removed line is gone locally");
    LocalPool lp2(dir);
    h = lp2.lookup("heavy-rain", "Press X to call Jason");
    check(lp2.isConfirmed("heavy-rain") && lp2.rev("heavy-rain") == "r2" && h
              && h->translation == "Jason'ı çağırmak için X",
          "reopened: confirmation, revision and lines from disk");
    lp2.lookup("heavy-rain", "Press X to call Jason");
    auto hits = lp2.takeHits("heavy-rain");
    check(hits == std::map<int64_t, int>{{2, 2}} && lp2.takeHits("heavy-rain").empty(), "hit counters taken");
    lp2.returnHits("heavy-rain", hits);
    check(lp2.takeHits("heavy-rain") == std::map<int64_t, int>{{2, 2}}, "unsent counters put back");
    lp2.forget("heavy-rain");
    check(!lp2.isConfirmed("heavy-rain") && !LocalPool(dir).get("heavy-rain"), "not this game: local pool removed");
    std::filesystem::remove_all(dir, ec);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: live_logic_test <case> <repo root>\n");
        return 2;
    }
    const std::string name = argv[1];
    g_root = win::utf8ToWide(argv[2]);
    const std::map<std::string, std::function<void()>> cases{
        {"normalization", testNormalization}, {"garbage", testGarbage},   {"ocr_filter", testOcrFilter},
        {"subtitle", testSubtitle},           {"console", testConsole},   {"local_pool", testLocalPool},
        {"speaker", testSpeaker},             {"line_issues", testLineIssues}, {"clipped_edge", testClippedEdge},
    };
    const auto it = cases.find(name);
    if (it == cases.end()) {
        std::printf("unknown case %s\n", name.c_str());
        return 2;
    }
    it->second();
    std::printf("%s: %s\n", name.c_str(), g_fails ? (std::to_string(g_fails) + " FAILED").c_str() : "passed");
    return g_fails ? 1 : 0;
}

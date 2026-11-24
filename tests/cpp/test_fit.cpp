// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// Fit plan and chunk player (src/fit.*), the subtitle finder's evidence and the temporary growth
// (src/subtitle_finder.*). live_fit_test <case>

#include "fit.h"
#include "subtitle_finder.h"
#include "unitext.h"

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace live;

namespace {

int g_fails = 0;

void check(bool ok, const std::string& what)
{
    if (!ok) {
        ++g_fails;
        std::printf("FAIL %s\n", what.c_str());
    }
}

std::u32string u(const std::string& s) { return text::fromUtf8(s); }

void testFit()
{
    // 1080p, 28 px, 2 rows: a short line is one chunk
    const fit::Budget b = fit::budget(1920, 28, 2);
    check(b.charsPerRow > 100 && b.charsPerRow < 160, "1080p row budget " + std::to_string(b.charsPerRow));
    check(fit::split(u("Ne oluyor?"), b).size() == 1, "short line: one chunk");

    // a narrow budget: sentences become chunks, each fits, nothing is lost
    const fit::Budget nb{30, 2};
    const std::u32string longLine = u("Bunu daha önce de söyledim. Kimse dinlemedi, şimdi herkes bana bakıyor. "
                                      "Yarın sabah erkenden yola çıkıyoruz ve geri dönmüyoruz.");
    const auto chunks = fit::split(longLine, nb);
    check(chunks.size() >= 3, "long line splits (" + std::to_string(chunks.size()) + ")");
    std::u32string joined;
    for (const auto& c : chunks) {
        check(fit::rows(c, nb) <= nb.maxLines, "chunk fits: " + text::toUtf8(c));
        joined += (joined.empty() ? U"" : U" ") + c;
    }
    check(joined == longLine, "chunks keep every word: " + text::toUtf8(joined));
    check(chunks[0] == u("Bunu daha önce de söyledim."), "first cut at the sentence end: " + text::toUtf8(chunks[0]));

    // the speaker label is never cut and leads every chunk of its line
    const auto spk = fit::split(u("Lucia: Bunu daha önce de söyledim, kimse dinlemedi ve şimdi herkes bana bakıyor."), nb);
    check(spk.size() >= 2, "labelled line splits");
    for (const auto& c : spk) check(text::startsWith(c, U"Lucia: "), "label in front: " + text::toUtf8(c));

    // two speakers that fit together stay one chunk; that do not, become one chunk each
    check(fit::split(u("Jason: Ne?\nLucia: Hiç."), fit::Budget{40, 2}).size() == 1, "two short speakers: one chunk");
    const auto two = fit::split(u("Jason: Bunu daha önce de söyledim kimse dinlemedi.\nLucia: Şimdi herkes bana bakıyor ama."),
                                fit::Budget{40, 2});
    check(two.size() == 2 && text::startsWith(two[1], U"Lucia: "), "two long speakers: a chunk each");

    // a word longer than the budget still ends up somewhere
    const auto word = fit::split(u("Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), fit::Budget{10, 2});
    std::u32string all;
    for (const auto& c : word) all += c;
    check(all.size() == 74 && word.size() >= 3, "long word broken into fitting pieces");

    // TV: tighter than the 1080p overlay
    const fit::Budget tv = fit::tvBudget(0.046, 2);
    check(tv.charsPerRow > 40 && tv.charsPerRow < b.charsPerRow, "tv budget " + std::to_string(tv.charsPerRow));
    check(fit::tighter(b, tv).charsPerRow == tv.charsPerRow, "tighter takes the smaller");

    // reading time: 17 chars/s, never under minShow
    check(fit::chunkSeconds(u("Ne?"), 1.2) == 1.2, "short chunk: minShow");
    check(fit::chunkSeconds(std::u32string(68, U'a'), 1.2) == 4.0, "68 chars: 4 s");

    // player: chunks in turn, the last stays
    fit::ChunkPlayer p;
    p.start(7, {std::u32string(34, U'a'), std::u32string(17, U'b'), U"c"}, 1.2, 100.0);
    check(p.active() && p.id() == 7 && p.count() == 3, "player started");
    check(p.at(100.0) == 0 && p.at(101.9) == 0 && p.at(102.1) == 1 && p.at(103.3) == 2 && p.at(500) == 2, "player order");
    check(std::abs(p.total() - (2.0 + 1.2 + 1.2)) < 1e-9, "player total");
    p.clear();
    check(!p.active(), "player cleared");
}

finder::Observed line(double x0, double y0, double x1, double y1, double fill, const std::string& text)
{
    return {{x0, y0, x1, y1}, fill, text};
}

void testFinder()
{
    using finder::State;
    check(finder::likeness(line(0.3, 0.82, 0.7, 0.86, 0.3, "War never changes.")) > 0.6, "subtitle line is subtitle-like");
    check(finder::likeness(line(0.02, 0.03, 0.12, 0.05, 0.3, "$1,250")) < 0.1, "money HUD is not");
    check(finder::likeness(line(0.85, 0.75, 0.97, 0.78, 0.3, "Downtown")) < 0.2, "minimap label is not");

    // a lower-third subtitle that changes and blinks, a static HUD and a top-left objective
    finder::SubtitleFinder f;
    const char* const lines[] = {"Where are you going?", "", "Lucia: Get in the car.", "I told you already.", "",
                                 "We leave at dawn.", "Jason: Fine.", "", "Nobody listens to me.", "Move!"};
    double t = 0;
    int foundAt = -1;
    for (int i = 0; i < 10; ++i, t += finder::kIntervalS) {
        std::vector<finder::Observed> obs = {line(0.86, 0.76, 0.97, 0.79, 0.3, "Vinewood"),
                                             line(0.03, 0.06, 0.30, 0.10, 0.25, "Find the boat")};
        if (*lines[i]) {
            const double half = 0.05 + 0.01 * static_cast<double>(std::string(lines[i]).size()) / 2;
            obs.push_back(line(0.5 - half, 0.84, 0.5 + half, 0.88, 0.35, lines[i]));
        }
        f.addSample(obs, t);
        if (foundAt < 0 && f.band()) foundAt = i;
    }
    check(foundAt >= 0 && foundAt * finder::kIntervalS <= 10, "found within 10 s (sample " + std::to_string(foundAt) + ")");
    const auto band = f.band();
    check(band && band->y0 < 0.84 && band->y1 > 0.88 && band->x0 < 0.3 && band->x1 > 0.7, "band covers the subtitles");
    check(band && band->y0 > 0.6, "band is not the HUD");
    check(f.state() == State::Locked, std::string("locked after 18 s: ") + finder::stateName(f.state()));
    check(!f.due(t), "locked: the detector stops");

    // static HUD only: never found
    finder::SubtitleFinder hud;
    for (int i = 0; i < 12; ++i)
        hud.addSample({line(0.35, 0.05, 0.65, 0.09, 0.3, "MISSION PASSED"), line(0.86, 0.76, 0.97, 0.79, 0.3, "Vinewood")},
                      i * finder::kIntervalS);
    check(hud.state() == State::Searching && !hud.band(), "static text is never a subtitle band");
    check(hud.due(100), "still searching: due");

    // accept / manual / restore
    finder::SubtitleFinder a;
    for (int i = 0; i < 4; ++i)
        a.addSample({line(0.3, 0.84, 0.7, 0.88, 0.35, i % 2 ? "Get in." : "Where to?")}, i * finder::kIntervalS);
    check(a.state() == State::Found, std::string("found before lock: ") + finder::stateName(a.state()));
    check(a.accept() && a.state() == State::Locked, "accept locks");
    a.setManual(true);
    check(a.state() == State::Manual && !a.band() && !a.due(1000), "manual overrides");
    a.setManual(false);
    check(a.state() == State::Searching, "manual off: searching again");
    a.restore({0.2, 0.7, 0.8, 0.9}, 0.8);
    check(a.state() == State::Locked && a.band() && a.band()->y0 == 0.7, "restored band is locked");
}

void testTempGrow()
{
    finder::TempGrow g;
    // a line running into the right edge: grows right, the next reading is not used
    check(g.update({{{100, 10, 1000, 40}}}, 1000, 60, 0.0), "right edge grows");
    check(g.grow()[1] == finder::TempGrow::kStep && g.grow()[0] == 0, "right only");
    check(g.update({{{100, 10, 1150, 40}}}, 1150, 60, 0.1), "still at the edge: grows again");
    check(!g.update({{{100, 10, 1250, 40}}}, 1300, 60, 0.2), "at most kMax");
    check(g.grow()[1] == finder::TempGrow::kMax, "capped");
    // text keeps the margin; after kHoldS without text there it shrinks back
    check(!g.update({{{100, 10, 1250, 40}}}, 1300, 60, 3.0), "text in the margin");
    g.update({{{300, 10, 700, 40}}}, 1300, 60, 6.0);
    check(g.grow()[1] == finder::TempGrow::kMax, "held while recent");
    g.update({{{300, 10, 700, 40}}}, 1300, 60, 7.5);
    check(g.grow()[1] == 0, "shrinks back after the hold");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: live_fit_test <case>\n");
        return 2;
    }
    const std::map<std::string, std::function<void()>> cases{
        {"fit", testFit}, {"finder", testFinder}, {"temp_grow", testTempGrow}};
    const auto it = cases.find(argv[1]);
    if (it == cases.end()) {
        std::printf("unknown case %s\n", argv[1]);
        return 2;
    }
    it->second();
    std::printf("%s: %s\n", argv[1], g_fails ? (std::to_string(g_fails) + " FAILED").c_str() : "passed");
    return g_fails ? 1 : 0;
}

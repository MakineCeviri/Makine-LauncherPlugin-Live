// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// OCR space restoration test. Inputs are real outputs of the Chinese PP-OCRv3 model on stylised English
// subtitles (see docs/MIMARI.md §3.3); expectations are what a reader would want.
// Run: ctest --test-dir build   (or: build/text_fix_test.exe resources/en_words.txt)

#include "text_fix.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: text_fix_test <en_words.txt>\n");
        return 2;
    }
    live::SpacingFixer fixer;
    std::string path = argv[1];
    if (!fixer.load(std::wstring(path.begin(), path.end()))) {
        std::fprintf(stderr, "word list not found: %s\n", argv[1]);
        return 2;
    }

    const std::vector<std::pair<std::string, std::string>> cases = {
        // glued words (real OCR output)
        {"Iwas going to find you, Shaun.I promise.", "I was going to find you, Shaun. I promise."},
        {"Where wereyou last night? I called you four times.", "Where were you last night? I called you four times."},
        {"PressXto call Jason. Hold to run, release to stop.", "Press X to call Jason. Hold to run, release to stop."},
        {"I don't know whatyou mean by that, Lieutenant.", "I don't know what you mean by that, Lieutenant."},
        {"We haveto get out of here before they come back.", "We have to get out of here before they come back."},
        {"You can'tjust leave herthere, it's too dangerous!", "You can't just leave her there, it's too dangerous!"},
        {"Is anybody there? Please, Ineed yourhelp.", "Is anybody there? Please, I need your help."},
        {"We have to getoutof here before theycomeback.", "We have to get out of here before they comeback."},  // "comeback" is a real word: acceptable
        {"Wherewereyou last night?", "Where were you last night?"},
        {"Iwas goingto findyou, Shaun.", "I was going to find you, Shaun."},
        {"Is anybodythere? Please, Ineedyour help.", "Is anybody there? Please, I need your help."},
        {"You can'tjustleaveherthere, it's too dangerous!", "You can't just leave her there, it's too dangerous!"},
        {"Press Xto call Jason.", "Press X to call Jason."},
        {"it'stoo late", "it's too late"},
        // must stay untouched: correct text, names, game terms, abbreviations, numbers, noise
        {"I was going to find you, Shaun. I promise.", "I was going to find you, Shaun. I promise."},
        {"Jodie, can you hear me? Stay with me.", "Jodie, can you hear me? Stay with me."},
        {"Connor and Markus met Hank at the precinct.", "Connor and Markus met Hank at the precinct."},
        {"Welcome to Blackwell Academy.", "Welcome to Blackwell Academy."},
        {"The U.S. embassy, e.g. the one in Paris.", "The U.S. embassy, e.g. the one in Paris."},
        {"Wait...what?", "Wait...what?"},
        {"HP 120/250  Level 12", "HP 120/250  Level 12"},
        {"Kamski", "Kamski"},
        // names the list does not know are not chopped into letters (real GTA VI log: "G or di K")
        {"and you suspect Gordi Kavanagh?", "and you suspect Gordi Kavanagh?"},
        {"Gordi has the time.", "Gordi has the time."},
        {"Jasn, get in the car.", "Jasn, get in the car."},
        {"tnkedattetres", "tnkedattetres"},
        {"Ok", "Ok"},
        {"Line one\nLinetwo", "Line one\nLine two"},
    };

    // CJK noise from the Chinese recognition model in English games (UTF-8 escapes keep this file ASCII-safe)
    const std::vector<std::pair<std::string, std::string>> cjk = {
        {"\xE5\x9B\xAD\xE5\x9B\xBE" "B", ""},                       // U+56ED U+56FE B  ("yuan tu B")
        {"Jason! \xE5\x9B\xAD\xE5\x9B\xBE", "Jason!"},                 // trailing noise token
        {"\xE5\x9B\xBE\nWhere were you?", "Where were you?"},           // noise line
        {"\xEF\xBC\x81\xEF\xBC\x9F", ""},                             // fullwidth ! ?
        {"Caf\xC3\xA9 cr\xC3\xA8me, se\xC3\xB1or.", "Caf\xC3\xA9 cr\xC3\xA8me, se\xC3\xB1or."},  // Latin accents stay
    };

    int failed = 0;
    for (const auto& [input, expected] : cjk) {
        const std::string got = live::dropForeignScriptTokens(input);
        if (got != expected) {
            ++failed;
            std::printf("FAIL  CJK  %s\n      got:      %s\n      expected: %s\n", input.c_str(), got.c_str(), expected.c_str());
        }
    }
    for (const auto& [input, expected] : cases) {
        const std::string got = fixer.fix(input);
        if (got != expected) {
            ++failed;
            std::printf("FAIL  %s\n      got:      %s\n      expected: %s\n", input.c_str(), got.c_str(), expected.c_str());
        }
    }
    const size_t total = cases.size() + cjk.size();
    std::printf("%zu/%zu passed\n", total - failed, total);
    return failed ? 1 : 0;
}

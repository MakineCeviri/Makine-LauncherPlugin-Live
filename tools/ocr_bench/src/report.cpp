// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// Report: reads the per-candidate result files, scores them against truth.tsv of their frame set and writes
// one JSON (OCR_MIMARI §7.3 shape) + a one-screen Markdown table.
//
// Accuracy definitions (all case-sensitive, lines joined with one space, whitespace collapsed):
//   CER raw      — recognizer output as is.
//   CER fixed    — after the production post-processing in live_core.cpp: dropForeignScriptTokens + SpacingFixer.
//                  "Fastest with CER <= baseline" is decided on this column.
//   CER nospace  — raw output and truth with all whitespace removed (spacing errors ignored).
//   line match   — share of truth lines found verbatim (as a token run) in the fixed output.
//   G-KEY        — pool key (lower-case letters/digits only, MIMARI §4.3) of the fixed output equals the truth's.
//   space ok     — of the frames whose characters are all right (nospace equal), the share whose spacing is right.
//   G-SPLIT      — frames returning more lines than the truth has (a line came back in pieces), OCR_MIMARI §6.3.
//   G-ORDER      — frames where reordering the returned lines lowers the edit distance by 2+ (pieces out of order).

#include "common.h"
#include "text_fix.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace bench {

namespace {

std::string unescape(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char c = s[++i];
            o += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::vector<std::string> splitTab(const std::string& line)
{
    std::vector<std::string> f;
    size_t start = 0;
    for (;;) {
        size_t p = line.find('\t', start);
        f.push_back(line.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return f;
}

std::u32string toU32(const std::string& s)
{
    std::u32string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        o += cp;
        i += n;
    }
    return o;
}

std::string normalize(const std::string& s)
{
    std::string o;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { space = !o.empty(); continue; }
        if (space) { o += ' '; space = false; }
        o += c;
    }
    return o;
}

std::string noSpace(const std::string& s)
{
    std::string o;
    for (char c : s)
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r') o += c;
    return o;
}

std::string poolKey(const std::string& s)
{
    std::string o;
    for (unsigned char c : s) {
        if (c >= 0x80) o += static_cast<char>(c);  // non-ASCII letters stay (CJK noise must break the key)
        else if (std::isalnum(c)) o += static_cast<char>(std::tolower(c));
    }
    return o;
}

size_t levenshtein(const std::u32string& a, const std::u32string& b)
{
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

bool containsLine(const std::string& haystack, const std::string& line)
{
    const std::string h = " " + normalize(haystack) + " ", n = " " + normalize(line) + " ";
    return h.find(n) != std::string::npos;
}

struct Truth {
    std::string text;  // lines joined by \n
    std::map<std::string, std::string> tags;  // res / variant / bg
};

std::map<std::string, Truth> loadTruth(const std::string& dir)
{
    std::map<std::string, Truth> t;
    std::ifstream f(std::filesystem::path(widen(dir) + L"\\truth.tsv"), std::ios::binary);
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        auto fields = splitTab(line);
        Truth tr;
        tr.text = unescape(fields.back());  // real/ sets may carry only "file<TAB>text"
        if (fields.size() >= 6) {
            tr.tags["res"] = fields[1];
            const std::string& v = fields[2];
            tr.tags["variant"] = v.find("typewriter") != std::string::npos ? "typewriter" : "full";
            tr.tags["speaker"] = v.find("+speaker") != std::string::npos ? "speaker" : "nospeaker";
            tr.tags["bg"] = fields[3];
        }
        t[fields[0]] = tr;
    }
    return t;
}

struct Stat { double p50 = -1, p95 = -1, max = -1; size_t n = 0; };

Stat stat(std::vector<double> v)
{
    Stat s;
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return x < 0; }), v.end());
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    auto q = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(std::ceil(p * v.size())) - 1)]; };
    s.p50 = q(0.5);
    s.p95 = q(0.95);
    s.max = v.back();
    s.n = v.size();
    return s;
}

struct Cand {
    std::map<std::string, std::string> head;
    // per pass: stage samples
    std::map<int, std::map<std::string, std::vector<double>>> samples;
    double cpuSum = 0, wallSum = 0;
    size_t editsRaw = 0, editsFixed = 0, editsNs = 0, chars = 0, charsNs = 0;
    size_t splitFrames = 0, orderFrames = 0;
    size_t truthLines = 0, linesFound = 0, frames = 0, framesExact = 0, keyOk = 0, nsOk = 0, spaceRaw = 0, spaceFixed = 0;
    std::map<std::string, std::pair<size_t, size_t>> subset;  // tag value → (edits fixed, chars)
    double cer(size_t e, size_t c) const { return c ? 100.0 * e / c : 0; }
    double cerFixed() const { return cer(editsFixed, chars); }
};

std::string jsonStr(const std::string& s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}

std::string fmt(double v, int prec = 1)
{
    if (v < 0) return "—";
    char b[32];
    std::snprintf(b, sizeof(b), "%.*f", prec, v);
    return b;
}

} // namespace

int report(const std::map<std::string, std::string>& opts, const std::vector<std::string>& files)
{
    auto opt = [&](const char* k) { auto it = opts.find(k); return it == opts.end() ? std::string() : it->second; };
    live::SpacingFixer fixer;
    if (!fixer.load(widen(opt("words")))) { std::fprintf(stderr, "cannot load word list %s\n", opt("words").c_str()); return 2; }

    std::map<std::string, std::map<std::string, Truth>> truthBySet;
    std::vector<Cand> cands;
    for (const auto& path : files) {
        std::ifstream f(std::filesystem::path(widen(path)), std::ios::binary);
        if (!f) continue;
        Cand c;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line[0] == '#') {
                auto fl = splitTab(line.substr(1));
                c.head[fl[0]] = fl.size() > 1 ? fl[1] : "";
                continue;
            }
            if (line.rfind("file\t", 0) == 0) continue;
            auto fl = splitTab(line);
            if (fl.size() < 11) continue;
            const int pass = std::stoi(fl[1]);
            const char* keys[] = {"pre", "S4d", "S2S3", "cls", "S4", "total"};
            for (int k = 0; k < 6; ++k) c.samples[pass][keys[k]].push_back(std::stod(fl[2 + k]));
            const double total = std::stod(fl[7]), cpu = std::stod(fl[8]);
            const int nl = std::stoi(fl[9]);
            if (nl > 0 && std::stod(fl[6]) >= 0) c.samples[pass]["S4line"].push_back(std::stod(fl[6]) / nl);
            if (pass != 1) continue;
            c.cpuSum += cpu;
            c.wallSum += total;

            auto& truthSet = truthBySet[c.head["set"]];
            if (truthSet.empty()) truthSet = loadTruth(c.head["set"]);
            auto it = truthSet.find(fl[0]);
            if (it == truthSet.end()) continue;
            const Truth& tr = it->second;
            const std::string raw = unescape(fl[10]);
            const std::string fixedText = fixer.fix(live::dropForeignScriptTokens(raw));
            const std::string tN = normalize(tr.text), rawN = normalize(raw), fixN = normalize(fixedText);
            const std::u32string t32 = toU32(tN);
            const size_t eFixed = levenshtein(toU32(fixN), t32);
            c.editsRaw += levenshtein(toU32(rawN), t32);
            c.editsFixed += eFixed;
            c.chars += t32.size();
            const std::u32string tNs = toU32(noSpace(tr.text));
            c.editsNs += levenshtein(toU32(noSpace(raw)), tNs);
            c.charsNs += tNs.size();
            ++c.frames;
            c.framesExact += fixN == tN;
            c.keyOk += poolKey(fixedText) == poolKey(tr.text);
            std::istringstream ls(tr.text);
            size_t frameTruthLines = 0;
            for (std::string tl; std::getline(ls, tl);) {
                if (normalize(tl).empty()) continue;
                ++c.truthLines;
                ++frameTruthLines;
                c.linesFound += containsLine(fixedText, tl);
            }
            std::vector<std::string> predLines;
            std::istringstream ps(raw);
            for (std::string pl; std::getline(ps, pl);)
                if (!normalize(pl).empty()) predLines.push_back(normalize(pl));
            c.splitFrames += predLines.size() > frameTruthLines;
            if (predLines.size() >= 2 && predLines.size() <= 6) {
                auto joined = [&](const std::vector<std::string>& v) {
                    std::string j;
                    for (const auto& x : v) j += (j.empty() ? "" : " ") + x;
                    return levenshtein(toU32(j), t32);
                };
                const size_t given = joined(predLines);
                std::vector<std::string> perm = predLines;
                std::sort(perm.begin(), perm.end());
                size_t best = given;
                do { best = std::min(best, joined(perm)); } while (std::next_permutation(perm.begin(), perm.end()));
                c.orderFrames += best + 2 <= given;
            }
            if (noSpace(raw) == noSpace(tr.text)) {
                ++c.nsOk;
                c.spaceRaw += rawN == tN;
                c.spaceFixed += fixN == tN;
            }
            for (const auto& [k, v] : tr.tags) {
                if (k == "speaker" && v == "nospeaker") continue;
                auto& s = c.subset[v];
                s.first += eFixed;
                s.second += t32.size();
            }
        }
        if (c.frames) cands.push_back(std::move(c));
    }
    if (cands.empty()) { std::fprintf(stderr, "no results\n"); return 2; }

    const std::string baselineId = opt("baseline");
    std::map<std::string, const Cand*> baseBySet;
    for (const auto& c : cands)
        if (c.head.at("id") == baselineId) baseBySet[c.head.at("set")] = &c;

    std::ofstream md(std::filesystem::path(widen(opt("md"))), std::ios::binary);
    std::ofstream js(std::filesystem::path(widen(opt("json"))), std::ios::binary);
    md << "# OCR kıyas raporu\n\n";
    md << "Makine: " << cands[0].head["cpu"] << " · commit " << opt("commit") << " · " << opt("date")
       << " · ısınma " << cands[0].head["warmup"] << " kare, kare başına 2 tur (gecikme 1. tur; \"p95 2.\" ikinci tur)\n\n";
    js << "{\n \"machine\": " << jsonStr(cands[0].head["cpu"]) << ",\n \"commit\": " << jsonStr(opt("commit"))
       << ",\n \"baseline\": " << jsonStr(baselineId) << ",\n \"runs\": [\n";

    std::set<std::string> sets;
    for (const auto& c : cands) sets.insert(c.head.at("set"));
    bool firstJson = true;
    for (const auto& set : sets) {
        const Cand* base = baseBySet.count(set) ? baseBySet[set] : nullptr;
        std::vector<const Cand*> list;
        for (const auto& c : cands)
            if (c.head.at("set") == set) list.push_back(&c);
        const std::string setName = set.substr(set.find_last_of("/\\") + 1);
        md << "## Set: `" << setName << "` (" << list[0]->frames << " kare)\n\n";
        md << "| Aday | iş p. | spin | toplam p50 | p95 | p95 2. | max | det p50 | maske+izdüşüm p50 | rec p50 | rec/satır p50/p95 "
              "| çekirdek | CER ham | **CER düz.** | CER boşluksuz | satır tam | G-KEY | boşluk doğru (ham/düz.) | G-ORDER | G-SPLIT |\n";
        md << "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
        const Cand* winner = nullptr;
        for (const Cand* c : list) {
            auto get = [&](int pass, const char* k) {
                auto it = c->samples.find(pass);
                if (it == c->samples.end() || !it->second.count(k)) return Stat{};
                return stat(it->second.at(k));
            };
            const Stat tot = get(1, "total"), tot2 = get(2, "total"), det = get(1, "S4d"), split = get(1, "S2S3"),
                       rec = get(1, "S4"), recl = get(1, "S4line"), pre = get(1, "pre"), cls = get(1, "cls");
            const double cores = c->wallSum > 0 ? c->cpuSum / c->wallSum : 0;
            const double lineMatch = c->truthLines ? 100.0 * c->linesFound / c->truthLines : 0;
            const double keyMatch = 100.0 * c->keyOk / c->frames;
            const double spRaw = c->nsOk ? 100.0 * c->spaceRaw / c->nsOk : -1;
            const double spFix = c->nsOk ? 100.0 * c->spaceFixed / c->nsOk : -1;
            md << "| " << c->head.at("label") << " | " << c->head.at("threads") << " | " << c->head.at("spin") << " | "
               << fmt(tot.p50) << " | " << fmt(tot.p95) << " | " << fmt(tot2.p95) << " | " << fmt(tot.max) << " | "
               << fmt(det.p50) << " | " << fmt(split.p50, 2) << " | " << fmt(rec.p50) << " | " << fmt(recl.p50) << " / "
               << fmt(recl.p95) << " | " << fmt(cores, 2) << " | " << fmt(c->cer(c->editsRaw, c->chars), 2) << " | **"
               << fmt(c->cerFixed(), 2) << "** | " << fmt(c->cer(c->editsNs, c->charsNs), 2) << " | " << fmt(lineMatch)
               << "% | " << fmt(keyMatch) << "% | " << fmt(spRaw) << "% / " << fmt(spFix) << "% | " << c->orderFrames << " | " << c->splitFrames << " |\n";
            if (base && c->cerFixed() <= base->cerFixed() + 1e-9) {
                const double p = tot.p50;
                if (!winner || p < stat(winner->samples.at(1).at("total")).p50) winner = c;
            }

            auto js1 = [&](const char* name, const Stat& s) {
                return std::string("\"") + name + "\": {\"p50\": " + fmt(s.p50, 3) + ", \"p95\": " + fmt(s.p95, 3)
                     + ", \"max\": " + fmt(s.max, 3) + ", \"n\": " + std::to_string(s.n) + "}";
            };
            auto num = [](double v) { return v < 0 ? std::string("null") : fmt(v, 3); };
            std::string stages = js1("pre", pre) + ", " + js1("S4d", det) + ", " + js1("S2S3", split) + ", " +
                                 js1("cls", cls) + ", " + js1("S4", rec) + ", " + js1("S4_line", recl) + ", " +
                                 js1("total", tot) + ", " + js1("total_pass2", tot2);
            std::string stagesClean;
            for (size_t i = 0; i < stages.size(); ++i) {  // "—" (no data) → null
                if (stages.compare(i, 3, "\xE2\x80\x94") == 0) { stagesClean += "null"; i += 2; }
                else stagesClean += stages[i];
            }
            js << (firstJson ? "" : ",\n") << "  {\"run\": " << jsonStr(c->head.at("id")) << ", \"label\": "
               << jsonStr(c->head.at("label")) << ", \"engine\": " << jsonStr(c->head.at("engine"))
               << ", \"scenario\": " << jsonStr(setName) << ", \"threads\": " << c->head.at("threads")
               << ", \"spin\": " << jsonStr(c->head.at("spin")) << ", \"ep\": " << jsonStr(c->head.at("ep"))
               << ",\n   \"stage\": {" << stagesClean << "},\n   \"cpu_cores\": " << num(cores)
               << ", \"cer\": {\"raw\": " << num(c->cer(c->editsRaw, c->chars)) << ", \"fixed\": " << num(c->cerFixed())
               << ", \"nospace\": " << num(c->cer(c->editsNs, c->charsNs)) << "}, \"line_match\": " << num(lineMatch)
               << ", \"frame_match\": " << num(100.0 * c->framesExact / c->frames) << ", \"key_match\": " << num(keyMatch)
               << ", \"space_ok_raw\": " << num(spRaw) << ", \"space_ok_fixed\": " << num(spFix) << ", \"g_order\": " << c->orderFrames
               << ", \"g_split\": " << c->splitFrames
               << ",\n   \"cer_fixed_by_subset\": {";
            bool first = true;
            for (const auto& [k, v] : c->subset) {
                js << (first ? "" : ", ") << jsonStr(k) << ": " << num(v.second ? 100.0 * v.first / v.second : 0);
                first = false;
            }
            js << "}}";
            firstJson = false;
        }
        md << "\nSüreler ms. *çekirdek* = süreç CPU zamanı / duvar saati (dönen iş parçacıkları dahil). "
              "Kazanan kuralı: CER düz. ≤ taban olanların en hızlısı (toplam p50).\n\n";
        if (base) {
            md << "**Taban:** " << base->head.at("label") << " — CER düz. " << fmt(base->cerFixed(), 2) << "%. ";
            md << "**Kazanan (en hızlı, CER düz. ≤ taban):** " << (winner ? winner->head.at("label") : std::string("yok"))
               << "\n\n";
        }
        // CER by subset, only for synthetic sets that carry tags
        std::vector<std::string> cols = {"1080p", "1440p", "4k", "rp720", "typewriter", "speaker", "dark", "sky", "busy", "gradient"};
        if (!list[0]->subset.empty()) {
            md << "CER düz. (%) alt kümelere göre:\n\n| Aday |";
            for (auto& col : cols) md << " " << col << " |";
            md << "\n|---|";
            for (size_t i = 0; i < cols.size(); ++i) md << "---|";
            md << "\n";
            for (const Cand* c : list) {
                md << "| " << c->head.at("label") << " |";
                for (auto& col : cols) {
                    auto it = c->subset.find(col);
                    md << " " << (it == c->subset.end() || !it->second.second ? std::string("—")
                                  : fmt(100.0 * it->second.first / it->second.second, 2)) << " |";
                }
                md << "\n";
            }
            md << "\n";
        }
    }
    js << "\n ]\n}\n";
    std::printf("report: %zu runs\n", cands.size());
    return 0;
}

} // namespace bench

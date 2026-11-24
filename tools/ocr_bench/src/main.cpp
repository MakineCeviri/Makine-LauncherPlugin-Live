// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// ocr_bench — OCR latency/accuracy harness for Makine Live (docs/OCR_MIMARI.md §7).
//
//   ocr_bench run --frames DIR --out FILE --id ID --engine dll|ort [options]   one candidate, one process
//   ocr_bench report --words en_words.txt --baseline ID --md FILE --json FILE RESULT...
//
// One process per candidate: RapidOcrOnnx's onnxruntime.dll and the DirectML build share a module name and must
// never meet in one process. Frames are decoded before timing; every frame runs `--passes` times after
// `--warmup` untimed frames, so pass-to-pass drift shows how noisy the machine was.

#include "common.h"
#include "ort_ocr.h"
#include "rapid_dll.h"

#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW (WIN32_LEAN_AND_MEAN leaves it out)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace bench {
int report(const std::map<std::string, std::string>& opts, const std::vector<std::string>& files);
}

using namespace bench;

namespace {

std::string escapeField(const std::string& s)
{
    std::string o;
    for (char c : s) {
        if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\\') o += "\\\\";
        else o += c;
    }
    return o;
}

std::string cpuName()
{
    char buf[256] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return "unknown";
    std::string s = buf;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

double processCpuMs()
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto ms = [](const FILETIME& f) {
        return static_cast<double>((static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000.0;
    };
    return ms(k) + ms(u);
}

std::vector<std::string> truthFiles(const std::wstring& dir)
{
    std::vector<std::string> files;
    std::ifstream f(std::filesystem::path(dir + L"\\truth.tsv"), std::ios::binary);
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#') continue;
        if (line.back() == '\r') line.pop_back();
        files.push_back(line.substr(0, line.find('\t')));
    }
    return files;
}

int run(std::map<std::string, std::string>& o)
{
    const std::wstring framesDir = widen(o["frames"]);
    const int threads = std::stoi(o.count("threads") ? o["threads"] : "4");
    const int passes = std::stoi(o.count("passes") ? o["passes"] : "2");
    const int warmup = std::stoi(o.count("warmup") ? o["warmup"] : "10");
    const bool spin = !o.count("spin") || o["spin"] != "0";
    std::string err, describe;

    std::vector<std::string> names = truthFiles(framesDir);
    if (names.empty()) { std::fprintf(stderr, "no frames listed in %s\\truth.tsv\n", o["frames"].c_str()); return 2; }
    std::vector<Image> frames(names.size());
    for (size_t i = 0; i < names.size(); ++i)
        if (!loadImage(framesDir + L"\\" + widen(names[i]), frames[i], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }

    RapidDll dll;
    OrtOcr ort;
    const bool useDll = o["engine"] == "dll";
    if (useDll) {
        if (!dll.init(widen(o["rapid-dir"]), widen(o["det"]), widen(o["cls"]), widen(o["rec"]), widen(o["dict"]),
                      threads, o.count("cls-on") > 0, err)) {
            std::fprintf(stderr, "init: %s\n", err.c_str());
            return 3;
        }
        describe = "RapidOcrOnnx.dll (file input, BMP)";
    } else {
        OrtConfig cfg;
        cfg.ortDir = widen(o["ort-dir"]);
        if (o["det"] != "none") cfg.detModel = widen(o["det"]);
        cfg.clsModel = widen(o["cls"]);
        cfg.recModel = widen(o["rec"]);
        cfg.dictFile = widen(o["dict"]);
        cfg.threads = threads;
        cfg.spin = spin;
        cfg.dml = o.count("dml") > 0;
        if (!ort.init(cfg, err)) { std::fprintf(stderr, "init: %s\n", err.c_str()); return 3; }
        describe = ort.describe();
    }
    auto runOne = [&](const Image& img, Times& t) {
        return useDll ? dll.run(img, t) : ort.run(img, t);
    };

    for (size_t i = 0; i < frames.size() && static_cast<int>(i) < warmup; ++i) {
        Times t;
        runOne(frames[i], t);
    }

    std::ofstream out(std::filesystem::path(widen(o["out"])), std::ios::binary);
    out << "#id\t" << o["id"] << "\n#label\t" << o["label"] << "\n#engine\t" << describe << "\n#threads\t" << threads
        << "\n#spin\t" << (useDll ? "default" : (spin ? "1" : "0")) << "\n#ep\t" << (o.count("dml") ? "dml" : "cpu")
        << "\n#set\t" << o["frames"] << "\n#cpu\t" << cpuName() << "\n#warmup\t" << warmup << "\n";
    out << "file\tpass\tpre\tdet\tsplit\tcls\trec\ttotal\tcpu\tlines\ttext\n";
    for (int p = 1; p <= passes; ++p) {
        for (size_t i = 0; i < frames.size(); ++i) {
            Times t;
            const double c0 = processCpuMs();
            const double t0 = nowMs();
            std::vector<std::string> lines = runOne(frames[i], t);
            t.total = nowMs() - t0;
            const double cpu = processCpuMs() - c0;
            std::string text;
            for (const auto& l : lines) text += (text.empty() ? "" : "\n") + l;
            char buf[256];
            if (useDll)
                std::snprintf(buf, sizeof(buf), "%d\t%.3f\t-1\t-1\t-1\t-1\t%.3f\t%.2f\t%d", p, t.pre, t.total, cpu, t.lines);
            else
                std::snprintf(buf, sizeof(buf), "%d\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%.2f\t%d", p, t.pre, t.det,
                              t.split, t.cls, t.rec, t.total, cpu, t.lines);
            out << names[i] << "\t" << buf << "\t" << escapeField(text) << "\n";
        }
    }
    std::printf("%s: %zu frames x %d passes\n", o["id"].c_str(), frames.size(), passes);
    return 0;
}

} // namespace

int main()
{
    // UTF-8 arguments (labels carry Turkish text; char** argv would be in the ANSI code page)
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(narrow(wargv[i]));
    LocalFree(wargv);
    std::vector<const char*> argvPtrs;
    for (const auto& a : args) argvPtrs.push_back(a.c_str());
    const char* const* argv = argvPtrs.data();
    if (argc < 2) {
        std::fprintf(stderr, "usage: ocr_bench run|report [--key value]... [files]\n");
        return 2;
    }
    std::map<std::string, std::string> opts;
    std::vector<std::string> files;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0) {
            std::string key = a.substr(2);
            if (key == "dml" || key == "cls-on") opts[key] = "1";
            else if (i + 1 < argc) opts[key] = argv[++i];
        } else {
            files.push_back(a);
        }
    }
    const std::string mode = argv[1];
    if (mode == "run") return run(opts);
    if (mode == "report") return report(opts, files);
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}

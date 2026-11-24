// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// Windows.Media.Ocr candidate for tools/ocr_bench (OCR_MIMARI K14 / B-WMO). C++/WinRT needs MSVC, so this is a
// separate executable (build.cmd); it writes the same result format as `ocr_bench run`, so `ocr_bench report`
// scores it with everything else.
//
//   winocr_bench.exe <framesDir> <out.tsv> [passes] [warmup]
//
// Timed: in-memory BGRA → SoftwareBitmap (`pre`) and RecognizeAsync (`total` includes both). The engine does its
// own line finding, so det/rec are not separable. Thread count is the system's, not ours.

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Ocr;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

namespace {

double nowMs()
{
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1000.0 / freq;
}

double processCpuMs()
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto ms = [](const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 10000.0; };
    return ms(k) + ms(u);
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

struct Frame {
    std::string name;
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;
};

} // namespace

static int run(int argc, wchar_t** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: winocr_bench <framesDir> <out.tsv> [passes] [warmup]\n");
        return 2;
    }
    init_apartment();
    const std::filesystem::path dir = std::filesystem::path(argv[1]).make_preferred();  // StorageFile wants backslashes
    const int passes = argc > 3 ? _wtoi(argv[3]) : 2;
    const int warmup = argc > 4 ? _wtoi(argv[4]) : 10;

    OcrEngine engine = OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(L"en-US"));
    if (!engine) engine = OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine) { std::fprintf(stderr, "no Windows.Media.Ocr recognizer installed\n"); return 3; }
    const std::string lang = to_string(engine.RecognizerLanguage().LanguageTag());

    std::vector<Frame> frames;
    std::ifstream truth(dir / "truth.tsv", std::ios::binary);
    for (std::string line; std::getline(truth, line);) {
        if (line.empty() || line[0] == '#') continue;
        Frame f;
        f.name = line.substr(0, line.find('\t'));
        auto file = StorageFile::GetFileFromPathAsync((dir / f.name).wstring()).get();
        auto stream = file.OpenAsync(FileAccessMode::Read).get();
        auto decoder = BitmapDecoder::CreateAsync(stream).get();
        auto bmp = decoder.GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied).get();
        f.w = bmp.PixelWidth();
        f.h = bmp.PixelHeight();
        Buffer buf(uint32_t(f.w * f.h * 4));
        bmp.CopyToBuffer(buf);
        f.bgra.assign(buf.data(), buf.data() + buf.Length());
        frames.push_back(std::move(f));
    }

    auto runOne = [&](const Frame& f, double& pre) {
        const double t0 = nowMs();
        Buffer buf(uint32_t(f.bgra.size()));
        std::memcpy(buf.data(), f.bgra.data(), f.bgra.size());
        buf.Length(uint32_t(f.bgra.size()));
        SoftwareBitmap bmp = SoftwareBitmap::CreateCopyFromBuffer(buf, BitmapPixelFormat::Bgra8, f.w, f.h,
                                                                  BitmapAlphaMode::Premultiplied);
        pre = nowMs() - t0;
        OcrResult r = engine.RecognizeAsync(bmp).get();
        std::string text;
        int n = 0;
        for (const auto& l : r.Lines()) {
            text += (text.empty() ? "" : "\n") + to_string(l.Text());
            ++n;
        }
        return std::make_pair(text, n);
    };

    for (size_t i = 0; i < frames.size() && int(i) < warmup; ++i) {
        double pre;
        runOne(frames[i], pre);
    }

    std::ofstream out(std::filesystem::path(argv[2]), std::ios::binary);
    out << "#id\twinocr\n#label\tWindows.Media.Ocr (" << lang << ")\n#engine\tWindows.Media.Ocr " << lang
        << "\n#threads\tn/a\n#spin\tn/a\n#ep\tcpu\n#set\t" << to_string(hstring(argv[1])) << "\n#cpu\t" << cpuName()
        << "\n#warmup\t" << warmup << "\n";
    out << "file\tpass\tpre\tdet\tsplit\tcls\trec\ttotal\tcpu\tlines\ttext\n";
    for (int p = 1; p <= passes; ++p)
        for (const Frame& f : frames) {
            double pre = 0;
            const double c0 = processCpuMs(), t0 = nowMs();
            auto [text, n] = runOne(f, pre);
            const double total = nowMs() - t0, cpu = processCpuMs() - c0;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%d\t%.3f\t-1\t-1\t-1\t-1\t%.3f\t%.2f\t%d", p, pre, total, cpu, n);
            out << f.name << "\t" << buf << "\t" << escapeField(text) << "\n";
        }
    std::printf("winocr (%s): %zu frames x %d passes\n", lang.c_str(), frames.size(), passes);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    try {
        return run(argc, argv);
    } catch (const winrt::hresult_error& e) {
        std::fprintf(stderr, "WinRT error 0x%08X: %s\n", static_cast<unsigned>(e.code()), to_string(e.message()).c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
    }
    return 1;
}

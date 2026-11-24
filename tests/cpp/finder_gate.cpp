// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// Subtitle-finder gate on whole screens (python tools/make_scene_frames.py --bench <ocrbench>): each
// episode is one scene sampled every finder::kIntervalS; the finder sees what the engine would give it
// (finder::observe: detector at ≤ finder::kMaxWidth px, fill score, the most subtitle-like line read).
//   precision  bands proposed that cover the subtitle / bands proposed (episodes without subtitles count)
//   recall     episodes with subtitles whose band was found and covers the subtitle
//   time       seconds until found / locked
//   cost       one sample (downscale + detector + fill + reading), p50 / p95, at the given thread count
//   live_finder_gate <scene dir> <ocr runtime dir> <threads>

#include "finder_ocr.h"
#include "subtitle_finder.h"
#include "winutil.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool loadImage(const std::wstring& path, std::vector<uint8_t>& bgra, int& w, int& h)
{
    static IWICImagingFactory* factory = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* f = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        return f;
    }();
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT uw = 0, uh = 0;
    bool ok = factory && SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                                      WICDecodeMetadataCacheOnDemand, &dec))
              && SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv))
              && SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                            WICBitmapPaletteTypeCustom))
              && SUCCEEDED(conv->GetSize(&uw, &uh));
    if (ok) {
        w = static_cast<int>(uw);
        h = static_cast<int>(uh);
        bgra.resize(static_cast<size_t>(w) * h * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, static_cast<UINT>(bgra.size()), bgra.data()));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return ok;
}

double pct(std::vector<double> v, double p)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))];
}

// The band covers the subtitle strip: most of its width, and the band sits on the strip
bool covers(const live::finder::Rect& b, const nlohmann::json& band, int w, int h)
{
    const double sx0 = band[0].get<double>() / w, sy0 = band[1].get<double>() / h;
    const double sx1 = band[2].get<double>() / w, sy1 = band[3].get<double>() / h;
    const double ox = std::max(0.0, std::min(b.x1, sx1) - std::max(b.x0, sx0));
    const double oy = std::max(0.0, std::min(b.y1, sy1) - std::max(b.y0, sy0));
    const double cy = (b.y0 + b.y1) / 2;
    return ox >= 0.9 * (sx1 - sx0) && oy >= 0.5 * b.h() && cy >= sy0 && cy <= sy1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::printf("usage: live_finder_gate <scene dir> <ocr runtime dir> <threads>\n");
        return 2;
    }
    const std::wstring dir = live::win::utf8ToWide(argv[1]), runtime = live::win::utf8ToWide(argv[2]);
    std::ifstream in(std::filesystem::path(dir) / "truth.json", std::ios::binary);
    if (!in) {
        std::printf("FAIL no truth.json in %s (python tools/make_scene_frames.py --bench <ocrbench>)\n", argv[1]);
        return 1;
    }
    const nlohmann::json truth = nlohmann::json::parse(in, nullptr, false);
    live::OcrOrt ocr;
    std::string error;
    if (!ocr.load({runtime + L"\\bin\\onnxruntime.dll", runtime + L"\\models\\ch_PP-OCRv3_det_infer.onnx",
                   runtime + L"\\models\\ch_PP-OCRv3_rec_infer.onnx", runtime + L"\\models\\ppocr_keys_v1.txt",
                   std::atoi(argv[3])},
                  error)) {
        std::printf("FAIL load: %s\n", error.c_str());
        return 1;
    }
    const int recognize = argc > 4 ? std::atoi(argv[4]) : 1;   // lines read per sample (as the engine)
    int positives = 0, proposed = 0, correct = 0, found = 0, locked = 0, falseBands = 0, moves = 0;   // moves: the band changed after it was proposed
    std::vector<double> ms, foundS, lockS;
    for (const auto& ep : truth) {
        const int w = ep["width"], h = ep["height"];
        const bool positive = !ep["band"].is_null();
        positives += positive;
        live::finder::SubtitleFinder f;
        std::optional<live::finder::Rect> prev;
        double t = 0, foundAt = -1, lockAt = -1;
        for (const auto& fr : ep["frames"]) {
            std::vector<uint8_t> px;
            int fw = 0, fh = 0;
            if (!loadImage(dir + L"\\" + live::win::utf8ToWide(fr["file"].get<std::string>()), px, fw, fh)) {
                std::printf("FAIL cannot read %s\n", fr["file"].get<std::string>().c_str());
                return 1;
            }
            const auto t0 = std::chrono::steady_clock::now();
            const auto obs = live::finder::observe(ocr, px.data(), fw, fh, error, recognize);
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            if (const char* dbg = std::getenv("FINDER_DEBUG"); dbg && ep["episode"].get<std::string>() == dbg)
                for (const auto& o : obs)
                    std::printf("  t%-3.0f %.3f,%.3f-%.3f,%.3f fill %.2f like %.2f '%s'\n", t, o.box.x0, o.box.y0, o.box.x1,
                                o.box.y1, o.fill, live::finder::likeness(o), o.text.c_str());
            f.addSample(obs, t);
            if (const auto now = f.band(); prev && now
                && (std::abs(prev->x0 - now->x0) > 0.01 || std::abs(prev->y0 - now->y0) > 0.01
                    || std::abs(prev->x1 - now->x1) > 0.01 || std::abs(prev->y1 - now->y1) > 0.01))
                ++moves;
            prev = f.band();
            if (foundAt < 0 && f.band()) foundAt = t + live::finder::kIntervalS;   // the sample's own run time
            if (lockAt < 0 && f.state() == live::finder::State::Locked) lockAt = t + live::finder::kIntervalS;
            t += live::finder::kIntervalS;
        }
        const auto band = f.band();
        const bool ok = band && positive && covers(*band, ep["band"], w, h);
        proposed += band.has_value();
        correct += ok;
        if (band && !positive) ++falseBands;   // a band where there is no subtitle at all
        if (band && positive) {
            ++found;
            foundS.push_back(foundAt);
        }
        if (f.state() == live::finder::State::Locked && positive) {
            ++locked;
            lockS.push_back(lockAt);
        }
        std::printf("%s %-5s %s %s conf %.2f found %.0fs lock %.0fs%s\n", ep["episode"].get<std::string>().c_str(),
                    ep["res"].get<std::string>().c_str(), positive ? "sub " : "none", live::finder::stateName(f.state()),
                    f.confidence(), foundAt, lockAt,
                    band ? (ok ? "" : positive ? "  WRONG BAND" : "  FALSE BAND") : (positive ? "  MISSED" : ""));
        if (band && !ok)
            std::printf("    band %.3f,%.3f - %.3f,%.3f\n", band->x0, band->y0, band->x1, band->y1);
    }
    const double precision = proposed ? double(correct) / proposed : 1.0;
    const double recall = positives ? double(correct) / positives : 1.0;
    std::printf("\n%s\nepisodes %zu (%d with subtitles)  precision %.3f  recall %.3f  bands without subtitles %d\n",
                ocr.describe().c_str(), truth.size(), positives, precision, recall, falseBands);
    std::printf("time to found p50 %.0f s max %.0f s · to locked p50 %.0f s max %.0f s (%d of %d locked), band moves after found %d\n",
                pct(foundS, 0.5), pct(foundS, 1.0), pct(lockS, 0.5), pct(lockS, 1.0), locked, found, moves);
    std::printf("sample cost p50 %.1f ms p95 %.1f ms (%s threads; one sample per %.0f s)\n", pct(ms, 0.5), pct(ms, 0.95),
                argv[3], live::finder::kIntervalS);
    // precision first: a wrong band reads the wrong text into the pool's game
    const bool pass = precision >= 0.95 && recall >= 0.85 && falseBands == 0 && pct(ms, 0.5) <= 150;
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

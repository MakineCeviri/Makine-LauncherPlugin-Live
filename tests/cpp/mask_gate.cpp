// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// K3 gate on D-C (tools/make_dc_frames.py): frames in playback order, the OCR runs only when the
// text mask differs from the mask of the last frame it ran on.
//   G-MISS  the mask said "skip" but the subtitle had changed since the last OCR — must be 0
//   rate    OCR runs against the frames played (and against the subtitle changes, the floor)
//   live_mask_gate <D-C dir>

#include "text_mask.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: live_mask_gate <D-C dir>\n");
        return 2;
    }
    const std::filesystem::path dir = std::filesystem::path(reinterpret_cast<const char8_t*>(argv[1]));
    std::ifstream tsv(dir / "truth.tsv", std::ios::binary);
    if (!tsv) {
        std::printf("FAIL no truth.tsv in %s (python tools/make_dc_frames.py)\n", argv[1]);
        return 1;
    }
    live::TextMask last;
    std::string lastTruth = "\x01";
    int frames = 0, runs = 0, changes = 0, misses = 0;
    double maxSame = 0, minChanged = 1;
    std::string prevTruth = "\x01";
    for (std::string line; std::getline(tsv, line);) {
        if (line.empty() || line[0] == '#') continue;
        if (line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        const std::string file = line.substr(0, tab), truth = tab == std::string::npos ? "" : line.substr(tab + 1);
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!loadImage((dir / file).wstring(), px, w, h)) {
            std::printf("FAIL cannot load %s\n", file.c_str());
            return 1;
        }
        ++frames;
        if (truth != prevTruth) ++changes;
        prevTruth = truth;
        const live::TextMask m = live::TextMask::of(px.data(), w, h);
        const double d = last.empty() ? 1.0 : m.difference(last);
        if (truth == lastTruth) maxSame = std::max(maxSame, d);
        else minChanged = std::min(minChanged, d);
        if (!last.empty() && (argc > 2 ? d <= std::atof(argv[2]) : m.sameText(last))) {   // argv[2]: try a threshold
            if (truth != lastTruth) {
                ++misses;
                std::printf("FAIL miss %s (difference %.3f): \"%s\" after \"%s\"\n", file.c_str(), d, truth.c_str(),
                            lastTruth.c_str());
            }
            continue;
        }
        if (truth == lastTruth && std::getenv("MASK_GATE_VERBOSE"))
            std::printf("same %s d=%.2f cells %d/%d\n", file.c_str(), d, m.cells(), last.cells());
        ++runs;
        last = m;
        lastTruth = truth;
    }
    std::printf("D-C: %d frames, %d subtitle changes, OCR ran %d times (%.0f%% of frames), G-MISS %d\n", frames, changes,
                runs, 100.0 * runs / std::max(1, frames), misses);
    std::printf("mask difference: same subtitle max %.3f, changed subtitle min %.3f, threshold %.2f\n", maxSame, minChanged,
                live::TextMask::kSameText);
    return misses == 0 && frames > 0 ? 0 : 1;
}

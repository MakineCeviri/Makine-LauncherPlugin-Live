// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// F1 gate: the plugin's OCR (src/ocr_ort.cpp) must read every benchmark frame exactly as the
// benchmark's "ORT: det v3 + rec ch v3" candidate did (build/ocr_bench/results/ort_v3ch_det.tsv,
// pass 1, in the ocrbench repo). Same text on every frame means the B0 figures (CER, G-ORDER,
// G-SPLIT, G-KEY) hold for the plugin unchanged.
//   live_ocr_gate <bench build dir> <ocr runtime dir> <threads>

#include "garbage.h"
#include "ocr_ort.h"
#include "winutil.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::wstring widen(const std::string& s)
{
    std::wstring w(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    return w;
}

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

std::string escapeField(const std::string& s)   // as the bench writes its .tsv
{
    std::string o;
    for (const char c : s) {
        if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\\') o += "\\\\";
        else o += c;
    }
    return o;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::printf("usage: live_ocr_gate <bench build dir> <ocr runtime dir> <threads>\n");
        return 2;
    }
    const std::wstring bench = widen(argv[1]), runtime = widen(argv[2]);
    live::OcrOrt ocr;
    std::string error;
    if (!ocr.load({runtime + L"\\bin\\onnxruntime.dll", runtime + L"\\models\\ch_PP-OCRv3_det_infer.onnx",
                   runtime + L"\\models\\ch_PP-OCRv3_rec_infer.onnx", runtime + L"\\models\\ppocr_keys_v1.txt",
                   std::atoi(argv[3])},
                  error)) {
        std::printf("FAIL load: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s\n", ocr.describe().c_str());
    std::ifstream tsv(std::filesystem::path(bench + L"\\results\\ort_v3ch_det.tsv"), std::ios::binary);
    if (!tsv) {
        std::printf("FAIL no results/ort_v3ch_det.tsv in the bench dir\n");
        return 1;
    }
    int frames = 0, same = 0, dialogue = 0, grow = 0;
    std::map<std::string, int> sides;
    live::text::Garbage garbage;
    const auto wordFile = argc > 4 ? live::win::readFile(widen(argv[4])) : std::nullopt;
    const bool words = wordFile && garbage.setWords(*wordFile);
    std::vector<double> ms;
    for (std::string line; std::getline(tsv, line);) {
        if (line.empty() || line[0] == '#' || line.rfind("file\t", 0) == 0) continue;
        if (line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        for (size_t b = 0, e; b <= line.size(); b = e + 1) {
            e = line.find('\t', b);
            if (e == std::string::npos) e = line.size();
            f.push_back(line.substr(b, e - b));
        }
        if (f.size() < 11 || f[1] != "1") continue;   // pass 1 only
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!loadImage(bench + L"\\frames\\" + widen(f[0]), px, w, h)) {
            std::printf("FAIL cannot load %s\n", f[0].c_str());
            return 1;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const auto lines = ocr.run(px.data(), w, h, error);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        std::string text;
        for (const auto& l : lines) text += (text.empty() ? "" : "\n") + l.text;
        ++frames;
        if (words) {   // the region-edge rule on real frames: how often would a line read make the region grow?
            std::vector<live::text::OcrBox> boxes;
            for (const auto& l : lines) boxes.push_back({l.text, l.x0, l.y0, l.x1, l.y1});
            if (!garbage.filterOcr(text).empty()) {
                ++dialogue;
                const auto v = garbage.edgeCheck(boxes, w, h);
                if (!v.send) {
                    ++grow;
                    for (const auto& side : v.cut) ++sides[side];
                }
            }
        }
        if (escapeField(text) == f[10]) ++same;
        else std::printf("FAIL %s\n  bench:  %s\n  plugin: %s\n", f[0].c_str(), f[10].c_str(), escapeField(text).c_str());
    }
    std::sort(ms.begin(), ms.end());
    std::printf("%d/%d frames read exactly as the bench, p50 %.1f ms (%s threads)\n", same, frames,
                ms.empty() ? 0.0 : ms[ms.size() / 2], argv[3]);
    if (words) {
        std::string list;
        for (const auto& [side, n] : sides) list += (list.empty() ? "" : ", ") + side + " " + std::to_string(n);
        std::printf("edge rule: %d of %d dialogue frames would grow the region (%s)\n", grow, dialogue, list.c_str());
    }
    return frames > 0 && same == frames ? 0 : 1;
}

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// OCR benchmark (tools/ocr_bench): shared types. Stage names follow docs/OCR_MIMARI.md §3:
// S2+S3 = text mask + projection line split, S4d = detector, S4 = recognizer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bench {

// 8-bit BGRA, top-down, tightly packed — the layout capture.cpp hands to the OCR engine.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> bgra;
};

struct Times {
    double pre = 0;   // detector input: padding, resize, normalisation (S4d pre)
    double det = 0;   // S4d: detector inference + DB post-processing
    double split = 0; // S2+S3: text mask + projection profile (detector-free path)
    double cls = 0;   // angle classifier (crop, resize, inference)
    double rec = 0;   // S4: crop, resize, normalise, inference, CTC decode — all lines of the frame
    double total = 0; // wall time from in-memory BGRA to text (DLL path: includes the BMP round-trip)
    int lines = 0;    // lines sent to the recognizer
};

double nowMs();
bool loadImage(const std::wstring& path, Image& out, std::string& err);
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);

} // namespace bench

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// OCR on the ONNX Runtime C API (F1, OCR_KARAR): PaddleOCR PP-OCRv3 text detection (DB) and
// Chinese recognition (CTC), read straight from the captured BGRA frame — no image file, no
// RapidOcrOnnx. Port of the benchmark's "ORT: det v3 + rec ch v3" path (tools/ocr_bench in the
// ocrbench repo, src/ort_ocr.cpp): same pre-processing, DB post-processing and line grouping,
// so one subtitle line never comes back as two lines in the wrong order (G-ORDER, G-SPLIT).
// onnxruntime.dll is the Microsoft-signed build, loaded by full path from the plugin folder.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace live {

struct OcrOrtConfig {
    std::wstring ortDll;      // full path of onnxruntime.dll (never System32's copy)
    std::wstring detModel;    // ch_PP-OCRv3_det_infer.onnx
    std::wstring recModel;    // ch_PP-OCRv3_rec_infer.onnx
    std::wstring dictFile;    // ppocr_keys_v1.txt, when the model carries no "character" metadata
    int threads = 1;          // intra-op threads: 1 in PC mode, 2 in console mode (K8); spinning off
};

struct OcrLine {
    std::string text;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // the line's boxes together, in frame px
};

class OcrOrt {
public:
    OcrOrt();
    ~OcrOrt();
    OcrOrt(const OcrOrt&) = delete;
    OcrOrt& operator=(const OcrOrt&) = delete;

    /// Loads the runtime and both models and runs one warm-up inference of each.
    bool load(const OcrOrtConfig& cfg, std::string& error);
    /// Text lines of a top-down BGRA frame, top to bottom, with their boxes; boxes on one line are read
    /// left to right.
    std::vector<OcrLine> run(const uint8_t* bgra, int width, int height, std::string& error);

    int threads() const;
    std::string describe() const;   // runtime version, the DLL actually loaded, classes

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace live

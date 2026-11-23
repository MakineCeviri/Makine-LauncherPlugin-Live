// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// Direct ONNX Runtime OCR pipeline for the benchmark: PaddleOCR det (DB) / cls / rec (CTC) re-implemented on the
// ORT C API, plus a detector-free path (text mask + horizontal projection). Parameters mirror the shipped
// RapidOcrOnnx call in src/ocr_rapid.cpp (padding 50, maxSideLen 1024, boxScore 0.6, box 0.3, unclip 2.0).
#pragma once

#include "common.h"

#include <memory>
#include <string>
#include <vector>

namespace bench {

struct OrtConfig {
    std::wstring ortDir;      // folder holding onnxruntime.dll (CPU build or the DirectML build)
    std::wstring detModel;    // empty = detector-free (projection) path
    std::wstring clsModel;    // empty = no angle classifier
    std::wstring recModel;
    std::wstring dictFile;    // used when the model carries no "character" metadata
    int threads = 4;
    bool spin = true;         // session.intra_op.allow_spinning (ORT default: on)
    bool dml = false;         // DirectML execution provider
};

class OrtOcr {
public:
    OrtOcr();
    ~OrtOcr();
    bool init(const OrtConfig& cfg, std::string& err);
    std::vector<std::string> run(const Image& img, Times& t);
    std::string describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace bench

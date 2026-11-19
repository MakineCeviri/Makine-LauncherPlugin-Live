// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

#include "ocr.h"
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace live {

/**
 * RapidOCR engine — uses prebuilt RapidOcrOnnx.dll via C API.
 *
 * RapidOcrOnnx bundles ONNX Runtime + OpenCV + PaddleOCR pipeline
 * in a single DLL with a simple C API (OcrInit/OcrDetect/OcrGetResult).
 *
 * This approach avoids OpenCV/ONNX build complexity — we just call the
 * prebuilt DLL via LoadLibrary + GetProcAddress (cross-compiler safe).
 */
class RapidOcrEngine : public OcrEngine {
public:
    bool init(const std::string& dataPath) override;
    void shutdown() override;
    bool isReady() const override { return m_ready; }

    std::vector<OcrBox> recognize(
        const uint8_t* pixels, int width, int height) override;

    std::string name() const override { return "RapidOCR"; }

    // Recognition model: "ch" (default; robust on game fonts) or "en" (English PP-OCRv3 model, used only when
    // models/en_PP-OCRv3_rec_infer.onnx + en_dict.txt exist). Measured on stylised subtitles the English model
    // occasionally produced garbage lines, so it is opt-in via the `ocrModel` setting. Call before init().
    void setLanguage(const std::string& model) { m_lang = model; }
    const std::string& language() const { return m_lang; }
    const std::string& activeModel() const { return m_activeModel; }
    std::string lastError() const override { return m_error; }

private:
    bool saveBmp(const uint8_t* bgra, int w, int h);
    bool prepareTempDir();

    // RapidOcrOnnx C API types
    using OcrInitFn    = void* (*)(const char*, const char*, const char*, const char*, int);
    using OcrDetectFn  = char (*)(void*, const char*, const char*, void*);
    using OcrGetLenFn  = int (*)(void*);
    using OcrGetResultFn = char (*)(void*, char*, int);
    using OcrDestroyFn = void (*)(void*);

    HMODULE m_hDll = nullptr;
    void* m_handle = nullptr; // OCR_HANDLE

    OcrInitFn    m_fnInit = nullptr;
    OcrDetectFn  m_fnDetect = nullptr;
    OcrGetLenFn  m_fnGetLen = nullptr;
    OcrGetResultFn m_fnGetResult = nullptr;
    OcrDestroyFn m_fnDestroy = nullptr;

    bool m_ready = false;
    std::string m_error;
    std::string m_dataPath;
    std::string m_lang = "ch";
    std::string m_activeModel;

    // Scratch image handed to OcrDetect (RapidOcrOnnx only reads from files).
    // Narrow-char API → ANSI path; created once with FILE_ATTRIBUTE_TEMPORARY so it stays in cache.
    std::wstring m_tempDirW;
    std::string m_tempDirAnsi;   // with trailing backslash, as OcrDetect expects
};

} // namespace live

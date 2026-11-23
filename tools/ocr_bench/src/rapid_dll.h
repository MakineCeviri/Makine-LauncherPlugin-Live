// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// The shipped 0.3.x path, called exactly as src/ocr_rapid.cpp does: BGRA → %TEMP% BMP → RapidOcrOnnx.dll OcrDetect.
// The DLL reports no stage times, so only the total (and the BMP write as `pre`) is known.
#pragma once

#include "common.h"

#include <windows.h>

#include <string>
#include <vector>

namespace bench {

class RapidDll {
public:
    ~RapidDll();
    bool init(const std::wstring& rapidDir, const std::wstring& det, const std::wstring& cls,
              const std::wstring& rec, const std::wstring& keys, int threads, bool doAngle, std::string& err);
    std::vector<std::string> run(const Image& img, Times& t);

private:
    bool saveBmp(const Image& img);

    using InitFn = void* (*)(const char*, const char*, const char*, const char*, int);
    using DetectFn = char (*)(void*, const char*, const char*, void*);
    using GetLenFn = int (*)(void*);
    using GetResultFn = char (*)(void*, char*, int);
    using DestroyFn = void (*)(void*);

    HMODULE m_dll = nullptr;
    void* m_handle = nullptr;
    DetectFn m_detect = nullptr;
    GetLenFn m_getLen = nullptr;
    GetResultFn m_getResult = nullptr;
    DestroyFn m_destroy = nullptr;
    bool m_doAngle = false;
    std::wstring m_tempDirW;
    std::string m_tempDirAnsi;
};

} // namespace bench

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "rapid_dll.h"

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace bench {

namespace {

template <typename Fn>
Fn resolve(HMODULE dll, const char* name)
{
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
}

std::string ansi(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace

RapidDll::~RapidDll()
{
    if (m_handle && m_destroy) m_destroy(m_handle);
    // The DLL is left loaded: RapidOcrOnnx's onnxruntime threads may still be winding down at exit.
}

bool RapidDll::init(const std::wstring& rapidDir, const std::wstring& det, const std::wstring& cls,
                    const std::wstring& rec, const std::wstring& keys, int threads, bool doAngle, std::string& err)
{
    m_doAngle = doAngle;
    m_dll = LoadLibraryExW((rapidDir + L"\\RapidOcrOnnx.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m_dll) { err = "RapidOcrOnnx.dll not loadable from " + narrow(rapidDir); return false; }
    auto init = resolve<InitFn>(m_dll, "OcrInit");
    m_detect = resolve<DetectFn>(m_dll, "OcrDetect");
    m_getLen = resolve<GetLenFn>(m_dll, "OcrGetLen");
    m_getResult = resolve<GetResultFn>(m_dll, "OcrGetResult");
    m_destroy = resolve<DestroyFn>(m_dll, "OcrDestroy");
    if (!init || !m_detect || !m_getLen || !m_getResult || !m_destroy) { err = "RapidOcrOnnx exports missing"; return false; }
    m_handle = init(ansi(det).c_str(), ansi(cls).c_str(), ansi(rec).c_str(), ansi(keys).c_str(), threads);
    if (!m_handle) { err = "OcrInit failed"; return false; }

    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    m_tempDirW = std::wstring(buf, n) + L"MakineLiveBench";
    CreateDirectoryW(m_tempDirW.c_str(), nullptr);
    m_tempDirAnsi = ansi(m_tempDirW) + "\\";
    return true;
}

std::vector<std::string> RapidDll::run(const Image& img, Times& t)
{
    const double t0 = nowMs();
    std::vector<std::string> lines;
    if (!saveBmp(img)) return lines;
    t.pre += nowMs() - t0;

    // ocr_rapid.cpp: {padding 50, maxSideLen 1024, boxScoreThresh 0.6, boxThresh 0.3, unClipRatio 2.0, doAngle, mostAngle}
    struct {
        int padding;
        int maxSideLen;
        float boxScoreThresh;
        float boxThresh;
        float unClipRatio;
        int doAngle;
        int mostAngle;
    } param = {50, 1024, 0.6f, 0.3f, 2.0f, m_doAngle ? 1 : 0, m_doAngle ? 1 : 0};
    if (!m_detect(m_handle, m_tempDirAnsi.c_str(), "capture.bmp", &param)) return lines;
    const int len = m_getLen(m_handle);
    if (len > 0) {
        std::string result(static_cast<size_t>(len), '\0');
        m_getResult(m_handle, result.data(), len);
        result.resize(std::strlen(result.c_str()));
        std::istringstream iss(result);
        for (std::string line; std::getline(iss, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(line);
        }
    }
    t.lines = static_cast<int>(lines.size());
    return lines;
}

bool RapidDll::saveBmp(const Image& img)
{
    // Same bytes and file flags as RapidOcrEngine::saveBmp (top-down 32-bit BMP, FILE_ATTRIBUTE_TEMPORARY).
    const uint32_t imageSize = static_cast<uint32_t>(img.w) * 4 * static_cast<uint32_t>(img.h);
    const uint32_t fileSize = 54 + imageSize, dataOffset = 54, dibSize = 40;
    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    std::memcpy(header + 2, &fileSize, 4);
    std::memcpy(header + 10, &dataOffset, 4);
    std::memcpy(header + 14, &dibSize, 4);
    const int32_t width = img.w, negHeight = -img.h;
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &negHeight, 4);
    const uint16_t planes = 1, bpp = 32;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &imageSize, 4);
    HANDLE f = CreateFileW((m_tempDirW + L"\\capture.bmp").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, header, sizeof(header), &written, nullptr) && written == sizeof(header);
    ok = ok && WriteFile(f, img.bgra.data(), imageSize, &written, nullptr) && written == imageSize;
    CloseHandle(f);
    return ok;
}

} // namespace bench

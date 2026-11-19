// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "ocr_rapid.h"
#include "winutil.h"
#include <cstring>
#include <filesystem>
#include <sstream>

namespace live {

static const wchar_t kImageName[] = L"capture.bmp";

template <typename Fn>
static Fn resolve(HMODULE dll, const char* name)
{
    // Via void* to silence -Wcast-function-type; the signatures come from RapidOcrOnnx's C API.
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
}

bool RapidOcrEngine::init(const std::string& dataPath)
{
    if (m_ready) return true;
    m_dataPath = dataPath;

    const std::wstring dataDir = win::withoutTrailingSlash(win::utf8ToWide(dataPath));
    // RapidOcrOnnx.dll, onnxruntime.dll and models/ ship next to the plugin DLL (.makine root).
    const std::wstring pluginDir = win::moduleDirectory();

    // LOAD_WITH_ALTERED_SEARCH_PATH: resolve RapidOcrOnnx.dll's own dependency (onnxruntime.dll)
    // from the folder it was loaded from, not from the launcher's executable folder.
    auto tryLoad = [](const std::wstring& dir) -> HMODULE {
        if (dir.empty()) return nullptr;
        return LoadLibraryExW((dir + L"\\RapidOcrOnnx.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    };
    m_hDll = tryLoad(pluginDir);
    if (!m_hDll) m_hDll = tryLoad(dataDir);
    if (!m_hDll) {
        m_error = "RapidOcrOnnx.dll yüklenemedi (hata " + win::lastErrorString()
                + "). Dosya ve onnxruntime.dll eklenti klasöründe olmalı.";
        return false;
    }

    m_fnInit      = resolve<OcrInitFn>(m_hDll, "OcrInit");
    m_fnDetect    = resolve<OcrDetectFn>(m_hDll, "OcrDetect");
    m_fnGetLen    = resolve<OcrGetLenFn>(m_hDll, "OcrGetLen");
    m_fnGetResult = resolve<OcrGetResultFn>(m_hDll, "OcrGetResult");
    m_fnDestroy   = resolve<OcrDestroyFn>(m_hDll, "OcrDestroy");

    if (!m_fnInit || !m_fnDetect || !m_fnGetLen || !m_fnGetResult || !m_fnDestroy) {
        m_error = "RapidOcrOnnx.dll beklenen fonksiyonları içermiyor";
        shutdown();
        return false;
    }

    std::wstring modelsDir = pluginDir + L"\\models";
    if (pluginDir.empty() || !win::isDirectory(modelsDir))
        modelsDir = dataDir + L"\\models";
    if (!win::isDirectory(modelsDir)) {
        m_error = "OCR modelleri bulunamadı: " + win::wideToUtf8(modelsDir);
        shutdown();
        return false;
    }

    // OcrInit takes narrow (ANSI) paths
    const std::string modelsAnsi = win::ansiPath(modelsDir);
    if (modelsAnsi.empty()) {
        m_error = "Model klasörünün yolu sistem kod sayfasında gösterilemiyor: " + win::wideToUtf8(modelsDir);
        shutdown();
        return false;
    }
    const std::string detModel = modelsAnsi + "\\ch_PP-OCRv3_det_infer.onnx";
    const std::string clsModel = modelsAnsi + "\\ch_ppocr_mobile_v2.0_cls_infer.onnx";
    // English source text: the English recognition model keeps word spaces; the Chinese model
    // often glues English words together ("Iwasgoingtofindyou"), which breaks translation and pool matching.
    const bool useEnglish = m_lang == "en"
        && GetFileAttributesW((modelsDir + L"\\en_PP-OCRv3_rec_infer.onnx").c_str()) != INVALID_FILE_ATTRIBUTES
        && GetFileAttributesW((modelsDir + L"\\en_dict.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    m_activeModel = useEnglish ? "en" : "ch";
    const std::string recModel = modelsAnsi + (useEnglish ? "\\en_PP-OCRv3_rec_infer.onnx" : "\\ch_PP-OCRv3_rec_infer.onnx");
    const std::string keysPath = modelsAnsi + (useEnglish ? "\\en_dict.txt" : "\\ppocr_keys_v1.txt");

    m_handle = m_fnInit(detModel.c_str(), clsModel.c_str(), recModel.c_str(), keysPath.c_str(), 4);
    if (!m_handle) {
        m_error = "OcrInit başarısız — model dosyalarını kontrol edin: " + win::wideToUtf8(modelsDir);
        shutdown();
        return false;
    }

    if (!prepareTempDir()) {
        shutdown();
        return false;
    }

    m_ready = true;
    return true;
}

bool RapidOcrEngine::prepareTempDir()
{
    // %TEMP%\MakineLive, else %ProgramData%\MakineLive\tmp when %TEMP% has characters
    // outside the ANSI code page (OpenCV's imread can't open such paths).
    std::wstring candidates[2];
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    if (n > 0 && n <= MAX_PATH) candidates[0] = win::withoutTrailingSlash(buf) + L"\\MakineLive";
    n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH + 1);
    if (n > 0 && n <= MAX_PATH) candidates[1] = win::withoutTrailingSlash(buf) + L"\\MakineLive\\tmp";

    for (const auto& dir : candidates) {
        if (dir.empty()) continue;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!win::isDirectory(dir)) continue;
        std::string ansi = win::ansiPath(dir);
        if (ansi.empty()) continue;
        m_tempDirW = dir;
        m_tempDirAnsi = ansi + "\\";
        return true;
    }
    m_error = "OCR için geçici klasör oluşturulamadı";
    return false;
}

void RapidOcrEngine::shutdown()
{
    if (m_handle && m_fnDestroy) {
        m_fnDestroy(m_handle);
        m_handle = nullptr;
    }
    if (m_hDll) {
        FreeLibrary(m_hDll);
        m_hDll = nullptr;
    }
    m_fnInit = nullptr;
    m_fnDetect = nullptr;
    m_fnGetLen = nullptr;
    m_fnGetResult = nullptr;
    m_fnDestroy = nullptr;
    m_ready = false;
}

std::vector<OcrBox> RapidOcrEngine::recognize(
    const uint8_t* pixels, int width, int height)
{
    if (!m_ready || !m_handle) return {};

    if (!saveBmp(pixels, width, height)) {
        m_error = "OCR görüntüsü yazılamadı (hata " + win::lastErrorString() + ")";
        return {};
    }

    struct {
        int padding;
        int maxSideLen;
        float boxScoreThresh;
        float boxThresh;
        float unClipRatio;
        int doAngle;
        int mostAngle;
    } param = {50, 1024, 0.6f, 0.3f, 2.0f, 0, 0};  // doAngle/mostAngle off: subtitles are horizontal (skips the cls pass)

    // OcrDetect expects imgPath = directory (with trailing \) and imgName = file name
    char ok = m_fnDetect(m_handle, m_tempDirAnsi.c_str(), "capture.bmp", &param);
    if (!ok) {
        m_error = "OcrDetect failed";
        return {};
    }

    int resultLen = m_fnGetLen(m_handle);
    if (resultLen <= 0) return {};

    std::string result(static_cast<size_t>(resultLen), '\0');
    m_fnGetResult(m_handle, result.data(), resultLen);
    result.resize(std::strlen(result.c_str()));

    // RapidOCR output: recognized text lines mixed with box/score diagnostics.
    std::vector<OcrBox> boxes;
    std::istringstream iss(result);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '-') continue;
        if (line.find("boxCount") != std::string::npos
            || line.find("×") != std::string::npos
            || line.find("score") != std::string::npos)
            continue;
        OcrBox box;
        box.text = line;
        box.confidence = 0.9f;  // RapidOcrOnnx text output carries no per-line score
        box.width = width;
        boxes.push_back(std::move(box));
    }
    return boxes;
}

bool RapidOcrEngine::saveBmp(const uint8_t* bgra, int w, int h)
{
    const uint32_t rowSize = static_cast<uint32_t>(w) * 4;
    const uint32_t imageSize = rowSize * static_cast<uint32_t>(h);
    const uint32_t fileSize = 54 + imageSize;

    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    std::memcpy(header + 2, &fileSize, 4);
    const uint32_t dataOffset = 54;
    std::memcpy(header + 10, &dataOffset, 4);
    const uint32_t dibSize = 40;
    std::memcpy(header + 14, &dibSize, 4);
    const int32_t width = w, negHeight = -h;  // top-down
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &negHeight, 4);
    const uint16_t planes = 1, bpp = 32;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &imageSize, 4);

    // FILE_ATTRIBUTE_TEMPORARY: the cache manager keeps the file in RAM instead of flushing to disk.
    const std::wstring path = m_tempDirW + L"\\" + kImageName;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, header, sizeof(header), &written, nullptr) && written == sizeof(header);
    ok = ok && WriteFile(f, bgra, imageSize, &written, nullptr) && written == imageSize;
    CloseHandle(f);
    return ok;
}

} // namespace live

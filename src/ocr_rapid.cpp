#include "ocr_rapid.h"
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace live {

bool RapidOcrEngine::init(const std::string& dataPath)
{
    if (m_ready) return true;
    m_dataPath = dataPath;

    // Normalize path separators
    std::string dllDir = dataPath;
    for (auto& c : dllDir) if (c == '/') c = '\\';

    // Find plugin's own directory (where .makine was extracted)
    // RapidOcrOnnx.dll lives next to the plugin DLL, not in dataPath
    static const char s_anchor = 0; // address anchor for GetModuleHandleEx
    std::string pluginDir;
    {
        char modulePath[MAX_PATH]{};
        HMODULE hSelf = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            &s_anchor,
            &hSelf);
        if (hSelf) {
            GetModuleFileNameA(hSelf, modulePath, MAX_PATH);
            pluginDir = modulePath;
            auto sep = pluginDir.find_last_of("\\/");
            if (sep != std::string::npos) pluginDir = pluginDir.substr(0, sep);
        }
    }

    // Search order: plugin dir → data dir → system DLL search path
    m_hDll = nullptr;
    if (!pluginDir.empty())
        m_hDll = LoadLibraryA((pluginDir + "\\RapidOcrOnnx.dll").c_str());
    if (!m_hDll)
        m_hDll = LoadLibraryA((dllDir + "\\RapidOcrOnnx.dll").c_str());
    if (!m_hDll)
        m_hDll = LoadLibraryA("RapidOcrOnnx.dll");
    if (!m_hDll) {
        m_error = "Cannot load RapidOcrOnnx.dll (error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    // Resolve C API functions
    m_fnInit      = reinterpret_cast<OcrInitFn>(GetProcAddress(m_hDll, "OcrInit"));
    m_fnDetect    = reinterpret_cast<OcrDetectFn>(GetProcAddress(m_hDll, "OcrDetect"));
    m_fnGetLen    = reinterpret_cast<OcrGetLenFn>(GetProcAddress(m_hDll, "OcrGetLen"));
    m_fnGetResult = reinterpret_cast<OcrGetResultFn>(GetProcAddress(m_hDll, "OcrGetResult"));
    m_fnDestroy   = reinterpret_cast<OcrDestroyFn>(GetProcAddress(m_hDll, "OcrDestroy"));

    if (!m_fnInit || !m_fnDetect || !m_fnGetLen || !m_fnGetResult || !m_fnDestroy) {
        m_error = "RapidOcrOnnx.dll missing required exports";
        FreeLibrary(m_hDll);
        m_hDll = nullptr;
        return false;
    }

    // Initialize OCR with model paths
    // Search: plugin dir/models → data dir/models
    std::string modelsDir = dllDir + "\\models";
    if (!pluginDir.empty()) {
        std::string tryDir = pluginDir + "\\models";
        DWORD attr = GetFileAttributesA(tryDir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            modelsDir = tryDir;
    }
    std::string detModel = modelsDir + "\\ch_PP-OCRv3_det_infer.onnx";
    std::string clsModel = modelsDir + "\\ch_ppocr_mobile_v2.0_cls_infer.onnx";
    std::string recModel = modelsDir + "\\ch_PP-OCRv3_rec_infer.onnx";
    std::string keysPath = modelsDir + "\\ppocr_keys_v1.txt";

    m_handle = m_fnInit(detModel.c_str(), clsModel.c_str(), recModel.c_str(), keysPath.c_str(), 4);
    if (!m_handle) {
        m_error = "OcrInit failed — check model files in " + modelsDir;
        return false;
    }

    m_ready = true;
    return true;
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

    // Save BGRA pixels as BMP to temp file (RapidOCR reads from file)
    std::string bmpDir = m_dataPath;
    for (auto& c : bmpDir) if (c == '/') c = '\\';
    fs::create_directories(bmpDir);

    std::string bmpPath = bmpDir + "\\capture.bmp";
    if (!saveBmp(pixels, width, height, bmpPath)) {
        m_error = "Failed to save capture BMP";
        return {};
    }

    // OcrDetect expects imgPath=directory (with trailing \) and imgName=filename
    std::string imgDir = bmpDir + "\\";
    std::string imgName = "capture.bmp";

    // Set OCR parameters
    struct {
        int padding;
        int maxSideLen;
        float boxScoreThresh;
        float boxThresh;
        float unClipRatio;
        int doAngle;
        int mostAngle;
    } param = {50, 1024, 0.6f, 0.3f, 2.0f, 1, 1};

    // Run OCR
    char ok = m_fnDetect(m_handle, imgDir.c_str(), imgName.c_str(), &param);
    if (!ok) {
        m_error = "OcrDetect failed";
        return {};
    }

    // Get result
    int resultLen = m_fnGetLen(m_handle);
    if (resultLen <= 0) return {};

    std::string result(resultLen, '\0');
    m_fnGetResult(m_handle, result.data(), resultLen);

    // Parse result text — each line is a detected text with box info
    std::vector<OcrBox> boxes;
    std::istringstream iss(result);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '-') continue;
        // RapidOCR output format: text lines mixed with box coordinates
        // Simple parse: extract non-empty text lines
        if (!line.empty() && line.find("boxCount") == std::string::npos
            && line.find("×") == std::string::npos
            && line.find("score") == std::string::npos) {
            OcrBox box;
            box.text = line;
            box.confidence = 0.9f;
            box.x = 0; box.y = 0;
            box.width = width; box.height = 0;
            boxes.push_back(std::move(box));
        }
    }

    return boxes;
}

bool RapidOcrEngine::saveBmp(const uint8_t* bgra, int w, int h, const std::string& path)
{
    const int rowSize = w * 4;
    const int imageSize = rowSize * h;
    const int fileSize = 54 + imageSize;

    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;

    // BMP Header
    uint8_t bmpHeader[14] = {};
    bmpHeader[0] = 'B'; bmpHeader[1] = 'M';
    std::memcpy(bmpHeader + 2, &fileSize, 4);
    int dataOffset = 54;
    std::memcpy(bmpHeader + 10, &dataOffset, 4);
    ofs.write(reinterpret_cast<char*>(bmpHeader), 14);

    // DIB Header
    uint8_t dibHeader[40] = {};
    int dibSize = 40;
    std::memcpy(dibHeader, &dibSize, 4);
    std::memcpy(dibHeader + 4, &w, 4);
    int negH = -h;
    std::memcpy(dibHeader + 8, &negH, 4);
    uint16_t planes = 1, bpp = 32;
    std::memcpy(dibHeader + 12, &planes, 2);
    std::memcpy(dibHeader + 14, &bpp, 2);
    std::memcpy(dibHeader + 20, &imageSize, 4);
    ofs.write(reinterpret_cast<char*>(dibHeader), 40);

    // Pixel data
    ofs.write(reinterpret_cast<const char*>(bgra), imageSize);
    return ofs.good();
}

} // namespace live

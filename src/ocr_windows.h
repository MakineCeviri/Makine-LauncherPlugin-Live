#pragma once

#include "ocr.h"
#include <string>

namespace live {

/**
 * Windows OCR engine — uses Windows.Media.Ocr via PowerShell.
 * Zero dependencies, works on Windows 10+ with language packs.
 *
 * Approach inspired by LunaTranslator's WindowsOCR implementation.
 * Uses a helper PowerShell script that:
 *   1. Loads a BMP file
 *   2. Runs OcrEngine.RecognizeAsync()
 *   3. Outputs recognized text lines to stdout
 */
class WindowsOcrEngine : public OcrEngine {
public:
    bool init(const std::string& dataPath) override;
    void shutdown() override;
    bool isReady() const override { return m_ready; }

    std::vector<OcrBox> recognize(
        const uint8_t* pixels, int width, int height) override;

    std::string name() const override { return "WindowsOCR"; }
    std::string lastError() const override { return m_error; }

private:
    bool saveBmp(const uint8_t* pixels, int w, int h, const std::string& path);
    std::string runOcrScript(const std::string& imagePath);

    bool m_ready = false;
    std::string m_dataPath;
    std::string m_error;
    std::string m_scriptPath;
};

} // namespace live

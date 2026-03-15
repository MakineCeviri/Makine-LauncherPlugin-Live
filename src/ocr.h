#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace live {

struct OcrBox {
    std::string text;
    float confidence = 0.0f;
    int x = 0, y = 0, width = 0, height = 0;
};

// OCR engine interface — implementations: RapidOCR, Tesseract
class OcrEngine {
public:
    virtual ~OcrEngine() = default;

    virtual bool init(const std::string& modelDir) = 0;
    virtual void shutdown() = 0;
    virtual bool isReady() const = 0;

    // Recognize text in BGRA image
    virtual std::vector<OcrBox> recognize(
        const uint8_t* pixels, int width, int height) = 0;

    virtual std::string name() const = 0;
    virtual std::string lastError() const = 0;
};

// Stub OCR engine (returns empty results — placeholder for RapidOCR integration)
class StubOcrEngine : public OcrEngine {
public:
    bool init(const std::string& /*modelDir*/) override { m_ready = true; return true; }
    void shutdown() override { m_ready = false; }
    bool isReady() const override { return m_ready; }

    std::vector<OcrBox> recognize(
        const uint8_t* /*pixels*/, int /*width*/, int /*height*/) override
    {
        // TODO: Replace with RapidOcrOnnx implementation
        return {};
    }

    std::string name() const override { return "Stub"; }
    std::string lastError() const override { return ""; }

private:
    bool m_ready = false;
};

} // namespace live

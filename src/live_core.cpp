/**
 * Live plugin core — initialization, shutdown, and pipeline coordination.
 */

#include "capture.h"
#include "ocr.h"
#include <memory>
#include <string>

namespace live {

static std::unique_ptr<ScreenCapture> s_capture;
static std::unique_ptr<OcrEngine> s_ocr;
static std::string s_dataPath;

bool init(const char* dataPath)
{
    s_dataPath = dataPath;

    // Initialize screen capture (GDI fallback)
    s_capture = std::make_unique<ScreenCapture>();
    if (!s_capture->init(CaptureMethod::Auto)) {
        // Non-fatal: capture can be retried later
    }

    // Initialize OCR engine (stub for now — RapidOCR to be integrated)
    s_ocr = std::make_unique<StubOcrEngine>();
    std::string modelDir = s_dataPath + "/models";
    s_ocr->init(modelDir);

    return true;
}

void shutdown()
{
    if (s_ocr) {
        s_ocr->shutdown();
        s_ocr.reset();
    }
    if (s_capture) {
        s_capture->shutdown();
        s_capture.reset();
    }
}

bool ready()
{
    return s_capture != nullptr;
}

} // namespace live

/**
 * Live plugin core — OCR + Translation pipeline with configurable settings.
 */

#include "capture.h"
#include "ocr_rapid.h"
#include "translator.h"
#include "settings.h"
#include <memory>
#include <string>

namespace live {

static std::unique_ptr<ScreenCapture> s_capture;
static std::unique_ptr<OcrEngine> s_ocr;
static std::unique_ptr<Translator> s_translator;
static Settings s_settings;
static std::string s_dataPath;

static void applySettings()
{
    if (s_translator) {
        std::string engine = s_settings.get("translatorEngine", "gpt");
        if (engine == "deepl")
            s_translator->setEngine(TranslatorEngine::DeepL);
        else if (engine == "google")
            s_translator->setEngine(TranslatorEngine::Google);
        else
            s_translator->setEngine(TranslatorEngine::GPT);

        s_translator->setApiKey(s_settings.get("apiKey", ""));
        s_translator->setSourceLang(s_settings.get("sourceLang", "en"));
        s_translator->setTargetLang(s_settings.get("targetLang", "tr"));
        s_translator->setModel(s_settings.get("gptModel", "gpt-4o-mini"));
    }
}

bool init(const char* dataPath)
{
    s_dataPath = dataPath;

    // Load settings
    std::string settingsPath = s_dataPath + "/settings.cfg";
    for (auto& c : settingsPath) if (c == '/') c = '\\';
    s_settings.load(settingsPath);

    // Initialize screen capture
    s_capture = std::make_unique<ScreenCapture>();
    s_capture->init(CaptureMethod::Auto);

    // Initialize OCR
    s_ocr = std::make_unique<RapidOcrEngine>();
    s_ocr->init(s_dataPath);

    // Initialize translator with saved settings
    s_translator = std::make_unique<Translator>();
    applySettings();

    return true;
}

void shutdown()
{
    // Save settings on shutdown
    s_settings.save();

    s_translator.reset();
    if (s_ocr) { s_ocr->shutdown(); s_ocr.reset(); }
    if (s_capture) { s_capture->shutdown(); s_capture.reset(); }
}

bool ready()
{
    return s_capture != nullptr;
}

const char* getSetting(const char* key)
{
    static std::string result;
    result = s_settings.get(key ? key : "");
    return result.c_str();
}

void setSetting(const char* key, const char* value)
{
    if (!key) return;
    s_settings.set(key, value ? value : "");
    s_settings.save();
    applySettings();
}

std::string captureAndRecognize(void* windowHandle, int x, int y, int w, int h)
{
    if (!s_capture || !s_ocr || !s_ocr->isReady()) return "";

    CapturedFrame frame;
    CaptureRegion region{x, y, w, h};

    if (!s_capture->captureRegion(windowHandle, region, frame))
        return "CAPTURE_FAILED: " + s_capture->lastError();

    auto results = s_ocr->recognize(frame.pixels.data(), frame.width, frame.height);

    std::string text;
    for (const auto& box : results) {
        if (!text.empty()) text += "\n";
        text += box.text;
    }
    return text;
}

std::string captureOcrAndTranslate(void* windowHandle, int x, int y, int w, int h)
{
    std::string ocrText = captureAndRecognize(windowHandle, x, y, w, h);
    if (ocrText.empty() || !s_translator) return ocrText;

    // Skip translation if no API key
    if (s_settings.get("apiKey").empty()) return ocrText;

    auto result = s_translator->translate(ocrText);
    if (result.success)
        return result.translated;

    return ocrText; // fallback to OCR text if translation fails
}

} // namespace live

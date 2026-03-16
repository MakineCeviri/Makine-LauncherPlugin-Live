/**
 * Live plugin core — World-class OCR + Translation pipeline.
 *
 * Performance layers:
 *   1. DXGI capture (~5ms) with GDI fallback
 *   2. Frame change detection (FNV-1a sampled hash) — skip unchanged frames
 *   3. OCR text similarity check — skip translate if <10% text change
 *   4. Translation LRU cache (200 entries) — instant return for repeated text
 *   5. Context-aware GPT — previous translations as context for consistency
 *   6. Pre-allocated capture buffer reuse
 */

#include "capture.h"
#include "ocr_rapid.h"
#include "translator.h"
#include "settings.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <algorithm>

namespace live {

// ── Core State ──

static std::unique_ptr<ScreenCapture> s_capture;
static std::unique_ptr<OcrEngine> s_ocr;
static std::unique_ptr<Translator> s_translator;
static Settings s_settings;
static std::string s_dataPath;

// ── Frame Change Detection ──

static uint64_t fnvHash(const uint8_t* data, size_t size)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    // Sample ~2500 pixels spread across the frame
    size_t step = size > 10000 ? size / 2500 : 4;
    for (size_t i = 0; i < size; i += step) {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static uint64_t s_lastFrameHash = 0;
static std::string s_cachedResult;
static std::string s_lastOcrText;     // Raw OCR text (for dual-phase display)
static CapturedFrame s_frameBuffer;   // Pre-allocated capture buffer reuse

// ── Translation LRU Cache ──
// Avoids redundant API calls — instant return for repeated/seen text

static constexpr size_t kCacheMaxSize = 200;

struct CacheEntry {
    std::string sourceText;
    std::string translatedText;
};

static std::list<CacheEntry> s_cacheList;
static std::unordered_map<size_t, std::list<CacheEntry>::iterator> s_cacheMap;

static size_t textHash(const std::string& text)
{
    size_t hash = 0xcbf29ce484222325ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static std::string cacheLookup(const std::string& text)
{
    size_t h = textHash(text);
    auto it = s_cacheMap.find(h);
    if (it != s_cacheMap.end() && it->second->sourceText == text) {
        // Move to front (most recently used)
        auto entry = *it->second;
        s_cacheList.erase(it->second);
        s_cacheList.push_front(entry);
        s_cacheMap[h] = s_cacheList.begin();
        return entry.translatedText;
    }
    return "";
}

static void cacheInsert(const std::string& text, const std::string& translation)
{
    size_t h = textHash(text);
    auto it = s_cacheMap.find(h);
    if (it != s_cacheMap.end()) {
        s_cacheList.erase(it->second);
        s_cacheMap.erase(it);
    }
    s_cacheList.push_front({text, translation});
    s_cacheMap[h] = s_cacheList.begin();
    // Evict oldest entries
    while (s_cacheList.size() > kCacheMaxSize) {
        auto last = std::prev(s_cacheList.end());
        s_cacheMap.erase(textHash(last->sourceText));
        s_cacheList.pop_back();
    }
}

// ── Fuzzy Text Matching ──
// If OCR text changed by <10%, use cached translation (handles minor OCR jitter)

static std::string s_prevOcrText;
static std::string s_prevTranslation;

static int levenshteinDistance(const std::string& a, const std::string& b)
{
    const size_t m = a.size(), n = b.size();
    if (m == 0) return static_cast<int>(n);
    if (n == 0) return static_cast<int>(m);

    // Optimization: limit comparison length for very long texts
    size_t maxLen = std::min(m, size_t(300));
    size_t maxLen2 = std::min(n, size_t(300));

    // Single-row DP (space-optimized)
    std::vector<int> prev(maxLen2 + 1), curr(maxLen2 + 1);
    for (size_t j = 0; j <= maxLen2; j++) prev[j] = static_cast<int>(j);

    for (size_t i = 1; i <= maxLen; i++) {
        curr[0] = static_cast<int>(i);
        for (size_t j = 1; j <= maxLen2; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[maxLen2];
}

static bool isTextSimilar(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty()) return false;
    // Quick length check — if length differs by >30%, definitely different
    float lenRatio = static_cast<float>(a.size()) / static_cast<float>(b.size());
    if (lenRatio < 0.7f || lenRatio > 1.3f) return false;
    // Quick prefix/suffix check
    size_t checkLen = std::min(a.size(), size_t(20));
    if (a.substr(0, checkLen) == b.substr(0, checkLen)) return true;
    // Levenshtein for short texts
    if (a.size() < 300) {
        int dist = levenshteinDistance(a, b);
        float threshold = static_cast<float>(a.size()) * 0.1f;
        return dist <= static_cast<int>(threshold);
    }
    return false;
}

// ── Context-Aware Translation History ──
// Sends last N translations as context to GPT for game terminology consistency

static constexpr size_t kHistoryMaxSize = 5;
static std::vector<std::pair<std::string, std::string>> s_translationHistory;

static void recordTranslation(const std::string& source, const std::string& translation)
{
    s_translationHistory.push_back({source, translation});
    if (s_translationHistory.size() > kHistoryMaxSize)
        s_translationHistory.erase(s_translationHistory.begin());
}

static std::string getTranslationContext()
{
    if (s_translationHistory.empty()) return "";
    std::string ctx;
    for (const auto& [src, tgt] : s_translationHistory)
        ctx += src + " -> " + tgt + "\n";
    return ctx;
}

// ── Settings ──

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

    if (s_capture) {
        std::string captureMethodStr = s_settings.get("captureMethod", "auto");
        CaptureMethod method = CaptureMethod::Auto;
        if (captureMethodStr == "dxgi") method = CaptureMethod::DXGI;
        else if (captureMethodStr == "gdi") method = CaptureMethod::GDI;

        if (s_capture->activeMethod() != method) {
            s_capture->shutdown();
            s_capture->init(method);
        }
    }
}

// ── Lifecycle ──

bool init(const char* dataPath)
{
    s_dataPath = dataPath;

    std::string settingsPath = s_dataPath + "/settings.cfg";
    for (auto& c : settingsPath) if (c == '/') c = '\\';
    s_settings.load(settingsPath);

    std::string captureMethodStr = s_settings.get("captureMethod", "auto");
    CaptureMethod method = CaptureMethod::Auto;
    if (captureMethodStr == "dxgi") method = CaptureMethod::DXGI;
    else if (captureMethodStr == "gdi") method = CaptureMethod::GDI;

    s_capture = std::make_unique<ScreenCapture>();
    s_capture->init(method);

    s_ocr = std::make_unique<RapidOcrEngine>();
    s_ocr->init(s_dataPath);

    s_translator = std::make_unique<Translator>();
    applySettings();

    // Reset all state
    s_lastFrameHash = 0;
    s_cachedResult.clear();
    s_lastOcrText.clear();
    s_prevOcrText.clear();
    s_prevTranslation.clear();
    s_cacheList.clear();
    s_cacheMap.clear();
    s_translationHistory.clear();

    // Pre-allocate frame buffer (1920x1080 BGRA = ~8MB)
    s_frameBuffer.pixels.reserve(1920 * 1080 * 4);

    return true;
}

void shutdown()
{
    s_settings.save();
    s_translator.reset();
    if (s_ocr) { s_ocr->shutdown(); s_ocr.reset(); }
    if (s_capture) { s_capture->shutdown(); s_capture.reset(); }
    s_cacheList.clear();
    s_cacheMap.clear();
    s_translationHistory.clear();
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

// ── Last OCR text accessor (for dual-phase display) ──

const char* getLastOcrText()
{
    return s_lastOcrText.c_str();
}

// ── OCR Pipeline ──

std::string captureAndRecognize(void* windowHandle, int x, int y, int w, int h)
{
    if (!s_capture || !s_ocr || !s_ocr->isReady()) return "";

    CaptureRegion region{x, y, w, h};
    if (!s_capture->captureRegion(windowHandle, region, s_frameBuffer))
        return "CAPTURE_FAILED: " + s_capture->lastError();

    auto results = s_ocr->recognize(
        s_frameBuffer.pixels.data(), s_frameBuffer.width, s_frameBuffer.height);

    std::string text;
    for (const auto& box : results) {
        if (!text.empty()) text += "\n";
        text += box.text;
    }
    s_lastOcrText = text;
    return text;
}

std::string captureOcrAndTranslate(void* windowHandle, int x, int y, int w, int h)
{
    // Check if OCR is enabled
    if (s_settings.get("ocrEnabled", "false") != "true") return "";
    if (!s_capture || !s_ocr || !s_ocr->isReady()) return "";

    CaptureRegion region{x, y, w, h};
    if (!s_capture->captureRegion(windowHandle, region, s_frameBuffer))
        return "CAPTURE_FAILED: " + s_capture->lastError();

    // Layer 1: Frame change detection — skip everything if pixels haven't changed
    uint64_t frameHash = fnvHash(s_frameBuffer.pixels.data(), s_frameBuffer.pixels.size());
    if (frameHash == s_lastFrameHash && !s_cachedResult.empty()) {
        return s_cachedResult;  // ~0ms — instant return
    }
    s_lastFrameHash = frameHash;

    // Layer 2: Run OCR
    auto results = s_ocr->recognize(
        s_frameBuffer.pixels.data(), s_frameBuffer.width, s_frameBuffer.height);

    std::string ocrText;
    for (const auto& box : results) {
        if (!ocrText.empty()) ocrText += "\n";
        ocrText += box.text;
    }
    s_lastOcrText = ocrText;  // Store for dual-phase display

    if (ocrText.empty()) {
        s_cachedResult.clear();
        return "";
    }

    // Layer 3: Fuzzy text similarity — if OCR text barely changed, reuse last translation
    if (!s_prevOcrText.empty() && isTextSimilar(ocrText, s_prevOcrText)
        && !s_prevTranslation.empty()) {
        s_cachedResult = s_prevTranslation;
        return s_cachedResult;  // ~0ms — fuzzy match
    }

    // Layer 4: Translation LRU cache — exact match from history
    std::string cached = cacheLookup(ocrText);
    if (!cached.empty()) {
        s_prevOcrText = ocrText;
        s_prevTranslation = cached;
        s_cachedResult = cached;
        return cached;  // ~0ms — cache hit
    }

    // Layer 5: Translate (API call — this is the expensive part)
    if (!s_translator || s_settings.get("apiKey").empty()) {
        s_cachedResult = ocrText;
        return ocrText;
    }

    // Set translation context for GPT (game terminology consistency)
    std::string context = getTranslationContext();
    s_translator->setContext(context);

    auto result = s_translator->translate(ocrText);
    if (result.success) {
        // Store in all caches
        cacheInsert(ocrText, result.translated);
        recordTranslation(ocrText, result.translated);
        s_prevOcrText = ocrText;
        s_prevTranslation = result.translated;
        s_cachedResult = result.translated;
        return result.translated;
    }

    // Fallback to raw OCR text
    s_cachedResult = ocrText;
    return ocrText;
}

} // namespace live

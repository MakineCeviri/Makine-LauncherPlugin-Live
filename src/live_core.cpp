// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

/**
 * Live plugin core — OCR + translation pipeline.
 *
 * Performance layers:
 *   1. DXGI capture (~5ms) with GDI fallback
 *   2. Frame change detection (full-frame 64-bit hash) — skip unchanged frames
 *   3. OCR text similarity check — reuse translation if text changed by <10% (OCR jitter)
 *   4. Translation LRU cache (200 entries) — instant return for repeated text
 *   5. Context-aware GPT — previous translations as context for consistency
 *   6. Pre-allocated capture buffer reuse
 *
 * Errors never travel in the text channel: on failure the functions return "" and
 * lastError() explains why (exported as makine_get_last_error).
 */

#include "capture.h"
#include "ocr_rapid.h"
#include "translator.h"
#include "settings.h"
#include "text_fix.h"
#include "winutil.h"
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
static std::unique_ptr<RapidOcrEngine> s_ocr;
static std::unique_ptr<Translator> s_translator;
static Settings s_settings;
static std::string s_dataPath;
static std::string s_lastError;

// ── Frame Change Detection ──
// Hashes every byte (8 at a time); ~0.1 ms for a 1000×200 subtitle region. Sampling a
// subset of pixels can miss a subtitle change and keep showing the previous line.

static uint64_t frameHash(const uint8_t* data, size_t size)
{
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ size;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        std::memcpy(&w, data + i, 8);
        h ^= w;
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 32;
    }
    for (; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static uint64_t s_lastFrameHash = 0;      // capture_ocr_translate path
static uint64_t s_lastOcrFrameHash = 0;   // capture_and_ocr path (kept separate so the paths don't mask each other)
static std::string s_lastOcrResult;
static SpacingFixer s_spacing;
static std::string s_cachedResult;
static std::string s_cachedError;     // error that produced s_cachedResult (kept for unchanged frames)
static std::string s_lastOcrText;     // Raw OCR text (for dual-phase display)
static CapturedFrame s_frameBuffer;   // Pre-allocated capture buffer reuse

// ── Translation LRU Cache ──

static constexpr size_t kCacheMaxSize = 200;

struct CacheEntry {
    std::string sourceText;
    std::string translatedText;
};

static std::list<CacheEntry> s_cacheList;
static std::unordered_map<std::string, std::list<CacheEntry>::iterator> s_cacheMap;

static std::string cacheLookup(const std::string& text)
{
    auto it = s_cacheMap.find(text);
    if (it == s_cacheMap.end()) return "";
    s_cacheList.splice(s_cacheList.begin(), s_cacheList, it->second);  // move to front
    return it->second->translatedText;
}

static void cacheInsert(const std::string& text, const std::string& translation)
{
    auto it = s_cacheMap.find(text);
    if (it != s_cacheMap.end()) {
        it->second->translatedText = translation;
        s_cacheList.splice(s_cacheList.begin(), s_cacheList, it->second);
        return;
    }
    s_cacheList.push_front({text, translation});
    s_cacheMap[text] = s_cacheList.begin();
    while (s_cacheList.size() > kCacheMaxSize) {
        s_cacheMap.erase(s_cacheList.back().sourceText);
        s_cacheList.pop_back();
    }
}

// ── Fuzzy Text Matching ──
// If OCR text changed by <10%, reuse the last translation (handles minor OCR jitter).

static std::string s_prevOcrText;
static std::string s_prevTranslation;

static constexpr size_t kMaxFuzzyLen = 600;

static size_t levenshteinDistance(const std::string& a, const std::string& b)
{
    const size_t m = a.size(), n = b.size();
    if (m == 0) return n;
    if (n == 0) return m;

    std::vector<size_t> prev(n + 1), curr(n + 1);
    for (size_t j = 0; j <= n; j++) prev[j] = j;
    for (size_t i = 1; i <= m; i++) {
        curr[0] = i;
        for (size_t j = 1; j <= n; j++) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[n];
}

static bool isTextSimilar(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    const size_t longer = std::max(a.size(), b.size());
    const size_t shorter = std::min(a.size(), b.size());
    // Length differs by >30% → different line. (No prefix shortcut: many dialogue lines share
    // their first words, e.g. "I don't know what you mean" / "I don't know what you did".)
    if (shorter * 10 < longer * 7) return false;
    if (longer > kMaxFuzzyLen) return false;
    return levenshteinDistance(a, b) * 10 <= longer;
}

// ── Context-Aware Translation History ──

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
    std::string ctx;
    for (const auto& [src, tgt] : s_translationHistory)
        ctx += src + " -> " + tgt + "\n";
    return ctx;
}

// ── Settings ──

static CaptureMethod captureMethodFromSettings()
{
    const std::string m = s_settings.get("captureMethod", "auto");
    if (m == "dxgi") return CaptureMethod::DXGI;
    if (m == "gdi") return CaptureMethod::GDI;
    return CaptureMethod::Auto;
}

static void applySettings()
{
    s_lastOcrFrameHash = 0;  // OCR post-processing settings may have changed

    if (s_translator) {
        // Default: Makine Live server (account token, no user API key). gpt/deepl/google = direct, with apiKey.
        const std::string engine = s_settings.get("translatorEngine", "makine");
        if (engine == "deepl")
            s_translator->setEngine(TranslatorEngine::DeepL);
        else if (engine == "google")
            s_translator->setEngine(TranslatorEngine::Google);
        else if (engine == "gpt")
            s_translator->setEngine(TranslatorEngine::GPT);
        else
            s_translator->setEngine(TranslatorEngine::Makine);

        s_translator->setApiKey(s_settings.get("apiKey", ""));
        s_translator->setServer(s_settings.get("serverUrl", "https://makineceviri.org"));
        s_translator->setAuthToken(s_settings.get("authToken", ""));
        s_translator->setGame(s_settings.get("gameSlug", ""), s_settings.get("gameName", ""));
        s_translator->setSourceLang(s_settings.get("sourceLang", "en"));
        s_translator->setTargetLang(s_settings.get("targetLang", "tr"));
        s_translator->setModel(s_settings.get("gptModel", "gpt-4o-mini"));
    }

    // OCR recognition model ("ch" default, "en" opt-in — see ocr_rapid.h)
    const std::string ocrModel = s_settings.get("ocrModel", "ch");
    if (s_ocr && s_ocr->language() != ocrModel) {
        s_ocr->shutdown();
        s_ocr->setLanguage(ocrModel);
        s_lastFrameHash = 0;
        if (!s_ocr->init(s_dataPath)) s_lastError = s_ocr->lastError();
    }

    // Compare against what was *requested*: Auto resolves to DXGI/GDI, so comparing with the
    // active method would restart capture on every settings change.
    const CaptureMethod method = captureMethodFromSettings();
    if (s_capture && s_capture->requestedMethod() != method) {
        s_capture->shutdown();
        s_lastFrameHash = 0;
        if (!s_capture->init(method)) {
            s_lastError = "Ekran yakalama başlatılamadı: " + s_capture->lastError();
            s_capture->init(CaptureMethod::Auto);
        }
    }
}

// ── Lifecycle ──

static void resetState()
{
    s_lastFrameHash = 0;
    s_lastOcrFrameHash = 0;
    s_lastOcrResult.clear();
    s_cachedResult.clear();
    s_lastOcrText.clear();
    s_prevOcrText.clear();
    s_prevTranslation.clear();
    s_cacheList.clear();
    s_cacheMap.clear();
    s_translationHistory.clear();
}

bool init(const char* dataPath)
{
    s_lastError.clear();
    s_dataPath = dataPath;
    while (!s_dataPath.empty() && (s_dataPath.back() == '/' || s_dataPath.back() == '\\'))
        s_dataPath.pop_back();

    std::string settingsPath = s_dataPath + "\\settings.cfg";
    for (auto& c : settingsPath) if (c == '/') c = '\\';
    s_settings.load(settingsPath);

    s_capture = std::make_unique<ScreenCapture>();
    if (!s_capture->init(captureMethodFromSettings()) && !s_capture->init(CaptureMethod::Auto)) {
        s_lastError = "Ekran yakalama başlatılamadı: " + s_capture->lastError();
        s_capture.reset();
        return false;
    }

    s_ocr = std::make_unique<RapidOcrEngine>();
    s_ocr->setLanguage(s_settings.get("ocrModel", "ch"));

    // Word list for restoring dropped spaces in English OCR output (optional; resources/en_words.txt)
    const std::wstring pluginDir = win::moduleDirectory();
    if (!s_spacing.load(pluginDir + L"\\resources\\en_words.txt"))
        s_spacing.load(win::utf8ToWide(s_dataPath) + L"\\resources\\en_words.txt");
    if (!s_ocr->init(s_dataPath)) {
        s_lastError = s_ocr->lastError();
        s_ocr.reset();
        s_capture.reset();
        return false;
    }

    s_translator = std::make_unique<Translator>();
    applySettings();
    resetState();

    s_frameBuffer.pixels.reserve(1920 * 1080 * 4);
    return true;
}

void shutdown()
{
    s_settings.save();
    s_translator.reset();
    if (s_ocr) { s_ocr->shutdown(); s_ocr.reset(); }
    if (s_capture) { s_capture->shutdown(); s_capture.reset(); }
    resetState();
}

bool ready()
{
    return s_capture && s_ocr && s_ocr->isReady();
}

const char* lastError()
{
    return s_lastError.c_str();
}

const char* getSetting(const char* key)
{
    static std::string result;
    const std::string k = key ? key : "";
    // Read-only diagnostics
    if (k == "_ocrModel") result = s_ocr ? s_ocr->activeModel() : "";
    else result = s_settings.get(k);
    return result.c_str();
}

void setSetting(const char* key, const char* value)
{
    if (!key || !*key) return;
    if (!s_settings.set(key, value ? value : "")) return;
    s_settings.save();
    applySettings();
}

const char* getLastOcrText()
{
    return s_lastOcrText.c_str();
}

// ── OCR Pipeline ──

static bool captureAndRunOcr(void* windowHandle, int x, int y, int w, int h, uint64_t& lastHash,
                             std::string& ocrText, bool& unchanged)
{
    unchanged = false;
    s_lastError.clear();
    if (!ready()) { s_lastError = "Eklenti hazır değil"; return false; }

    if (!s_capture->captureRegion(windowHandle, CaptureRegion{x, y, w, h}, s_frameBuffer)) {
        s_lastError = "Ekran yakalanamadı: " + s_capture->lastError();
        return false;
    }

    // Layer 1: frame change detection — skip OCR if no pixel changed
    // (region geometry is mixed in so moving the region always re-runs OCR)
    uint64_t hash = frameHash(s_frameBuffer.pixels.data(), s_frameBuffer.pixels.size());
    hash ^= (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32 | static_cast<uint32_t>(y)) * 0x9E3779B97F4A7C15ULL;
    hash ^= (static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32 | static_cast<uint32_t>(h)) * 0xC2B2AE3D27D4EB4FULL;
    if (hash == lastHash) {
        unchanged = true;
        return true;
    }
    lastHash = hash;

    // Layer 2: OCR
    auto results = s_ocr->recognize(s_frameBuffer.pixels.data(), s_frameBuffer.width, s_frameBuffer.height);
    ocrText.clear();
    for (const auto& box : results) {
        if (!ocrText.empty()) ocrText += "\n";
        ocrText += box.text;
    }
    // Layer 2b: Latin-script games — CJK tokens are recognition noise from the Chinese model ("园图B")
    const std::string srcLang = s_settings.get("sourceLang", "en");
    if (isLatinScriptLanguage(srcLang))
        ocrText = dropForeignScriptTokens(ocrText);
    // Layer 2c: restore word spaces OCR dropped in English text ("Iwas goingto" → "I was going to")
    if (srcLang == "en" && s_settings.get("fixSpacing", "true") == "true")
        ocrText = s_spacing.fix(ocrText);
    s_lastOcrText = ocrText;
    return true;
}

std::string captureAndRecognize(void* windowHandle, int x, int y, int w, int h)
{
    // Unchanged pixels → previous text without running OCR (~ms instead of ~80 ms), so callers can poll fast.
    std::string text;
    bool unchanged = false;
    if (!captureAndRunOcr(windowHandle, x, y, w, h, s_lastOcrFrameHash, text, unchanged)) {
        s_lastOcrFrameHash = 0;
        return "";
    }
    if (unchanged) return s_lastOcrResult;
    s_lastOcrResult = text;
    return text;
}

std::string captureOcrAndTranslate(void* windowHandle, int x, int y, int w, int h)
{
    if (s_settings.get("ocrEnabled", "false") != "true") return "";

    std::string ocrText;
    bool unchanged = false;
    if (!captureAndRunOcr(windowHandle, x, y, w, h, s_lastFrameHash, ocrText, unchanged)) return "";
    if (unchanged) {  // ~0ms — same pixels as last call
        s_lastError = s_cachedError;
        return s_cachedResult;
    }
    s_cachedError.clear();

    if (ocrText.empty()) {
        s_cachedResult.clear();
        return "";
    }

    // Layer 3: fuzzy similarity — OCR jitter on the same line reuses the last translation
    if (!s_prevTranslation.empty() && isTextSimilar(ocrText, s_prevOcrText)) {
        s_cachedResult = s_prevTranslation;
        return s_cachedResult;
    }

    // Layer 4: exact LRU cache hit
    std::string cached = cacheLookup(ocrText);
    if (!cached.empty()) {
        s_prevOcrText = ocrText;
        s_prevTranslation = cached;
        s_cachedResult = cached;
        return cached;
    }

    // Layer 5: translation API (the expensive part)
    if (!s_translator || !s_translator->ready()) {
        s_cachedResult = ocrText;
        return ocrText;
    }

    s_translator->setContext(getTranslationContext());
    auto result = s_translator->translate(ocrText);
    if (result.success) {
        cacheInsert(ocrText, result.translated);
        recordTranslation(ocrText, result.translated);
        s_prevOcrText = ocrText;
        s_prevTranslation = result.translated;
        s_cachedResult = result.translated;
        return result.translated;
    }

    // Translation failed: show the raw OCR text and report why
    s_lastError = "Çeviri başarısız: " + result.error;
    s_cachedError = s_lastError;
    s_cachedResult = ocrText;
    return ocrText;
}

} // namespace live

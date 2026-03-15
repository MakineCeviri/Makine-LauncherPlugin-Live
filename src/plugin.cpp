/**
 * MakineAI Live Plugin — Entry point + C ABI exports
 *
 * Exports:
 *   Base: get_info, initialize, shutdown, is_ready, get_last_error
 *   OCR:  capture_and_ocr, capture_ocr_translate
 *   Settings: get_setting, set_setting
 */

#include <makineai/plugin/plugin_api.h>
#include <cstring>
#include <string>

namespace live {
    bool init(const char* dataPath);
    void shutdown();
    bool ready();
    std::string captureAndRecognize(void* windowHandle, int x, int y, int w, int h);
    std::string captureOcrAndTranslate(void* windowHandle, int x, int y, int w, int h);
    const char* getSetting(const char* key);
    void setSetting(const char* key, const char* value);
}

static bool s_initialized = false;
static char s_error[512] = "";

// ── Required Plugin Exports ──

extern "C" __declspec(dllexport)
MakineAiPluginInfo makineai_get_info(void)
{
    return {
        "com.makineceviri.live",
        "MakineAI Live",
        "0.1.0",
        MAKINEAI_PLUGIN_API_VERSION
    };
}

extern "C" __declspec(dllexport)
MakineAiError makineai_initialize(const char* dataPath)
{
    if (s_initialized) return MAKINEAI_OK;
    if (!dataPath) {
        std::strncpy(s_error, "dataPath is null", sizeof(s_error) - 1);
        return MAKINEAI_ERR_INVALID_PARAM;
    }
    if (!live::init(dataPath))
        return MAKINEAI_ERR_INIT_FAILED;
    s_initialized = true;
    return MAKINEAI_OK;
}

extern "C" __declspec(dllexport)
void makineai_shutdown(void)
{
    if (s_initialized) { live::shutdown(); s_initialized = false; }
}

extern "C" __declspec(dllexport)
bool makineai_is_ready(void)
{
    return s_initialized && live::ready();
}

extern "C" __declspec(dllexport)
const char* makineai_get_last_error(void)
{
    return s_error;
}

// ── OCR Exports ──

static std::string s_lastOcr;
static std::string s_lastTranslation;

extern "C" __declspec(dllexport)
const char* makineai_capture_and_ocr(void* windowHandle, int x, int y, int w, int h)
{
    if (!s_initialized) return "";
    s_lastOcr = live::captureAndRecognize(windowHandle, x, y, w, h);
    return s_lastOcr.c_str();
}

extern "C" __declspec(dllexport)
const char* makineai_capture_ocr_translate(void* windowHandle, int x, int y, int w, int h)
{
    if (!s_initialized) return "";
    s_lastTranslation = live::captureOcrAndTranslate(windowHandle, x, y, w, h);
    return s_lastTranslation.c_str();
}

// ── Settings Exports ──

extern "C" __declspec(dllexport)
const char* makineai_get_setting(const char* key)
{
    if (!s_initialized) return "";
    return live::getSetting(key);
}

extern "C" __declspec(dllexport)
void makineai_set_setting(const char* key, const char* value)
{
    if (!s_initialized) return;
    live::setSetting(key, value);
}

/**
 * MakineAI Live Plugin — Entry point
 *
 * Provides real-time screen OCR + translation + overlay pipeline.
 * Pipeline: Screen Capture → OCR → Translation → Overlay Display
 */

#include <makineai/plugin/plugin_api.h>
#include <cstring>
#include <string>

// Forward declarations
namespace live {
    bool init(const char* dataPath);
    void shutdown();
    bool ready();
    std::string captureAndRecognize(void* windowHandle, int x, int y, int w, int h);
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
    if (s_initialized)
        return MAKINEAI_OK;

    if (!dataPath) {
        std::strncpy(s_error, "dataPath is null", sizeof(s_error) - 1);
        return MAKINEAI_ERR_INVALID_PARAM;
    }

    if (!live::init(dataPath)) {
        return MAKINEAI_ERR_INIT_FAILED;
    }

    s_initialized = true;
    return MAKINEAI_OK;
}

extern "C" __declspec(dllexport)
void makineai_shutdown(void)
{
    if (s_initialized) {
        live::shutdown();
        s_initialized = false;
    }
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

// ── Live-specific exports ──

// Capture a screen region and run OCR, returns recognized text
// windowHandle: HWND of the target window (or nullptr for desktop)
// x, y, w, h: capture region in window coordinates
static std::string s_lastResult;

extern "C" __declspec(dllexport)
const char* makineai_capture_and_ocr(void* windowHandle, int x, int y, int w, int h)
{
    if (!s_initialized) {
        std::strncpy(s_error, "Plugin not initialized", sizeof(s_error) - 1);
        return "";
    }

    s_lastResult = live::captureAndRecognize(windowHandle, x, y, w, h);
    return s_lastResult.c_str();
}

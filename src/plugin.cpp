/**
 * MakineAI Live Plugin — Entry point
 *
 * Provides real-time screen OCR + translation + overlay pipeline.
 * Pipeline: Screen Capture → OCR → Translation → Overlay Display
 */

#include <makineai/plugin/plugin_api.h>
#include <cstring>

// Forward declarations
namespace live {
    bool init(const char* dataPath);
    void shutdown();
    bool ready();
}

static bool s_initialized = false;
static char s_error[512] = "";
static char s_dataPath[1024] = "";

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

    std::strncpy(s_dataPath, dataPath, sizeof(s_dataPath) - 1);

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

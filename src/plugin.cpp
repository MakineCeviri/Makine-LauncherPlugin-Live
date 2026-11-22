// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

/**
 * Makine Live Plugin — Entry point + C ABI exports
 *
 * Exports:
 *   Base: get_info, initialize, shutdown, is_ready, get_last_error
 *   OCR:  capture_and_ocr, capture_ocr_translate, get_last_ocr_text
 *   Settings: get_setting, set_setting
 *
 * Threading: every export takes one plugin-wide lock, so calls from several launcher
 * threads are serialized (a capture_ocr_translate call can hold it for an API round-trip).
 *
 * String lifetime: returned `const char*` are UTF-8, owned by the plugin and valid until the
 * next call of the SAME function. Callers must copy them. C# callers must declare the return
 * type as IntPtr and use Marshal.PtrToStringUTF8 — declaring `string` makes the marshaller
 * free plugin-owned memory and crash.
 */

#include <makine/plugin/plugin_api.h>
#include <cstring>
#include <mutex>
#include <string>

namespace live {
    bool init(const char* dataPath);
    void shutdown();
    bool ready();
    const char* lastError();
    std::string captureAndRecognize(void* windowHandle, int x, int y, int w, int h);
    std::string captureOcrAndTranslate(void* windowHandle, int x, int y, int w, int h);
    const char* getSetting(const char* key);
    void setSetting(const char* key, const char* value);
    const char* getLastOcrText();
}

static std::mutex s_mutex;
static bool s_initialized = false;
static std::string s_error;

static void setError(const char* message)
{
    s_error = message ? message : "";
}

// -- Required Plugin Exports --

extern "C" __declspec(dllexport)
MakinePluginInfo makine_get_info(void)
{
    return {
        "com.makineceviri.live",
        "Makine Live",
        "0.3.2",
        MAKINE_PLUGIN_API_VERSION
    };
}

extern "C" __declspec(dllexport)
MakineError makine_initialize(const char* dataPath)
{
    std::lock_guard lock(s_mutex);
    if (s_initialized) return MAKINE_OK;
    if (!dataPath || !*dataPath) {
        setError("dataPath is null");
        return MAKINE_ERR_INVALID_PARAM;
    }
    if (!live::init(dataPath)) {
        setError(live::lastError());
        return MAKINE_ERR_INIT_FAILED;
    }
    setError("");
    s_initialized = true;
    return MAKINE_OK;
}

extern "C" __declspec(dllexport)
void makine_shutdown(void)
{
    std::lock_guard lock(s_mutex);
    if (s_initialized) { live::shutdown(); s_initialized = false; }
}

extern "C" __declspec(dllexport)
bool makine_is_ready(void)
{
    std::lock_guard lock(s_mutex);
    return s_initialized && live::ready();
}

extern "C" __declspec(dllexport)
const char* makine_get_last_error(void)
{
    std::lock_guard lock(s_mutex);
    if (s_initialized) setError(live::lastError());
    return s_error.c_str();
}

// -- OCR Exports --
// Return "" on failure; makine_get_last_error() says why.

static std::string s_lastOcr;
static std::string s_lastTranslation;

extern "C" __declspec(dllexport)
const char* makine_capture_and_ocr(void* windowHandle, int x, int y, int w, int h)
{
    std::lock_guard lock(s_mutex);
    if (!s_initialized) return "";
    s_lastOcr = live::captureAndRecognize(windowHandle, x, y, w, h);
    return s_lastOcr.c_str();
}

extern "C" __declspec(dllexport)
const char* makine_capture_ocr_translate(void* windowHandle, int x, int y, int w, int h)
{
    std::lock_guard lock(s_mutex);
    if (!s_initialized) return "";
    s_lastTranslation = live::captureOcrAndTranslate(windowHandle, x, y, w, h);
    return s_lastTranslation.c_str();
}

// -- Settings Exports --

extern "C" __declspec(dllexport)
const char* makine_get_setting(const char* key)
{
    std::lock_guard lock(s_mutex);
    if (!s_initialized) return "";
    return live::getSetting(key);
}

extern "C" __declspec(dllexport)
void makine_set_setting(const char* key, const char* value)
{
    std::lock_guard lock(s_mutex);
    if (!s_initialized) return;
    live::setSetting(key, value);
}

// -- Dual-Phase Display Export --
// Returns raw OCR text from the last capture (before translation)

extern "C" __declspec(dllexport)
const char* makine_get_last_ocr_text(void)
{
    std::lock_guard lock(s_mutex);
    if (!s_initialized) return "";
    return live::getLastOcrText();
}

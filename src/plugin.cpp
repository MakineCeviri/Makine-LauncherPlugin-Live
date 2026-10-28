// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// Makine Live: reads a screen region (OCR), translates it and shows it over the
// game through the launcher's overlay. Plugin ABI 2 (makine/plugin/sdk.hpp).

#include "config.h"
#include "ocr_rapid.h"
#include "winutil.h"
#include "worker.h"

#include <makine/plugin/sdk.hpp>
#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

using json = nlohmann::json;
using makine::plugin::Result;

namespace {

std::string stringOf(const json& j, const char* key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// One value from init.settings or settings.changed. A value of the wrong shape
// keeps the current one; the launcher validates against the manifest anyway.
void applySetting(live::Config& c, const std::string& key, const json& v)
{
    if (key == "provider" && v.is_string()) {
        const std::string p = v.get<std::string>();
        if (p == "openai" || p == "groq" || p == "openrouter" || p == "ollama"
            || p == "lmstudio" || p == "compatible" || p == "deepl" || p == "google") c.provider = p;
    } else if (key == "apiKey" && v.is_string()) {
        c.apiKey = v.get<std::string>();
    } else if (key == "model" && v.is_string()) {
        c.model = v.get<std::string>();
    } else if (key == "baseUrl" && v.is_string()) {
        c.baseUrl = v.get<std::string>();
    } else if (key == "sourceLang" && v.is_string()) {
        c.sourceLang = v.get<std::string>();
    } else if (key == "targetLang" && v.is_string()) {
        c.targetLang = v.get<std::string>();
    } else if (key == "interval" && v.is_number()) {
        c.intervalMs = std::clamp(v.get<int>(), 250, 10000);   // manifest min/max
    } else if (key == "showSource" && v.is_boolean()) {
        c.showSource = v.get<bool>();
    } else if (key == "autoStart" && v.is_boolean()) {
        c.autoStart = v.get<bool>();
    }
}

class LivePlugin : public makine::plugin::Plugin {
public:
    MakineStatus initialize(std::string_view initJson) override
    {
        const json init = json::parse(initJson, nullptr, false);
        const std::wstring dataDir = live::widen(stringOf(init, "dataDir"));
        const std::wstring pluginDir = live::widen(stringOf(init, "pluginDir"));
        if (dataDir.empty() || pluginDir.empty()) {
            host().error("init: dataDir ya da pluginDir yok");
            return MAKINE_E_INVALID_ARGUMENT;
        }
        live::Config config;
        if (init.contains("settings") && init.at("settings").is_object())
            for (const auto& [key, value] : init.at("settings").items()) applySetting(config, key, value);

        auto ocr = std::make_unique<live::RapidOcr>();
        std::string error;
        if (const MakineStatus status = ocr->load(pluginDir, dataDir, error); status != MAKINE_OK) {
            // The host reports the last error line as the reason initialize failed.
            host().error(error);
            return status;
        }
        host().info("OCR hazır (RapidOcrOnnx 1.2.3, İngilizce model) · kod sayfası " + std::to_string(GetACP()));

        m_worker = std::make_shared<live::Worker>(host(), dataDir, std::move(ocr), std::move(config),
                                                  live::loadRegion(dataDir));
        m_worker->launch();
        return MAKINE_OK;
    }

    void shutdown() override
    {
        if (!m_worker) return;
        m_worker->shutdown();
        m_worker.reset();
    }

    Result invoke(std::string_view method, std::string_view /*paramsJson*/) override
    {
        if (!m_worker) return Result::error(MAKINE_E_FAILED, "not initialized");
        if (method == "pickRegion") return m_worker->pickRegion();
        if (method == "start") return m_worker->start();
        if (method == "stop") return m_worker->stop();
        if (method == "readOnce") return m_worker->readOnce();
        return Result::notFound(method);
    }

    void onEvent(std::string_view event, std::string_view payloadJson) override
    {
        if (!m_worker) return;
        if (event == "settings.changed") {
            const json p = json::parse(payloadJson, nullptr, false);
            const std::string key = stringOf(p, "key");
            if (key.empty()) return;
            live::Config config = *m_worker->config();
            applySetting(config, key, p.contains("value") ? p.at("value") : json());
            m_worker->setConfig(std::move(config));
        } else if (event == "game.started") {
            if (m_worker->config()->autoStart && m_worker->hasRegion()) m_worker->start();
        } else if (event == "game.stopped") {
            m_worker->stop();
        }
    }

private:
    std::shared_ptr<live::Worker> m_worker;
};

} // namespace

MAKINE_PLUGIN(LivePlugin)

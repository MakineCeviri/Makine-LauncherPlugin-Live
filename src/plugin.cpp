// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// Makine Live as a Makine Launcher plugin (ABI 2, makine/plugin/sdk.hpp): reads the
// subtitle region of a PC game or of a console mirrored on the PC, translates it through
// the shared pool on makineceviri.org and shows it over the game through the launcher's
// overlay. The launcher's Makine Live screen talks to it with invoke() and draws ui.state
// (README "Launcher screen API").
//
// initialize only reads settings and starts the threads; the OCR runtime loads on the
// first start / readOnce. The ABI 1 exports of the Python test harness are in abi1.cpp.

#include "engine.h"
#include "winutil.h"

#include <makine/plugin/sdk.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

using json = nlohmann::json;
using makine::plugin::Result;

namespace {

class LivePlugin : public makine::plugin::Plugin {
public:
    MakineStatus initialize(std::string_view initJson) override
    {
        const json init = json::parse(initJson, nullptr, false);
        const auto str = [&](const char* key) {
            return init.is_object() && init.contains(key) && init[key].is_string() ? init[key].get<std::string>()
                                                                                   : std::string();
        };
        live::EngineInit e;
        e.dataDir = live::win::withoutTrailingSlash(live::win::utf8ToWide(str("dataDir")));
        e.pluginDir = live::win::withoutTrailingSlash(live::win::utf8ToWide(str("pluginDir")));
        e.version = str("version").empty() ? MAKINE_LIVE_VERSION : str("version");
        e.launcherVersion = str("launcherVersion").empty() ? "unknown" : str("launcherVersion");
        if (init.is_object() && init.contains("settings")) e.settings = init["settings"];
        if (e.dataDir.empty() || e.pluginDir.empty()) {
            host().error("init: dataDir ya da pluginDir yok");
            return MAKINE_E_INVALID_ARGUMENT;
        }
        std::error_code ec;
        std::filesystem::create_directories(e.dataDir, ec);   // never pluginDir: read-only under MSIX
        m_engine = std::make_shared<live::Engine>(host(), std::move(e));
        m_engine->launch();
        host().info(std::string("Makine Live ") + MAKINE_LIVE_VERSION + " hazır");
        return MAKINE_OK;
    }

    void shutdown() override
    {
        if (!m_engine) return;
        m_engine->shutdown();
        m_engine.reset();
    }

    Result invoke(std::string_view method, std::string_view paramsJson) override
    {
        if (!m_engine) return Result::error(MAKINE_E_FAILED, "not initialized");
        return m_engine->invoke(method, json::parse(paramsJson, nullptr, false));
    }

    void onEvent(std::string_view event, std::string_view payloadJson) override
    {
        if (m_engine) m_engine->onEvent(event, json::parse(payloadJson, nullptr, false));
    }

private:
    std::shared_ptr<live::Engine> m_engine;
};

} // namespace

MAKINE_PLUGIN(LivePlugin)

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// The Makine Live API on makineceviri.org (/api/live/*; docs/MIMARI.md §4), as
// makine_live/net/pool_client.py speaks it: device-code login with the Makine account,
// profile, translate, local pool sync/hits, server status. WinHTTP, one session kept
// alive (keep-alive connections). Thread-safe: each call opens its own request.

#include "jsonutil.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <string>

namespace live {

struct ApiResult {
    int status = 0;          // HTTP status; 0 = no answer (network error)
    nlohmann::json data;     // the JSON body (object), {} when none
    std::string error;       // data.error, or the network error, for the user

    bool ok() const { return status >= 200 && status < 300; }
    bool network() const { return status == 0; }
    std::string code() const { return jstr(data, "code"); }
};

class ServerClient {
public:
    ServerClient(std::string baseUrl, std::string pluginVersion, std::string launcherVersion, std::string deviceId,
                 std::string deviceName);
    ~ServerClient();

    void setToken(std::string token);
    std::string token() const;

    ApiResult deviceStart();                                   // {device, code, url, expires_in, interval}
    ApiResult devicePoll(const std::string& deviceSecret);     // status "approved" → token, once
    ApiResult me();
    ApiResult appLogin(const std::string& username, const std::string& password);   // team accounts
    ApiResult logout();
    /// scene: the line's speaker and context ({speaker?, context?, scene_id?, script_line_id?}), all optional
    ApiResult translate(const std::string& game, const std::string& gameName, const std::string& text,
                        const std::string& sourceLang, const std::string& targetLang,
                        const nlohmann::json& scene = nlohmann::json::object());
    ApiResult poolSync(const std::string& game, const std::string& rev, bool head);
    ApiResult poolHits(const std::string& game, const nlohmann::json& hits);
    ApiResult status();
    /// K13: a cheap request that leaves a warm keep-alive connection for the translation that follows.
    void ping();
    double idleSeconds() const;
    /// Usage report (makine_live/net/telemetry.py): → {beat_s, log_request}; payload.bye on the way out.
    ApiResult beat(const nlohmann::json& payload, int timeoutMs = 6000);
    /// The log the team asked for (beat → log_request), gzip.
    ApiResult uploadLog(const std::string& requestId, const std::string& gzip);
    /// The log went to Sentry Logs (host API telemetry.logs) instead: the request is answered.
    ApiResult logSent(const std::string& requestId);
    ApiResult announcementRead(int64_t id);

    /// Aborts requests in flight and refuses new ones (shutdown).
    void cancel();

private:
    ApiResult call(const wchar_t* method, const std::string& pathAndQuery, const nlohmann::json* payload,
                   int timeoutMs, const std::string* gzipBody = nullptr);

    bool m_secure = true;
    std::wstring m_host;
    int m_port = 443;
    std::string m_basePath;
    std::string m_userAgent, m_version, m_client, m_device, m_deviceName;

    mutable std::mutex m_mutex;
    std::string m_token;
    void* m_session = nullptr;   // HINTERNET
    std::atomic<bool> m_cancelled{false};
    std::atomic<int64_t> m_lastRequestMs{0};
};

/// Per-user device id for this plugin: 32 hex characters, kept in <dataDir>\device_id.
std::string loadDeviceId(const std::wstring& dataDir);

} // namespace live

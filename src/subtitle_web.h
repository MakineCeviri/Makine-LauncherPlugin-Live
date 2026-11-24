// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// The translation on a phone / tablet (/) and as an OBS browser source (/obs), streamed with
// Server-Sent Events (/events). Port of makine_live/tv/subtitle_web.py, same pages and events.
// A minimal Winsock server: it listens on 127.0.0.1 only (OBS is on the same PC) unless the
// phone screen is on; then it listens on the home network and Windows asks the user once
// whether makine-plugin-host.exe may accept connections on private networks. The plugin never
// adds a firewall rule itself.

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace live {

std::string lanIp();   // this PC's address on the home network (the default route's interface)
bool isPrivateLan(const std::string& ip);   // 10/8, 172.16/12, 192.168/16

class SubtitleWeb : public std::enable_shared_from_this<SubtitleWeb> {
public:
    static constexpr int kPort = 47712;
    static constexpr int kPortTries = 8;

    explicit SubtitleWeb(std::string logo = {});   // logo: the bytes of /logo.webp

    /// Opens, closes or re-opens the server (lan: on every interface instead of 127.0.0.1).
    void configure(bool enabled, bool lan);
    void stop() { configure(false, false); }

    void sendLine(int id, const std::string& text, const std::string& kind, int chunk = 0, int chunks = 1);   // chunk i of n (fit.h)
    void clear();
    void setStyle(const nlohmann::json& style);   // subtitle_web keys: size, opacity, text, band, font, outline, …

    bool running() const;
    bool lan() const;
    int port() const;
    int clients() const;
    std::string error() const;
    std::string obsUrl() const;
    std::string phoneUrl() const;
    /// Called (no lock held) when running / clients change.
    void onChange(std::function<void()> fn);

private:
    void acceptLoop(uintptr_t listener, uint64_t generation);
    void serve(uintptr_t sock, uint64_t generation);
    void serveBody(uintptr_t sock, uint64_t generation);
    void stream(uintptr_t sock, uint64_t generation);
    nlohmann::json snapshotLocked() const;
    void bump();

    const std::string m_logo;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    uint64_t m_version = 0, m_generation = 0;
    nlohmann::json m_line = nullptr;
    std::deque<nlohmann::json> m_history;
    nlohmann::json m_style = {{"size", 26}, {"opacity", 78}};
    uintptr_t m_listener = ~uintptr_t(0);
    bool m_lan = false;
    int m_port = 0, m_clients = 0;
    std::string m_error, m_lanIp;
    std::function<void()> m_onChange;
    std::atomic<int> m_connections{0};
};

} // namespace live

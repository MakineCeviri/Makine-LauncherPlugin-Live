// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Makine Live → Makine TV (port of makine_live/tv/tv_link.py, protocol v1, home network):
//   discovery  UDP 47710 broadcast {"t":"discover","v":1} → {"t":"here","v":1,"id","name","port"}
//   stream     TCP <port>, one UTF-8 JSON per line:
//              PC → TV hello{v,code,pc} line{id,text,kind} clear{} style{size,pos,opacity} ping{}
//              TV → PC ok{tv} | bad_code{}
// The pairing code is the 4 digits on the TV screen. Sending runs on its own thread and the
// newest line wins: a line the TV could not take in time is skipped.

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace live {

struct TvInfo {
    std::string id, name, host;
    int port = 0;
    nlohmann::json toJson() const { return {{"id", id}, {"name", name}, {"host", host}, {"port", port}}; }
    static std::optional<TvInfo> fromJson(const nlohmann::json& j);
};

constexpr int kTvDiscoveryPort = 47710;
constexpr int kTvPort = 47711;   // the TVs' default stream port ("connect by address")

/// Makine TVs that answer on the home network within `timeoutMs`.
std::vector<TvInfo> discoverTvs(int timeoutMs = 1500, int port = kTvDiscoveryPort,
                                const std::vector<std::string>& targets = {});   // targets: tests (default: broadcast)

class TvLink : public std::enable_shared_from_this<TvLink> {
public:
    explicit TvLink(std::string pcName, int discoveryPort = kTvDiscoveryPort);

    void start();
    void close();
    void pair(std::optional<TvInfo> tv, const std::string& code);   // nullopt: let go

    void sendLine(int id, const std::string& text, const std::string& kind);
    void clear();
    void setStyle(int size, const std::string& pos, int opacity);

    /// idle | connecting | connected | bad_code | offline, and the TV's name
    std::pair<std::string, std::string> state() const;
    std::optional<TvInfo> tv() const;
    /// Called (on the link's thread, no lock held) when state() changes.
    void onChange(std::function<void()> fn);

private:
    void run();
    std::string session(int gen, const TvInfo& tv, const std::string& code);
    void setState(const std::string& state, const std::string& detail);
    void put(nlohmann::json msg);

    const std::string m_pcName;
    const int m_discoveryPort;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::optional<TvInfo> m_tv;
    std::string m_code;
    nlohmann::json m_line = nullptr, m_style = nullptr;
    nlohmann::json m_shown = nullptr;   // the line on screen: sent again when the TV (re)connects
    std::function<void()> m_onChange;
    int m_gen = 0;
    bool m_closed = false;
    std::string m_state = "idle", m_detail;
};

} // namespace live

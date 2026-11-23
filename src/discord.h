// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// "Makine Live ile Türkçe oynuyor" on the player's Discord profile (port of
// makine_live/net/discord_presence.py). Talks to the Discord desktop app over its local IPC
// pipe (\\.\pipe\discord-ipc-0..9): no network, no dependency, no account data. Always on in
// the launcher (product decision): shown while reading, cleared when it stops.

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace live {

inline const char* const kDiscordAppId = "1573370697676488765";   // Discord Developer Portal → Makine Live

/// The activity Discord shows (build_activity): pure, tested.
nlohmann::json discordActivity(const std::string& game, int64_t startedAt, int lines, bool console,
                               const std::string& steamAppId, const std::string& slug);

class DiscordPresence : public std::enable_shared_from_this<DiscordPresence> {
public:
    explicit DiscordPresence(std::string pipeFormat = "\\\\.\\pipe\\discord-ipc-");

    void start();   // the thread holds a shared_ptr: stop() may leave it to the process exit

    void set(std::optional<nlohmann::json> activity);   // latest wins; returns at once
    void clear() { set(std::nullopt); }
    void stop();   // clears and closes; a thread stuck on the pipe is left to the process exit after 1 s
    bool connected() const { return m_connected; }

private:
    void run();
    bool connect();
    void disconnect();
    bool send(uint32_t op, const nlohmann::json& payload);
    bool recv(uint32_t& op, nlohmann::json& payload);

    static constexpr double kMinInterval = 15.0;   // Discord allows 5 updates per 20 s
    static constexpr double kRetry = 20.0;

    const std::string m_pipeFormat;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::optional<nlohmann::json> m_want;
    bool m_dirty = false, m_stop = false, m_done = false;
    void* m_pipe = nullptr;   // HANDLE
    bool m_connected = false;
    std::thread m_thread;
};

} // namespace live

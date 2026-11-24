// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "discord.h"

#include "jsonutil.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace live {

namespace {

constexpr const char* kDownloadUrl = "https://makineceviri.org/live/";
constexpr const char* kLogoUrl = "https://makineceviri.org/images/icon-512.png";
constexpr const char* kGta6Slug = "grand-theft-auto-vi";
constexpr const char* kGta6Art =   // the Makine × GTA VI logo of 0.3.2 (Discord caches by address)
    "https://raw.githubusercontent.com/MakineCeviri/Makine-LauncherPlugin-Live/main/assets/discord/gta6.png";
constexpr uint32_t kHandshake = 0, kFrame = 1;

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::string cut(const std::string& s, size_t chars)   // UTF-8 safe s[:chars] (Discord counts characters)
{
    size_t n = 0, i = 0;
    for (; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (n == chars) break;
            ++n;
        }
    }
    return s.substr(0, i);
}

std::string nonce()
{
    static uint64_t counter = 0;
    return std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++counter);
}

} // namespace

json discordActivity(const std::string& gameIn, int64_t startedAt, int lines, bool console, const std::string& steamAppId,
                     const std::string& slug)
{
    std::string game = gameIn;
    while (!game.empty() && game.back() == ' ') game.pop_back();
    while (!game.empty() && game.front() == ' ') game.erase(game.begin());
    if (game.empty()) game = "Bir oyun";
    const std::string details = game + " · " + (console ? "konsolda " : "") + "Türkçe oynuyor";
    const std::string state = lines ? "Bu oturumda " + std::to_string(lines) + " diyalog çevrildi" : "Türkçe altyazı açık";
    const bool steam = !steamAppId.empty() && std::all_of(steamAppId.begin(), steamAppId.end(), ::isdigit);
    const std::string large = slug == kGta6Slug ? kGta6Art
                              : steam ? "https://cdn.cloudflare.steamstatic.com/steam/apps/" + steamAppId + "/header.jpg"
                                      : kLogoUrl;
    return {{"type", 0},
            {"details", cut(details, 128)},
            {"state", cut(state, 128)},
            {"timestamps", {{"start", startedAt}}},
            {"assets", {{"large_image", large}, {"large_text", cut(game, 128)}}},
            {"buttons", json::array({{{"label", "Makine Live'ı indir"}, {"url", kDownloadUrl}}})},
            {"instance", false}};
}

DiscordPresence::DiscordPresence(std::string pipeFormat) : m_pipeFormat(std::move(pipeFormat)) {}

void DiscordPresence::start()
{
    m_thread = std::thread([self = shared_from_this()] { self->run(); });
}

void DiscordPresence::set(std::optional<json> activity)
{
    std::lock_guard lock(m_mutex);
    if (activity == m_want && !m_dirty) return;
    m_want = std::move(activity);
    m_dirty = true;
    m_cv.notify_all();
}

void DiscordPresence::stop()
{
    {
        std::lock_guard lock(m_mutex);
        m_want.reset();
        m_dirty = true;
        m_stop = true;
        m_cv.notify_all();
    }
    if (!m_thread.joinable()) return;
    bool done = false;
    {
        std::unique_lock lock(m_mutex);
        done = m_cv.wait_for(lock, std::chrono::milliseconds(200), [this] { return m_done; });
    }
    done ? m_thread.join() : m_thread.detach();
}

bool DiscordPresence::send(uint32_t op, const json& payload)
{
    const std::string data = payload.dump(-1, ' ', false, json::error_handler_t::replace);
    std::vector<char> frame(8 + data.size());
    const uint32_t len = static_cast<uint32_t>(data.size());
    std::memcpy(frame.data(), &op, 4);
    std::memcpy(frame.data() + 4, &len, 4);
    std::memcpy(frame.data() + 8, data.data(), data.size());
    DWORD put = 0;
    return WriteFile(m_pipe, frame.data(), static_cast<DWORD>(frame.size()), &put, nullptr) && put == frame.size();
}

bool DiscordPresence::recv(uint32_t& op, json& payload)
{
    const auto readAll = [&](void* buf, DWORD n) {
        auto* p = static_cast<char*>(buf);
        while (n) {
            DWORD got = 0;
            if (!ReadFile(m_pipe, p, n, &got, nullptr) || !got) return false;
            p += got;
            n -= got;
        }
        return true;
    };
    uint32_t head[2];
    if (!readAll(head, 8) || head[1] > (1u << 20)) return false;
    std::string data(head[1], '\0');
    if (head[1] && !readAll(data.data(), head[1])) return false;
    op = head[0];
    payload = json::parse(data, nullptr, false);
    return true;
}

bool DiscordPresence::connect()
{
    for (int i = 0; i < 10; ++i) {
        const std::string name = m_pipeFormat + std::to_string(i);
        m_pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (m_pipe == INVALID_HANDLE_VALUE) {
            m_pipe = nullptr;
            continue;
        }
        uint32_t op = 0;
        json reply;
        if (send(kHandshake, {{"v", 1}, {"client_id", kDiscordAppId}}) && recv(op, reply) && op == kFrame
            && jstr(reply, "evt") == "READY") {
            m_connected = true;
            if (m_onChange) m_onChange();
            return true;
        }
        CloseHandle(m_pipe);
        m_pipe = nullptr;
    }
    return false;
}

void DiscordPresence::disconnect()
{
    if (m_pipe) CloseHandle(m_pipe);
    m_pipe = nullptr;
    if (m_connected.exchange(false) && m_onChange) m_onChange();
}

void DiscordPresence::run()
{
    try {
        runBody();
    } catch (...) {   // a thread that throws would end the plugin host
        disconnect();
        std::lock_guard lock(m_mutex);
        m_done = true;
        m_cv.notify_all();
    }
}

void DiscordPresence::runBody()
{
    auto sentAt = Clock::now() - std::chrono::hours(1);
    auto nextTry = Clock::now();
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_cv.wait_for(lock, std::chrono::seconds(5), [&] { return m_dirty || m_stop; });
        if (!m_dirty) {
            if (m_stop) break;
            continue;
        }
        const std::optional<json> activity = m_want;
        const bool stopping = m_stop;
        if (activity && !stopping) {
            const double wait = std::max(kMinInterval - secondsSince(sentAt),
                                         m_pipe ? 0.0 : -std::chrono::duration<double>(Clock::now() - nextTry).count());
            if (wait > 0) {   // Discord's rate limit / the next connection attempt
                m_cv.wait_for(lock, std::chrono::milliseconds(static_cast<int>(wait * 1000) + 1),
                              [&] { return m_stop; });
                continue;
            }
        }
        m_dirty = false;
        lock.unlock();
        bool ok = true;
        if (!m_pipe) {
            if (!activity) {   // nothing to clear while not connected
                lock.lock();
                if (stopping) break;
                continue;
            }
            ok = connect();
            if (!ok) nextTry = Clock::now() + std::chrono::seconds(static_cast<int>(kRetry));
        }
        if (ok) {
            const json args = {{"pid", GetCurrentProcessId()}, {"activity", activity ? *activity : json(nullptr)}};
            uint32_t op = 0;
            json reply;
            ok = send(kFrame, {{"cmd", "SET_ACTIVITY"}, {"nonce", nonce()}, {"args", args}}) && recv(op, reply);
            if (ok) {
                sentAt = Clock::now();
            } else {
                disconnect();
                nextTry = Clock::now() + std::chrono::seconds(static_cast<int>(kRetry));
            }
        }
        lock.lock();
        if (!ok) m_dirty = m_want.has_value();
        if (stopping) break;
    }
    lock.unlock();
    disconnect();
    lock.lock();
    m_done = true;
    m_cv.notify_all();
}

} // namespace live

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "tv_link.h"

#include "subtitle_web.h"   // lanIp()

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace live {

namespace {

constexpr int kProtocol = 1;
constexpr double kPingS = 5.0;
constexpr int kConnectTimeoutMs = 3000;
constexpr auto kRetry = std::chrono::seconds(2);

using Clock = std::chrono::steady_clock;

void winsock()
{
    static const bool ready = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)ready;
}

std::string encode(const json& msg) { return msg.dump(-1, ' ', false, json::error_handler_t::replace) + "\n"; }

// The general broadcast plus the /24 broadcast of this PC's IPv4 networks (some routers drop 255.255.255.255)
std::vector<std::string> broadcastAddresses()
{
    std::vector<std::string> out{"255.255.255.255"};
    char host[256] = {};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (gethostname(host, sizeof host) == 0 && getaddrinfo(host, nullptr, &hints, &res) == 0) {
        for (addrinfo* a = res; a; a = a->ai_next) {
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr, ip, sizeof ip);
            std::string s = ip;
            if (s.rfind("127.", 0) == 0) continue;
            s = s.substr(0, s.rfind('.')) + ".255";
            if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        }
        freeaddrinfo(res);
    }
    return out;
}

bool sendAll(SOCKET s, const std::string& data)
{
    size_t off = 0;
    while (off < data.size()) {
        const int n = ::send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

SOCKET connectTimeout(const std::string& host, int port, int timeoutMs)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return INVALID_SOCKET;
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, nullptr, &w, &e, &tv) != 1 || FD_ISSET(s, &e)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    nonblocking = 0;
    ioctlsocket(s, FIONBIO, &nonblocking);
    return s;
}

std::optional<json> readLine(SOCKET s, int timeoutMs)
{
    DWORD t = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    std::string buf;
    char chunk[1024];
    while (buf.find('\n') == std::string::npos && buf.size() < 4096) {
        const int n = recv(s, chunk, sizeof chunk, 0);
        if (n <= 0) return std::nullopt;
        buf.append(chunk, static_cast<size_t>(n));
    }
    json msg = json::parse(buf.substr(0, buf.find('\n')), nullptr, false);
    if (!msg.is_object()) return std::nullopt;
    return msg;
}

bool peerClosed(SOCKET s)   // the TV says nothing after ok: readable means closed
{
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s, &r);
    timeval zero{0, 0};
    const int ready = select(0, &r, nullptr, nullptr, &zero);
    if (ready < 0) return true;
    if (ready == 0) return false;
    char b[1024];
    return recv(s, b, sizeof b, 0) <= 0;
}

} // namespace

std::optional<TvInfo> TvInfo::fromJson(const json& j)
{
    if (!j.is_object() || !j.contains("id") || !j.contains("host") || !j.contains("port")) return std::nullopt;
    TvInfo t;
    t.id = j["id"].is_string() ? j["id"].get<std::string>() : j["id"].dump();
    t.name = j.value("name", std::string("TV"));
    if (t.name.empty()) t.name = "TV";
    if (!j["host"].is_string() || !j["port"].is_number_integer()) return std::nullopt;
    t.host = j["host"].get<std::string>();
    t.port = j["port"].get<int>();
    return t;
}

std::vector<TvInfo> discoverTvs(int timeoutMs, int port, const std::vector<std::string>& targets)
{
    winsock();
    std::map<std::string, TvInfo> found;
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return {};
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
    sockaddr_in any{};
    any.sin_family = AF_INET;
    if (targets.empty()) {   // the replies come in on the home-network interface only
        const std::string ip = lanIp();
        if (ip != "127.0.0.1") inet_pton(AF_INET, ip.c_str(), &any.sin_addr);
    }
    bind(s, reinterpret_cast<sockaddr*>(&any), sizeof any);
    const std::string probe = encode({{"t", "discover"}, {"v", kProtocol}});
    for (const std::string& addr : targets.empty() ? broadcastAddresses() : targets) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, addr.c_str(), &to.sin_addr);
        sendto(s, probe.data(), static_cast<int>(probe.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    }
    const auto end = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
        if (left <= 0) break;
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{static_cast<long>(left / 1000), static_cast<long>((left % 1000) * 1000)};
        if (select(0, &r, nullptr, nullptr, &tv) != 1) break;
        char buf[4096];
        sockaddr_in from{};
        int len = sizeof from;
        const int n = recvfrom(s, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n <= 0) continue;
        json msg = json::parse(std::string(buf, static_cast<size_t>(n)), nullptr, false);
        if (!msg.is_object() || msg.value("t", std::string()) != "here" || !msg.contains("id")) continue;
        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        msg["host"] = ip;
        if (const auto tv = TvInfo::fromJson(msg)) found[tv->id] = *tv;
    }
    closesocket(s);
    std::vector<TvInfo> out;
    for (auto& [id, tv] : found) out.push_back(tv);
    std::sort(out.begin(), out.end(), [](const TvInfo& a, const TvInfo& b) { return a.name < b.name; });
    return out;
}

TvLink::TvLink(std::string pcName, int discoveryPort) : m_pcName(std::move(pcName)), m_discoveryPort(discoveryPort)
{
    winsock();
}

void TvLink::start()
{
    std::thread([self = shared_from_this()] { self->run(); }).detach();
}

void TvLink::close()
{
    std::lock_guard lock(m_mutex);
    m_closed = true;
    ++m_gen;
    m_cv.notify_all();
}

void TvLink::pair(std::optional<TvInfo> tv, const std::string& code)
{
    {
        std::lock_guard lock(m_mutex);
        m_tv = std::move(tv);
        m_code = code;
        while (!m_code.empty() && m_code.back() == ' ') m_code.pop_back();
        ++m_gen;
        m_cv.notify_all();
    }
    const auto t = this->tv();
    setState(t ? "connecting" : "idle", t ? t->name : "");
}

void TvLink::put(json msg)
{
    std::lock_guard lock(m_mutex);
    m_shown = msg.value("t", std::string()) == "line" ? msg : json(nullptr);
    m_line = std::move(msg);
    m_cv.notify_all();
}

void TvLink::onChange(std::function<void()> fn)
{
    std::lock_guard lock(m_mutex);
    m_onChange = std::move(fn);
}

void TvLink::sendLine(int id, const std::string& text, const std::string& kind)
{
    put({{"t", "line"}, {"id", id}, {"text", text}, {"kind", kind}});
}

void TvLink::clear() { put({{"t", "clear"}}); }

void TvLink::setStyle(int size, const std::string& pos, int opacity)
{
    std::lock_guard lock(m_mutex);
    m_style = {{"t", "style"}, {"size", size}, {"pos", pos}, {"opacity", opacity}};
    m_cv.notify_all();
}

std::pair<std::string, std::string> TvLink::state() const
{
    std::lock_guard lock(m_mutex);
    return {m_state, m_detail};
}

std::optional<TvInfo> TvLink::tv() const
{
    std::lock_guard lock(m_mutex);
    return m_tv;
}

void TvLink::setState(const std::string& state, const std::string& detail)
{
    std::function<void()> fn;
    {
        std::lock_guard lock(m_mutex);
        if (m_state == state && m_detail == detail) return;
        m_state = state;
        m_detail = detail;
        fn = m_onChange;
    }
    if (fn) fn();
}

void TvLink::run()
{
    for (;;) {
        int gen = 0;
        TvInfo tv;
        std::string code;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [&] { return m_closed || (m_tv && !m_code.empty()); });
            if (m_closed) return;
            gen = m_gen;
            tv = *m_tv;
            code = m_code;
        }
        if (state().first != "offline") setState("connecting", tv.name);   // retries stay "offline" (no flicker)
        const std::string result = session(gen, tv, code);
        if (result == "bad_code") {
            setState("bad_code", tv.name);
            std::lock_guard lock(m_mutex);
            if (m_gen == gen) m_code.clear();   // wait for a new code
            continue;
        }
        if (result == "stale") continue;
        setState("offline", tv.name);
        std::optional<TvInfo> moved;   // DHCP may have given the TV another address
        for (const TvInfo& t : discoverTvs(1000, m_discoveryPort))
            if (t.id == tv.id && (t.host != tv.host || t.port != tv.port)) moved = t;
        std::unique_lock lock(m_mutex);
        if (m_closed) return;
        if (m_gen == gen && moved && m_tv && m_tv->id == tv.id) {
            m_tv = moved;
            continue;
        }
        m_cv.wait_for(lock, kRetry, [&] { return m_closed || m_gen != gen; });
    }
}

std::string TvLink::session(int gen, const TvInfo& tv, const std::string& code)
{
    const SOCKET s = connectTimeout(tv.host, tv.port, kConnectTimeoutMs);
    if (s == INVALID_SOCKET) return "lost";
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    std::string result = "lost";
    const auto finish = [&] {
        closesocket(s);
        return result;
    };
    if (!sendAll(s, encode({{"t", "hello"}, {"v", kProtocol}, {"code", code}, {"pc", m_pcName}}))) return finish();
    const auto reply = readLine(s, kConnectTimeoutMs);
    if (!reply) return finish();
    const std::string t = reply->value("t", std::string());
    if (t == "bad_code") {
        result = "bad_code";
        return finish();
    }
    if (t != "ok") return finish();
    setState("connected", reply->contains("tv") && (*reply)["tv"].is_string() ? (*reply)["tv"].get<std::string>() : tv.name);
    json sentStyle, shown;
    {
        std::lock_guard lock(m_mutex);
        sentStyle = m_style;
        if (m_line.is_null()) shown = m_shown;   // the line on the PC screen shows on the TV at once
    }
    std::string first = sentStyle.is_null() ? std::string() : encode(sentStyle);
    if (!shown.is_null()) first += encode(shown);
    if (!first.empty() && !sendAll(s, first)) return finish();
    auto lastSend = Clock::now();
    for (;;) {
        if (peerClosed(s)) return finish();   // the TV app closed: reconnect at once
        std::string out;
        {
            std::unique_lock lock(m_mutex);
            if (m_gen != gen) {
                result = "stale";
                return finish();
            }
            if (m_line.is_null() && m_style == sentStyle) {
                const double since = std::chrono::duration<double>(Clock::now() - lastSend).count();
                const double wait = std::min(1.0, std::max(0.0, kPingS - since));
                m_cv.wait_for(lock, std::chrono::milliseconds(static_cast<int>(wait * 1000)));
            }
            if (m_gen != gen) {
                result = "stale";
                return finish();
            }
            if (m_style != sentStyle) {
                sentStyle = m_style;
                out += encode(sentStyle);
            }
            if (!m_line.is_null()) {
                out += encode(m_line);
                m_line = nullptr;
            }
        }
        if (out.empty() && std::chrono::duration<double>(Clock::now() - lastSend).count() >= kPingS)
            out = encode({{"t", "ping"}});
        if (!out.empty()) {
            if (!sendAll(s, out)) return finish();
            lastSend = Clock::now();
        }
    }
}

} // namespace live

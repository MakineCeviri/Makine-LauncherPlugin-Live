// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "subtitle_web.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace live {

namespace {

#include "web_pages.inc"

constexpr size_t kHistory = 4;
constexpr int kMaxConnections = 16;              // earlier lines shown above the last one on the phone
constexpr auto kHeartbeat = std::chrono::seconds(15);
constexpr const char* kCsp =
    "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; img-src 'self'";

void winsock()
{
    static const bool ready = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)ready;
}

bool sendAll(SOCKET s, const std::string& data)
{
    size_t off = 0;
    while (off < data.size()) {
        const int n = ::send(s, data.data() + off, static_cast<int>(std::min<size_t>(data.size() - off, 1 << 20)), 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

std::string response(int status, const std::string& body, const std::string& type, const std::string& extra, bool head)
{
    const char* reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request";
    std::string r = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nServer: MakineLive\r\nContent-Type: " + type
                    + "\r\nContent-Length: " + std::to_string(body.size())
                    + "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n" + extra
                    + "\r\n";
    if (!head) r += body;
    return r;
}

std::string sse(const json& j) { return "data: " + j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n"; }

} // namespace

bool isPrivateLan(const std::string& ip)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

std::string lanIp()
{
    winsock();
    for (const char* probe : {"192.168.1.1", "10.255.255.255", "8.8.8.8"}) {
        // UDP "connect" only picks the route: no packet is sent
        const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) continue;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(9);
        inet_pton(AF_INET, probe, &to.sin_addr);
        sockaddr_in me{};
        int len = sizeof me;
        std::string ip;
        if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to) == 0
            && getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf);
            ip = buf;
        }
        closesocket(s);
        if (!ip.empty() && ip.rfind("127.", 0) != 0 && ip.rfind("0.", 0) != 0 && ip.rfind("169.254.", 0) != 0)
            return ip;
    }
    return "127.0.0.1";
}

SubtitleWeb::SubtitleWeb(std::string logo) : m_logo(std::move(logo)) { winsock(); }

void SubtitleWeb::configure(bool enabled, bool lan)
{
    std::lock_guard lock(m_mutex);
    if (enabled && m_listener != INVALID_SOCKET && m_lan == lan) return;
    if (m_listener != INVALID_SOCKET) {
        closesocket(m_listener);   // the accept loop ends; open /events streams see the new generation
        m_listener = INVALID_SOCKET;
    }
    ++m_generation;
    bump();
    m_port = 0;
    if (!enabled) return;
    m_error.clear();
    for (int port = kPort; port < kPort + kPortTries; ++port) {
        const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) break;
        BOOL exclusive = TRUE;   // never share a port another process listens on
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        // The home-network interface only, never every interface
        const std::string ip = lan ? lanIp() : std::string();
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (lan && (!isPrivateLan(ip) || inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1)) {
            m_error = "Ev ağı (özel) adresi bulunamadı: telefon sayfası açılamaz. Bilgisayar modem / Wi-Fi ağına bağlı mı?";
            closesocket(s);
            return;
        }
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 8) != 0) {
            m_error = "kapı " + std::to_string(port) + " kullanılamıyor (" + std::to_string(WSAGetLastError()) + ")";
            closesocket(s);
            continue;
        }
        m_listener = s;
        m_lan = lan;
        m_port = port;
        m_lanIp = ip;   // once, not on every state publish
        std::thread([self = shared_from_this(), s, gen = m_generation] { self->acceptLoop(s, gen); }).detach();
        return;
    }
}

void SubtitleWeb::acceptLoop(uintptr_t listener, uint64_t generation)
{
    for (;;) {
        const SOCKET c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) return;   // closed by configure()
        {
            std::lock_guard lock(m_mutex);
            if (generation != m_generation) {
                closesocket(c);
                return;
            }
        }
        if (m_connections >= kMaxConnections) {   // a phone and OBS need a few; anything beyond is refused
            closesocket(c);
            continue;
        }
        ++m_connections;
        std::thread([self = shared_from_this(), c, generation] { self->serve(c, generation); }).detach();
    }
}

void SubtitleWeb::serve(uintptr_t sock, uint64_t generation)
{
    try {
        serveBody(sock, generation);
    } catch (...) {   // a thread that throws would end the plugin host
        closesocket(sock);
    }
    --m_connections;
}

void SubtitleWeb::serveBody(uintptr_t sock, uint64_t generation)
{
    DWORD timeout = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    DWORD sendTimeout = 5000;   // a client that stops reading cannot hold a thread forever
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout), sizeof sendTimeout);
    BOOL nodelay = TRUE;   // a subtitle line goes out at once
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    std::string req;
    char buf[2048];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);   // the whole request head
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192
           && std::chrono::steady_clock::now() < deadline) {
        const int n = recv(sock, buf, sizeof buf, 0);
        if (n == 0 || (n < 0 && WSAGetLastError() != WSAETIMEDOUT)) break;
        if (n > 0) req.append(buf, static_cast<size_t>(n));
    }
    const size_t sp1 = req.find(' '), sp2 = sp1 == std::string::npos ? sp1 : req.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        closesocket(sock);
        return;
    }
    const std::string method = req.substr(0, sp1);
    std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    path = path.substr(0, path.find('?'));
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    const bool head = method == "HEAD";
    const std::string csp = std::string("Content-Security-Policy: ") + kCsp + "\r\n";
    if (method != "GET" && !head) {
        sendAll(sock, response(400, "bad request", "text/plain; charset=utf-8", "", false));
    } else if (path == "/") {
        sendAll(sock, response(200, kPhoneHtml, "text/html; charset=utf-8", csp, head));
    } else if (path == "/obs") {
        sendAll(sock, response(200, kObsHtml, "text/html; charset=utf-8", csp, head));
    } else if (path == "/logo.webp" && !m_logo.empty()) {
        sendAll(sock, response(200, m_logo, "image/webp", "Cache-Control: max-age=86400\r\n", head));
    } else if (path == "/events" && !head) {
        stream(sock, generation);
    } else {
        sendAll(sock, response(404, "not found", "text/plain; charset=utf-8", "", head));
    }
    shutdown(sock, SD_BOTH);
    closesocket(sock);
}

// While the connection lives, the whole (small) state goes out on every change.
void SubtitleWeb::stream(uintptr_t sock, uint64_t generation)
{
    if (!sendAll(sock, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-store\r\n"
                       "Connection: close\r\n\r\n"))
        return;
    std::unique_lock lock(m_mutex);
    ++m_clients;
    if (auto fn = m_onChange) {   // a phone / OBS connected
        lock.unlock();
        fn();
        lock.lock();
    }
    uint64_t seen = m_version;
    std::string data = sse(snapshotLocked());
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        lock.unlock();
        const bool ok = data.empty() || sendAll(sock, data);
        if (!data.empty()) last = std::chrono::steady_clock::now();
        lock.lock();
        if (!ok || generation != m_generation) break;
        m_cv.wait_for(lock, std::chrono::seconds(1), [&] { return m_version != seen; });
        if (generation != m_generation) break;
        data.clear();
        if (m_version != seen) {
            seen = m_version;
            data = sse(snapshotLocked());
        } else if (std::chrono::steady_clock::now() - last >= kHeartbeat) {
            data = ": ping\n\n";
        }
    }
    --m_clients;
    if (auto fn = m_onChange) {
        lock.unlock();
        fn();
    }
}

json SubtitleWeb::snapshotLocked() const
{
    return {{"t", "state"}, {"line", m_line}, {"history", json(m_history)}, {"style", m_style}};
}

void SubtitleWeb::bump()
{
    ++m_version;
    m_cv.notify_all();
}

void SubtitleWeb::sendLine(int id, const std::string& text, const std::string& kind)
{
    std::lock_guard lock(m_mutex);
    if (m_line.is_object() && !m_line.value("text", std::string()).empty() && m_line.value("text", std::string()) != text) {
        m_history.push_back(m_line);
        while (m_history.size() > kHistory) m_history.pop_front();
    }
    m_line = {{"t", "line"}, {"id", id}, {"text", text}, {"kind", kind}};
    bump();
}

void SubtitleWeb::clear()
{
    std::lock_guard lock(m_mutex);
    if (m_line.is_null()) return;
    m_history.push_back(m_line);
    while (m_history.size() > kHistory) m_history.pop_front();
    m_line = nullptr;
    bump();
}

void SubtitleWeb::setStyle(const json& extra)
{
    json style = {{"size", extra.value("size", 26)}, {"opacity", extra.value("opacity", 78)}};
    for (const char* k : {"text", "band", "font", "pos"})
        if (extra.contains(k) && extra[k].is_string()) style[k] = extra[k].get<std::string>().substr(0, 120);
    for (const char* k : {"outline", "width", "offset"})
        if (extra.contains(k) && extra[k].is_number()) style[k] = extra[k].get<double>();
    if (extra.contains("shadow") && extra["shadow"].is_boolean()) style["shadow"] = extra["shadow"];
    std::lock_guard lock(m_mutex);
    if (style == m_style) return;
    m_style = style;
    bump();
}

bool SubtitleWeb::running() const
{
    std::lock_guard lock(m_mutex);
    return m_listener != INVALID_SOCKET;
}

bool SubtitleWeb::lan() const
{
    std::lock_guard lock(m_mutex);
    return m_lan && m_listener != INVALID_SOCKET;
}

int SubtitleWeb::port() const
{
    std::lock_guard lock(m_mutex);
    return m_port;
}

int SubtitleWeb::clients() const
{
    std::lock_guard lock(m_mutex);
    return m_clients;
}

std::string SubtitleWeb::error() const
{
    std::lock_guard lock(m_mutex);
    return m_error;
}

std::string SubtitleWeb::obsUrl() const
{
    std::lock_guard lock(m_mutex);   // with the phone on, the server listens on the home-network address only
    const std::string host = m_lan && !m_lanIp.empty() && m_lanIp != "127.0.0.1" ? m_lanIp : "127.0.0.1";
    return "http://" + host + ":" + std::to_string(m_port ? m_port : kPort) + "/obs";
}

std::string SubtitleWeb::phoneUrl() const
{
    std::lock_guard lock(m_mutex);
    return "http://" + (m_lanIp.empty() ? lanIp() : m_lanIp) + ":" + std::to_string(m_port ? m_port : kPort);
}

void SubtitleWeb::onChange(std::function<void()> fn)
{
    std::lock_guard lock(m_mutex);
    m_onChange = std::move(fn);
}

} // namespace live

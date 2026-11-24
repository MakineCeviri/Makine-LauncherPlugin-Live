// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "server.h"

#include "winutil.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cstdlib>
#include <set>
#include <string>

using json = nlohmann::json;

namespace live {

namespace {

std::mutex g_activeMutex;
std::set<HINTERNET> g_active;   // requests in flight, closed by cancel()

std::string urlEncode(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'
            || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string asciiOnly(const std::string& s)   // header values: non-ASCII → '?', as the harness does
{
    std::string out;
    for (const unsigned char c : s) {
        if (c >= 0x80 && (c & 0xC0) == 0x80) continue;   // continuation byte of the same character
        out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    return out;
}

} // namespace

ServerClient::ServerClient(std::string baseUrl, std::string pluginVersion, std::string launcherVersion,
                           std::string deviceId, std::string deviceName)
    : m_userAgent("MakineLive/" + pluginVersion)
    , m_version(std::move(pluginVersion))
    , m_client("launcher/" + launcherVersion)
    , m_device(std::move(deviceId))
    , m_deviceName(std::move(deviceName))
{
    std::string rest = baseUrl;
    if (rest.rfind("http://", 0) == 0) {
        m_secure = false;
        m_port = 80;
        rest = rest.substr(7);
    } else if (rest.rfind("https://", 0) == 0) {
        rest = rest.substr(8);
    }
    const size_t slash = rest.find('/');
    std::string hostPort = rest.substr(0, slash);
    m_basePath = slash == std::string::npos ? "" : rest.substr(slash);
    while (!m_basePath.empty() && m_basePath.back() == '/') m_basePath.pop_back();
    if (const size_t colon = hostPort.find(':'); colon != std::string::npos) {
        m_port = std::atoi(hostPort.c_str() + colon + 1);
        hostPort.resize(colon);
    }
    m_host = win::utf8ToWide(hostPort);

    const std::wstring agent = win::utf8ToWide(m_userAgent);
    m_session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!m_session)   // AUTOMATIC_PROXY needs Windows 8.1+
        m_session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
}

ServerClient::~ServerClient()
{
    cancel();
    if (m_session) WinHttpCloseHandle(static_cast<HINTERNET>(m_session));
}

void ServerClient::setToken(std::string token)
{
    std::lock_guard lock(m_mutex);
    m_token = std::move(token);
}

std::string ServerClient::token() const
{
    std::lock_guard lock(m_mutex);
    return m_token;
}

void ServerClient::cancel()
{
    m_cancelled = true;
    std::lock_guard lock(g_activeMutex);
    for (HINTERNET h : g_active) WinHttpCloseHandle(h);   // the blocked call returns with an error
    g_active.clear();
}

ApiResult ServerClient::call(const wchar_t* method, const std::string& pathAndQuery, const json* payload,
                             int timeoutMs, const std::string* gzipBody)
{
    ApiResult r;
    r.data = json::object();
    if (m_cancelled || !m_session || m_host.empty()) {
        r.error = "bağlantı kapalı";
        return r;
    }
    m_lastRequestMs = static_cast<int64_t>(GetTickCount64());
    const HINTERNET connect = WinHttpConnect(static_cast<HINTERNET>(m_session), m_host.c_str(),
                                             static_cast<INTERNET_PORT>(m_port), 0);
    if (!connect) {
        r.error = "bağlantı kurulamadı (" + std::to_string(GetLastError()) + ")";
        return r;
    }
    const std::wstring path = win::utf8ToWide(m_basePath + pathAndQuery);
    const HINTERNET req = WinHttpOpenRequest(connect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, m_secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) {
        WinHttpCloseHandle(connect);
        r.error = "istek açılamadı";
        return r;
    }
    {
        std::lock_guard lock(g_activeMutex);
        g_active.insert(req);
    }
    WinHttpSetTimeouts(req, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    std::string headers = "Accept: application/json\r\nX-Makine-Device: " + m_device + "\r\nX-Makine-Version: "
                          + m_version + "\r\nX-Makine-Client: " + m_client + "\r\n";
    if (!m_deviceName.empty()) headers += "X-Makine-Device-Name: " + asciiOnly(m_deviceName) + "\r\n";
    const std::string tok = token();
    if (!tok.empty()) headers += "Authorization: Bearer " + tok + "\r\n";
    std::string body;
    if (payload) {
        headers += "Content-Type: application/json\r\n";
        body = payload->dump(-1, ' ', false, json::error_handler_t::replace);
    } else if (gzipBody) {
        headers += "Content-Type: application/gzip\r\n";
        body = *gzipBody;
    }
    const std::wstring wHeaders = win::utf8ToWide(headers);

    bool ok = WinHttpSendRequest(req, wHeaders.c_str(), static_cast<DWORD>(-1),
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data(),
                                 static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)
              && WinHttpReceiveResponse(req, nullptr);
    std::string raw;
    if (ok) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        char buf[8192];
        DWORD got = 0;
        while (WinHttpReadData(req, buf, sizeof buf, &got) && got > 0) raw.append(buf, got);
        r.status = static_cast<int>(status);
    } else {
        const DWORD err = GetLastError();
        r.error = m_cancelled ? "iptal edildi"
                  : err == ERROR_WINHTTP_TIMEOUT ? "sunucu zamanında yanıt vermedi"
                                                  : "sunucuya ulaşılamadı (" + std::to_string(err) + ")";
    }
    {
        std::lock_guard lock(g_activeMutex);
        if (g_active.erase(req)) WinHttpCloseHandle(req);   // else cancel() closed it
    }
    WinHttpCloseHandle(connect);
    if (!ok) return r;

    json data = json::parse(raw, nullptr, false);
    if (data.is_object()) r.data = std::move(data);
    if (!r.ok()) {
        const auto e = r.data.find("error");
        r.error = e != r.data.end() && e->is_string() && !e->get<std::string>().empty()
                      ? e->get<std::string>()
                      : r.status == 401 ? "Giriş gerekli" : "HTTP " + std::to_string(r.status);
    }
    return r;
}

ApiResult ServerClient::deviceStart()
{
    const json body = {{"device", m_deviceName}};
    return call(L"POST", "/api/live/auth?action=device_start", &body, 8000);
}

ApiResult ServerClient::devicePoll(const std::string& deviceSecret)
{
    return call(L"GET", "/api/live/auth?action=device_poll&device=" + urlEncode(deviceSecret), nullptr, 6000);
}

ApiResult ServerClient::me()
{
    return call(L"GET", "/api/live/auth?action=app_me", nullptr, 5000);
}

ApiResult ServerClient::logout()
{
    const json body = json::object();
    return call(L"POST", "/api/live/auth?action=app_logout", &body, 3000);
}

ApiResult ServerClient::translate(const std::string& game, const std::string& gameName, const std::string& text,
                                  const std::string& sourceLang, const std::string& targetLang)
{
    const json body = {{"game", game},
                       {"game_name", gameName},
                       {"text", text},
                       {"source_lang", sourceLang},
                       {"target_lang", targetLang}};
    return call(L"POST", "/api/live/translate", &body, 12000);
}

ApiResult ServerClient::poolSync(const std::string& game, const std::string& rev, bool head)
{
    std::string q = "/api/live/pool?action=sync&game=" + urlEncode(game);
    // A game with no approved line yet: send a revision so "unchanged" can come back
    if (!rev.empty() || !head) q += "&rev=" + urlEncode(rev.empty() ? "r0" : rev);
    if (head) q += "&head=1";
    return call(L"GET", q, nullptr, 20000);
}

ApiResult ServerClient::poolHits(const std::string& game, const json& hits)
{
    const json body = {{"game", game}, {"hits", hits}};
    return call(L"POST", "/api/live/pool?action=hits", &body, 5000);
}

void ServerClient::ping()
{
    call(L"GET", "/api/live/ping", nullptr, 3000);
}

double ServerClient::idleSeconds() const
{
    return (static_cast<int64_t>(GetTickCount64()) - m_lastRequestMs) / 1000.0;
}

ApiResult ServerClient::status()
{
    return call(L"GET", "/api/live/app?action=status", nullptr, 5000);
}

ApiResult ServerClient::beat(const json& payload, int timeoutMs)
{
    return call(L"POST", "/api/live/app?action=beat", &payload, timeoutMs);
}

ApiResult ServerClient::uploadLog(const std::string& requestId, const std::string& gzip)
{
    return call(L"POST", "/api/live/app?action=log_upload&id=" + urlEncode(requestId), nullptr, 30000, &gzip);
}

ApiResult ServerClient::announcementRead(int64_t id)
{
    const json body = {{"id", id}};
    return call(L"POST", "/api/live/app?action=ann_read", &body, 5000);
}

std::string loadDeviceId(const std::wstring& dataDir)
{
    const std::wstring path = dataDir + L"\\device_id";
    if (const auto text = win::readFile(path)) {
        std::string v = *text;
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
        bool hex = v.size() == 32;
        for (const char c : v) hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        if (hex) return v;
    }
    unsigned char raw[16] = {};
    BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char digits[] = "0123456789abcdef";
    std::string v;
    for (const unsigned char b : raw) {
        v += digits[b >> 4];
        v += digits[b & 15];
    }
    win::writeFileAtomic(path, v);
    return v;
}

} // namespace live

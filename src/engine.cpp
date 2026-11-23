// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "engine.h"

#include "unitext.h"
#include "winutil.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using json = nlohmann::json;
using makine::plugin::Result;

namespace live {

namespace {

constexpr double kPollS = 0.10;           // a reading every 100 ms (an unchanged frame costs ~1 ms)
constexpr double kBlockedPollS = 0.20;
constexpr double kStaleJobS = 2.0;        // a line whose source left this long ago is not translated any more
constexpr double kCacheTtlS = 900;        // unapproved translations are reused this long in a session
constexpr double kPoolSyncS = 180;        // local pool vs server while reading
constexpr double kStatusPollS = 300;      // maintenance / version check
constexpr double kOfflineRetryS = 30;
constexpr auto kShutdownBudget = std::chrono::milliseconds(2500);
constexpr auto kReadOnceTimeout = std::chrono::seconds(25);   // invoke times out at 30 s
constexpr const char* kSourceLang = "en";  // product decision: English → Turkish only
constexpr const char* kTargetLang = "tr";

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

Result okMessage(std::string_view text) { return Result::ok(dump({{"message", text}})); }

std::string str(const json& j, const char* key, std::string def = {})
{
    if (!j.is_object()) return def;
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : def;
}

double num(const json& j, const char* key, double def)
{
    if (!j.is_object()) return def;
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : def;
}

bool flag(const json& j, const char* key)
{
    if (!j.is_object()) return false;
    const auto it = j.find(key);
    return it != j.end() && ((it->is_boolean() && it->get<bool>()) || (it->is_number() && it->get<double>() != 0));
}

bool validToken(const std::string& t)
{
    return t.size() == 64 && std::all_of(t.begin(), t.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

json boxJson(const console::Box& b) { return {{"x", b.x}, {"y", b.y}, {"width", b.w}, {"height", b.h}}; }

std::optional<console::Box> boxFrom(const json& j)
{
    if (!j.is_object()) return std::nullopt;
    const console::Box b{static_cast<int>(num(j, "x", 0)), static_cast<int>(num(j, "y", 0)),
                         static_cast<int>(num(j, "width", 0)), static_cast<int>(num(j, "height", 0))};
    return b.w > 0 && b.h > 0 ? std::optional(b) : std::nullopt;
}

std::vector<std::string> splitOn(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t b = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) {
            if (i > b) out.push_back(s.substr(b, i - b));
            b = i + 1;
        }
    return out;
}

uint64_t frameHash(const uint8_t* data, size_t size)   // as live_core.cpp
{
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ size;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        std::memcpy(&w, data + i, 8);
        h ^= w;
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 32;
    }
    for (; i < size; ++i) {
        h ^= data[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

// The console source to read: the window the user selected, else their usual app, else the topmost.
std::optional<console::Source> pickSource(const std::string& key, unsigned long pid,
                                          const std::vector<std::string>& customExes)
{
    const auto found = console::findSources(customExes);
    for (const auto& s : found)
        if (pid && s.pid == pid) return s;
    for (const auto& s : found)
        if (!key.empty() && s.app.key == key) return s;
    if (!found.empty()) return found.front();
    return std::nullopt;
}

json sourceJson(const console::Source& s)
{
    return {{"id", "pid:" + std::to_string(s.pid)}, {"key", s.app.key},  {"label", s.app.label},
            {"console", s.app.console},             {"exe", s.exe},      {"title", s.title},
            {"pid", s.pid}};
}

std::string computerName()
{
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(buf, &n) ? win::wideToUtf8(buf) : std::string("PC");
}

std::string serverUrl()
{
    // Development only (tools/offline.py): a local test server, as the harness allows
    wchar_t buf[512] = {};
    const DWORD n = GetEnvironmentVariableW(L"MAKINE_LIVE_SERVER", buf, 512);
    std::string url = n > 0 && n < 512 ? win::wideToUtf8(buf) : std::string("https://makineceviri.org");
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

} // namespace

// ── setup ───────────────────────────────────────────────────────────────────

Engine::Engine(makine::plugin::Host host, EngineInit init)
    : m_host(host)
    , m_init(std::move(init))
    , m_server(serverUrl(), m_init.version, m_init.launcherVersion, loadDeviceId(m_init.dataDir),
               computerName() + " · Makine Live " + m_init.version + " (Makine)")
    , m_pool(m_init.dataDir)
{
    m_settings.load(win::wideToUtf8(m_init.dataDir + L"\\settings.cfg"));
    const std::string token = m_settings.get("authToken");
    if (validToken(token)) {
        m_server.setToken(token);
        m_user = m_settings.get("user");
        m_loginState = "checking";
    }
    if (m_settings.get("mode") == "console") m_mode = "console";
    const auto pc = splitOn(m_settings.get("pcRegion"), ',');
    if (pc.size() == 4) {
        const console::Box b{std::atoi(pc[0].c_str()), std::atoi(pc[1].c_str()), std::atoi(pc[2].c_str()),
                             std::atoi(pc[3].c_str())};
        if (b.w > 0 && b.h > 0) m_pcRegion = b;
    }
    const auto cr = splitOn(m_settings.get("consoleRegion"), ',');
    if (cr.size() == 4) {
        const console::Frac f{std::strtod(cr[0].c_str(), nullptr), std::strtod(cr[1].c_str(), nullptr),
                              std::strtod(cr[2].c_str(), nullptr), std::strtod(cr[3].c_str(), nullptr)};
        if (console::validRegion(f)) m_consoleRegion = f;
    }
    m_sourceKey = m_settings.get("consoleSource");
    m_customExes = splitOn(m_settings.get("customExes"), '|');
    const json style = json::parse(m_settings.get("style", "{}"), nullptr, false);
    if (style.is_object()) m_style = style;
}

void Engine::launch()
{
    m_captureThread = std::thread([self = shared_from_this()] { self->runCapture(); });
    for (int i = 0; i < 2; ++i) m_netThreads.emplace_back([self = shared_from_this()] { self->runNet(); });
    if (m_loginState == "checking") post([this] { validateSession(); });
    post([this] { statusLoop(); });
    post([this] {   // the game the launcher already sees running
        const Result r = m_host.call("games.running");
        const json g = json::parse(r.json, nullptr, false);
        if (r.succeeded() && g.is_object()) onEvent("game.started", g);
    });
    publish();
}

void Engine::shutdown()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
        m_jobs.clear();
    }
    m_captureCv.notify_all();
    m_netCv.notify_all();
    m_server.cancel();
    bool finished = false;
    {
        std::unique_lock lock(m_mutex);
        finished = m_captureCv.wait_for(lock, kShutdownBudget, [this] {
            return m_finished == 1 + static_cast<int>(m_netThreads.size());
        });
    }
    const auto end = [&](std::thread& t) {
        if (t.joinable()) finished ? t.join() : t.detach();
    };
    end(m_captureThread);
    for (auto& t : m_netThreads) end(t);
    if (!finished) {
        // Inside OCR or a host call that waits for the user; the host ends the process right after.
        m_host.warn("İş parçacıkları zamanında durmadı; süreç kapanırken kesilecek.");
        m_host.emitEvent("overlay.hide", "{}");
    }
}

double Engine::now() const
{
    return std::chrono::duration<double>(Clock::now() - m_t0).count();
}

// ── state for the launcher screen ───────────────────────────────────────────

json Engine::stateLocked() const
{
    json selected = nullptr;
    if (!m_sourceKey.empty() || m_sourcePid) selected = {{"key", m_sourceKey}, {"pid", m_sourcePid}};
    const bool console = m_mode == "console";
    return {
        {"login", {{"state", m_loginState}, {"code", m_loginCode}, {"url", m_loginUrl}, {"user", m_user},
                   {"message", m_loginMessage}}},
        {"mode", m_mode},
        {"sources", m_sources},
        {"selectedSource", selected},
        {"region", {{"pc", m_pcRegion ? boxJson(*m_pcRegion) : json(nullptr)},
                    {"console", {{"x", m_consoleRegion.x}, {"y", m_consoleRegion.y}, {"width", m_consoleRegion.w},
                                 {"height", m_consoleRegion.h}}}}},
        {"picking", m_picking},
        {"running", m_running},
        {"status", m_status},
        {"lastLine", m_lastLine},
        {"counters", {{"lines", m_lines}, {"poolHits", m_poolHits}}},
        {"server", {{"maintenance", m_maintenance}, {"message", m_serverMessage}}},
        {"notice", m_notice},
        {"style", m_style},
        {"game", {{"name", console ? console::kGameName : m_gameName},
                  {"slug", console ? console::kGameSlug : m_gameSlug}}},
    };
}

void Engine::publish()
{
    std::string payload;
    {
        std::lock_guard lock(m_mutex);
        payload = dump(stateLocked());
    }
    m_host.emitEvent("ui.state", payload);
}

void Engine::setStatus(const std::string& text)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_status == text) return;
        m_status = text;
    }
    m_host.emitEvent("ui.setStatus", dump({{"text", text}}));
    publish();
}

void Engine::notifyError(const std::string& message)
{
    {
        std::lock_guard lock(m_mutex);
        if (message == m_lastError) return;
        m_lastError = message;
    }
    m_host.error(message);
    m_host.emitEvent("ui.notify", dump({{"title", "Makine Live"}, {"message", message}, {"kind", "error"}}));
}

void Engine::saveSettings()   // under m_mutex
{
    m_settings.set("mode", m_mode);
    m_settings.set("pcRegion", m_pcRegion ? std::to_string(m_pcRegion->x) + "," + std::to_string(m_pcRegion->y) + ","
                                                + std::to_string(m_pcRegion->w) + "," + std::to_string(m_pcRegion->h)
                                          : std::string());
    m_settings.set("consoleRegion", std::to_string(m_consoleRegion.x) + "," + std::to_string(m_consoleRegion.y) + ","
                                        + std::to_string(m_consoleRegion.w) + "," + std::to_string(m_consoleRegion.h));
    m_settings.set("consoleSource", m_sourceKey);
    std::string exes;
    for (const auto& e : m_customExes) exes += (exes.empty() ? "" : "|") + e;
    m_settings.set("customExes", exes);
    m_settings.set("style", dump(m_style));
    m_settings.set("user", m_user);
    m_settings.set("authToken", m_server.token());
    if (!m_settings.save()) m_host.warn("Ayarlar kaydedilemedi (settings.cfg).");
}

std::string Engine::currentSlug() const
{
    return m_mode == "console" ? std::string(console::kGameSlug) : m_gameSlug;
}

// ── launcher → plugin ───────────────────────────────────────────────────────

Result Engine::invoke(std::string_view method, const json& params)
{
    if (method == "state") {
        std::lock_guard lock(m_mutex);
        return Result::ok(dump(stateLocked()));
    }
    if (method == "login.start") return loginStart();
    if (method == "login.cancel") {
        {
            std::lock_guard lock(m_mutex);
            ++m_loginGen;
            if (m_loginState != "signedIn") {
                m_loginState = "signedOut";
                m_loginCode.clear();
                m_loginUrl.clear();
                m_loginMessage.clear();
            }
        }
        publish();
        return okMessage("Giriş iptal edildi.");
    }
    if (method == "logout") {
        post([this] {
            m_server.logout();
            signedOut("");
        });
        return okMessage("Çıkış yapılıyor.");
    }
    if (method == "mode.set") {
        const std::string mode = str(params, "mode");
        if (mode != "pc" && mode != "console") return Result::error(MAKINE_E_INVALID_ARGUMENT, "mode: pc ya da console");
        {
            std::lock_guard lock(m_mutex);
            m_mode = mode;
            saveSettings();
        }
        m_captureCv.notify_all();
        publish();
        post([this] { syncPool(true); });
        return okMessage(mode == "console" ? "Konsol modu" : "PC modu");
    }
    if (method == "console.sources") return listSources(flag(params, "all"));
    if (method == "console.select") return selectSource(params);
    if (method == "region.pick") return pickRegion();
    if (method == "start") return startReading();
    if (method == "stop") {
        {
            std::lock_guard lock(m_mutex);
            m_running = false;
        }
        m_captureCv.notify_all();
        publish();
        return okMessage("Durduruldu.");
    }
    if (method == "readOnce") return readOnce();
    if (method == "style.set") {
        if (!params.is_object()) return Result::error(MAKINE_E_INVALID_ARGUMENT, "style: bir nesne bekleniyor");
        {
            std::lock_guard lock(m_mutex);
            for (const auto& [k, v] : params.items()) {
                if (v.is_null()) m_style.erase(k);
                else m_style[k] = v;
            }
            ++m_styleVersion;
            saveSettings();
        }
        m_captureCv.notify_all();
        publish();
        std::lock_guard lock(m_mutex);
        return Result::ok(dump({{"style", m_style}}));
    }
    return Result::notFound(method);
}

void Engine::onEvent(std::string_view event, const json& payload)
{
    if (event == "game.started") {
        const std::string name = str(payload, "name");
        if (name.empty()) return;
        {
            std::lock_guard lock(m_mutex);
            m_gameName = name;
            m_gameSlug = text::slugify(name);
        }
        m_host.info("Oyun: " + name);
        publish();
        post([this] { syncPool(true); });
    } else if (event == "game.stopped") {
        {
            std::lock_guard lock(m_mutex);
            m_gameName.clear();
            m_gameSlug.clear();
        }
        m_captureCv.notify_all();
        publish();
    }
}

Result Engine::loginStart()
{
    int gen = 0;
    {
        std::lock_guard lock(m_mutex);
        if (m_loginState == "signedIn") return okMessage("Zaten giriş yapıldı.");
        gen = ++m_loginGen;
        m_loginState = "starting";
        m_loginCode.clear();
        m_loginUrl.clear();
        m_loginMessage.clear();
    }
    publish();
    post([this, gen] {
        const ApiResult r = m_server.deviceStart();
        {
            std::lock_guard lock(m_mutex);
            if (gen != m_loginGen) return;
            if (!r.ok() || str(r.data, "device").empty() || str(r.data, "code").empty()) {
                m_loginState = "signedOut";
                m_loginMessage = r.network()  ? "Sunucuya ulaşılamıyor. İnternet bağlantınızı kontrol edin."
                                 : r.ok()     ? "Giriş başlatılamadı. Biraz sonra tekrar deneyin."
                                 : r.error;
            } else {
                m_loginState = "waiting";
                m_loginCode = str(r.data, "code");
                m_loginUrl = str(r.data, "url");
                m_loginMessage = "Onaylayınca Makine Live kendiliğinden giriş yapar.";
            }
        }
        publish();
        if (!r.ok()) return;
        const std::string url = str(r.data, "url");
        if (!url.empty()) {
            const Result opened = m_host.call("ui.openUrl", dump({{"url", url}}));
            if (!opened.succeeded()) m_host.warn("Tarayıcı açılamadı; bağlantı ekranda: " + url);
        }
        const double interval = std::max(2.0, num(r.data, "interval", 3));
        loginPoll(gen, str(r.data, "device"), now() + num(r.data, "expires_in", 600), interval);
    });
    return okMessage("Giriş kodu isteniyor.");
}

void Engine::loginPoll(int gen, std::string secret, double deadline, double interval)
{
    post([this, gen, secret, deadline, interval] {
        {
            std::lock_guard lock(m_mutex);
            if (gen != m_loginGen) return;
            if (now() >= deadline) {
                m_loginState = "signedOut";
                m_loginCode.clear();
                m_loginMessage = "Kodun süresi doldu. Tekrar deneyin.";
            }
        }
        if (now() >= deadline) return publish();
        const ApiResult r = m_server.devicePoll(secret);
        bool again = false;
        {
            std::lock_guard lock(m_mutex);
            if (gen != m_loginGen) return;
            if (r.network()) {
                m_loginMessage = "Bağlantı koptu · yeniden deneniyor…";
                again = true;
            } else if (!r.ok()) {
                m_loginState = "signedOut";
                m_loginCode.clear();
                m_loginMessage = r.status == 410 ? "Kodun süresi doldu. Tekrar deneyin." : r.error;
            } else if (str(r.data, "status") == "approved" && validToken(str(r.data, "token"))) {
                ++m_loginGen;
                m_server.setToken(str(r.data, "token"));
            } else {
                m_loginMessage = "Onaylayınca Makine Live kendiliğinden giriş yapar.";
                again = true;
            }
        }
        if (r.ok() && str(r.data, "status") == "approved" && validToken(str(r.data, "token"))) return signedIn(r.data);
        publish();
        if (again) loginPoll(gen, secret, deadline, interval);
    }, interval);
}

void Engine::signedIn(const json& profile)
{
    {
        std::lock_guard lock(m_mutex);
        m_loginState = "signedIn";
        m_user = str(profile, "user", m_user);
        m_loginCode.clear();
        m_loginUrl.clear();
        m_loginMessage.clear();
        if (profile.is_object() && profile.contains("translation_ready") && !flag(profile, "translation_ready"))
            m_notice = "Sunucuda çeviri anahtarı yok · yalnızca havuzdaki satırlar gelir";
        saveSettings();
    }
    m_host.info("Giriş: " + m_user);
    publish();
    post([this] { syncPool(true); });
}

void Engine::signedOut(const std::string& message)
{
    {
        std::lock_guard lock(m_mutex);
        ++m_loginGen;
        m_server.setToken("");
        m_loginState = "signedOut";
        m_loginCode.clear();
        m_loginUrl.clear();
        m_loginMessage = message;
        m_user.clear();
        m_running = false;
        saveSettings();
    }
    m_captureCv.notify_all();
    publish();
}

void Engine::handleAuthFailure(const ApiResult& r)
{
    if (r.status == 401) signedOut("Oturumunuz kapatıldı. Lütfen yeniden giriş yapın.");
    else if (r.code() == "banned") signedOut(r.error);
}

void Engine::validateSession()
{
    const ApiResult r = m_server.me();
    if (r.ok()) return signedIn(r.data);
    if (r.status == 401) return signedOut("Oturumun süresi doldu. Lütfen yeniden giriş yapın.");
    if (r.code() == "banned") return signedOut(r.error);
    {
        std::lock_guard lock(m_mutex);
        m_loginState = "offline";   // the token is kept
        m_loginMessage = "Sunucuya ulaşılamıyor · bağlantı gelince yeniden denenir.";
    }
    publish();
    if (r.code() == "maintenance") post([this] { refreshStatus(); });
    post([this] { validateSession(); }, kOfflineRetryS);
}

void Engine::refreshStatus()
{
    const ApiResult r = m_server.status();
    if (r.ok()) {
        const json m = r.data.value("maintenance", json::object());
        const json upd = r.data.value("update", json::object());
        const bool maintenance = flag(m, "on") && !flag(m, "bypass");
        const bool mandatory = flag(upd, "mandatory") && !str(upd, "latest").empty();
        {
            std::lock_guard lock(m_mutex);
            m_maintenance = maintenance;
            m_blocked = maintenance || mandatory;
            m_serverMessage = maintenance ? str(m, "message", "Makine Live şu an bakımda.")
                              : mandatory ? "Makine Live'ın bu sürümü artık desteklenmiyor. Makine'yi güncelleyin."
                                          : "";
            if (m_blocked) m_running = false;
        }
        m_captureCv.notify_all();
        publish();
    }
}

void Engine::statusLoop()
{
    refreshStatus();
    post([this] { statusLoop(); }, kStatusPollS);
}

void Engine::syncPool(bool force)
{
    std::string slug, name;
    {
        std::lock_guard lock(m_mutex);
        slug = currentSlug();
        name = m_mode == "console" ? std::string(console::kGameName) : m_gameName;
        if (slug.empty() || m_loginState != "signedIn") return;
        if (!force && now() - m_poolSyncedAt < kPoolSyncS) return;
        m_poolSyncedAt = now();
        m_poolSlug = slug;
    }
    // The game comes from the launcher's library (or is GTA VI in console mode), not from a guess:
    // its approved lines are downloaded without asking, unlike the harness.
    if (!m_pool.isConfirmed(slug)) m_pool.confirm(slug, name);
    const auto hits = m_pool.takeHits(slug);
    if (!hits.empty()) {
        json h = json::object();
        for (const auto& [id, n] : hits) h[std::to_string(id)] = n;
        if (!m_server.poolHits(slug, h).ok()) m_pool.returnHits(slug, hits);
    }
    const ApiResult r = m_server.poolSync(slug, m_pool.rev(slug), false);
    if (r.status == 401 || r.code() == "banned") return handleAuthFailure(r);
    if (!r.ok()) return;
    const SyncStats st = m_pool.applySync(slug, r.data);
    if (!st.unchanged)
        m_host.info("Yerel havuz " + slug + ": +" + std::to_string(st.added) + " ~" + std::to_string(st.changed) + " −"
                    + std::to_string(st.removed) + " (" + std::to_string(st.total) + " onaylı satır)");
}

Result Engine::startReading()
{
    {
        std::lock_guard lock(m_mutex);
        if (m_loginState != "signedIn") return Result::error(MAKINE_E_DENIED, "Önce Makine hesabınla giriş yap.");
        if (m_blocked) return Result::error(MAKINE_E_DENIED, m_serverMessage);
        if (m_mode == "pc" && !m_pcRegion)
            return Result::error(MAKINE_E_FAILED, "Önce okunacak bölgeyi seç.");
        m_running = true;
    }
    m_captureCv.notify_all();
    publish();
    post([this] { syncPool(true); });
    return okMessage("Canlı çeviri başladı.");
}

Result Engine::listSources(bool all)
{
    std::vector<std::string> exes;
    {
        std::lock_guard lock(m_mutex);
        exes = m_customExes;
    }
    json list = json::array();
    for (const auto& s : console::findSources(exes)) list.push_back(sourceJson(s));
    if (all) {
        // Any other large window, for "seçilen pencere": selecting it remembers its exe as a source
        const DWORD self = GetCurrentProcessId();
        std::set<unsigned long> seen;
        for (const auto& s : list) seen.insert(s.value("pid", 0ul));
        for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
            if (!IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER)) continue;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (!pid || pid == self || seen.count(pid)) continue;
            const console::Rect r = console::clientRect(h);
            if (r.r - r.l < 320 || r.b - r.t < 200) continue;
            wchar_t title[256] = {};
            if (!GetWindowTextW(h, title, 256)) continue;
            seen.insert(pid);
            std::string exe;
            if (const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t buf[1024];
                DWORD size = 1024;
                if (QueryFullProcessImageNameW(p, 0, buf, &size)) {
                    const std::wstring path(buf, size);
                    exe = win::wideToUtf8(path.substr(path.find_last_of(L"\\/") + 1));
                }
                CloseHandle(p);
            }
            if (exe.empty()) continue;
            list.push_back({{"id", "pid:" + std::to_string(pid)}, {"key", "window"}, {"label", win::wideToUtf8(title)},
                            {"console", "custom"}, {"exe", exe}, {"title", win::wideToUtf8(title)}, {"pid", pid}});
        }
    }
    {
        std::lock_guard lock(m_mutex);
        m_sources = list;
    }
    publish();
    return Result::ok(dump({{"sources", list}}));
}

Result Engine::selectSource(const json& params)
{
    const std::string id = str(params, "sourceId");
    json chosen;
    {
        std::lock_guard lock(m_mutex);
        for (const auto& s : m_sources)
            if (s.value("id", std::string()) == id) chosen = s;
    }
    if (chosen.is_null()) return Result::error(MAKINE_E_NOT_FOUND, "Bu kaynak artık yok; listeyi yenileyin.");
    {
        std::lock_guard lock(m_mutex);
        m_sourcePid = chosen.value("pid", 0ul);
        m_sourceKey = chosen.value("key", std::string());
        if (m_sourceKey == "window") {   // a window the user picked: its exe becomes a custom source
            const std::string exe = text::toUtf8(text::lower(text::fromUtf8(chosen.value("exe", std::string()))));
            if (std::find(m_customExes.begin(), m_customExes.end(), exe) == m_customExes.end())
                m_customExes.push_back(exe);
            m_sourceKey = "custom";
        }
        saveSettings();
    }
    m_captureCv.notify_all();
    publish();
    return okMessage("Kaynak: " + chosen.value("label", std::string()));
}

Result Engine::pickRegion()
{
    std::string mode, key;
    unsigned long pid = 0;
    std::vector<std::string> exes;
    {
        std::lock_guard lock(m_mutex);
        if (m_picking) return Result::error(MAKINE_E_BUSY, "Bölge seçimi zaten açık.");
        m_picking = true;
        mode = m_mode;
        key = m_sourceKey;
        pid = m_sourcePid;
        exes = m_customExes;
    }
    publish();
    // Blocks until the user draws a rectangle or cancels: on a net thread
    post([this, mode, key, pid, exes] {
        json params = json::object();
        std::optional<console::Rect> video;
        if (mode == "console") {
            const auto src = pickSource(key, pid, exes);
            const std::string reason = console::blockReason(src ? &*src : nullptr);
            if (!reason.empty()) {
                {
                    std::lock_guard lock(m_mutex);
                    m_picking = false;
                }
                notifyError(reason);
                return publish();
            }
            video = console::videoRect(console::clientRect(src->hwnd));
            params["within"] = boxJson({video->l, video->t, video->r - video->l, video->b - video->t});
        }
        const Result r = m_host.call("overlay.pickRegion", dump(params));
        const auto box = r.succeeded() ? boxFrom(json::parse(r.json, nullptr, false)) : std::nullopt;
        {
            std::lock_guard lock(m_mutex);
            m_picking = false;
            if (box && mode == "pc") m_pcRegion = box;
            if (box && video) {
                const console::Frac f = console::screenToRegion(*video, *box);
                if (console::validRegion(f)) m_consoleRegion = f;
            }
            if (box) saveSettings();
        }
        if (!box && r.status != MAKINE_E_DENIED) notifyError("Bölge seçilemedi.");
        m_captureCv.notify_all();
        publish();
    });
    return okMessage("Ekranda okunacak bölgeyi seçin.");
}

Result Engine::readOnce()
{
    auto done = std::make_shared<std::promise<Result>>();
    auto result = done->get_future();
    {
        std::lock_guard lock(m_mutex);
        m_captureJobs.push_back([this, done] {
            std::string error;
            if (!ensureOcr(error)) return done->set_value(Result::error(MAKINE_E_FAILED, error));
            void* hwnd = nullptr;
            CaptureRegion region;
            console::Box overlay;
            const std::string reason = resolveTarget(hwnd, region, overlay, false);
            if (!reason.empty()) return done->set_value(Result::error(MAKINE_E_FAILED, reason));
            m_lastHash = 0;   // read now, even an unchanged frame
            std::string raw;
            if (!readRegion(hwnd, region, raw, error)) return done->set_value(Result::error(MAKINE_E_FAILED, error));
            const std::string filtered = m_garbage.filterOcr(raw, kSourceLang);
            done->set_value(Result::ok(dump({
                {"message", raw.empty() ? "Bölgede metin bulunamadı." : raw},
                {"text", raw},
                {"filtered", filtered},
                {"region", boxJson({region.x, region.y, region.width, region.height})},
                {"window", hwnd != nullptr},
            })));
        });
    }
    m_captureCv.notify_all();
    if (result.wait_for(kReadOnceTimeout) != std::future_status::ready)
        return Result::error(MAKINE_E_TIMEOUT, "Okuma zamanında bitmedi.");
    return result.get();
}

// ── net threads ─────────────────────────────────────────────────────────────

void Engine::post(std::function<void()> fn, double delaySeconds)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_quit) return;
        m_jobs.push_back({Clock::now() + std::chrono::milliseconds(static_cast<int64_t>(delaySeconds * 1000)),
                          std::move(fn)});
    }
    m_netCv.notify_one();
}

void Engine::runNet()
{
    std::unique_lock lock(m_mutex);
    while (!m_quit) {
        if (m_jobs.empty()) {
            m_netCv.wait(lock);
            continue;
        }
        const auto next = std::min_element(m_jobs.begin(), m_jobs.end(),
                                           [](const Job& a, const Job& b) { return a.due < b.due; });
        if (next->due > Clock::now()) {
            m_netCv.wait_until(lock, next->due);
            continue;
        }
        auto fn = std::move(next->fn);
        m_jobs.erase(next);
        lock.unlock();
        try {
            fn();
        } catch (const std::exception& e) {
            m_host.error(std::string("İş: ") + e.what());
        }
        lock.lock();
    }
    ++m_finished;
    m_captureCv.notify_all();
}

// Port of app.py _translate_job: local pool first, then this session's cache, then one
// /api/live/translate request (retried once on network / 502 / 504 errors).
void Engine::translateJob(int line, std::string textUtf8, int seq, std::string slug, std::string gameName)
{
    const auto finish = [&](std::string translated, std::string kind) {
        std::lock_guard lock(m_mutex);
        m_results.push_back({line, std::move(translated), std::move(kind)});
    };
    {
        std::lock_guard lock(m_mutex);
        if (m_dispatchSeq[line] != seq) return;   // a newer reading of this line is queued
        const auto gone = m_goneAt.find(line);
        if (gone != m_goneAt.end() && now() - gone->second > kStaleJobS) {
            m_results.push_back({line, {}, {}});
            return;
        }
    }
    const std::u32string key = text::compactPoolKey(text::fromUtf8(textUtf8));
    std::string poolSlug;
    {
        std::lock_guard lock(m_mutex);
        poolSlug = m_poolSlug;
    }
    if (!slug.empty() && slug == poolSlug) {
        if (const auto hit = m_pool.lookup(slug, textUtf8)) {
            {
                std::lock_guard lock(m_mutex);
                ++m_poolHits;
                m_lastLine = {{"source", textUtf8}, {"text", hit->translation}, {"kind", "verified"}, {"origin", "local"}};
            }
            finish(hit->translation, "verified");
            return publish();
        }
    }
    bool owner = false;
    {
        std::unique_lock lock(m_mutex);
        if (m_skipKeys.count(key)) {
            m_results.push_back({line, {}, {}});
            return;
        }
        for (int attempt = 0; attempt < 2 && !owner; ++attempt) {
            const auto c = m_cache.find(key);
            if (c != m_cache.end() && (c->second.kind == "verified" || now() - c->second.at <= kCacheTtlS)) {
                m_results.push_back({line, c->second.text, c->second.kind});
                return;
            }
            if (!m_inflight.count(key)) {
                m_inflight.insert(key);
                owner = true;
            } else {
                m_netCv.wait_for(lock, std::chrono::seconds(12), [&] { return m_quit || !m_inflight.count(key); });
            }
        }
        if (!owner) {
            m_results.push_back({line, {}, {}});
            return;
        }
    }

    ApiResult res;
    std::string notice;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        res = m_server.translate(slug, gameName, textUtf8, kSourceLang, kTargetLang);
        bool retry = false;
        if (res.ok()) break;
        if (res.status == 401 || res.code() == "banned") {
            handleAuthFailure(res);
            break;
        }
        if (res.network()) {
            notice = "Sunucuya ulaşılamıyor · yeniden deneniyor";
            retry = true;
        } else if (res.status == 422) {
            std::lock_guard lock(m_mutex);
            m_skipKeys.insert(key);
            m_host.debug("Sunucu çöp saydı (" + str(res.data, "reason") + "): " + textUtf8);
            break;
        } else if (res.status == 429) {
            notice = "Günlük çeviri sınırı doldu — havuzdaki çeviriler gelmeye devam ediyor";
            break;
        } else if (res.code() == "maintenance") {
            post([this] { refreshStatus(); });
            break;
        } else if (res.status == 503) {
            notice = "Sunucuda çeviri anahtarı yok · yalnızca havuzdaki satırlar gelir";
            break;
        } else {
            m_host.warn("Sunucu " + std::to_string(res.status) + ": " + res.error);
            retry = res.status == 502 || res.status == 504;
            if (retry) notice = "Çeviri servisi yanıt vermedi · yeniden deneniyor";
        }
        if (!retry || attempt == 2) break;
        {
            std::lock_guard lock(m_mutex);
            const auto gone = m_goneAt.find(line);
            if (m_quit || (gone != m_goneAt.end() && now() - gone->second > kStaleJobS)) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    const std::string translated = res.ok() ? str(res.data, "translated") : std::string();
    std::string kind = "api";
    if (!translated.empty() && str(res.data, "origin", "deepl") == "pool")
        kind = str(res.data, "status", "pending") == "verified" ? "verified" : "pending";
    {
        std::lock_guard lock(m_mutex);
        m_inflight.erase(key);
        if (!notice.empty()) m_notice = notice;
        if (!translated.empty()) {
            if (m_notice.find("yeniden deneniyor") != std::string::npos) m_notice.clear();
            m_cache[key] = {translated, kind, now()};
            if (m_cache.size() > 1000) m_cache.erase(m_cache.begin());
            if (kind != "api") ++m_poolHits;
            m_lastLine = {{"source", textUtf8}, {"text", translated}, {"kind", kind},
                          {"origin", str(res.data, "origin", "deepl")}};
        }
        m_results.push_back({line, translated, kind});
    }
    m_netCv.notify_all();   // a job waiting for the same line
    publish();
}

// ── capture thread ──────────────────────────────────────────────────────────

bool Engine::ensureOcr(std::string& error)
{
    if (m_ocr && m_ocr->isReady() && m_capture) return true;
    const std::wstring words = m_init.pluginDir + L"\\resources\\en_words.txt";
    const auto list = win::readFile(words);
    if (!list || !m_garbage.setWords(*list)) {
        error = "Kelime listesi eksik (resources\\en_words.txt). Makine'yi onarın ya da yeniden kurun.";
        return false;
    }
    m_spacing.load(words);
    m_capture = std::make_unique<ScreenCapture>();
    if (!m_capture->init(CaptureMethod::Auto)) {
        error = "Ekran yakalama başlatılamadı: " + m_capture->lastError();
        m_capture.reset();
        return false;
    }
    m_ocr = std::make_unique<RapidOcrEngine>();
    m_ocr->setLanguage("ch");
    m_ocr->setPaths(m_init.pluginDir + L"\\bin", m_init.pluginDir + L"\\models", m_init.dataDir + L"\\ocr");
    if (!m_ocr->init(win::wideToUtf8(m_init.dataDir))) {
        error = m_ocr->lastError();
        m_ocr.reset();
        return false;
    }
    m_frame.pixels.reserve(1920 * 200 * 4);
    m_host.info("OCR hazır (RapidOcrOnnx, PP-OCRv3)");
    return true;
}

// Port of live_core.cpp captureAndRunOcr: an unchanged frame returns the previous text without OCR.
bool Engine::readRegion(void* hwnd, const CaptureRegion& region, std::string& textOut, std::string& error)
{
    if (!m_capture->captureRegion(hwnd, region, m_frame)) {
        error = "Ekran yakalanamadı: " + m_capture->lastError();
        m_lastHash = 0;
        return false;
    }
    uint64_t hash = frameHash(m_frame.pixels.data(), m_frame.pixels.size());
    hash ^= (static_cast<uint64_t>(static_cast<uint32_t>(region.x)) << 32 | static_cast<uint32_t>(region.y))
            * 0x9E3779B97F4A7C15ULL;
    hash ^= (static_cast<uint64_t>(static_cast<uint32_t>(region.width)) << 32 | static_cast<uint32_t>(region.height))
            * 0xC2B2AE3D27D4EB4FULL;
    hash ^= reinterpret_cast<uintptr_t>(hwnd);
    if (hash == m_lastHash) {
        textOut = m_lastOcr;
        return true;
    }
    m_lastHash = hash;
    std::string text;
    for (const auto& box : m_ocr->recognize(m_frame.pixels.data(), m_frame.width, m_frame.height)) {
        if (!text.empty()) text += "\n";
        text += box.text;
    }
    text = dropForeignScriptTokens(text);   // Latin-script source: CJK tokens are model noise
    text = m_spacing.fix(text);             // "Iwas goingto" → "I was going to"
    m_lastOcr = text;
    textOut = std::move(text);
    return true;
}

std::string Engine::resolveTarget(void*& hwnd, CaptureRegion& region, console::Box& overlay, bool needGame)
{
    std::string mode, key, gameName;
    unsigned long pid = 0;
    std::vector<std::string> exes;
    std::optional<console::Box> pc;
    console::Frac frac;
    {
        std::lock_guard lock(m_mutex);
        mode = m_mode;
        key = m_sourceKey;
        pid = m_sourcePid;
        exes = m_customExes;
        pc = m_pcRegion;
        frac = m_consoleRegion;
        gameName = m_gameName;
    }
    hwnd = nullptr;
    if (mode == "console") {
        // The console window's own content (PrintWindow): read even when covered, never minimised
        if ((!m_source || !console::alive(*m_source) || (pid && m_source->pid != pid)) && now() >= m_nextPick) {
            m_nextPick = now() + 1.0;
            m_source = pickSource(key, pid, exes);
        }
        const std::string reason = console::blockReason(m_source ? &*m_source : nullptr);
        if (!reason.empty()) return reason;
        hwnd = m_source->hwnd;
        const console::Box b = console::clientRegion(hwnd, frac);
        region = {b.x, b.y, b.w, b.h};
        overlay = console::regionToScreen(console::videoRect(console::clientRect(hwnd)), frac);
        return {};
    }
    if (!pc) return "Önce okunacak bölgeyi seç.";
    if (needGame && gameName.empty()) return "Oyun algılanmadı · oyunu Makine'den başlat ya da konsol modunu kullan.";
    region = {pc->x, pc->y, pc->w, pc->h};
    overlay = *pc;
    return {};
}

void Engine::handleEvents(const std::vector<LineEvent>& events)
{
    for (const LineEvent& e : events) {
        if (e.kind == LineEvent::New) {
            m_scheduler.sourceNew(e.id, e.t);
            std::lock_guard lock(m_mutex);
            ++m_lines;
        } else if (e.kind == LineEvent::Gone) {
            m_scheduler.sourceGone(e.id, e.t);
            std::lock_guard lock(m_mutex);
            m_goneAt[e.id] = e.t;
            if (m_goneAt.size() > 400) m_goneAt.erase(m_goneAt.begin());
        } else {
            const std::string text = text::toUtf8(e.text);
            m_lineSource[e.id] = text;
            if (m_lineSource.size() > 200) m_lineSource.erase(m_lineSource.begin());
            int seq = 0;
            std::string slug, name;
            {
                std::lock_guard lock(m_mutex);
                seq = ++m_dispatchSeq[e.id];
                if (m_dispatchSeq.size() > 400) m_dispatchSeq.erase(m_dispatchSeq.begin());
                slug = currentSlug();
                name = m_mode == "console" ? std::string(console::kGameName) : m_gameName;
            }
            if (!slug.empty()) post([this, id = e.id, text, seq, slug, name] { translateJob(id, text, seq, slug, name); });
        }
    }
}

void Engine::showOverlay(const Display& d, const console::Box& rect)
{
    json style;
    {
        std::lock_guard lock(m_mutex);
        style = m_style;
    }
    const std::string text = text::toUtf8(d.text);
    json line = {{"text", text}};
    if (style.value("showSource", false) && m_lineSource.count(d.id)) line["sourceText"] = m_lineSource[d.id];
    json payload = {{"text", text}, {"lines", json::array({line})}, {"x", rect.x}, {"y", rect.y},
                    {"width", rect.w}, {"height", rect.h}, {"kind", d.kind}};
    if (!style.empty()) payload["style"] = style;
    m_host.emitEvent("overlay.show", dump(payload));
    m_overlayShown = true;
    m_shownKeys.push_back(text::compactPoolKey(d.text));
    if (m_shownKeys.size() > 4) m_shownKeys.erase(m_shownKeys.begin());
}

void Engine::hideOverlay()
{
    if (!m_overlayShown) return;
    m_host.emitEvent("overlay.hide", "{}");
    m_overlayShown = false;
}

void Engine::stopTracking()
{
    m_tracker.reset();
    m_scheduler.clear();
    m_shown.reset();
    hideOverlay();
    m_lastHash = 0;
    m_lastRaw.clear();
    m_lastFiltered.clear();
    std::lock_guard lock(m_mutex);
    m_results.clear();
}

void Engine::runCapture()
{
    // GDI then uses physical pixels, whatever the host process declares
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::string activeMode;
    int seenStyle = 0;
    std::unique_lock lock(m_mutex);
    while (!m_quit) {
        if (!m_captureJobs.empty()) {
            auto job = std::move(m_captureJobs.front());
            m_captureJobs.pop_front();
            lock.unlock();
            try {
                job();
            } catch (const std::exception& e) {
                m_host.error(std::string("Okuma: ") + e.what());
            }
            lock.lock();
            continue;
        }
        const bool running = m_running;
        const std::string mode = m_mode;
        const int styleVersion = m_styleVersion;
        lock.unlock();

        double wait = kPollS;
        const double t0 = now();
        try {
            if (!running) {
                if (m_active) {
                    m_active = false;
                    stopTracking();
                    setStatus("Durduruldu");
                }
                wait = -1;
            } else {
                std::string error;
                if (!m_active || mode != activeMode) {
                    if (m_active) stopTracking();   // the mode changed under a running session
                    activeMode = mode;
                    m_active = true;
                    std::lock_guard guard(m_mutex);
                    m_lastError.clear();
                }
                if (!ensureOcr(error)) {
                    notifyError(error);
                    {
                        std::lock_guard guard(m_mutex);
                        m_running = false;
                    }
                    m_active = false;
                    setStatus(error);
                    publish();
                    wait = -1;
                } else {
                    void* hwnd = nullptr;
                    CaptureRegion region;
                    console::Box overlay;
                    const std::string reason = resolveTarget(hwnd, region, overlay, true);
                    std::string filtered;
                    bool read = false;
                    if (!reason.empty()) {
                        setStatus(reason);
                        wait = kBlockedPollS;
                    } else {
                        std::string raw;
                        if (readRegion(hwnd, region, raw, error)) {
                            read = true;
                            if (raw != m_lastRaw) {
                                m_lastRaw = raw;
                                m_lastFiltered = m_garbage.filterOcr(raw, kSourceLang);
                            }
                            filtered = m_lastFiltered;
                            setStatus(mode == "console" && m_source ? "Okunuyor · " + m_source->app.label
                                                                    : std::string("Okunuyor"));
                        } else {
                            setStatus(error);
                            wait = kBlockedPollS;
                        }
                    }
                    // Our own overlay read back from the screen is never a source line
                    if (!filtered.empty()) {
                        const std::u32string k = text::compactPoolKey(text::fromUtf8(filtered));
                        for (const auto& shown : m_shownKeys)
                            if (keysSimilar(k, shown) || text::sequenceRatio(k, shown) >= 0.8) filtered.clear();
                    }
                    const double t = now();
                    handleEvents(read ? m_tracker.update(text::fromUtf8(filtered), t) : m_tracker.reset());

                    std::deque<Translated> results;
                    {
                        std::lock_guard guard(m_mutex);
                        results.swap(m_results);
                    }
                    for (const auto& r : results) {
                        if (r.text.empty()) m_scheduler.translationFailed(r.line);
                        else m_scheduler.translationReady(r.line, text::fromUtf8(r.text), r.kind, t);
                    }
                    const auto d = m_scheduler.tick(t);
                    if (d && (!m_shown || m_shown->id != d->id || m_shown->text != d->text || styleVersion != seenStyle)) {
                        if (reason.empty()) showOverlay(*d, overlay);
                        m_shown = d;
                        seenStyle = styleVersion;
                    } else if (!d && m_shown) {
                        hideOverlay();
                        m_shown.reset();
                    }
                }
            }
        } catch (const std::exception& e) {
            notifyError(std::string("Beklenmeyen hata: ") + e.what());
            wait = kBlockedPollS;
        }

        lock.lock();
        const auto woken = [this] { return m_quit || !m_captureJobs.empty() || m_running != (m_active); };
        if (wait < 0) {
            m_captureCv.wait(lock, [this] { return m_quit || !m_captureJobs.empty() || m_running; });
        } else {
            const double left = std::max(0.0, wait - (now() - t0));
            m_captureCv.wait_for(lock, std::chrono::milliseconds(static_cast<int>(left * 1000)), woken);
        }
    }
    lock.unlock();
    hideOverlay();
    lock.lock();
    ++m_finished;
    m_captureCv.notify_all();
}

} // namespace live

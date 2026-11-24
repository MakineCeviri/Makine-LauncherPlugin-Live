// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "engine.h"

#include "unitext.h"
#include "winutil.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <ctime>
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
constexpr auto kShutdownBudget = std::chrono::milliseconds(1500);
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

uint64_t frameHash(const uint8_t* data, size_t size)   // as 0.3.x live_core.cpp
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
    // Steam Link, Moonlight, Parsec, GeForce NOW can show any PC game, and console mode is the GTA VI
    // pool: they are read only when the user picked them (console.select)
    for (const auto& s : found)
        if (s.app.console != "stream" && s.app.console != "cloud") return s;
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

unsigned long parentPid()   // the launcher that started this plugin host
{
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    unsigned long parent = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (e.th32ProcessID == GetCurrentProcessId()) parent = e.th32ParentProcessID;
    CloseHandle(snap);
    return parent;
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
    m_launcherPid = parentPid();
    m_startedAt = static_cast<int64_t>(std::time(nullptr));
    m_autoStart = flag(m_init.settings, "autoStart");
    if (m_init.settings.is_object() && m_init.settings.contains("usageReport") && m_init.settings["usageReport"].is_boolean())
        m_usageReport = m_init.settings["usageReport"].get<bool>();
    const std::string token = m_settings.get("authToken");
    if (validToken(token) && m_settings.get("authServer") == serverUrl()) {   // a token is for its own server
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
    if (const std::string out = m_settings.get("output"); out == "tv" || out == "phone") m_output = out;
    m_obs = m_settings.get("obs") == "true";
    m_donateSeen = m_settings.get("donateSeen") == "true";
    m_consoleConsent = m_settings.get("consoleConsent") == "true";
    m_everStarted = m_settings.get("everStarted") == "true";
    m_lanOnce = m_settings.get("lanOnce") == "true";
    m_tvSaved = json::parse(m_settings.get("tv", "null"), nullptr, false);
    if (m_tvSaved.is_discarded()) m_tvSaved = nullptr;
    m_tvCode = m_settings.get("tvCode");
    m_customExes = splitOn(m_settings.get("customExes"), '|');
    const json style = json::parse(m_settings.get("style", "{}"), nullptr, false);
    if (style.is_object()) m_style = style;
}

void Engine::launch()
{
    m_captureThread = std::thread([self = shared_from_this()] { self->runCapture(); });
    for (int i = 0; i < 2; ++i) m_netThreads.emplace_back([self = shared_from_this()] { self->runNet(); });
    startOutputs();
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
    json bye;
    {
        std::lock_guard lock(m_mutex);
        if (m_usageReport && m_loginState == "signedIn") bye = beatPayloadLocked(true);
    }
    if (!bye.is_null()) m_server.beat(bye, 700);   // short: the host's whole shutdown budget is 3 s
    stopOutputs();
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
        logLine(MAKINE_LOG_WARN, "İş parçacıkları zamanında durmadı; süreç kapanırken kesilecek.");
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
    json state = {
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
        {"autoStart", m_autoStart},
        {"usageReport", m_usageReport},
        {"consoleConsent", m_consoleConsent},
        {"everStarted", m_everStarted},
        {"game", {{"name", console ? console::kGameName : m_gameName},
                  {"slug", console ? console::kGameSlug : m_gameSlug}}},
    };
    state["reason"] = reasonLocked();
    const json outputs = outputsStateLocked();   // a named value: items() of a temporary would dangle
    for (const auto& [k, v] : outputs.items()) state[k] = v;
    return state;
}

void Engine::publish()
{
    std::string payload;
    {
        std::lock_guard lock(m_mutex);
        payload = dump(stateLocked());
    }
    m_host.emitEvent("ui.state", payload);
    updateDiscord();
    updateTvNote();
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
        ++m_errors;
        if (message == m_lastError) return;
        m_lastError = message;
    }
    logLine(MAKINE_LOG_ERROR, message);
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
    m_settings.set("output", m_output);
    m_settings.set("obs", m_obs ? "true" : "false");
    m_settings.set("donateSeen", m_donateSeen ? "true" : "false");
    m_settings.set("consoleConsent", m_consoleConsent ? "true" : "false");
    m_settings.set("everStarted", m_everStarted ? "true" : "false");
    m_settings.set("lanOnce", m_lanOnce ? "true" : "false");
    m_settings.set("tv", dump(m_tvSaved));
    m_settings.set("tvCode", m_tvCode);
    m_settings.set("user", m_user);
    m_settings.set("authToken", m_server.token());
    m_settings.set("authServer", m_server.token().empty() ? std::string() : serverUrl());
    if (!m_settings.save()) logLine(MAKINE_LOG_WARN, "Ayarlar kaydedilemedi (settings.cfg).");
}

std::string Engine::currentSlug() const
{
    return m_mode == "console" ? std::string(console::kGameSlug) : m_gameSlug;
}

void Engine::setHostSetting(const std::string& key, bool value)
{
    post([this, key, value] {
        const Result r = m_host.call("settings.set", dump({{"key", key}, {"value", value}}));
        if (!r.succeeded()) logLine(MAKINE_LOG_WARN, "Ayar Makine'ye kaydedilemedi: " + key);
    });
}

// The launcher said a game started and the user wants Makine Live to open with it.
void Engine::maybeAutoStart()
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_autoStart || m_running || !m_everStarted) return;   // nothing is captured before the first explicit start
        if (m_loginState == "checking" || m_loginState == "offline") {   // the session is still being checked
            m_pendingStart = true;
            return;
        }
    }
    const Result r = startReading();
    if (!r.succeeded()) logLine(MAKINE_LOG_INFO, "Kendiliğinden başlatılamadı: " + r.json);
}

// ── usage report (net/telemetry.py) ─────────────────────────────────────────

json Engine::beatPayloadLocked(bool bye)
{
    static const json system = [] {
        json info = json::object();
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof v;
        if (const auto fn = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
            fn(&v);
        info["os"] = std::string(v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000 ? "Windows 11"
                                                                                     : "Windows " + std::to_string(v.dwMajorVersion))
                     + " (" + std::to_string(v.dwBuildNumber) + ")";
        wchar_t cpu[256] = {};
        DWORD size = sizeof cpu;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                         L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, cpu, &size) == ERROR_SUCCESS)
            info["cpu"] = text::toUtf8(text::join(text::split(text::fromUtf8(win::wideToUtf8(cpu))), U" ")).substr(0, 120);
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof mem;
        if (GlobalMemoryStatusEx(&mem)) info["ram_gb"] = std::nearbyint(mem.ullTotalPhys / 107374182.4) / 10.0;
        info["screen"] = std::to_string(GetSystemMetrics(SM_CXSCREEN)) + "x" + std::to_string(GetSystemMetrics(SM_CYSCREEN));
        info["monitors"] = GetSystemMetrics(SM_CMONITORS);
        info["scale"] = std::nearbyint(GetDpiForSystem() / 96.0 * 100) / 100;
        return info;
    }();
    const auto median = [](const std::deque<double>& xs) -> json {
        if (xs.empty()) return nullptr;
        std::vector<double> v(xs.begin(), xs.end());
        std::sort(v.begin(), v.end());
        return static_cast<int64_t>(std::nearbyint(v[v.size() / 2]));
    };
    const bool console = m_mode == "console";
    json info = system;
    const int looked = m_poolCount + m_apiCount;
    const auto pool = m_poolSlug.empty() ? nullptr : m_pool.get(m_poolSlug);
    const json extra = {{"ocr_ms", median(m_ocrMs)}, {"pool_ms", median(m_poolMs)}, {"api_ms", median(m_apiMs)},
                        {"hit_pct", looked ? json(static_cast<int64_t>(std::nearbyint(100.0 * m_poolCount / looked))) : json(nullptr)},
                        {"local_lines", pool ? pool->size() : 0}, {"errors", m_errors}, {"last_error", m_lastError},
                        {"uptime_s", static_cast<int64_t>(std::time(nullptr)) - m_startedAt},
                        {"client", "launcher/" + m_init.launcherVersion}};
    for (const auto& [k, v] : extra.items())
        if (!v.is_null() && !(v.is_string() && v.get<std::string>().empty())) info[k] = v;
    const int runS = static_cast<int>(m_runAcc);
    m_runAcc -= runS;   // the fraction goes into the next report
    const int lines = std::max(0, m_linesTotal - m_linesSent);
    m_linesSent = std::max(m_linesTotal, m_linesSent);
    json payload = {{"started_at", m_startedAt},
                    {"running", m_running},
                    {"mode", console ? "konsol" : "pc"},
                    {"output", "box"},
                    {"game", console ? std::string(console::kGameSlug) : m_gameSlug},
                    {"game_name", console ? std::string(console::kGameName) : m_gameName},
                    {"session_lines", m_sessionLines},
                    {"info", info},
                    {"run_s", runS},
                    {"lines", lines}};
    if (bye) payload["bye"] = true;
    return payload;
}

void Engine::beatLoop(int gen)
{
    json payload;
    {
        std::lock_guard lock(m_mutex);
        if (gen != m_beatGen || m_loginState != "signedIn" || !m_usageReport) return;
        payload = beatPayloadLocked(false);
    }
    const ApiResult r = m_server.beat(payload);
    double next = 300;
    {
        std::lock_guard lock(m_mutex);
        if (gen != m_beatGen) return;
        if (r.ok()) m_beatS = std::clamp(num(r.data, "beat_s", 300), 60.0, 1800.0);
        next = m_beatS;
    }
    if (r.ok()) onLogRequest(str(r.data, "log_request"));   // the team asked for this session's log
    // r.data.log_request (the team asks for the app log) is not answered: the plugin keeps no log file of its
    // own; its log lines are in the launcher's plugin log.
    post([this, gen] { beatLoop(gen); }, next);
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
            json bye;
            {
                std::lock_guard lock(m_mutex);
                if (m_usageReport && m_loginState == "signedIn") bye = beatPayloadLocked(true);
            }
            if (!bye.is_null()) m_server.beat(bye, 1500);
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
        applyOutputs();
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
    if (method == "readOnce") return readOnce();   // an explicit action of the player too
    if (method == "consoleConsent.set") {
        {
            std::lock_guard lock(m_mutex);
            m_consoleConsent = flag(params, "granted");
            saveSettings();
        }
        publish();
        return okMessage(flag(params, "granted") ? "Onaylandı." : "Onay geri alındı.");
    }
    if (method == "autoStart.set" || method == "usageReport.set") {
        const bool on = flag(params, "enabled");
        const std::string key = method == "autoStart.set" ? "autoStart" : "usageReport";
        int gen = 0;
        {
            std::lock_guard lock(m_mutex);
            (key == "autoStart" ? m_autoStart : m_usageReport) = on;
            if (key == "usageReport" && on && m_loginState == "signedIn") gen = ++m_beatGen;
        }
        setHostSetting(key, on);   // the launcher keeps it: it starts the plugin on game.started without a host
        publish();
        if (gen) post([this, gen] { beatLoop(gen); }, 2);
        return Result::ok(dump({{key, on}}));
    }
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
        applyStyleOutputs();
        publish();
        std::lock_guard lock(m_mutex);
        return Result::ok(dump({{"style", m_style}}));
    }
    if (auto r = invokeOutputs(method, params)) return *r;
    return Result::notFound(method);
}

void Engine::onEvent(std::string_view event, const json& payload)
{
    if (event == "game.started") {
        const std::string name = str(payload, "name");
        if (name.empty()) return;
        {
            std::lock_guard lock(m_mutex);
            // A console stream app (Remote Play, Asobi, Xbox…) shows the console's game: GTA VI (0.3.2)
            const bool stream = console::isConsoleStream(name) || console::isConsoleStream(str(payload, "exe"));
            m_gameName = stream ? std::string(console::kGameName) : name;
            m_gameSlug = stream ? std::string(console::kGameSlug) : text::gameSlug(name);
            m_gamePid = static_cast<unsigned long>(num(payload, "pid", 0));   // optional (contract addition)
        }
        logLine(MAKINE_LOG_INFO, "Oyun: " + name);
        publish();
        post([this] { syncPool(true); });
        maybeAutoStart();
    } else if (event == "settings.changed") {
        const std::string key = str(payload, "key");
        if (key != "autoStart" && key != "usageReport") return;
        const bool on = flag(payload, "value");
        int gen = 0;
        {
            std::lock_guard lock(m_mutex);
            (key == "autoStart" ? m_autoStart : m_usageReport) = on;
            if (key == "usageReport" && on && m_loginState == "signedIn") gen = ++m_beatGen;
        }
        publish();
        if (gen) post([this, gen] { beatLoop(gen); }, 2);
    } else if (event == "game.stopped") {
        {
            std::lock_guard lock(m_mutex);
            m_gameName.clear();
            m_gameSlug.clear();
            m_gamePid = 0;
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
            if (!opened.succeeded()) logLine(MAKINE_LOG_WARN, "Tarayıcı açılamadı; bağlantı ekranda: " + url);
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
    logLine(MAKINE_LOG_INFO, "Giriş: " + m_user);
    publish();
    post([this] { syncPool(true); });
    bool start = false;
    int gen = 0;
    {
        std::lock_guard lock(m_mutex);
        gen = ++m_beatGen;
        start = std::exchange(m_pendingStart, false);
    }
    post([this, gen] { beatLoop(gen); }, 2);   // usage report 2 s after sign-in
    post([this] { refreshStatus(); });          // announcements need the session
    if (start) maybeAutoStart();
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
        ++m_beatGen;
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
    if (r.ok()) onStatus(r.data);
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
        logLine(MAKINE_LOG_INFO, "Yerel havuz " + slug + ": +" + std::to_string(st.added) + " ~" + std::to_string(st.changed) + " −"
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
        if (m_mode == "console" && !m_consoleConsent)   // the screen asks once (consoleConsent.set)
            return Result::error(MAKINE_E_DENIED, "consent_required: Konsol modunda okunan her satır Makine Çeviri'nin "
                                                  "GTA VI çeviri havuzuna gider. Onaylıyor musun?");
        m_running = true;
        if (!m_everStarted) {
            m_everStarted = true;
            saveSettings();
        }
        m_sessionLines = 0;
        m_readingSince = static_cast<int64_t>(std::time(nullptr));
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
    m_netCv.notify_all();   // translateJob waits on it too
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
            logLine(MAKINE_LOG_ERROR, std::string("İş: ") + e.what());
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
                ++m_poolCount;
                ++m_linesTotal;
                ++m_sessionLines;
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
    std::string notice, noticeCode;
    double requestMs = 0;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        const auto sent = Clock::now();
        res = m_server.translate(slug, gameName, textUtf8, kSourceLang, kTargetLang);
        requestMs = std::chrono::duration<double, std::milli>(Clock::now() - sent).count();
        bool retry = false;
        if (res.ok()) break;
        if (res.status == 401 || res.code() == "banned") {
            handleAuthFailure(res);
            break;
        }
        if (res.network()) {
            notice = "Sunucuya ulaşılamıyor · yeniden deneniyor";
            noticeCode = "offline";
            retry = true;
        } else if (res.status == 422) {
            std::lock_guard lock(m_mutex);
            m_skipKeys.insert(key);
            logLine(MAKINE_LOG_DEBUG, "Sunucu çöp saydı (" + str(res.data, "reason") + "): " + textUtf8);
            break;
        } else if (res.status == 429) {
            notice = "Günlük çeviri sınırı doldu — havuzdaki çeviriler gelmeye devam ediyor";
            noticeCode = "quota";
            break;
        } else if (res.code() == "maintenance") {
            post([this] { refreshStatus(); });
            break;
        } else if (res.status == 503) {
            notice = "Sunucuda çeviri anahtarı yok · yalnızca havuzdaki satırlar gelir";
            noticeCode = "no_key";
            break;
        } else {
            logLine(MAKINE_LOG_WARN, "Sunucu " + std::to_string(res.status) + ": " + res.error);
            retry = res.status == 502 || res.status == 504;
            if (retry) notice = "Çeviri servisi yanıt vermedi · yeniden deneniyor";
            if (retry) noticeCode = "upstream";
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
        if (!notice.empty()) {
            m_notice = notice;
            m_noticeCode = noticeCode;
        }
        if (!translated.empty()) {
            if (m_notice.find("yeniden deneniyor") != std::string::npos) {
                m_notice.clear();
                m_noticeCode.clear();
            }
            m_cache[key] = {translated, kind, now()};
            if (m_cache.size() > 1000) m_cache.erase(m_cache.begin());
            if (kind != "api") ++m_poolHits;
            ++m_linesTotal;
            ++m_sessionLines;
            auto& samples = kind == "api" ? m_apiMs : m_poolMs;
            samples.push_back(requestMs);
            if (samples.size() > 50) samples.pop_front();
            ++(kind == "api" ? m_apiCount : m_poolCount);
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
    bool console = false;
    {
        std::lock_guard lock(m_mutex);
        console = m_mode == "console";
    }
    // K8: PC mode leaves the cores to the game (one OCR thread, the capture thread below normal);
    // console mode has the PC to itself (two threads, normal priority)
    const int threads = console ? 2 : 1;
    SetThreadPriority(GetCurrentThread(), console ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
    if (m_ocr && m_ocr->threads() == threads && m_capture) return true;
    const std::wstring words = m_init.pluginDir + L"\\resources\\en_words.txt";
    const auto list = win::readFile(words);
    if (!list || !m_garbage.setWords(*list)) {
        error = "Kelime listesi eksik (resources\\en_words.txt). Makine'yi onarın ya da yeniden kurun.";
        return false;
    }
    m_spacing.load(words);
    if (!m_capture) {
        m_capture = std::make_unique<ScreenCapture>();
        if (!m_capture->init(CaptureMethod::Auto)) {
            error = "Ekran yakalama başlatılamadı: " + m_capture->lastError();
            m_capture.reset();
            return false;
        }
    }
    // The OCR reads the captured frame in memory: no file is written per frame
    auto ocr = std::make_unique<OcrOrt>();
    const std::wstring models = m_init.pluginDir + L"\\models\\";
    if (!ocr->load({m_init.pluginDir + L"\\bin\\onnxruntime.dll", models + L"ch_PP-OCRv3_det_infer.onnx",
                    models + L"ch_PP-OCRv3_rec_infer.onnx", models + L"ppocr_keys_v1.txt", threads},
                   error))
        return false;
    m_ocr = std::move(ocr);
    m_lastHash = 0;
    m_frame.pixels.reserve(1920 * 200 * 4);
    logLine(MAKINE_LOG_INFO, "OCR hazır: " + m_ocr->describe());
    return true;
}

// An unchanged frame returns the previous text without OCR (as 0.3.x captureAndRunOcr).
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
    const auto ocrStart = Clock::now();
    std::string ocrError;
    const auto lines = m_ocr->run(m_frame.pixels.data(), m_frame.width, m_frame.height, ocrError);
    if (!ocrError.empty()) logLine(MAKINE_LOG_WARN, ocrError);
    const double ocrMs = std::chrono::duration<double, std::milli>(Clock::now() - ocrStart).count();
    if (ocrMs > 40) {   // as the harness: only real OCR runs, not unchanged frames
        std::lock_guard lock(m_mutex);
        m_ocrMs.push_back(ocrMs);
        if (m_ocrMs.size() > 50) m_ocrMs.pop_front();
    }
    for (const auto& line : lines) {
        if (!text.empty()) text += "\n";
        text += line;
    }
    text = dropForeignScriptTokens(text);   // Latin-script source: CJK tokens are model noise
    text = m_spacing.fix(text);             // "Iwas goingto" → "I was going to"
    m_lastOcr = text;
    textOut = std::move(text);
    return true;
}

std::string Engine::resolveTarget(void*& hwnd, CaptureRegion& region, console::Box& overlay, bool needGame,
                                  std::string* code)
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
        if (code) *code = console::blockCode(m_source ? &*m_source : nullptr);
        if (!reason.empty()) return reason;
        hwnd = m_source->hwnd;
        const console::Box b = console::clientRegion(hwnd, frac);
        region = {b.x, b.y, b.w, b.h};
        overlay = console::regionToScreen(console::videoRect(console::clientRect(hwnd)), frac);
        return {};
    }
    if (!pc) {
        if (code) *code = "region_missing";
        return "Önce okunacak bölgeyi seç.";
    }
    if (needGame) {
        // Read only while the game is in front (the harness's target lock): text of another
        // window over the region (Discord, a browser) must never reach the pool
        const HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        unsigned long gamePid = 0;
        {
            std::lock_guard lock(m_mutex);
            gamePid = m_gamePid;
        }
        RECT r{};
        const int cx = pc->x + pc->w / 2, cy = pc->y + pc->h / 2;
        const bool covers = fg && GetWindowRect(fg, &r) && cx >= r.left && cx < r.right && cy >= r.top && cy < r.bottom;
        if (!fg || fgPid == GetCurrentProcessId() || fgPid == m_launcherPid || (gamePid && fgPid != gamePid) || !covers)
        {
            if (code) *code = "game_not_front";
            return "Oyun önde değil · oyuna dönünce okuma sürer.";
        }
    }
    if (needGame && gameName.empty() && code) *code = "game_missing";
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
    if (boxWanted()) {   // console on the TV / phone: no box over the Remote Play window
        m_host.emitEvent("overlay.show", dump(payload));
        m_boxShown = true;
    } else if (m_boxShown) {
        m_host.emitEvent("overlay.hide", "{}");
        m_boxShown = false;
    }
    routeLine(d);
    m_overlayShown = true;
    m_shownKeys.push_back(text::compactPoolKey(d.text));
    ++m_shownCount;
    if (m_shownKeys.size() > 4) m_shownKeys.erase(m_shownKeys.begin());
}

// OCR boxes that are (pieces of) a translation we showed: with screen capture the overlay
// sits inside the region it reads.
std::string Engine::withoutOwnOverlay(const std::string& raw) const
{
    if (m_shownKeys.empty()) return raw;
    std::string kept;
    size_t b = 0;
    for (size_t i = 0; i <= raw.size(); ++i) {
        if (i < raw.size() && raw[i] != '\n') continue;
        const std::string line = raw.substr(b, i - b);
        b = i + 1;
        const std::u32string k = text::compactPoolKey(text::fromUtf8(line));
        bool own = false;
        for (const auto& shown : m_shownKeys)
            own = own || k == shown || keysSimilar(k, shown) || (k.size() >= 8 && shown.find(k) != std::u32string::npos)
                  || (shown.size() >= 8 && k.find(shown) != std::u32string::npos)
                  || (k.size() >= 4 && text::sequenceRatio(k, shown) >= 0.8);
        if (own) continue;
        if (!kept.empty()) kept += '\n';
        kept += line;
    }
    return kept;
}

void Engine::hideOverlay()
{
    if (!m_overlayShown) return;
    if (m_boxShown) m_host.emitEvent("overlay.hide", "{}");
    m_boxShown = false;
    routeClear();
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
    double lastLoop = 0;
    std::unique_lock lock(m_mutex);
    while (!m_quit) {
        if (!m_captureJobs.empty()) {
            auto job = std::move(m_captureJobs.front());
            m_captureJobs.pop_front();
            lock.unlock();
            try {
                job();
            } catch (const std::exception& e) {
                logLine(MAKINE_LOG_ERROR, std::string("Okuma: ") + e.what());
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
        if (running && lastLoop > 0) {
            std::lock_guard guard(m_mutex);
            m_runAcc += std::min(1.0, t0 - lastLoop);   // reading time for the usage report
        }
        lastLoop = running ? t0 : 0;
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
                    std::string code;
                    const std::string reason = resolveTarget(hwnd, region, overlay, true, &code);
                    std::string filtered;
                    bool read = false;
                    if (!reason.empty()) {
                        setStatus(reason);
                        wait = kBlockedPollS;
                    } else {
                        std::string raw;
                        if (readRegion(hwnd, region, raw, error)) {
                            read = true;
                            // A console window that gives only black (HDCP on a capture card, a GPU path
                            // PrintWindow cannot see): checked on the whole window every few seconds
                            bool dark = hwnd != nullptr;
                            for (size_t i = 0; dark && i < m_frame.pixels.size(); i += 4 * 97)
                                dark = m_frame.pixels[i] < 16 && m_frame.pixels[i + 1] < 16 && m_frame.pixels[i + 2] < 16;
                            const double t = now();
                            if (!dark) m_darkSince = -1;
                            else if (m_darkSince < 0) m_darkSince = t;
                            if (dark && t - m_darkSince > 3 && t >= m_blackCheckAt) {
                                m_blackCheckAt = t + 5;
                                if (!console::windowHasImage(hwnd)) code = "black_frame";
                                else m_darkSince = t;   // a dark scene: look again later
                            }
                            // Our own overlay read back from the screen is never a source line: its
                            // boxes go before the boxes are joined into one sentence. A window read
                            // through PrintWindow (console) never contains the overlay
                            if (raw != m_lastRaw || m_shownCount != m_filteredWith) {
                                m_lastRaw = raw;
                                m_filteredWith = m_shownCount;
                                m_lastFiltered = m_garbage.filterOcr(hwnd ? raw : withoutOwnOverlay(raw), kSourceLang);
                            }
                            filtered = m_lastFiltered;
                            setStatus(mode == "console" && m_source ? "Okunuyor · " + m_source->app.label
                                                                    : std::string("Okunuyor"));
                        } else {
                            code = hwnd && !console::windowHasImage(hwnd) ? "black_frame" : "capture_failed";
                            setStatus(error);
                            wait = kBlockedPollS;
                        }
                    }
                    bool codeChanged = false;
                    {
                        std::lock_guard guard(m_mutex);
                        codeChanged = code != m_captureCode;
                        m_captureCode = code;
                    }
                    if (codeChanged) publish();
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

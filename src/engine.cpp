// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "engine.h"

#include "finder_ocr.h"

#include "unitext.h"
#include "winutil.h"

#include <windows.h>
#include <tlhelp32.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
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
constexpr double kMaskMaxSkipS = 2.5;      // K3: at most this long on the mask's word (G-RATE ≤ 0.5/s)
constexpr double kStaleJobS = 2.0;        // a line whose source left this long ago is not translated any more
constexpr double kCacheTtlS = 900;        // unapproved translations are reused this long in a session
constexpr double kPoolSyncS = 180;        // local pool vs server while reading
constexpr double kMaintenancePollS = 30;   // while maintenance is on: back as soon as it ends
constexpr double kStatusPollS = 300;      // maintenance / version check
constexpr double kOfflineRetryS = 30;
constexpr auto kShutdownBudget = std::chrono::milliseconds(700);   // + bye 400 + Discord 200 < 1.5 s
constexpr auto kReadOnceTimeout = std::chrono::seconds(25);   // invoke times out at 30 s
constexpr const char* kSourceLang = "en";  // product decision: English → Turkish only
constexpr const char* kTargetLang = "tr";

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

uint64_t wallUs()   // span timestamps: microseconds since the epoch (the steady Clock has no epoch)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

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
    // A custom window (a media player…) only when the player picked it in this session (pid above)
    for (const auto& s : found)
        if (!key.empty() && s.app.key == key && s.app.key != "custom") return s;
    // Streaming / cloud apps can show any PC game: read only when picked (console.select), like custom
    for (const auto& s : found)
        if (s.app.key != "custom" && s.app.console != "stream" && s.app.console != "cloud") return s;
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

constexpr size_t kHistoryLines = 50;

// The client area (screen px) of a process's biggest visible window; nothing when it has none worth reading
std::optional<console::Rect> gameWindowArea(unsigned long pid)
{
    struct Find {
        DWORD pid;
        HWND best = nullptr;
        long area = 0;
    } find{static_cast<DWORD>(pid)};
    EnumWindows(
        [](HWND w, LPARAM p) -> BOOL {
            auto* f = reinterpret_cast<Find*>(p);
            DWORD wp = 0;
            GetWindowThreadProcessId(w, &wp);
            RECT r{};
            if (wp == f->pid && IsWindowVisible(w) && !IsIconic(w) && GetWindowRect(w, &r)) {
                const long a = (r.right - r.left) * (r.bottom - r.top);
                if (a > f->area) {
                    f->area = a;
                    f->best = w;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    if (!find.best) return std::nullopt;
    const console::Rect c = console::clientRect(find.best);
    if (c.r - c.l < 64 || c.b - c.t < 64) return std::nullopt;
    return c;
}
constexpr double kGrowStep = 0.10, kMaxGrow = 0.30;   // a cut line grows the region 10 % at a time, at most 30 %

bool consentGiven(const json& params)   // the screen sends {granted} or {enabled}
{
    return params.is_object() && params.contains("granted") ? flag(params, "granted") : flag(params, "enabled");
}

// The name a game is shown and pooled under. The launcher names games outside its catalogue itself; whatever it
// sends, a path or an exe name never becomes a pool: only the file's stem, and only if letters remain.
std::string gameDisplayName(std::string name)
{
    if (const size_t cut = name.find_last_of("\\/"); cut != std::string::npos) name = name.substr(cut + 1);
    if (name.size() > 4) {
        std::string tail = name.substr(name.size() - 4);
        for (auto& ch : tail) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (tail == ".exe") name.resize(name.size() - 4);
    }
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(0, 1);
    return name.size() > 120 ? name.substr(0, 120) : name;
}

const char* hotkeySetting(const std::string& action)
{
    return action == "start" ? "hotkeyStart" : action == "region" ? "hotkeyRegion" : "hotkeyMove";
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
    for (auto& [action, key] : m_hotkeyKeys) key = str(m_init.settings, hotkeySetting(action), key);
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
    // the player's own console region; before the finder, a saved region other than the default was one
    const std::string manual = m_settings.get("consoleRegionManual");
    const console::Frac d = console::kDefaultRegion;
    m_consoleManual = manual.empty() ? cr.size() == 4 && (std::abs(m_consoleRegion.x - d.x) > 1e-6 || std::abs(m_consoleRegion.y - d.y) > 1e-6
                                                          || std::abs(m_consoleRegion.w - d.w) > 1e-6 || std::abs(m_consoleRegion.h - d.h) > 1e-6)
                                     : manual == "true";
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
    std::weak_ptr<Engine> weak = shared_from_this();
    m_hotkeys = std::make_unique<Hotkeys>([weak](const std::string& action) {
        if (const auto self = weak.lock()) self->onHotkey(action);
    });
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
    if (m_hotkeys) m_hotkeys->configure({});
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
        m_jobs.clear();   // a style save still waiting on its debounce is written here instead
        if (m_styleSaveGen) saveSettings();
    }
    m_captureCv.notify_all();
    m_netCv.notify_all();
    json bye;
    {
        std::lock_guard lock(m_mutex);
        if (m_usageReport && m_loginState == "signedIn") bye = beatPayloadLocked(true);
    }
    if (!bye.is_null()) m_server.beat(bye, 400);   // the launcher ends the host 1.5 s after shutdown starts
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
                   {"role", m_role}, {"message", m_loginMessage}}},
        {"history", json(m_history)},
        {"boxMove", {{"on", m_boxMovable}, {"supported", m_boxMoveSupported}}},
        {"regionGrow", {{"left", m_grow[0]}, {"right", m_grow[1]}, {"top", m_grow[2]}, {"bottom", m_grow[3]}}},
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
        {"counters", countersLocked()},
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
    state["autoRegion"] = autoRegionLocked();
    {
        const json registered = m_hotkeys ? m_hotkeys->state() : json::object();
        json keys = json::object();
        for (const auto& [action, key] : m_hotkeyKeys) {
            keys[action] = registered.contains(action) ? registered[action]
                                                       : json{{"key", key}, {"active", false}, {"error", ""}};
            keys[action]["key"] = key;
        }
        // armed: signed in with a game (or console mode) — only then are the keys taken from other programs
        state["hotkeys"] = {{"armed", m_hotkeysArmed}, {"keys", keys}};
    }
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

void Engine::notifyError(const std::string& message, const char* reason, const char* level)
{
    {
        std::lock_guard lock(m_mutex);
        ++m_errors;
        if (message == m_lastError) return;
        m_lastError = message;
    }
    report(reason, level, message);
    logLine(MAKINE_LOG_ERROR, message);
    m_host.emitEvent("ui.notify", dump({{"title", "Makine Live"}, {"message", message}, {"kind", "error"}}));
}

// Defects go to Sentry through the launcher (one SDK, its consent and gate; the portal reads them there).
// Only with the usage report on. The launcher drops repeats of a reason for a day.
void Engine::report(const char* reason, const char* level, const std::string& message)
{
    if (!reason) return;
    json tags;
    {
        std::lock_guard lock(m_mutex);
        if (!m_usageReport) return;
        const bool console = m_mode == "console";
        tags = {{"mode", console ? "konsol" : "pc"},
                {"game", console ? std::string(console::kGameSlug) : m_gameSlug}};
    }
    m_host.emitEvent("telemetry.capture",
                     dump({{"reason", reason}, {"level", level}, {"message", message}, {"tags", tags}}));
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
    m_settings.set("consoleRegionManual", m_consoleManual ? "true" : "false");
    m_settings.set("tv", dump(m_tvSaved));
    m_settings.set("tvCode", m_tvCode);
    m_settings.set("user", m_user);
    m_settings.set("authToken", m_server.token());
    m_settings.set("authServer", m_server.token().empty() ? std::string() : serverUrl());
    if (!m_settings.save()) logLine(MAKINE_LOG_WARN, "Ayarlar kaydedilemedi (settings.cfg).");
}

bool Engine::consentMissingLocked() const
{
    return currentSlug() == console::kGameSlug && !m_consoleConsent;
}

void Engine::stopIfConsentMissing()
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_running || !consentMissingLocked()) return;
        m_running = false;
    }
    logLine(MAKINE_LOG_INFO, "Okuma durdu: GTA VI havuzu için oyuncunun onayı yok");
    m_captureCv.notify_all();
    publish();
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
        if (r.ok()) m_traceBudget = static_cast<int>(std::clamp(num(r.data, "trace_n", 0), 0.0, 20.0));
        next = m_beatS;
    }
    if (r.ok()) onLogRequest(str(r.data, "log_request"));   // the team asked for this session's log: sent (onLogRequest)
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
    if (method == "login.team") return loginTeam(params);
    if (method == "status.refresh") {   // "Yeniden dene": maintenance, version and announcements now
        post([this] { refreshStatus(); });
        return okMessage("Yenileniyor…");
    }
    if (method == "sample") return showSample();
    if (method == "region.keepGrown") {   // the player keeps the region the cut lines grew
        {
            std::lock_guard lock(m_mutex);
            if (m_grow == std::array<double, 4>{}) return okMessage("Bölge büyümedi.");
            const auto g = m_grow;
            if (m_mode == "pc" && m_pcRegion) {
                console::Box& b = *m_pcRegion;
                const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int vr = vx + GetSystemMetrics(SM_CXVIRTUALSCREEN), vb = vy + GetSystemMetrics(SM_CYVIRTUALSCREEN);
                const int x0 = std::max(vx, b.x - static_cast<int>(b.w * g[0]));
                const int y0 = std::max(vy, b.y - static_cast<int>(b.h * g[2]));
                const int x1 = std::min(vr, b.x + b.w + static_cast<int>(b.w * g[1]));
                const int y1 = std::min(vb, b.y + b.h + static_cast<int>(b.h * g[3]));
                b = {x0, y0, x1 - x0, y1 - y0};
            } else if (m_mode == "console") {
                console::Frac& f = m_consoleRegion;
                const double x0 = std::max(0.0, f.x - f.w * g[0]), y0 = std::max(0.0, f.y - f.h * g[2]);
                const double x1 = std::min(1.0, f.x + f.w * (1 + g[1])), y1 = std::min(1.0, f.y + f.h * (1 + g[3]));
                f = {x0, y0, x1 - x0, y1 - y0};
            }
            if (const auto b = m_finder.band(); b && ((m_mode == "pc" && !m_pcRegion) || (m_mode == "console" && !m_consoleManual))) {
                const double w = b->w(), hh = b->h();   // the finder's band, kept grown for this game
                m_finder.restore({std::max(0.0, b->x0 - w * g[0]), std::max(0.0, b->y0 - hh * g[2]),
                                  std::min(1.0, b->x1 + w * g[1]), std::min(1.0, b->y1 + hh * g[3])},
                                 m_finder.confidence());
                storeBandLocked();
            }
            m_grow = {};
            saveSettings();
        }
        publish();
        return okMessage("Büyüyen bölge kaydedildi.");
    }
    if (method == "box.move") {
        bool on = false;
        {
            std::lock_guard lock(m_mutex);
            on = params.is_object() && params.contains("on") ? flag(params, "on") : !m_boxMovable;
        }
        return setBoxMovable(on);
    }
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
            m_convo.clear();
            m_bandArea.reset();
            saveSettings();
        }
        m_captureCv.notify_all();
        stopIfConsentMissing();
        syncHotkeys();
        applyOutputs();
        post([this] { syncPool(true); });
        return okMessage(mode == "console" ? "Konsol modu" : "PC modu");
    }
    if (method == "console.sources") return listSources(flag(params, "all"));
    if (method == "console.select") return selectSource(params);
    if (method == "region.pick") return pickRegion();
    if (method == "autoRegion.accept") {   // the proposed band is right: keep it for this game
        {
            std::lock_guard lock(m_mutex);
            if (!m_finder.accept()) return Result::error(MAKINE_E_FAILED, "Önerilen bir altyazı bölgesi yok.");
            storeBandLocked();
        }
        publish();
        return okMessage("Altyazı bölgesi bu oyun için kaydedildi.");
    }
    if (method == "autoRegion.reset") {   // forget the band and the player's own region: search again
        {
            std::lock_guard lock(m_mutex);
            if (m_mode == "pc") m_pcRegion.reset();
            else {
                m_consoleManual = false;
                m_consoleRegion = console::kDefaultRegion;
            }
            m_finderSlug = currentSlug();
            if (!m_finderSlug.empty()) m_settings.set("autoRegion:" + m_finderSlug, "");
            m_finder.reset();
            m_grow = {};
            saveSettings();
        }
        m_captureCv.notify_all();
        publish();
        return okMessage("Altyazı aranıyor.");
    }
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
            m_consoleConsent = consentGiven(params);
            saveSettings();
        }
        stopIfConsentMissing();   // consent taken back: reading stops at once
        publish();
        return okMessage(consentGiven(params) ? "Onaylandı." : "Onay geri alındı.");
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
        }
        int gen = 0;
        {
            std::lock_guard lock(m_mutex);
            gen = ++m_styleSaveGen;
        }
        post([this, gen] {   // written once the slider rests for half a second
            std::lock_guard lock(m_mutex);
            if (gen == m_styleSaveGen) saveSettings();
        }, 0.5);
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
        // Catalogue games and, since launcher 0.3, any other game it sees ({gameId: "exe:<exe>", catalog: false})
        const std::string exe = str(payload, "exe"), gameId = str(payload, "gameId");
        const std::string name = gameDisplayName(str(payload, "name"));
        // A console stream app (Remote Play, Asobi, Xbox…) shows the console's game: GTA VI (0.3.2), whichever
        // way the launcher names it; never a PC game of its own
        const bool stream = console::isConsoleStream(str(payload, "name")) || console::isConsoleStream(exe)
                            || (gameId.rfind("exe:", 0) == 0 && console::isConsoleStream(gameId.substr(4)));
        // gameSlug gives a placeholder for a name without letters: such a name is no name
        const bool named = std::any_of(name.begin(), name.end(), [](char ch) {
            return std::isalnum(static_cast<unsigned char>(ch)) || static_cast<unsigned char>(ch) >= 0x80;
        });
        const std::string slug = stream ? std::string(console::kGameSlug) : named ? text::gameSlug(name) : std::string();
        {
            std::lock_guard lock(m_mutex);
            m_gameUnknown = slug.empty();   // no usable name: no pool to send to, so nothing is read
            m_convo.clear();
            m_bandArea.reset();
            m_gameName = stream ? std::string(console::kGameName) : m_gameUnknown ? std::string() : name;
            m_gameSlug = slug;
            m_gamePid = static_cast<unsigned long>(num(payload, "pid", 0));   // optional (contract addition)
            m_gameExe = text::toUtf8(text::lower(text::fromUtf8(exe.substr(exe.find_last_of("\\/") + 1))));
        }
        logLine(MAKINE_LOG_INFO, "Oyun: " + (slug.empty() ? std::string("(adı bilinmiyor)") : name));
        stopIfConsentMissing();
        syncHotkeys();
        publish();
        post([this] { syncPool(true); });
        maybeAutoStart();
    } else if (event == "overlay.moved") {
        // The launcher's word on where the player left the box ({x, y, width, height}, physical px): kept as the
        // box centre in fractions of its monitor, like overlay.setMovable's answer
        const double w = num(payload, "width", 0), h = num(payload, "height", 0);
        if (w <= 0 || h <= 0) return;
        const POINT c{static_cast<LONG>(num(payload, "x", 0) + w / 2), static_cast<LONG>(num(payload, "y", 0) + h / 2)};
        MONITORINFO mi{};
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi)) return;
        const RECT& m = mi.rcMonitor;
        if (m.right <= m.left || m.bottom <= m.top) return;
        {
            std::lock_guard lock(m_mutex);
            m_style["boxX"] = std::clamp((c.x - m.left) / double(m.right - m.left), 0.0, 1.0);
            m_style["boxY"] = std::clamp((c.y - m.top) / double(m.bottom - m.top), 0.0, 1.0);
            ++m_styleVersion;
            saveSettings();
        }
        m_captureCv.notify_all();
        applyStyleOutputs();
        publish();
    } else if (event == "settings.changed") {
        const std::string key = str(payload, "key");
        for (const char* action : {"start", "region", "move"}) {
            if (key != hotkeySetting(action)) continue;
            {
                std::lock_guard lock(m_mutex);
                m_hotkeyKeys[action] = str(payload, "value", "off");
            }
            syncHotkeys();
            return publish();
        }
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
            m_convo.clear();
            m_bandArea.reset();
            m_gameUnknown = false;
            m_gamePid = 0;
            m_gameExe.clear();
        }
        m_captureCv.notify_all();
        syncHotkeys();
        publish();
    }
}

json Engine::countersLocked() const
{
    const int looked = m_poolCount + m_apiCount;
    double sum = 0;
    for (const double ms : m_latMs) sum += ms;
    return {{"lines", m_lines},
            {"poolHits", m_poolHits},
            {"hitPct", looked ? json(static_cast<int64_t>(std::nearbyint(100.0 * m_poolCount / looked))) : json(nullptr)},
            {"avgMs", m_latMs.empty() ? json(nullptr) : json(static_cast<int64_t>(std::nearbyint(sum / m_latMs.size())))},
            {"startedAt", m_running ? json(m_readingSince) : json(nullptr)}};
}

// "Örnek göster": one real line from a game (spec/sample_quotes.json) on every output that is on, for 4 s
Result Engine::showSample()
{
    std::string line = "Savaş. Savaş asla değişmez.";
    int gen = 0;
    {
        std::lock_guard lock(m_mutex);
        if (m_samples.empty()) {
            std::ifstream in(std::filesystem::path(m_init.pluginDir) / L"resources" / L"sample_quotes.json",
                             std::ios::binary);
            const json doc = json::parse(in, nullptr, false);
            if (doc.is_object() && doc.contains("quotes") && doc["quotes"].is_array())
                for (const json& q : doc["quotes"])
                    if (!str(q, "tr").empty()) m_samples.push_back(str(q, "tr"));
        }
        gen = ++m_sampleGen;
        if (!m_samples.empty()) line = m_samples[static_cast<size_t>(gen) % m_samples.size()];
        m_captureJobs.push_back([this, line, gen] {
            void* hwnd = nullptr;
            CaptureRegion region;
            console::Box box;
            if (!resolveTarget(hwnd, region, box, false).empty()) {   // no region yet: low in the middle of the screen
                const int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
                box = {w / 5, h * 3 / 4, w * 3 / 5, h / 8};
            }
            showOverlay({-1, text::fromUtf8(line), "verified"}, box);
            m_sampleOnScreen = true;
            post([this, gen] {
                std::lock_guard lock(m_mutex);
                m_captureJobs.push_back([this, gen] {
                    {
                        std::lock_guard lock(m_mutex);
                        if (gen != m_sampleGen) return;   // a newer sample owns the screen
                    }
                    if (!m_sampleOnScreen) return;        // a real line replaced it
                    m_sampleOnScreen = false;
                    hideOverlay();
                });
                m_captureCv.notify_all();
            }, 4);
        });
    }
    m_captureCv.notify_all();
    return okMessage("Örnek gösteriliyor.");
}

// A line the region cut (its edge word half outside, a wrapped subtitle's other line outside): it is not
// sent; the region grows 10 % that way, for this session only, and the next frame is read again
// (spec/clipped_edge_vectors.json). Each side grows at most three times.
bool Engine::growForCutLine(bool screen)
{
    std::vector<text::OcrBox> boxes;
    for (const text::OcrBox& b : m_lastBoxes)   // our own subtitle read back off the screen is never a cut line
        if (!screen || !withoutOwnOverlay(b.text).empty()) boxes.push_back(b);
    const text::EdgeVerdict v = m_garbage.edgeCheck(boxes, m_frame.width, m_frame.height);
    if (v.send) return false;
    static const char* const kSides[4] = {"left", "right", "top", "bottom"};
    std::string grew;
    {
        std::lock_guard lock(m_mutex);
        for (const std::string& side : v.widen)
            for (int s = 0; s < 4; ++s)
                if (side == kSides[s] && m_grow[s] < kMaxGrow - 1e-9) {
                    m_grow[s] += kGrowStep;
                    grew += (grew.empty() ? "" : ", ") + side;
                }
    }
    if (grew.empty()) return false;   // already grown as far as it goes: read what is there
    logLine(MAKINE_LOG_INFO, "Bölge satırı kesiyordu; bu oturumda büyüdü: " + grew);
    m_lastHash = 0;
    publish();
    return true;
}

// The line's scene: who says it, and up to three lines before it from this conversation (each shown at most
// 8 s after the one before it, the newest at most 8 s ago), oldest first, as read, plain text. A gap ends the
// conversation: nothing before it is sent. The pool key never sees any of this.
std::pair<json, std::vector<std::string>> Engine::sceneFor(const text::ReadLine& rl, const std::string& slug)
{
    json inp = json::object();
    if (!rl.speaker.empty()) inp["speaker"] = rl.speaker;
    json context = json::array();
    {
        std::lock_guard lock(m_mutex);
        const double t = now();
        double newer = t;
        std::vector<json> picked;
        for (auto it = m_convo.rbegin(); it != m_convo.rend() && picked.size() < scene::kMaxContext; ++it) {
            if ((newer - it->at) * 1000 > scene::kContextFreshMs) break;
            newer = it->at;
            const std::string said = scene::plain(it->text);
            if (said.empty() || text::fromUtf8(said).size() > scene::kMaxContextLine) continue;   // dropped, not cut
            json item = {{"text", said}, {"age_ms", static_cast<int64_t>(std::llround((t - it->at) * 1000))}};
            if (!it->speaker.empty()) item["speaker"] = it->speaker;
            const std::u32string tr = text::fromUtf8(scene::plain(it->translated));
            if (!tr.empty()) item["translated"] = text::toUtf8(tr.substr(0, std::min(tr.size(), scene::kMaxContextLine)));
            picked.push_back(std::move(item));
        }
        for (auto it = picked.rbegin(); it != picked.rend(); ++it) context.push_back(*it);
    }
    if (!context.empty()) inp["context"] = context;
    // no scene_id / script_line_id yet: they come with the game's script dump, never from a guess
    const scene::Scene s = scene::readScene(inp, rl.speaker, m_garbage);
    const std::string form = slug.empty() ? std::string() : m_pool.ruleForm(slug, s.speaker, scene::listenerOf(s));
    return {inp, scene::variantSignals(s, form)};
}

void Engine::addHistoryLocked(const std::string& source, const std::string& text, const std::string& kind,
                              const std::string& origin)
{
    const auto cut = [](const std::string& s) {   // whole characters, at most 300
        const std::u32string u = text::fromUtf8(s);
        return u.size() <= 300 ? s : text::toUtf8(u.substr(0, 300));
    };
    m_history.push_front({{"source", cut(source)}, {"text", cut(text)}, {"kind", kind}, {"origin", origin},
                          {"at", static_cast<int64_t>(std::time(nullptr))}});
    while (m_history.size() > kHistoryLines) m_history.pop_back();
}

void Engine::syncHotkeys()
{
    std::vector<Hotkeys::Binding> bindings;
    {
        std::lock_guard lock(m_mutex);
        if (!m_hotkeys) return;
        // RegisterHotKey takes the key from every other program, the game too: only while there is
        // something to read (signed in, and a game running or console mode)
        m_hotkeysArmed = m_loginState == "signedIn" && (m_mode == "console" || !m_gameName.empty()) && !m_quit;
        if (m_hotkeysArmed)
            for (const auto& [action, key] : m_hotkeyKeys) bindings.push_back({action, key});
    }
    m_hotkeys->configure(bindings);
}

void Engine::onHotkey(const std::string& action)
{
    post([this, action] {   // never on the key thread: a start or a pick may wait on the launcher
        Result r = Result::ok();
        if (action == "start") {
            bool running = false;
            {
                std::lock_guard lock(m_mutex);
                running = m_running;
            }
            r = invoke(running ? "stop" : "start", json::object());
        } else if (action == "region") {
            r = pickRegion();
        } else if (action == "move") {
            bool on = false;
            {
                std::lock_guard lock(m_mutex);
                on = !m_boxMovable;
            }
            r = setBoxMovable(on);
        }
        if (!r.succeeded()) {   // the player is in the game: say it where the launcher shows notices
            const json m = json::parse(r.json, nullptr, false);
            m_host.emitEvent("ui.notify", dump({{"title", "Makine Live"}, {"message", str(m, "message")}, {"kind", "info"}}));
        }
    });
}

Result Engine::setBoxMovable(bool on)
{
    // overlay.setMovable (optional host method, launcher 0.3+): the subtitle box takes the mouse and can be
    // dragged; turning it off answers where it was left ({x, y}: the box centre as screen fractions)
    const Result r = m_host.call("overlay.setMovable", dump({{"on", on}}));
    if (r.status == MAKINE_E_NOT_FOUND || r.status == MAKINE_E_UNSUPPORTED) {
        {
            std::lock_guard lock(m_mutex);
            m_boxMovable = false;
            m_boxMoveSupported = false;
        }
        publish();
        return Result::error(MAKINE_E_UNSUPPORTED, "Bu Makine sürümü altyazı kutusunu taşımayı henüz desteklemiyor.");
    }
    if (!r.succeeded()) return r;
    const json where = json::parse(r.json, nullptr, false);
    bool moved = false;
    {
        std::lock_guard lock(m_mutex);
        m_boxMovable = on;
        m_boxMoveSupported = true;
        const double x = num(where, "x", -1), y = num(where, "y", -1);
        if (!on && x >= 0 && x <= 1 && y >= 0 && y <= 1) {
            m_style["boxX"] = x;
            m_style["boxY"] = y;
            ++m_styleVersion;
            saveSettings();
            moved = true;
        }
    }
    if (moved) {
        m_captureCv.notify_all();
        applyStyleOutputs();
    }
    publish();
    return okMessage(on ? "Kutuyu oyunda sürükle; bitince yeniden bas." : "Kutu kilitlendi.");
}

Result Engine::loginTeam(const json& params)
{
    // Team accounts (admin, translator, tester): username and password. The password lives only in this
    // call: never stored, never logged; the token it brings is kept like the device login's (DPAPI).
    std::string username = str(params, "username"), password = str(params, "password");
    while (!username.empty() && username.back() == ' ') username.pop_back();
    while (!username.empty() && username.front() == ' ') username.erase(0, 1);
    if (username.empty() || password.empty())
        return Result::error(MAKINE_E_INVALID_ARGUMENT, "Kullanıcı adı ve şifre gerekli.");
    int gen = 0;
    {
        std::lock_guard lock(m_mutex);
        if (m_loginState == "signedIn") return okMessage("Zaten giriş yapıldı.");
        gen = ++m_loginGen;
        m_loginState = "starting";
        m_loginCode.clear();
        m_loginUrl.clear();
        m_loginMessage = "Giriş yapılıyor…";
    }
    publish();
    post([this, gen, username, password]() mutable {
        const ApiResult r = m_server.appLogin(username, password);
        SecureZeroMemory(password.data(), password.size());
        const bool ok = r.ok() && validToken(str(r.data, "token"));
        {
            std::lock_guard lock(m_mutex);
            if (gen != m_loginGen) return;
            if (ok) {
                ++m_loginGen;
                m_server.setToken(str(r.data, "token"));
            } else {
                m_loginState = "signedOut";
                m_loginMessage = r.network() ? "Sunucuya ulaşılamıyor. İnternet bağlantınızı kontrol edin."
                                 : r.ok()    ? "Giriş yapılamadı. Biraz sonra tekrar deneyin."
                                 : r.error;
            }
        }
        if (ok) return signedIn(r.data);
        publish();
    });
    return okMessage("Giriş yapılıyor…");
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
        m_role = str(profile, "role", m_role);
        m_loginCode.clear();
        m_loginUrl.clear();
        m_loginMessage.clear();
        if (profile.is_object() && profile.contains("translation_ready") && !flag(profile, "translation_ready"))
            m_notice = "Sunucuda çeviri anahtarı yok · yalnızca havuzdaki satırlar gelir";
        saveSettings();
    }
    logLine(MAKINE_LOG_INFO, "Giriş: " + m_user);
    syncHotkeys();
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
        m_role.clear();
        ++m_beatGen;
        m_running = false;
        saveSettings();
    }
    m_captureCv.notify_all();
    syncHotkeys();
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
    try {
        refreshStatus();
    } catch (const std::exception& e) {
        logLine(MAKINE_LOG_WARN, std::string("Durum: ") + e.what());
    }
    bool maintenance = false;
    {
        std::lock_guard lock(m_mutex);
        maintenance = m_maintenance;
    }
    post([this] { statusLoop(); }, maintenance ? kMaintenancePollS : kStatusPollS);
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
        if (consentMissingLocked())   // console mode or a console stream app in PC mode; the screen asks once
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
        m_grow = {};
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
                notifyError(reason, nullptr);   // the player's setup, not a defect: no Sentry event
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
            if (box) m_grow = {};
            if (box && mode == "pc") m_pcRegion = box;
            if (box && video) {
                const console::Frac f = console::screenToRegion(*video, *box);
                if (console::validRegion(f)) {
                    m_consoleRegion = f;
                    m_consoleManual = true;
                }
            }
            if (box) saveSettings();
        }
        if (!box && r.status != MAKINE_E_DENIED) notifyError("Bölge seçilemedi.", nullptr);
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
        } catch (...) {
            logLine(MAKINE_LOG_ERROR, "İş: beklenmeyen hata");
        }
        lock.lock();
    }
    ++m_finished;
    m_captureCv.notify_all();
}

// Port of app.py _translate_job: local pool first, then this session's cache, then one
// /api/live/translate request (retried once on network / 502 / 504 errors).
void Engine::translateJob(int line, std::string textUtf8, int seq, std::string slug, std::string gameName,
                          std::pair<uint64_t, uint64_t> ocrUs)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_dispatchSeq[line] != seq) return;   // a newer reading of this line is queued
        const auto gone = m_goneAt.find(line);
        if (gone != m_goneAt.end() && now() - gone->second > kStaleJobS) {
            m_results.push_back({line, {}, {}});
            return;
        }
    }
    // Two speakers read as one block are two lines ("…you! Cop: I want…"); each is keyed and translated
    // without its label (one key whoever says it) and shown with the label in front, untranslated
    std::vector<std::string> parts = m_garbage.speakerLines(textUtf8);
    if (parts.empty()) parts.push_back(textUtf8);
    const auto rankOf = [](const std::string& k) { return k == "verified" ? 2 : k == "pending" ? 1 : 0; };
    std::string shown, kind;
    bool fresh = false;   // a part came from the pool or the server, not the cache: counters changed
    for (const std::string& part : parts) {
        const text::ReadLine rl = m_garbage.readLine(part, kSourceLang);
        const auto [scene, signals] = sceneFor(rl, slug);
        const auto [translated, partKind] =
            translatePart(line, part, rl.line, slug, gameName, ocrUs, rl.prefix, !rl.hard, scene, signals, fresh);
        if (translated.empty()) continue;
        {
            std::lock_guard lock(m_mutex);
            m_convo.push_back({part, rl.speaker, translated, now()});
            while (m_convo.size() > scene::kMaxContext) m_convo.pop_front();
        }
        shown += (shown.empty() ? "" : "\n") + rl.prefix + translated;
        if (kind.empty() || rankOf(partKind) < rankOf(kind)) kind = partKind;
    }
    {
        std::lock_guard lock(m_mutex);
        m_results.push_back({line, shown, kind});
    }
    m_netCv.notify_all();   // a job waiting for the same line
    if (fresh) publish();
}

std::pair<std::string, std::string> Engine::translatePart(int line, const std::string& textUtf8, const std::string& bare,
                                                          const std::string& slug, const std::string& gameName,
                                                          std::pair<uint64_t, uint64_t> ocrUs, const std::string& prefix,
                                                          bool clean, const json& scene,
                                                          const std::vector<std::string>& signals, bool& fresh)
{
    const std::u32string poolKey = text::compactPoolKey(text::fromUtf8(bare));
    // the cache, the in-flight set and the skip list go by what is asked: the line, and a variant signal if any
    std::u32string key = poolKey;
    for (const std::string& s : signals) key += U"\x1f" + text::fromUtf8(s);
    std::string poolSlug;
    {
        std::lock_guard lock(m_mutex);
        poolSlug = m_poolSlug;
    }
    if (!slug.empty() && slug == poolSlug) {
        // (key, variant) → plain key; a cut or merged reading gets the exact key only
        if (const auto hit = m_pool.lookup(slug, bare, clean, signals)) {
            const std::string hitKind = hit->variant.empty() ? "verified" : "pending";   // a variant is no approval
            {
                std::lock_guard lock(m_mutex);
                ++m_poolHits;
                ++m_poolCount;
                ++m_linesTotal;
                ++m_sessionLines;
                m_lastLine = {{"source", textUtf8}, {"text", prefix + hit->translation}, {"kind", hitKind}, {"origin", "local"}};
                addHistoryLocked(textUtf8, prefix + hit->translation, hitKind, "local");
            }
            fresh = true;
            return {hit->translation, hitKind};
        }
    }
    bool owner = false;
    {
        std::unique_lock lock(m_mutex);
        if (m_skipKeys.count(poolKey)) return {};
        for (int attempt = 0; attempt < 2 && !owner; ++attempt) {
            const auto c = m_cache.find(key);
            if (c != m_cache.end() && (c->second.kind == "verified" || now() - c->second.at <= kCacheTtlS))
                return {c->second.text, c->second.kind};
            if (!m_inflight.count(key)) {
                m_inflight.insert(key);
                owner = true;
            } else {
                m_netCv.wait_for(lock, std::chrono::seconds(12), [&] { return m_quit || !m_inflight.count(key); });
            }
        }
        if (!owner) return {};
    }
    struct InflightGuard {   // whatever happens below, the key leaves m_inflight and waiters wake
        Engine* e;
        std::u32string key;
        ~InflightGuard()
        {
            {
                std::lock_guard lock(e->m_mutex);
                e->m_inflight.erase(key);
            }
            e->m_netCv.notify_all();
        }
    } inflightGuard{this, key};

    ApiResult res;
    std::string notice, noticeCode;
    double requestMs = 0;
    const uint64_t requestStartUs = wallUs();
    for (int attempt = 1; attempt <= 2; ++attempt) {
        const auto sent = Clock::now();
        res = m_server.translate(slug, gameName, bare, kSourceLang, kTargetLang, scene);   // the line without its label
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
            m_skipKeys.insert(poolKey);
            logLine(MAKINE_LOG_DEBUG, "Sunucu çöp saydı (" + str(res.data, "reason") + "): " + bare);
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
    // verified only for an approved pool line; a provisional pool answer is pending, a shared or own one
    // (DeepL's text another player already got) is machine — never shown as approved
    std::string kind = "api";
    if (!translated.empty() && str(res.data, "origin", "deepl") == "pool")
        kind = str(res.data, "status", "pending") == "verified" && !flag(res.data, "provisional") ? "verified" : "pending";
    if (!str(res.data, "variant").empty() && kind == "verified") kind = "pending";   // a variant is no approval
    // A translation made with the line before as a hint is this player's alone: never cached for another reading
    const bool hinted = flag(res.data, "context_used");
    if (flag(res.data, "address_flip")) {   // counted on the server too (formflip): a missing address rule
        std::lock_guard lock(m_mutex);
        ++m_addressFlips;
        logLine(MAKINE_LOG_DEBUG, "Hitap değişti (sen/siz), kural yok: " + bare);
    }
    {
        std::lock_guard lock(m_mutex);
        if (!notice.empty()) {
            m_notice = notice;
            m_noticeCode = noticeCode;
        }
        if (!translated.empty()) {
            if (m_notice.find("yeniden deneniyor") != std::string::npos) {
                m_notice.clear();
                m_noticeCode.clear();
            }
            if (!hinted) m_cache[key] = {translated, kind, now()};
            if (m_cache.size() > 1000) m_cache.erase(m_cache.begin());
            if (kind != "api") ++m_poolHits;
            ++m_linesTotal;
            ++m_sessionLines;
            m_latMs.push_back(requestMs);
            if (m_latMs.size() > 30) m_latMs.pop_front();
            auto& samples = kind == "api" ? m_apiMs : m_poolMs;
            samples.push_back(requestMs);
            if (samples.size() > 50) samples.pop_front();
            ++(kind == "api" ? m_apiCount : m_poolCount);
            m_lastLine = {{"source", textUtf8}, {"text", prefix + translated}, {"kind", kind},
                          {"origin", str(res.data, "origin", "deepl")}};
            addHistoryLocked(textUtf8, prefix + translated, kind, str(res.data, "origin", "deepl"));
        }
    }
    fresh = fresh || !notice.empty() || !translated.empty();
    if (!translated.empty()) traceLine(ocrUs, requestStartUs, wallUs(), kind);
    return {translated, kind};
}

// A sampled line as a Sentry transaction: ocr → translate.pool|api. Tokens only, no text of the line.
void Engine::traceLine(std::pair<uint64_t, uint64_t> ocrUs, uint64_t requestStartUs, uint64_t requestEndUs,
                       const std::string& kind)
{
    json tags;
    {
        std::lock_guard lock(m_mutex);
        if (!m_usageReport || m_traceSent >= m_traceBudget) return;
        if (m_traceSent >= 2 && ++m_traceSeen % 20 != 0) return;
        ++m_traceSent;
        const bool console = m_mode == "console";
        tags = {{"mode", console ? "konsol" : "pc"},
                {"game", console ? std::string(console::kGameSlug) : m_gameSlug}};
    }
    // The OCR run counts when it ended shortly before the request (an unchanged frame reuses an old read).
    const bool ocr = ocrUs.second && ocrUs.first <= ocrUs.second && ocrUs.second <= requestStartUs
                     && requestStartUs - ocrUs.second < 2'000'000;
    const uint64_t start = ocr ? ocrUs.first : requestStartUs;
    json spans = json::array();
    if (ocr) spans.push_back({{"op", "ocr"}, {"start_us", ocrUs.first}, {"end_us", ocrUs.second}});
    spans.push_back({{"op", kind == "api" ? "translate.api" : "translate.pool"},
                     {"start_us", requestStartUs}, {"end_us", requestEndUs}});
    m_host.emitEvent("telemetry.transaction",
                     dump({{"name", "live.line"}, {"op", "live.line"}, {"start_us", start}, {"end_us", requestEndUs},
                           {"tags", tags}, {"spans", spans}}));
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
    const bool forced = m_lastHash == 0;   // a new region, mode or settings: read for sure
    m_lastHash = hash;
    if (hwnd) {
        // K3, console mode: a stream never repeats a frame, the subtitle does. Same text mask as at the
        // last OCR → the same text; the OCR runs again at least every few seconds whatever the mask says
        TextMask mask = TextMask::of(m_frame.pixels.data(), m_frame.width, m_frame.height);
        if (!forced && mask.sameText(m_lastMask) && now() - m_lastOcrAt < kMaskMaxSkipS) {
            textOut = m_lastOcr;
            return true;
        }
        m_lastMask = std::move(mask);
    }
    m_lastOcrAt = now();
    std::string text;
    const auto ocrStart = Clock::now();
    const uint64_t ocrStartUs = wallUs();
    std::string ocrError;
    const auto lines = m_ocr->run(m_frame.pixels.data(), m_frame.width, m_frame.height, ocrError);
    if (!ocrError.empty()) logLine(MAKINE_LOG_WARN, ocrError);
    const double ocrMs = std::chrono::duration<double, std::milli>(Clock::now() - ocrStart).count();
    if (ocrMs > 40) {   // as the harness: only real OCR runs, not unchanged frames
        m_lastOcrUs = {ocrStartUs, wallUs()};
        std::lock_guard lock(m_mutex);
        m_ocrMs.push_back(ocrMs);
        if (m_ocrMs.size() > 50) m_ocrMs.pop_front();
    }
    m_lastBoxes.clear();
    for (const auto& line : lines) {
        if (!text.empty()) text += "\n";
        text += line.text;
        m_lastBoxes.push_back({m_spacing.fix(dropForeignScriptTokens(line.text)), line.x0, line.y0, line.x1, line.y1});
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
    bool gameUnknown = false;
    std::array<double, 4> grow{};
    unsigned long pid = 0;
    std::vector<std::string> exes;
    std::optional<console::Box> pc;
    console::Frac frac;
    bool consoleManual = false;
    std::optional<finder::Rect> band;
    {
        std::lock_guard lock(m_mutex);
        consoleManual = m_consoleManual;
        band = m_finder.band();
        if (!band)   // not reading yet (readOnce): the band stored for this game
            if (const auto stored = storedBandLocked(currentSlug())) band = stored->first;
        mode = m_mode;
        key = m_sourceKey;
        pid = m_sourcePid;
        exes = m_customExes;
        pc = m_pcRegion;
        frac = m_consoleRegion;
        gameName = m_gameName;
        gameUnknown = m_gameUnknown;
        grow = m_grow;
    }
    hwnd = nullptr;
    m_videoFrame.reset();
    m_finderArea.reset();
    for (size_t s = 0; s < 4; ++s) grow[s] = std::max(grow[s], m_tempGrow.grow()[s]);   // cut lines, wide lines
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
        if (!consoleManual && band) frac = {band->x0, band->y0, band->w(), band->h()};   // the finder's band
        const console::Box all = console::clientRegion(hwnd, {0, 0, 1, 1});   // the finder reads the whole video area
        m_finderHwnd = hwnd;
        m_finderArea = CaptureRegion{all.x, all.y, all.w, all.h};
        if (grow != std::array<double, 4>{}) {   // grown past a cut line, inside the video
            const double w = frac.w, h = frac.h;
            const double x0 = std::max(0.0, frac.x - w * grow[0]), y0 = std::max(0.0, frac.y - h * grow[2]);
            const double x1 = std::min(1.0, frac.x + w * (1 + grow[1])), y1 = std::min(1.0, frac.y + h * (1 + grow[3]));
            frac = {x0, y0, x1 - x0, y1 - y0};
        }
        const console::Box b = console::clientRegion(hwnd, frac);
        region = {b.x, b.y, b.w, b.h};
        const console::Rect video = console::videoRect(console::clientRect(hwnd));
        {
            std::lock_guard lock(m_mutex);
            m_bandArea = video;
        }
        overlay = console::regionToScreen(video, frac);
        m_videoFrame = console::Box{video.l, video.t, video.r - video.l, video.b - video.t};
        return {};
    }
    if (!pc && !needGame) {   // readOnce with no region of the player's: the band (or low in the middle)
        unsigned long gamePid = 0;
        {
            std::lock_guard lock(m_mutex);
            gamePid = m_gamePid;
        }
        // the game's biggest visible window, else the main screen (the launcher is in front while it asks)
        const console::Rect screen{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        const console::Rect c = gamePid ? gameWindowArea(gamePid).value_or(screen) : screen;
        {
            std::lock_guard lock(m_mutex);
            m_bandArea = c;
        }
        const console::Frac f = band ? console::Frac{band->x0, band->y0, band->w(), band->h()} : console::kDefaultRegion;
        const console::Box b = console::regionToScreen(c, f);
        region = {b.x, b.y, b.w, b.h};
        overlay = b;
        return {};
    }
    HWND gameWnd = nullptr;
    if (needGame && gameUnknown) {
        if (code) *code = "game_unidentified";
        return "Oyun tanınamadı; adı bilinmeyen bir oyun okunmaz.";
    }
    if (needGame && gameName.empty()) {
        if (code) *code = "game_missing";
        return "Oyun algılanmadı · oyunu Makine'den başlat ya da konsol modunu kullan.";
    }
    if (needGame) {
        // Read only while the game is in front (the harness's target lock): text of another
        // window over the region (Discord, a browser) must never reach the pool
        const HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        unsigned long gamePid = 0;
        std::string gameExe;
        {
            std::lock_guard lock(m_mutex);
            gamePid = m_gamePid;
            gameExe = m_gameExe;
        }
        if (!gamePid && gameExe.empty()) {   // the launcher said neither: we cannot tell the game's window
            if (code) *code = "game_unidentified";
            return "Oyunun penceresi tanınamadı; Makine oyunun sürecini bildirmedi.";
        }
        bool isGame = gamePid && fgPid == gamePid;
        if (!isGame && !gameExe.empty() && fgPid) {
            if (const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fgPid)) {
                wchar_t buf[1024];
                DWORD size = 1024;
                if (QueryFullProcessImageNameW(p, 0, buf, &size)) {
                    const std::wstring path(buf, size);
                    isGame = text::toUtf8(text::lower(text::fromUtf8(win::wideToUtf8(path.substr(path.find_last_of(L"\\/") + 1)))))
                             == gameExe;
                }
                CloseHandle(p);
            }
        }
        RECT r{};
        const int cx = pc ? pc->x + pc->w / 2 : 0, cy = pc ? pc->y + pc->h / 2 : 0;
        const bool covers = fg && GetWindowRect(fg, &r) && (!pc || (cx >= r.left && cx < r.right && cy >= r.top && cy < r.bottom));
        if (!fg || fgPid == GetCurrentProcessId() || fgPid == m_launcherPid || !isGame || !covers)
        {
            if (code) *code = "game_not_front";
            return "Oyun önde değil · oyuna dönünce okuma sürer.";
        }
        gameWnd = fg;
    }
    // never past the screen, nor past the game's window
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vr = vx + GetSystemMetrics(SM_CXVIRTUALSCREEN), vb = vy + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (RECT gr{}; gameWnd && GetWindowRect(gameWnd, &gr)) {
        vx = std::max(vx, static_cast<int>(gr.left));
        vy = std::max(vy, static_cast<int>(gr.top));
        vr = std::min(vr, static_cast<int>(gr.right));
        vb = std::min(vb, static_cast<int>(gr.bottom));
    }
    console::Box base;
    if (pc) {
        base = *pc;
    } else {   // the finder's band (or low in the middle while it searches) in the game's client area
        console::Rect c = console::clientRect(gameWnd);
        c = {std::max(c.l, vx), std::max(c.t, vy), std::min(c.r, vr), std::min(c.b, vb)};
        if (c.r - c.l < 64 || c.b - c.t < 64) {
            if (code) *code = "game_not_front";
            return "Oyun önde değil · oyuna dönünce okuma sürer.";
        }
        m_finderHwnd = nullptr;
        m_finderArea = CaptureRegion{c.l, c.t, c.r - c.l, c.b - c.t};
        {
            std::lock_guard lock(m_mutex);
            m_bandArea = c;
        }
        const console::Frac f = band ? console::Frac{band->x0, band->y0, band->w(), band->h()} : console::kDefaultRegion;
        base = console::regionToScreen(c, f);
    }
    console::Box box = base;
    if (grow != std::array<double, 4>{}) {   // grown past a cut line or for a wide line, on the screen
        const int x0 = std::max(vx, base.x - static_cast<int>(base.w * grow[0]));
        const int y0 = std::max(vy, base.y - static_cast<int>(base.h * grow[2]));
        const int x1 = std::min(vr, base.x + base.w + static_cast<int>(base.w * grow[1]));
        const int y1 = std::min(vb, base.y + base.h + static_cast<int>(base.h * grow[3]));
        box = {x0, y0, x1 - x0, y1 - y0};
    }
    region = {box.x, box.y, box.w, box.h};
    overlay = box;
    return {};
}

void Engine::handleEvents(const std::vector<LineEvent>& events, const std::u32string& reading)
{
    for (const LineEvent& e : events) {
        if (e.kind == LineEvent::New) {
            m_scheduler.sourceNew(e.id, e.t);
            std::string slug;
            bool warm = false;
            {
                std::lock_guard lock(m_mutex);
                ++m_lines;
                slug = currentSlug() == m_poolSlug && !consentMissingLocked() ? m_poolSlug : std::string();
                warm = m_loginState == "signedIn";
            }
            // K12: an approved line found by its exact key on the very first reading shows at once;
            // the dispatch after the two-read rule still runs (and counts the hit) as before
            if (!slug.empty() && !reading.empty()) {
                const std::string readingUtf8 = text::toUtf8(reading);
                if (m_garbage.speakerLines(readingUtf8).size() <= 1) {   // one speaker: keyed without the label
                    const text::ReadLine rl = m_garbage.readLine(readingUtf8, kSourceLang);
                    if (const auto hit = m_pool.lookupExact(slug, rl.line, sceneFor(rl, slug).second))
                        m_scheduler.translationReady(e.id, text::fromUtf8(rl.prefix + hit->translation),
                                                     hit->variant.empty() ? "verified" : "pending", e.t);
                }
            }
            // K13: the line settles in ~200 ms; open the connection now if it went idle
            if (warm && m_server.idleSeconds() > 15) post([this] { m_server.ping(); });
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
                slug = consentMissingLocked() ? std::string() : currentSlug();   // never to the GTA VI pool unasked
                name = m_mode == "console" ? std::string(console::kGameName) : m_gameName;
            }
            if (!slug.empty())
                post([this, id = e.id, text, seq, slug, name, ocr = m_lastOcrUs] { translateJob(id, text, seq, slug, name, ocr); });
        }
    }
}

void Engine::showOverlay(const Display& d, const console::Box& rect, int chunk, int chunks)
{
    json style;
    {
        std::lock_guard lock(m_mutex);
        style = m_style;
    }
    const std::string text = text::toUtf8(d.text);
    json lines = json::array();   // one entry per speaker ("Jason: …", "Lucia: …")
    for (size_t b = 0; b <= text.size();) {
        const size_t e = std::min(text.find('\n', b), text.size());
        if (e > b) lines.push_back({{"text", text.substr(b, e - b)}});
        b = e + 1;
    }
    const bool showSource = style.contains("showSource") && style["showSource"].is_boolean() && style["showSource"].get<bool>();
    // The source is not added to m_shownKeys: it is the game's own line, which OCR also reads under the
    // overlay (the launcher excludes the overlay from capture); filtering it would end the line early
    // and never read a repeat of it. Read off our overlay instead, it is the same line, so no new line.
    if (showSource && m_lineSource.count(d.id) && !lines.empty()) lines.back()["sourceText"] = m_lineSource[d.id];
    json payload = {{"text", text}, {"lines", lines}, {"x", rect.x}, {"y", rect.y},
                    {"width", rect.w}, {"height", rect.h}, {"kind", d.kind}};
    if (!style.empty()) payload["style"] = style;
    // Fit plan (fit.h): the renderer wraps to maxLines, may shrink to minFontScale and widen to maxWidthFrac of
    // the frame; a line that still does not fit arrives as chunk i of n (text / lines hold the chunk)
    if (d.id >= 0)
        payload["fit"] = {{"maxLines", static_cast<int>(num(style, "maxLines", 2))}, {"minFontScale", fit::kMinFontScale},
                          {"maxWidthFrac", fit::kMaxWidthFrac}, {"chunk", chunk}, {"chunks", chunks}};
    // Console on this screen: the 16:9 video inside the stream window (physical px), so Alt / Üst sit in the picture
    if (m_videoFrame)
        payload["frame"] = {{"x", m_videoFrame->x}, {"y", m_videoFrame->y}, {"width", m_videoFrame->w}, {"height", m_videoFrame->h}};
    m_sampleOnScreen = false;   // a real line replaces the sample
    if (boxWanted()) {   // console on the TV / phone: no box over the Remote Play window
        m_host.emitEvent("overlay.show", dump(payload));
        m_boxShown = true;
    } else if (m_boxShown) {
        m_host.emitEvent("overlay.hide", "{}");
        m_boxShown = false;
    }
    routeLine(d, chunk, chunks);
    m_overlayShown = true;
    for (const json& l : lines)   // one key per speaker line, label off: a label never makes two lines look alike
        m_shownKeys.push_back(text::compactPoolKey(text::fromUtf8(text::splitSpeaker(l["text"].get<std::string>()).text)));
    ++m_shownCount;
    while (m_shownKeys.size() > 12) m_shownKeys.erase(m_shownKeys.begin());
}

// ── the subtitle finder (subtitle_finder.h) ─────────────────────────────────

bool Engine::syncFinderLocked()
{
    const std::string slug = currentSlug();
    const bool manual = m_mode == "pc" ? m_pcRegion.has_value() : m_consoleManual;
    const finder::State was = m_finder.state();
    const auto restore = [&] {   // a band locked earlier for this game
        if (const auto stored = storedBandLocked(slug)) m_finder.restore(stored->first, stored->second);
    };
    bool changed = false;
    if (slug != m_finderSlug) {
        m_finderSlug = slug;
        m_finder.reset();
        restore();
        changed = true;
    }
    if (manual && m_finder.state() != finder::State::Manual) {
        m_finder.setManual(true);
    } else if (!manual && m_finder.state() == finder::State::Manual) {
        m_finder.setManual(false);
        restore();
    }
    return changed || m_finder.state() != was;
}

void Engine::storeBandLocked()
{
    const auto b = m_finder.band();
    if (!b || m_finderSlug.empty()) return;
    m_settings.set("autoRegion:" + m_finderSlug, std::to_string(b->x0) + "," + std::to_string(b->y0) + "," + std::to_string(b->x1)
                                                     + "," + std::to_string(b->y1) + "," + std::to_string(m_finder.confidence()));
    saveSettings();
    logLine(MAKINE_LOG_INFO, "Altyazı bölgesi bu oyun için kaydedildi: " + m_finderSlug);
}

std::optional<std::pair<finder::Rect, double>> Engine::storedBandLocked(const std::string& slug) const
{
    const auto v = splitOn(m_settings.get("autoRegion:" + slug), ',');
    if (slug.empty() || v.size() < 5) return std::nullopt;
    const finder::Rect r{std::strtod(v[0].c_str(), nullptr), std::strtod(v[1].c_str(), nullptr),
                         std::strtod(v[2].c_str(), nullptr), std::strtod(v[3].c_str(), nullptr)};
    if (!(r.w() > 0.05 && r.h() > 0.02 && r.x0 >= 0 && r.y0 >= 0 && r.x1 <= 1 && r.y1 <= 1)) return std::nullopt;
    return std::make_pair(r, std::strtod(v[4].c_str(), nullptr));
}

// autoRegion plus screenRect: the band (or the player's own region) in physical screen pixels, for the
// launcher's region selector; null while it is not known where the game's window / video area is
json Engine::autoRegionLocked() const
{
    json out = autoRegionBaseLocked();
    json screen = nullptr;
    if (m_mode == "pc" && m_pcRegion) {
        screen = {{"x", m_pcRegion->x}, {"y", m_pcRegion->y}, {"width", m_pcRegion->w}, {"height", m_pcRegion->h}};
    } else if (out["rect"].is_object()) {
        std::optional<console::Rect> area = m_bandArea;
        if (!area && m_mode == "pc" && m_gamePid) area = gameWindowArea(m_gamePid);
        if (area) {
            const json& r = out["rect"];
            const console::Box b = console::regionToScreen(*area, console::Frac{r["x"].get<double>(), r["y"].get<double>(),
                                                                               r["width"].get<double>(), r["height"].get<double>()});
            screen = {{"x", b.x}, {"y", b.y}, {"width", b.w}, {"height", b.h}};
        }
    }
    out["screenRect"] = screen;
    return out;
}

json Engine::autoRegionBaseLocked() const
{
    const auto rectJson = [](double x, double y, double w, double h) {
        return json{{"x", x}, {"y", y}, {"width", w}, {"height", h}};
    };
    // Not reading (the finder syncs only while reading): what reading would start from — the player's own
    // region, else the band stored for this game, else nothing yet
    const std::string slug = currentSlug();
    if (!m_running || m_finderSlug != slug) {
        const bool manual = m_mode == "pc" ? m_pcRegion.has_value() : m_consoleManual;
        if (manual) {
            const json rect = m_mode == "console" ? rectJson(m_consoleRegion.x, m_consoleRegion.y, m_consoleRegion.w, m_consoleRegion.h)
                                                  : json(nullptr);   // PC: the screen region is region.pc
            return {{"state", "manual"}, {"rect", rect}, {"confidence", 1.0}};
        }
        if (const auto stored = storedBandLocked(slug)) {
            const finder::Rect& b = stored->first;
            return {{"state", "locked"}, {"rect", rectJson(b.x0, b.y0, b.w(), b.h())},
                    {"confidence", std::round(stored->second * 100) / 100}};
        }
        if (m_finderSlug != slug) return {{"state", "off"}, {"rect", nullptr}, {"confidence", 0.0}};
    }
    const finder::State s = m_finder.state();
    const auto b = m_finder.band();
    // searching only while reading: nothing is captured before the player's start
    const char* name = s == finder::State::Searching && !m_running ? "off" : finder::stateName(s);
    return {{"state", name},
            {"rect", b ? json{{"x", b->x0}, {"y", b->y0}, {"width", b->w()}, {"height", b->h()}} : json(nullptr)},
            {"confidence", std::round(m_finder.confidence() * 100) / 100}};
}

// Every finder::kIntervalS while searching (or proposing): the whole game window / console video area,
// downscaled, through the detector. Its text never reaches the tracker or the pool.
void Engine::runFinder()
{
    const double t = now();
    if (!m_finderArea || !m_ocr || t - m_finderAt < finder::kIntervalS) return;
    {
        std::lock_guard lock(m_mutex);
        if (!m_finder.due(t)) return;
    }
    m_finderAt = t;
    if (!m_capture->captureRegion(m_finderHwnd, *m_finderArea, m_finderFrame)) return;
    std::string error;
    const auto obs = finder::observe(*m_ocr, m_finderFrame.pixels.data(), m_finderFrame.width, m_finderFrame.height, error,
                                     1, [this](const std::string& s) { return withoutOwnOverlay(s).empty(); });
    if (!error.empty()) logLine(MAKINE_LOG_WARN, error);
    std::optional<finder::Rect> before, after;
    finder::State was, is;
    {
        std::lock_guard lock(m_mutex);
        before = m_finder.band();
        was = m_finder.state();
        m_finder.addSample(obs, now());
        after = m_finder.band();
        is = m_finder.state();
        if (is == finder::State::Locked && was != finder::State::Locked) storeBandLocked();
    }
    const auto apart = [](double a, double b) { return std::abs(a - b) > 0.01; };
    const bool moved = before.has_value() != after.has_value()
                       || (before && after && (apart(before->x0, after->x0) || apart(before->y0, after->y0)
                                               || apart(before->x1, after->x1) || apart(before->y1, after->y1)));
    if (moved) {   // another region: the line read so far is gone, nothing half read is sent
        handleEvents(m_tracker.reset());
        m_tempGrow.clear();
        m_lastHash = 0;
        logLine(MAKINE_LOG_INFO, std::string("Altyazı bölgesi ") + finder::stateName(is));
    }
    if (moved || is != was) publish();
}

// A long line that the renderer cannot fit even at its smallest font in its widest band goes in chunks
// (fit.h), each for its reading time; the scheduler holds the line for all of them.
void Engine::showLine(const Display& d, const console::Box& rect, double t)
{
    json style;
    bool tv = false;
    {
        std::lock_guard lock(m_mutex);
        style = m_style;
        tv = m_mode == "console" && m_output == "tv";
    }
    int frameW = m_videoFrame ? m_videoFrame->w : 0;
    if (frameW <= 0) {   // the screen the box is on
        const RECT r{rect.x, rect.y, rect.x + std::max(1, rect.w), rect.y + std::max(1, rect.h)};
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        frameW = GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi) ? mi.rcMonitor.right - mi.rcMonitor.left
                                                                                      : GetSystemMetrics(SM_CXSCREEN);
    }
    // physical px → the overlay's logical px (style.sizePx is logical): the monitor's scale
    {
        const RECT r{rect.x, rect.y, rect.x + std::max(1, rect.w), rect.y + std::max(1, rect.h)};
        UINT dpiX = 96, dpiY = 96;
        if (SUCCEEDED(GetDpiForMonitor(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX >= 96)
            frameW = frameW * 96 / static_cast<int>(dpiX);
    }
    fit::Budget budget = fit::budget(frameW, num(style, "sizePx", 28), static_cast<int>(num(style, "maxLines", 2)));
    if (tv) budget = fit::tighter(budget, fit::tvBudget(0.046, 2));   // Makine TV's relSize / maxLines
    std::vector<std::u32string> chunks = fit::split(d.text, budget);
    if (chunks.size() <= 1) {
        m_chunks.clear();
        m_shownChunk = 0;
        showOverlay(d, rect);
        return;
    }
    m_chunks.start(d.id, std::move(chunks), m_scheduler.minBase, t);
    m_scheduler.holdFor(d.id, m_chunks.total());
    m_shownChunk = 0;
    showOverlay({d.id, m_chunks.chunk(0), d.kind}, rect, 0, m_chunks.count());
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
        // OCR of our own Turkish line may lose its ç ğ ı ö ş ü: compare with those letters folded too
        const auto fold = [](std::u32string s) {
            for (auto& c : s) {
                switch (c) {
                case U'ç': c = U'c'; break;
                case U'ğ': c = U'g'; break;
                case U'ı': c = U'i'; break;
                case U'ö': c = U'o'; break;
                case U'ş': c = U's'; break;
                case U'ü': c = U'u'; break;
                case U'â': c = U'a'; break;
                case U'î': c = U'i'; break;
                case U'û': c = U'u'; break;
                default: break;
                }
            }
            return s;
        };
        const std::u32string k = fold(text::compactPoolKey(text::fromUtf8(text::splitSpeaker(line).text)));
        bool own = false;
        for (const auto& shownRaw : m_shownKeys) {
            const std::u32string shown = fold(shownRaw);
            own = own || k == shown || keysSimilar(k, shown) || (k.size() >= 8 && shown.find(k) != std::u32string::npos)
                  || (shown.size() >= 8 && k.find(shown) != std::u32string::npos)
                  || (k.size() >= 4 && text::sequenceRatio(k, shown) >= 0.7);
        }
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
    m_chunks.clear();
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
            } catch (...) {
                logLine(MAKINE_LOG_ERROR, "Okuma: beklenmeyen hata");
            }
            lock.lock();
            continue;
        }
        const bool running = m_running;
        const std::string mode = m_mode;
        const int styleVersion = m_styleVersion;
        // "En kısa gösterim" (0.3.2 min_show_ms): the shortest time a line stays up, on every output
        m_scheduler.minBase = std::clamp(num(m_style, "minShowMs", 1200), 600.0, 3000.0) / 1000.0;
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
                    m_host.emitEvent("telemetry.breadcrumb", dump({{"message", "reading started: " + mode}}));
                    std::lock_guard guard(m_mutex);
                    m_lastError.clear();
                }
                if (!ensureOcr(error)) {
                    notifyError(error, "ocr_init");
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
                    bool finderMoved = false;
                    {
                        std::lock_guard guard(m_mutex);
                        finderMoved = syncFinderLocked();
                    }
                    if (finderMoved) {
                        m_tempGrow.clear();
                        m_lastHash = 0;
                    }
                    const std::string reason = resolveTarget(hwnd, region, overlay, true, &code);
                    std::string filtered;
                    bool read = false, regrow = false;
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
                            if (!filtered.empty() && growForCutLine(hwnd == nullptr)) {
                                regrow = true;
                            } else {   // a line running into the region's edge: grow that way for a while
                                std::vector<std::array<int, 4>> boxes;
                                if (!filtered.empty())
                                    for (const text::OcrBox& b : m_lastBoxes)
                                        if (hwnd || !withoutOwnOverlay(b.text).empty()) boxes.push_back({b.x0, b.y0, b.x1, b.y1});
                                const auto before = m_tempGrow.grow();
                                if (m_tempGrow.update(boxes, m_frame.width, m_frame.height, now())) regrow = true;
                                if (before != m_tempGrow.grow()) m_lastHash = 0;
                            }
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
                    const std::u32string reading = text::fromUtf8(filtered);
                    if (regrow) wait = 0;   // read the grown region at once; the cut reading is not a line
                    else handleEvents(read ? m_tracker.update(reading, t) : m_tracker.reset(), reading);

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
                        if (reason.empty()) {
                            showLine(*d, overlay, t);
                            m_shown = d;
                            seenStyle = styleVersion;
                        }
                    } else if (d && m_chunks.active() && m_chunks.id() == d->id && m_chunks.at(t) != m_shownChunk) {
                        if (reason.empty()) {   // the next chunk of a long line
                            m_shownChunk = m_chunks.at(t);
                            showOverlay({d->id, m_chunks.chunk(m_shownChunk), d->kind}, overlay, m_shownChunk, m_chunks.count());
                        }
                    } else if (!d && m_shown) {
                        hideOverlay();
                        m_shown.reset();
                        m_chunks.clear();
                    }
                    if (read && !regrow) runFinder();
                }
            }
        } catch (const std::exception& e) {
            notifyError(std::string("Beklenmeyen hata: ") + e.what(), "loop_exception");
            wait = kBlockedPollS;
        } catch (...) {
            notifyError("Beklenmeyen hata", "loop_unknown");
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

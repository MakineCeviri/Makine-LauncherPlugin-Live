// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// Everything around the reading loop that MakineLive.exe had besides the overlay: where the
// translation goes (the launcher overlay, Makine TV, a phone / tablet page, an OBS browser
// source), Discord, installing Makine TV, the server's announcements, the support log and the
// donation link. Kept out of engine.cpp so the reading pipeline stays separate.

#include "engine.h"

#include "gzip_store.h"
#include "unitext.h"
#include "winutil.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <string>
#include <thread>

using json = nlohmann::json;
using makine::plugin::Result;

namespace live {

namespace {

constexpr const char* kDonateUrl = "https://kreosus.com/makineceviri/posts";
constexpr size_t kLogRingBytes = 512 * 1024;

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string str(const json& j, const char* key)
{
    return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

bool flag(const json& j, const char* key)
{
    return j.is_object() && j.contains(key)
           && ((j[key].is_boolean() && j[key].get<bool>()) || (j[key].is_number() && j[key].get<double>() != 0));
}

double num(const json& j, const char* key, double def)
{
    return j.is_object() && j.contains(key) && j[key].is_number() ? j[key].get<double>() : def;
}

std::string nowText()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

const json& installGuide()
{
    static const json g = {
        {"android", json::array({"TV'de Ayarlar › Cihaz Tercihleri › Hakkında › Derleme'ye 7 kez basın (Geliştirici seçenekleri açılır).",
                                 "Ayarlar › Cihaz Tercihleri › Geliştirici seçenekleri › Ağ üzerinden hata ayıklama (ADB) seçeneğini açın.",
                                 "TV'nin adresini aşağıdan seçin (ya da yazın) ve Kur'a basın; TV'de çıkan soruya İzin ver deyin."})},
        {"firetv", json::array({"Fire TV'de Ayarlar › My Fire TV › Hakkında › cihaz adına 7 kez basın.",
                                "Ayarlar › My Fire TV › Geliştirici Seçenekleri › ADB hata ayıklama: Açık.",
                                "TV'nin adresini seçin ve Kur'a basın; TV'deki soruya İzin ver deyin."})},
        {"lg", json::array({"TV'de LG Content Store'dan Developer Mode uygulamasını kurun, LG geliştirici hesabınızla girin ve Dev Mode Status'u açın.",
                            "\"LG paketini kaydet\" ile makine-tv.ipk dosyasını bilgisayara kaydedin.",
                            "Paketi webOS Dev Manager (webosbrew.org) ya da LG'nin ares-install aracıyla kurun: TV'nin adresi ve Developer Mode ekranındaki Passphrase gerekir."})},
    };
    return g;
}

} // namespace

// ── log ring: the plugin's own log lines, for the support log ───────────────

void Engine::logLine(int32_t level, const std::string& message)
{
    m_host.log(level, message);
    static const char* names[] = {"debug", "info", "warn", "error"};
    std::string line = nowText() + " " + names[std::clamp(level, 0, 3)] + " " + message + "\n";
    std::lock_guard lock(m_logMutex);
    m_logBytes += line.size();
    m_logRing.push_back(std::move(line));
    while (m_logBytes > kLogRingBytes && !m_logRing.empty()) {
        m_logBytes -= m_logRing.front().size();
        m_logRing.pop_front();
    }
}

void Engine::onLogRequest(const std::string& requestId)
{
    {
        std::lock_guard lock(m_mutex);
        if (requestId.empty() || requestId == m_logSending) return;
        m_logSending = requestId;
    }
    post([this, requestId] {
        std::string text;
        {
            std::lock_guard lock(m_mutex);
            text = "──── Makine Live günlüğü\nsürüm: " + m_init.version + " (Makine " + m_init.launcherVersion
                   + ")\nzaman: " + nowText() + "\noyun: " + (m_gameName.empty() ? "—" : m_gameName) + "\nmod: " + m_mode
                   + "\n\n";
        }
        {
            std::lock_guard lock(m_logMutex);
            for (const auto& l : m_logRing) text += l;
        }
        const std::string gz = gzipStored(text);
        const ApiResult r = m_server.uploadLog(requestId, gz);
        {
            std::lock_guard lock(m_mutex);
            if (!r.ok()) {
                m_logSending.clear();   // the next report asks again
            } else {
                char hhmm[8];
                const std::time_t t = std::time(nullptr);
                std::tm tm{};
                localtime_s(&tm, &t);
                std::strftime(hhmm, sizeof hhmm, "%H:%M", &tm);
                // A card until the user closes it: the team asked for the log to look into a problem
                m_support = {{"time", hhmm}, {"sizeKb", std::max<size_t>(1, gz.size() / 1024)}};
            }
        }
        if (r.ok()) logLine(MAKINE_LOG_INFO, "Makine Live ekibinin istediği kayıt gönderildi");
        publish();
    });
}

// ── outputs ─────────────────────────────────────────────────────────────────

void Engine::startOutputs()
{
    // The modules change on their own threads; the screen hears it through a publish
    const auto changed = [weak = std::weak_ptr<Engine>(shared_from_this())] {
        if (const auto self = weak.lock()) self->post([self] { self->publish(); });
    };
    m_discord = std::make_shared<DiscordPresence>();
    m_discord->onChange(changed);
    m_discord->start();   // always on in the launcher: no setting (product decision)
    const auto logo = win::readFile(m_init.pluginDir + L"\\resources\\logo.webp");
    m_web = std::make_shared<SubtitleWeb>(logo ? *logo : std::string());
    m_web->onChange(changed);
    wchar_t pc[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    m_tv = std::make_shared<TvLink>(GetComputerNameW(pc, &n) ? win::wideToUtf8(pc) : std::string("PC"));
    m_tv->onChange(changed);
    m_tv->start();
    applyOutputs();
    applyStyleOutputs();
}

void Engine::stopOutputs()
{
    if (m_discord) m_discord->stop();
    if (m_web) m_web->stop();
    if (m_tv) m_tv->close();
}

// Where a translation goes: the launcher overlay unless a console is shown on the TV / phone;
// Makine TV in console mode with output "tv"; the phone page (console, output "phone") and the
// OBS source share one local web server.
void Engine::applyOutputs()
{
    bool console = false, phone = false, obs = false, tv = false;
    std::optional<TvInfo> saved;
    std::string code;
    {
        std::lock_guard lock(m_mutex);
        console = m_mode == "console";
        phone = console && m_output == "phone";
        tv = console && m_output == "tv";
        obs = m_obs;
        saved = TvInfo::fromJson(m_tvSaved);
        code = m_tvCode;
    }
    if (m_web) m_web->configure(phone || obs, phone);
    if (m_tv) m_tv->pair(tv ? saved : std::nullopt, code);
    publish();
}

void Engine::applyStyleOutputs()
{
    json s;
    {
        std::lock_guard lock(m_mutex);
        s = m_style;
    }
    const int size = static_cast<int>(num(s, "sizePx", 26));
    const int opacity = str(s, "preset") == "minimal" ? 0 : static_cast<int>(std::lround(num(s, "bandOpacity", 0.78) * 100));
    const std::string pos = str(s, "position") == "top" ? "ust" : "alt";
    json web = {{"size", size}, {"opacity", opacity}, {"pos", pos}};
    if (!str(s, "textColor").empty()) web["text"] = str(s, "textColor");
    if (!str(s, "bandColor").empty()) web["band"] = str(s, "bandColor");
    if (!str(s, "font").empty()) web["font"] = str(s, "font");
    if (s.contains("outlinePx") && s["outlinePx"].is_number()) web["outline"] = s["outlinePx"];
    if (s.contains("widthPct") && s["widthPct"].is_number()) web["width"] = s["widthPct"];
    if (s.contains("shadow") && s["shadow"].is_boolean()) web["shadow"] = s["shadow"];
    if (m_web) m_web->setStyle(web);
    if (m_tv) m_tv->setStyle(size, pos, opacity);
}

bool Engine::boxWanted() const
{
    std::lock_guard lock(m_mutex);
    return m_mode != "console" || m_output == "pc";
}

void Engine::routeLine(const Display& d)
{
    bool tv = false;
    {
        std::lock_guard lock(m_mutex);
        tv = m_mode == "console" && m_output == "tv";
    }
    ++m_lineSeq;
    const std::string text = text::toUtf8(d.text);
    if (tv && m_tv) m_tv->sendLine(m_lineSeq, text, d.kind);
    if (m_web && m_web->running()) m_web->sendLine(m_lineSeq, text, d.kind);
}

void Engine::routeClear()
{
    if (m_tv) m_tv->clear();
    if (m_web) m_web->clear();
}

void Engine::updateDiscord()
{
    if (!m_discord) return;
    std::optional<json> activity;
    {
        std::lock_guard lock(m_mutex);
        const bool console = m_mode == "console";
        if (m_running)
            activity = discordActivity(console ? std::string(console::kGameName) : m_gameName, m_readingSince, m_sessionLines,
                                       console, "", console ? std::string(console::kGameSlug) : m_gameSlug);
    }
    m_discord->set(std::move(activity));
}

// ── state and methods for the launcher screen ───────────────────────────────

json Engine::outputsStateLocked() const
{
    const auto tvState = m_tv ? m_tv->state() : std::pair<std::string, std::string>{"idle", ""};
    json web = {{"running", false}};
    if (m_web) {
        const bool lan = m_mode == "console" && m_output == "phone";
        web = {{"running", m_web->running()},
               {"lan", m_web->lan()},
               {"port", m_web->port()},
               {"clients", m_web->clients()},
               {"obsUrl", m_web->obsUrl()},
               {"phoneUrl", lan ? m_web->phoneUrl() : std::string()},
               {"error", m_web->error()},
               {"firewallNote", lan ? "Telefon sayfası ev ağına açılır. Windows ilk seferde \"makine-plugin-host.exe\" için "
                                      "güvenlik duvarı izni sorar: Özel ağlar'ı seçip İzin ver'e basın. Makine Live kendisi "
                                      "kural eklemez; izin verilmezse telefon bağlanamaz."
                                    : ""}};
    }
    json tvInstall = m_tvInstall;
    tvInstall["apkAvailable"] = win::fileExists(m_init.pluginDir + L"\\resources\\makine-tv.apk");
    tvInstall["ipkAvailable"] = win::fileExists(m_init.pluginDir + L"\\resources\\makine-tv.ipk");
    tvInstall["guide"] = installGuide();
    return {
        {"output", m_output},
        {"obs", m_obs},
        {"web", web},
        {"tv", {{"state", tvState.first}, {"detail", tvState.second}, {"paired", m_tvSaved},
                {"found", m_tvFound}, {"searching", m_tvSearching},
                {"firewallNote", "TV aramasında Windows \"makine-plugin-host.exe\" için güvenlik duvarı izni sorabilir: "
                                 "Özel ağlar'ı seçip İzin ver'e basın (TV'nin yanıtı bu izinle gelir). Makine Live kural eklemez."}}},
        {"tvInstall", tvInstall},
        {"discord", {{"connected", m_discord && m_discord->connected()}}},
        {"announcements", m_announcements},
        {"support", m_support},
        {"donate", {{"url", kDonateUrl}, {"seen", m_donateSeen}}},
    };
}

void Engine::onStatus(const json& data)
{
    if (!data.is_object() || !data.contains("announcements") || !data["announcements"].is_array()) return;
    std::lock_guard lock(m_mutex);
    for (const json& a : data["announcements"]) {
        if (!a.is_object() || !a.contains("id") || !a["id"].is_number_integer()) continue;
        const int64_t id = a["id"].get<int64_t>();
        if (m_annSeen.count(id)) continue;
        m_annSeen.insert(id);
        m_announcements.push_back({{"id", id},
                                   {"kind", a.value("kind", std::string("info"))},
                                   {"kindLabel", str(a, "kind_label").empty() ? "Duyuru" : str(a, "kind_label")},
                                   {"title", str(a, "title")},
                                   {"body", str(a, "body")},
                                   {"author", str(a, "author").empty() ? "Yönetici" : str(a, "author")},
                                   {"createdAt", str(a, "created_at")}});
    }
}

std::optional<Result> Engine::invokeOutputs(std::string_view method, const json& params)
{
    const auto ok = [](std::string_view m) { return Result::ok(dump({{"message", m}})); };
    if (method == "output.set") {
        const std::string out = str(params, "output");
        if (out != "pc" && out != "tv" && out != "phone")
            return Result::error(MAKINE_E_INVALID_ARGUMENT, "output: pc, tv ya da phone");
        {
            std::lock_guard lock(m_mutex);
            m_output = out;
            saveSettings();
        }
        routeClear();
        applyOutputs();
        return ok(out == "pc" ? "Altyazı bilgisayarda" : out == "tv" ? "Altyazı Makine TV'de" : "Altyazı telefonda / tablette");
    }
    if (method == "obs.set") {
        {
            std::lock_guard lock(m_mutex);
            m_obs = flag(params, "enabled");
            saveSettings();
        }
        applyOutputs();
        return Result::ok(dump({{"obsUrl", m_web ? m_web->obsUrl() : std::string()}}));
    }
    if (method == "tv.discover") {
        {
            std::lock_guard lock(m_mutex);
            if (m_tvSearching) return Result::error(MAKINE_E_BUSY, "TV zaten aranıyor.");
            m_tvSearching = true;
        }
        publish();
        post([this] {
            json found = json::array();
            for (const TvInfo& t : discoverTvs(1600)) found.push_back(t.toJson());
            {
                std::lock_guard lock(m_mutex);
                m_tvFound = found;
                m_tvSearching = false;
            }
            publish();
        });
        return ok("Ev ağında Makine TV aranıyor.");
    }
    if (method == "tv.pair") {
        // {id} from tv.discover, or {host, port?} ("connect by address"), and the code on the TV
        const std::string code = str(params, "code");
        if (code.empty()) return Result::error(MAKINE_E_INVALID_ARGUMENT, "TV ekranındaki kodu girin.");
        json chosen;
        {
            std::lock_guard lock(m_mutex);
            for (const json& t : m_tvFound)
                if (t.value("id", std::string()) == str(params, "id")) chosen = t;
        }
        if (chosen.is_null() && !str(params, "host").empty())
            chosen = {{"id", "adres:" + str(params, "host")}, {"name", str(params, "host")}, {"host", str(params, "host")},
                      {"port", static_cast<int>(num(params, "port", kTvPort))}};
        if (!TvInfo::fromJson(chosen)) return Result::error(MAKINE_E_NOT_FOUND, "Bu TV listede yok; yeniden arayın.");
        {
            std::lock_guard lock(m_mutex);
            m_tvSaved = chosen;
            m_tvCode = code;
            if (m_mode == "console") m_output = "tv";
            saveSettings();
        }
        applyOutputs();
        return ok("TV'ye bağlanılıyor.");
    }
    if (method == "tv.unpair") {
        {
            std::lock_guard lock(m_mutex);
            m_tvSaved = nullptr;
            m_tvCode.clear();
            saveSettings();
        }
        applyOutputs();
        return ok("TV bağlantısı kaldırıldı.");
    }
    if (method == "tv.install.scan") {
        const std::string platform = str(params, "platform");
        post([this, platform] {
            json hosts = json::array();
            for (const auto& h : adb::scanHosts(platform == "lg" ? 9922 : adb::kPort)) hosts.push_back(h);
            {
                std::lock_guard lock(m_mutex);
                m_tvInstall["hosts"] = hosts;
                m_tvInstall["scanning"] = false;
            }
            publish();
        });
        {
            std::lock_guard lock(m_mutex);
            m_tvInstall["scanning"] = true;
        }
        publish();
        return ok("Ev ağında TV aranıyor.");
    }
    if (method == "tv.install") {
        const std::string platform = str(params, "platform");
        const std::string host = str(params, "host");
        if (platform == "lg")
            return Result::error(MAKINE_E_UNSUPPORTED, "LG'ye kurulum şimdilik elle: tvInstall.guide.lg adımlarını izleyin.");
        if (host.empty()) return Result::error(MAKINE_E_INVALID_ARGUMENT, "TV'nin adresini seçin.");
        const int port = static_cast<int>(num(params, "port", adb::kPort));
        {
            std::lock_guard lock(m_mutex);
            if (m_tvInstall.value("state", std::string()) == "running") return Result::error(MAKINE_E_BUSY, "Kurulum sürüyor.");
            m_tvInstall["platform"] = platform.empty() ? "android" : platform;
            m_tvInstall["state"] = "running";
            m_tvInstall["step"] = "Hazırlanıyor…";
            m_tvInstall["message"] = "";
        }
        publish();
        // Minutes with a user prompt on the TV: its own thread, not a net thread
        std::thread([self = shared_from_this(), host, port] {
            const auto setStep = [&](const std::string& s) {
                {
                    std::lock_guard lock(self->m_mutex);
                    self->m_tvInstall["step"] = s;
                }
                self->publish();
            };
            std::string error, note;
            const auto apk = win::readFile(self->m_init.pluginDir + L"\\resources\\makine-tv.apk");
            if (!apk) {
                error = "Makine TV paketi bu kurulumda yok (resources\\makine-tv.apk).";
            } else if (!self->m_adbKey.loadOrCreate(self->m_init.dataDir, error)) {
            } else {
                wchar_t pc[MAX_COMPUTERNAME_LENGTH + 1] = {};
                DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
                const std::string name = "makine-live@" + (GetComputerNameW(pc, &n) ? win::wideToUtf8(pc) : std::string("PC"));
                error = adb::installApk(host, port, std::vector<uint8_t>(apk->begin(), apk->end()), self->m_adbKey, name,
                                        setStep, note);
            }
            {
                std::lock_guard lock(self->m_mutex);
                self->m_tvInstall["state"] = error.empty() ? "done" : "error";
                self->m_tvInstall["message"] = !error.empty() ? error : note.empty() ? "Makine TV kuruldu ve açıldı." : note;
            }
            self->logLine(error.empty() ? MAKINE_LOG_INFO : MAKINE_LOG_WARN,
                          "TV'ye kurulum (" + host + "): " + (error.empty() ? "tamam" : error));
            self->publish();
        }).detach();
        return ok("Makine TV kuruluyor.");
    }
    if (method == "tv.install.saveFile") {
        // The package file into Downloads, for a USB stick or a manual install (LG)
        const std::string platform = str(params, "platform");
        const std::wstring name = platform == "lg" ? L"makine-tv.ipk" : L"makine-tv.apk";
        const auto data = win::readFile(m_init.pluginDir + L"\\resources\\" + name);
        if (!data) return Result::error(MAKINE_E_NOT_FOUND, "Bu paket bu kurulumda yok.");
        PWSTR dir = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dir) != S_OK) {
            CoTaskMemFree(dir);
            return Result::error(MAKINE_E_FAILED, "İndirilenler klasörü bulunamadı.");
        }
        const std::wstring path = std::wstring(dir) + L"\\" + name;
        CoTaskMemFree(dir);
        if (!win::writeFileAtomic(path, *data)) return Result::error(MAKINE_E_FAILED, "Dosya kaydedilemedi.");
        return Result::ok(dump({{"message", "Kaydedildi: " + win::wideToUtf8(path)}, {"path", win::wideToUtf8(path)}}));
    }
    if (method == "announcement.read") {
        const auto id = static_cast<int64_t>(num(params, "id", 0));
        {
            std::lock_guard lock(m_mutex);
            json kept = json::array();
            for (const json& a : m_announcements)
                if (a.value("id", int64_t(0)) != id) kept.push_back(a);
            m_announcements = kept;
        }
        post([this, id] { m_server.announcementRead(id); });
        publish();
        return ok("Okundu.");
    }
    if (method == "support.dismiss") {
        {
            std::lock_guard lock(m_mutex);
            m_support = nullptr;
        }
        publish();
        return ok("Kapatıldı.");
    }
    if (method == "donate" || method == "donate.dismiss") {
        {
            std::lock_guard lock(m_mutex);
            m_donateSeen = true;
            saveSettings();
        }
        if (method == "donate")
            post([this] {
                if (!m_host.call("ui.openUrl", dump({{"url", kDonateUrl}})).succeeded())
                    logLine(MAKINE_LOG_WARN, std::string("Bağış sayfası açılamadı: ") + kDonateUrl);
            });
        publish();
        return ok(method == "donate" ? "Bağış sayfası açılıyor." : "Kapatıldı.");
    }
    return std::nullopt;
}

} // namespace live

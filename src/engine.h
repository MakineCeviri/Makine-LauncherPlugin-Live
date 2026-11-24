// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Makine Live inside the launcher (plugin ABI 2): the screen the launcher draws from
// ui.state, the reading loop and the server. Threads:
//   host thread     invoke / onEvent only flip state and queue work; never blocks
//   capture thread  owns capture, OCR, the text filter, LineTracker and SubtitleScheduler,
//                   and drives overlay.show / overlay.hide
//   net threads     (2) a timed job queue: login, profile, status, pool sync, translations,
//                   and host calls that wait for the user (overlay.pickRegion)
// Every thread holds a shared_ptr to the Engine, so one that misses the shutdown budget is
// detached and finishes on memory that is still alive.

#include "adb.h"
#include "capture.h"
#include "discord.h"
#include "console_mode.h"
#include "garbage.h"
#include "hotkeys.h"
#include "local_pool.h"
#include "ocr_ort.h"
#include "server.h"
#include "settings.h"
#include "subtitle_timing.h"
#include "subtitle_web.h"
#include "tv_link.h"
#include "text_fix.h"
#include "text_mask.h"
#include "fit.h"
#include "subtitle_finder.h"

#include <makine/plugin/sdk.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace live {

struct EngineInit {
    std::wstring dataDir, pluginDir;
    std::string version, launcherVersion;
    nlohmann::json settings;   // init.settings: the manifest's settings, kept by the launcher
};

class Engine : public std::enable_shared_from_this<Engine> {
public:
    Engine(makine::plugin::Host host, EngineInit init);

    void launch();
    void shutdown();   // within ~2.5 s of the host's 3 s budget

    makine::plugin::Result invoke(std::string_view method, const nlohmann::json& params);
    void onEvent(std::string_view event, const nlohmann::json& payload);

private:
    using Clock = std::chrono::steady_clock;
    struct Job {
        Clock::time_point due;
        std::function<void()> fn;
    };
    struct Translated {   // a translation for the capture thread
        int line;
        std::string text;   // empty: failed
        std::string kind;
    };
    struct CachedTranslation {
        std::string text, kind;
        double at;
    };

    // host thread
    makine::plugin::Result loginStart();
    makine::plugin::Result loginTeam(const nlohmann::json& params);
    makine::plugin::Result setBoxMovable(bool on);
    makine::plugin::Result showSample();
    makine::plugin::Result pickRegion();
    makine::plugin::Result readOnce();
    makine::plugin::Result startReading();
    makine::plugin::Result listSources(bool all);
    makine::plugin::Result selectSource(const nlohmann::json& params);

    // net threads
    void post(std::function<void()> fn, double delaySeconds = 0);
    void runNet();
    void loginPoll(int gen, std::string secret, double deadline, double interval);
    void validateSession();
    void refreshStatus();
    void statusLoop();
    void syncPool(bool force);
    void syncHotkeys();                       // never under m_mutex
    bool growForCutLine(bool screen);          // capture thread
    nlohmann::json countersLocked() const;
    void onHotkey(const std::string& action);
    void addHistoryLocked(const std::string& source, const std::string& text, const std::string& kind,
                          const std::string& origin);
    // ocrUs: wall-clock start/end (µs) of the OCR run that read the line, for a sampled span; {0, 0} when unknown.
    void translateJob(int line, std::string text, int seq, std::string slug, std::string gameName,
                      std::pair<uint64_t, uint64_t> ocrUs = {});
    // One speaker's line: source as read (label included), bare = what is keyed and sent. → {translation, kind}
    std::pair<std::string, std::string> translatePart(int line, const std::string& source, const std::string& bare,
                                                      const std::string& slug, const std::string& gameName,
                                                      std::pair<uint64_t, uint64_t> ocrUs, const std::string& prefix,
                                                      bool clean, bool& fresh);
    void traceLine(std::pair<uint64_t, uint64_t> ocrUs, uint64_t requestStartUs, uint64_t requestEndUs,
                   const std::string& kind);
    void signedOut(const std::string& message);
    void signedIn(const nlohmann::json& profile);
    void handleAuthFailure(const ApiResult& r);

    // capture thread
    void runCapture();
    bool ensureOcr(std::string& error);
    bool readRegion(void* hwnd, const CaptureRegion& region, std::string& text, std::string& error);
    std::string resolveTarget(void*& hwnd, CaptureRegion& region, console::Box& overlay, bool needGame,
                              std::string* code = nullptr);
    void handleEvents(const std::vector<LineEvent>& events, const std::u32string& reading = {});
    void showOverlay(const Display& d, const console::Box& rect, int chunk = 0, int chunks = 1);
    void showLine(const Display& d, const console::Box& rect, double t);   // plans the chunks (fit.h), shows the first
    void hideOverlay();
    std::string withoutOwnOverlay(const std::string& raw) const;
    void stopTracking();
    bool syncFinderLocked();      // the finder follows the game (slug) and the manual region; true when it changed
    void storeBandLocked();       // a locked band, per game
    void runFinder();             // one detector run over the whole window, when due
    nlohmann::json autoRegionLocked() const;

    // any thread
    nlohmann::json stateLocked() const;
    void publish();
    void setStatus(const std::string& text);
    // `reason`: a stable code naming the defect in Sentry (host API telemetry.capture); nullptr for the player's own
    // setup (no console app, cancelled pick), which is no defect and must not cost quota in a GTA VI surge.
    void notifyError(const std::string& message, const char* reason, const char* level = "error");
    void report(const char* reason, const char* level, const std::string& message);
    void maybeAutoStart();
    void logLine(int32_t level, const std::string& message);   // to the host log and the support log ring

    // engine_outputs.cpp: overlay / TV / phone / OBS routing, Discord, TV install, announcements, support
    void startOutputs();
    void stopOutputs();
    void applyOutputs();
    void applyStyleOutputs();
    bool boxWanted() const;
    void routeLine(const Display& d, int chunk = 0, int chunks = 1);
    void routeClear();
    void updateDiscord();
    nlohmann::json outputsStateLocked() const;
    std::optional<makine::plugin::Result> invokeOutputs(std::string_view method, const nlohmann::json& params);
    void onStatus(const nlohmann::json& data);
    void onLogRequest(const std::string& requestId);
    nlohmann::json reasonLocked() const;   // the most blocking reason {code, text, action} or null
    void updateTvNote();
    void setHostSetting(const std::string& key, bool value);
    void beatLoop(int gen);
    nlohmann::json beatPayloadLocked(bool bye);
    void saveSettings();
    std::string currentSlug() const;   // under m_mutex
    bool consentMissingLocked() const;   // lines would go to the GTA VI pool without the player's consent
    void stopIfConsentMissing();
    double now() const;

    const makine::plugin::Host m_host;
    const EngineInit m_init;
    const Clock::time_point m_t0 = Clock::now();
    ServerClient m_server;
    LocalPool m_pool;

    // ── guarded by m_mutex ──
    mutable std::mutex m_mutex;
    std::condition_variable m_captureCv, m_netCv;
    Settings m_settings;
    bool m_quit = false;
    int m_finished = 0;
    std::deque<std::function<void()>> m_captureJobs;
    std::vector<Job> m_jobs;
    // screen state
    std::string m_loginState = "signedOut", m_loginCode, m_loginUrl, m_loginMessage, m_user, m_role;
    int m_loginGen = 0;
    std::string m_mode = "pc";
    std::optional<console::Box> m_pcRegion;
    console::Frac m_consoleRegion = console::kDefaultRegion;
    bool m_consoleManual = false;             // the player picked the console region (else the finder's band)
    finder::SubtitleFinder m_finder;          // the subtitle band found by itself (subtitle_finder.h)
    std::string m_finderSlug;                 // the game the finder's band is for
    std::string m_sourceKey;                  // preferred console source
    unsigned long m_sourcePid = 0;            // the one the user selected
    std::vector<std::string> m_customExes;
    nlohmann::json m_sources = nlohmann::json::array();
    nlohmann::json m_style = nlohmann::json::object();
    bool m_running = false, m_picking = false;
    std::string m_status = "Hazır";
    nlohmann::json m_lastLine = nullptr;
    std::deque<double> m_latMs;               // the last 30 translate requests (counters.avgMs)
    std::vector<std::string> m_samples;       // sample lines (resources/sample_quotes.json)
    int m_sampleGen = 0;
    std::deque<nlohmann::json> m_history;     // this session's lines, newest first (ui.state history)
    std::map<std::string, std::string> m_hotkeyKeys{{"start", "F9"}, {"region", "F10"}, {"move", "F8"}};
    bool m_hotkeysArmed = false;
    bool m_boxMovable = false, m_boxMoveSupported = true;
    int m_lines = 0, m_poolHits = 0;
    bool m_maintenance = false, m_blocked = false;
    std::string m_serverMessage, m_notice;
    int m_styleVersion = 0;
    std::string m_gameName, m_gameSlug;       // PC: the game the launcher says is running
    bool m_gameUnknown = false;                // a game the launcher saw but could not name: not read
    unsigned long m_gamePid = 0;               // its process, when the launcher sends one
    std::string m_gameExe;                     // or its exe name (lower case)
    unsigned long m_launcherPid = 0;
    std::string m_poolSlug;                    // the local pool in use
    double m_poolSyncedAt = -1e9;
    // translation bookkeeping
    std::deque<Translated> m_results;
    std::unordered_map<int, int> m_dispatchSeq;
    std::unordered_map<int, double> m_goneAt;
    std::set<std::u32string> m_skipKeys;       // the server called them garbage (422)
    std::map<std::u32string, CachedTranslation> m_cache;
    std::set<std::u32string> m_inflight;
    std::string m_lastError;
    // launcher-kept settings (manifest contributes.settings)
    bool m_autoStart = false, m_usageReport = true, m_pendingStart = false;
    // usage report (port of net/telemetry.py + app._beat_payload)
    int m_beatGen = 0;
    double m_beatS = 300;
    // Sampled lines as Sentry spans (host API telemetry.transaction): the server sets the session's budget
    // (beat reply trace_n; 0 = off, nothing before the first reply); the first two lines, then one in 20.
    int m_traceBudget = 0, m_traceSent = 0, m_traceSeen = 0;
    std::string m_logInSentry;   // the log request Sentry Logs already took (only log_sent is retried)
    std::pair<uint64_t, uint64_t> m_lastOcrUs{};   // capture thread only
    int64_t m_startedAt = 0;
    double m_runAcc = 0;
    int m_linesTotal = 0, m_linesSent = 0, m_sessionLines = 0, m_errors = 0, m_poolCount = 0, m_apiCount = 0;
    std::deque<double> m_ocrMs, m_poolMs, m_apiMs;
    // outputs (engine_outputs.cpp)
    std::string m_output = "pc";               // console mode: pc | tv | phone
    bool m_obs = false, m_donateSeen = false, m_tvSearching = false;
    nlohmann::json m_tvSaved = nullptr;        // the paired TV
    std::string m_tvCode;
    nlohmann::json m_tvFound = nlohmann::json::array(), m_tvInstall = nlohmann::json::object();
    nlohmann::json m_announcements = nlohmann::json::array(), m_support = nullptr;
    std::set<int64_t> m_annSeen;
    std::string m_logSending;
    int64_t m_readingSince = 0;
    // Fable's rules: console lines go to the GTA VI pool only after the player agreed once; nothing is
    // captured before the player's first explicit start (autoStart waits for it); the screen is told a
    // firewall question is coming before the first home-network listener opens
    std::string m_captureCode, m_noticeCode, m_noteCode;   // fixed reason codes (OCR_KARAR §6)
    bool m_noteRunning = false;
    int m_styleSaveGen = 0;
    double m_darkSince = -1, m_blackCheckAt = 0;   // capture thread: black-frame detection
    bool m_consoleConsent = false, m_everStarted = false, m_lanOnce = false, m_firewallHeadsUp = false;

    // ── capture thread only ──
    std::unique_ptr<ScreenCapture> m_capture;
    std::unique_ptr<OcrOrt> m_ocr;
    SpacingFixer m_spacing;
    text::Garbage m_garbage;
    CapturedFrame m_frame;
    uint64_t m_lastHash = 0;
    std::string m_lastOcr, m_lastRaw, m_lastFiltered;
    TextMask m_lastMask;     // K3: the text mask of the last OCR'd frame (console mode)
    double m_lastOcrAt = -1e9;
    LineTracker m_tracker;
    SubtitleScheduler m_scheduler;
    bool m_active = false, m_overlayShown = false;
    bool m_sampleOnScreen = false;             // capture thread
    std::vector<text::OcrBox> m_lastBoxes;     // capture thread: the last OCR's lines and their boxes
    // This session only: how far the region grew past a cut line (left, right, top, bottom; share of its size)
    std::array<double, 4> m_grow{};
    std::optional<console::Box> m_videoFrame;  // capture thread: the console video rect (overlay.show frame)
    std::optional<Display> m_shown;
    CapturedFrame m_finderFrame;               // capture thread: the whole window for the finder
    void* m_finderHwnd = nullptr;              // capture thread: what the finder reads (console window / screen)
    std::optional<CaptureRegion> m_finderArea; // … and where: the console video area / the game's client area
    double m_finderAt = -1e9;
    finder::TempGrow m_tempGrow;               // capture thread: a wide line grows the region for a while
    fit::ChunkPlayer m_chunks;                 // the shown line in chunks, when it does not fit
    int m_shownChunk = 0;
    std::map<int, std::string> m_lineSource;   // line id -> the text sent for translation
    std::vector<std::u32string> m_shownKeys;   // recent overlay texts (and their sources): never read back as source
    size_t m_shownCount = 0, m_filteredWith = 0;
    std::optional<console::Source> m_source;
    double m_nextPick = 0;

    std::shared_ptr<DiscordPresence> m_discord;
    std::shared_ptr<SubtitleWeb> m_web;
    std::shared_ptr<TvLink> m_tv;
    std::unique_ptr<Hotkeys> m_hotkeys;
    adb::Key m_adbKey;
    int m_lineSeq = 0;      // capture thread
    bool m_boxShown = false;
    std::mutex m_logMutex;
    std::deque<std::string> m_logRing;
    size_t m_logBytes = 0;

    std::thread m_captureThread;
    std::vector<std::thread> m_netThreads;
};

} // namespace live

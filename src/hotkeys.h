// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Global keys (F9 start/stop, F10 pick region, F8 move the subtitle box) for players who cannot
// alt-tab out of a game. RegisterHotKey on a message-only window in this class's own thread: no
// keyboard hook, nothing an antivirus reads as a key logger. A key another program holds fails to
// register, and that failure is reported (state()), never swallowed.

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace live {

class Hotkeys {
public:
    struct Binding {
        std::string action;   // "start" | "region" | "move"
        std::string key;      // "F1".."F12", or "off"
        bool operator==(const Binding&) const = default;
    };
    using Fire = std::function<void(const std::string& action)>;

    explicit Hotkeys(Fire fire);
    ~Hotkeys();
    Hotkeys(const Hotkeys&) = delete;
    Hotkeys& operator=(const Hotkeys&) = delete;

    /// Registers these keys (an empty list releases every key). Returns once they are registered or
    /// have failed; the same list again does nothing.
    void configure(const std::vector<Binding>& bindings);
    /// {action: {"key", "active", "error"}} for the screen.
    nlohmann::json state() const;

    static unsigned virtualKey(const std::string& key);   // "F9" → VK_F9; anything else 0

private:
    void stopThread();
    void run(std::vector<Binding> bindings);

    Fire m_fire;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    void* m_window = nullptr;   // HWND of the running thread's window
    bool m_ready = false;
    std::vector<Binding> m_bindings;
    nlohmann::json m_state = nlohmann::json::object();
};

} // namespace live

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "hotkeys.h"

#include <windows.h>

#include <chrono>

namespace live {

namespace {

constexpr wchar_t kClass[] = L"MakineLiveHotkeys";
constexpr UINT kStop = WM_APP + 1;

LRESULT CALLBACK windowProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, msg, w, l);
}

} // namespace

Hotkeys::Hotkeys(Fire fire) : m_fire(std::move(fire)) {}

Hotkeys::~Hotkeys()
{
    stopThread();
}

unsigned Hotkeys::virtualKey(const std::string& key)
{
    if (key.size() < 2 || key.size() > 3 || key[0] != 'F') return 0;
    int n = 0;
    for (size_t i = 1; i < key.size(); ++i) {
        if (key[i] < '0' || key[i] > '9') return 0;
        n = n * 10 + (key[i] - '0');
    }
    return n >= 1 && n <= 12 ? static_cast<unsigned>(VK_F1 + n - 1) : 0;
}

void Hotkeys::configure(const std::vector<Binding>& bindings)
{
    {
        std::lock_guard lock(m_mutex);
        // Same keys, all taken: nothing to do. A key that failed is tried again (its owner may have let go)
        bool allActive = true;
        for (const auto& b : bindings)
            allActive = allActive && (!virtualKey(b.key) || (m_state.contains(b.action) && m_state[b.action]["active"] == true));
        if (bindings == m_bindings && allActive && (m_thread.joinable() || bindings.empty())) return;
    }
    stopThread();
    std::unique_lock lock(m_mutex);
    m_bindings = bindings;
    m_state = nlohmann::json::object();
    for (const auto& b : bindings) m_state[b.action] = {{"key", b.key}, {"active", false}, {"error", ""}};
    bool any = false;
    for (const auto& b : bindings) any = any || virtualKey(b.key) != 0;
    if (!any) return;   // every key off: no thread, no window
    m_ready = false;
    m_thread = std::thread([this, bindings] { run(bindings); });
    m_cv.wait_for(lock, std::chrono::seconds(2), [this] { return m_ready; });
}

nlohmann::json Hotkeys::state() const
{
    std::lock_guard lock(m_mutex);
    return m_state;
}

void Hotkeys::stopThread()
{
    std::thread t;
    {
        std::lock_guard lock(m_mutex);
        if (m_window) PostMessageW(static_cast<HWND>(m_window), kStop, 0, 0);
        t = std::move(m_thread);
    }
    if (t.joinable()) t.join();
}

void Hotkeys::run(std::vector<Binding> bindings)
{
    try {
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = windowProc;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);   // fails harmlessly when a previous thread registered it
        const HWND h = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
        std::vector<int> ids;
        {
            std::lock_guard lock(m_mutex);
            m_window = h;
            for (size_t i = 0; i < bindings.size(); ++i) {
                const unsigned vk = virtualKey(bindings[i].key);
                auto& s = m_state[bindings[i].action];
                if (!vk) continue;
                if (!h) {
                    s["error"] = "Kısayol penceresi açılamadı.";
                    continue;
                }
                const int id = static_cast<int>(i) + 1;
                if (RegisterHotKey(h, id, MOD_NOREPEAT, vk)) {
                    s["active"] = true;
                    ids.push_back(id);
                } else {
                    s["error"] = GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED
                                     ? bindings[i].key + " başka bir programda kullanılıyor; ayarlardan başka bir tuş seç."
                                     : bindings[i].key + " kısayol olarak alınamadı.";
                }
            }
            m_ready = true;
        }
        m_cv.notify_all();
        if (!h) return;
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == kStop) break;
            if (msg.message == WM_HOTKEY && msg.wParam >= 1 && msg.wParam <= bindings.size())
                m_fire(bindings[msg.wParam - 1].action);
        }
        for (const int id : ids) UnregisterHotKey(h, id);
        {
            std::lock_guard lock(m_mutex);
            m_window = nullptr;
        }
        DestroyWindow(h);
    } catch (...) {   // a thread that throws would end the plugin host
        std::lock_guard lock(m_mutex);
        m_window = nullptr;
        m_ready = true;
        m_cv.notify_all();
    }
}

} // namespace live

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "console_mode.h"

#include "unitext.h"
#include "winutil.h"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace live::console {

namespace {

// Python round(): half to even (the default FE_TONEAREST mode)
int pyRound(double v) { return static_cast<int>(std::nearbyint(v)); }
double pyRound4(double v) { return std::nearbyint(v * 10000.0) / 10000.0; }
int floorDiv2(int v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); }

std::string lowerUtf8(const std::string& s) { return text::toUtf8(text::lower(text::fromUtf8(s))); }

struct Known {
    SourceApp app;
    std::vector<std::string> exes;   // lower case
    int title;                        // 0 any, 1 OBS projector, 2 camera
};

const std::vector<Known>& known()
{
    static const std::vector<Known> k{
        {{"ps-remote-play", "PS Remote Play", "ps5"}, {"remoteplay.exe"}, 0},
        {{"chiaki", "chiaki", "ps5"}, {"chiaki.exe", "chiaki-ng.exe"}, 0},
        {{"xbox-app", "Xbox uygulaması", "xbox"}, {"xboxpcapp.exe"}, 0},
        {{"greenlight", "Greenlight", "xbox"}, {"greenlight.exe"}, 0},
        {{"elgato", "Elgato 4K Capture Utility", "capture"}, {"4kcaptureutility.exe", "gamecapture.exe"}, 0},
        {{"obs-projector", "OBS projektör", "capture"}, {"obs64.exe", "obs32.exe"}, 1},
        {{"windows-camera", "Windows Kamera", "capture"}, {"applicationframehost.exe", "windowscamera.exe"}, 2},
    };
    return k;
}

std::wstring className(HWND h)
{
    wchar_t buf[128] = {};
    GetClassNameW(h, buf, 128);
    return buf;
}

std::string windowTitle(HWND h)
{
    wchar_t buf[256] = {};
    GetWindowTextW(h, buf, 256);
    return win::wideToUtf8(buf);
}

DWORD windowPid(HWND h)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    return pid;
}

std::string processExe(DWORD pid)
{
    const HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t buf[1024];
    DWORD size = 1024;
    std::wstring path = QueryFullProcessImageNameW(p, 0, buf, &size) ? std::wstring(buf, size) : std::wstring();
    CloseHandle(p);
    const size_t slash = path.find_last_of(L"\\/");
    return win::wideToUtf8(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

bool cloaked(HWND h)
{
    int v = 0;
    return DwmGetWindowAttribute(h, DWMWA_CLOAKED, &v, sizeof v) == S_OK && v != 0;
}

bool sourceWindow(HWND h)
{
    if (!IsWindowVisible(h) || cloaked(h)) return false;
    const std::wstring cls = className(h);
    if (cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd" || cls == L"Progman" || cls == L"WorkerW")
        return false;
    const Rect r = clientRect(h);
    // A minimised window still counts (the user is told to restore it); small popups do not
    return IsIconic(h) || (r.r - r.l >= 320 && r.b - r.t >= 200);
}

} // namespace

Rect videoRect(const Rect& c, double aspect)
{
    const int w = c.r - c.l, h = c.b - c.t;
    if (w <= 0 || h <= 0) return {c.l, c.t, c.l, c.t};
    if (static_cast<double>(w) / h > aspect) {   // bars at the sides
        const int vw = pyRound(h * aspect);
        const int x = c.l + floorDiv2(w - vw);
        return {x, c.t, x + vw, c.b};
    }
    const int vh = pyRound(w / aspect);           // bars above and below
    const int y = c.t + floorDiv2(h - vh);
    return {c.l, y, c.r, y + vh};
}

Box regionToScreen(const Rect& v, const Frac& f)
{
    const int vw = v.r - v.l, vh = v.b - v.t;
    return {v.l + pyRound(f.x * vw), v.t + pyRound(f.y * vh), std::max(1, pyRound(f.w * vw)),
            std::max(1, pyRound(f.h * vh))};
}

Frac screenToRegion(const Rect& v, const Box& r)
{
    const int vw = std::max(1, v.r - v.l), vh = std::max(1, v.b - v.t);
    const int x0 = std::min(std::max(r.x, v.l), v.r), y0 = std::min(std::max(r.y, v.t), v.b);
    const int x1 = std::min(std::max(r.x + r.w, v.l), v.r), y1 = std::min(std::max(r.y + r.h, v.t), v.b);
    return {pyRound4(static_cast<double>(x0 - v.l) / vw), pyRound4(static_cast<double>(y0 - v.t) / vh),
            pyRound4(static_cast<double>(std::max(0, x1 - x0)) / vw),
            pyRound4(static_cast<double>(std::max(0, y1 - y0)) / vh)};
}

bool validRegion(const Frac& f)
{
    return f.x >= 0 && f.x < 1 && f.y >= 0 && f.y < 1 && f.w >= 0.02 && f.w <= 1 && f.h >= 0.02 && f.h <= 1
           && f.x + f.w <= 1.001 && f.y + f.h <= 1.001;
}

bool matchSource(const std::string& exeName, const std::string& title, const std::vector<std::string>& customExes,
                 SourceApp& out)
{
    const std::string exe = lowerUtf8(exeName);
    const std::string t = lowerUtf8(title);
    for (const Known& k : known()) {
        if (std::find(k.exes.begin(), k.exes.end(), exe) == k.exes.end()) continue;
        // OBS: r"project|projekt"; Windows Camera: r"^(camera|kamera)$" (re.IGNORECASE)
        if (k.title == 1 && t.find("project") == std::string::npos && t.find("projekt") == std::string::npos) continue;
        if (k.title == 2 && t != "camera" && t != "kamera") continue;
        out = k.app;
        return true;
    }
    if (!exe.empty())
        for (const std::string& c : customExes)
            if (lowerUtf8(c) == exe) {
                const size_t dot = exe.rfind('.');
                out = {"custom", dot == std::string::npos ? exe : exe.substr(0, dot), "custom"};
                return true;
            }
    return false;
}

Rect clientRect(void* hwnd)
{
    RECT r{};
    if (!GetClientRect(static_cast<HWND>(hwnd), &r)) return {};
    POINT p{0, 0};
    ClientToScreen(static_cast<HWND>(hwnd), &p);
    return {p.x, p.y, p.x + r.right, p.y + r.bottom};
}

std::vector<Source> findSources(const std::vector<std::string>& customExes)
{
    std::vector<Source> found;
    std::set<DWORD> seen;
    const DWORD self = GetCurrentProcessId();
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!sourceWindow(h)) continue;
        const DWORD pid = windowPid(h);
        if (!pid || pid == self || seen.count(pid)) continue;
        Source s;
        s.exe = processExe(pid);
        s.title = windowTitle(h);
        if (!matchSource(s.exe, s.title, customExes, s.app)) continue;
        seen.insert(pid);
        s.hwnd = h;
        s.pid = pid;
        found.push_back(std::move(s));
    }
    return found;
}

bool alive(const Source& s)
{
    return s.hwnd && IsWindow(static_cast<HWND>(s.hwnd)) && windowPid(static_cast<HWND>(s.hwnd)) == s.pid;
}

std::string blockReason(const Source* s)
{
    if (!s) return "konsol görüntüsü bulunamadı · Remote Play ya da yakalama penceresini açın";
    if (!alive(*s)) return s->app.label + " kapandı";
    if (IsIconic(static_cast<HWND>(s->hwnd)))
        return s->app.label + " penceresi küçültülmüş · okunabilmesi için ekranda açık kalmalı";
    const Rect r = clientRect(s->hwnd);
    const int w = r.r - r.l, h = r.b - r.t;
    if (w < kMinWidth || h < kMinHeight)
        return s->app.label + " penceresi küçük (" + std::to_string(w) + "×" + std::to_string(h) + ") · en az "
               + std::to_string(kMinWidth) + "×" + std::to_string(kMinHeight) + " yapın";
    return {};
}

Box clientRegion(void* hwnd, const Frac& frac)
{
    const Rect c = clientRect(hwnd);
    return regionToScreen(videoRect({0, 0, c.r - c.l, c.b - c.t}), frac);
}

bool windowHasImage(void* hwndPtr)
{
    const HWND hwnd = static_cast<HWND>(hwndPtr);
    const Rect c = clientRect(hwnd);
    const int w = c.r - c.l, h = c.b - c.t;
    if (w < 64 || h < 64) return false;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    const HDC screen = GetDC(nullptr);
    const HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    const HBITMAP bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool result = false;
    if (mem && bmp && bits) {
        const HGDIOBJ old = SelectObject(mem, bmp);
        const BOOL ok = PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
        SelectObject(mem, old);
        if (ok) {
            // ~4000 samples over B, G, R, A alike: statistics only
            const auto* px = static_cast<const uint8_t*>(bits);
            const size_t total = static_cast<size_t>(w) * h * 4;
            const size_t step = static_cast<size_t>(std::max(4, (w * h) / 4096)) * 4;
            size_t samples = 0, bright = 0;
            for (size_t i = 0; i < total; i += step) {
                ++samples;
                if (px[i] > 24) ++bright;
            }
            result = static_cast<double>(bright) > static_cast<double>(samples) * 0.02;
        }
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return result;
}

} // namespace live::console

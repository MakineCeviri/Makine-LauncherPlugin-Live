// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Console mode (port of makine_live/games/console_mode.py): PS5 / Xbox games are read
// from the PC window that mirrors the console (PS Remote Play, chiaki, the Xbox app,
// Greenlight, a capture card preview, an OBS projector, Windows Camera, or a window the
// user picked). The subtitle region is kept as a fraction of the window's 16:9 video
// area, so it follows the window when it moves or resizes.

#include <string>
#include <vector>

namespace live::console {

struct SourceApp {
    std::string key;       // ps-remote-play, chiaki, xbox-app, greenlight, elgato, obs-projector, windows-camera, custom
    std::string label;
    std::string console;   // ps5 | xbox | stream | cloud | capture | custom
};

struct Rect {   // left, top, right, bottom
    int l = 0, t = 0, r = 0, b = 0;
    bool operator==(const Rect&) const = default;
};
struct Box {    // x, y, width, height
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const Box&) const = default;
};
struct Frac {   // fractions of the video area
    double x = 0, y = 0, w = 0, h = 0;
};

constexpr Frac kDefaultRegion{0.12, 0.70, 0.76, 0.26};   // bottom centre
constexpr int kMinWidth = 960, kMinHeight = 540;          // smaller: subtitles too small for OCR

// Console mode is GTA VI only: every line goes to its pool (user decision).
inline const char* const kGameName = "Grand Theft Auto VI";
inline const char* const kGameSlug = "grand-theft-auto-vi";

/// An app that streams the console picture to the PC (PS Remote Play, Asobi, chiaki, the Xbox
/// app, Greenlight), by exe name or the name a game library shows. In PC mode too it is GTA VI:
/// the console pool is GTA VI's (game_library.is_console_stream_exe, 0.3.2).
bool isConsoleStream(const std::string& nameOrExe);

Rect videoRect(const Rect& client, double aspect = 16.0 / 9.0);
Box regionToScreen(const Rect& video, const Frac& frac);
Frac screenToRegion(const Rect& video, const Box& rect);
bool validRegion(const Frac& frac);

/// The source app a window belongs to, by exe name and title; false when it is none.
bool matchSource(const std::string& exe, const std::string& title, const std::vector<std::string>& customExes,
                 SourceApp& out);

struct Source {
    void* hwnd = nullptr;
    unsigned long pid = 0;
    std::string exe;
    std::string title;
    SourceApp app;
};

/// Open source windows in z-order, one per process (our own process excluded).
std::vector<Source> findSources(const std::vector<std::string>& customExes = {});
/// Still the same window of the same process.
bool alive(const Source& s);
/// Client area in screen coordinates.
Rect clientRect(void* hwnd);
/// Why the source cannot be read now ("" = it can).
std::string blockReason(const Source* s);
/// Does PrintWindow give the window's picture (not black)?
bool windowHasImage(void* hwnd);
/// Fractional region → client coordinates of the source window (what the capture reads).
Box clientRegion(void* hwnd, const Frac& frac);

} // namespace live::console

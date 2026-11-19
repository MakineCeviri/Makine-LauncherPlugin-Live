// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Windows string / path helpers.
// The plugin ABI passes UTF-8 strings. Some third-party C APIs (RapidOcrOnnx → OpenCV imread)
// only accept ANSI (CP_ACP) paths, which break for user folders such as C:\Users\Çağrı when the
// name is not representable in the active code page.

#include <string>

#ifdef _WIN32
#include <windows.h>

namespace live::win {

inline std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (n <= 0) {  // not valid UTF-8 → assume the caller used the ANSI code page
        cp = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Converts to the ANSI code page; returns false if any character was lost.
inline bool wideToAnsi(const std::wstring& w, std::string& out)
{
    out.clear();
    if (w.empty()) return true;
    BOOL lossy = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, &lossy);
    if (n <= 0 || lossy) return false;
    out.assign(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return true;
}

// ANSI path usable by narrow-char C APIs. Falls back to the 8.3 short path of an existing
// file/folder when the long path is not representable. Returns empty on failure.
inline std::string ansiPath(const std::wstring& path)
{
    std::string out;
    if (wideToAnsi(path, out)) return out;
    DWORD n = GetShortPathNameW(path.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring shortPath(n, L'\0');
    n = GetShortPathNameW(path.c_str(), shortPath.data(), n);
    shortPath.resize(n);
    if (wideToAnsi(shortPath, out)) return out;
    return {};
}

inline std::wstring withoutTrailingSlash(std::wstring p)
{
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

inline bool isDirectory(const std::wstring& p)
{
    DWORD attr = GetFileAttributesW(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

// Folder containing this DLL.
inline std::wstring moduleDirectory()
{
    static const char anchor = 0;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&anchor), &self))
        return {};
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) { path.resize(n); break; }
        path.resize(path.size() * 2);
    }
    auto sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? std::wstring{} : path.substr(0, sep);
}

inline std::string lastErrorString(DWORD code = GetLastError())
{
    return std::to_string(code);
}

} // namespace live::win
#endif

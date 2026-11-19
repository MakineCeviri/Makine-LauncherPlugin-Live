// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "settings.h"
#include "winutil.h"

#include <filesystem>
#include <fstream>
#include <map>

#ifdef _WIN32
#include <wincrypt.h>
#endif

namespace live {

static std::string escapeValue(const std::string& v)
{
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

static std::string unescapeValue(const std::string& v)
{
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char n = v[++i];
            out += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else {
            out += v[i];
        }
    }
    return out;
}

static const char kDpapiPrefix[] = "dpapi:";

static std::string toHex(const unsigned char* data, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 0xF];
    }
    return s;
}

static bool fromHex(const std::string& s, std::string& out)
{
    if (s.size() % 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>((hi << 4) | lo);
    }
    return true;
}

static std::string protect(const std::string& plain)
{
#ifdef _WIN32
    if (plain.empty()) return {};
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Makine Live", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};
    std::string hex = kDpapiPrefix + toHex(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return hex;
#else
    return plain;
#endif
}

static std::string unprotect(const std::string& stored)
{
#ifdef _WIN32
    if (stored.rfind(kDpapiPrefix, 0) != 0) return stored;  // legacy plain text
    std::string raw;
    if (!fromHex(stored.substr(sizeof(kDpapiPrefix) - 1), raw)) return {};
    DATA_BLOB in{static_cast<DWORD>(raw.size()), reinterpret_cast<BYTE*>(raw.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return {};  // different user/machine — the key has to be entered again
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
#else
    return stored;
#endif
}

static std::filesystem::path fsPath(const std::string& utf8)
{
#ifdef _WIN32
    return std::filesystem::path(win::utf8ToWide(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

bool Settings::isSecret(const std::string& key)
{
    return key == "apiKey" || key == "authToken";
}

void Settings::load(const std::string& utf8Path)
{
    m_path = utf8Path;
    m_values.clear();
    std::ifstream ifs(fsPath(utf8Path), std::ios::binary);
    if (!ifs) return;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = line.substr(0, eq);
        std::string value = unescapeValue(line.substr(eq + 1));
        m_values[key] = isSecret(key) ? unprotect(value) : value;
    }
}

bool Settings::save() const
{
    if (m_path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(fsPath(m_path).parent_path(), ec);

    // Sorted output keeps the file stable between saves.
    std::map<std::string, std::string> sorted(m_values.begin(), m_values.end());
    std::ofstream ofs(fsPath(m_path), std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    for (const auto& [k, v] : sorted) {
        std::string stored = isSecret(k) ? protect(v) : v;
        if (isSecret(k) && !v.empty() && stored.empty()) continue;  // never fall back to plain text
        ofs << k << '=' << escapeValue(stored) << '\n';
    }
    return ofs.good();
}

std::string Settings::get(const std::string& key, const std::string& def) const
{
    auto it = m_values.find(key);
    return it != m_values.end() ? it->second : def;
}

bool Settings::set(const std::string& key, const std::string& value)
{
    auto it = m_values.find(key);
    if (it != m_values.end() && it->second == value) return false;
    m_values[key] = value;
    return true;
}

} // namespace live

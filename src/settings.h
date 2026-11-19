// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

#include <string>
#include <unordered_map>

namespace live {

/**
 * Plugin settings — persisted as `key=value` lines in dataPath/settings.cfg (UTF-8).
 *
 * Values are escaped (\n, \r, \\) so they can't break the line format.
 * Secret keys (apiKey, authToken) are stored encrypted with Windows DPAPI for the current user:
 * `apiKey=dpapi:<hex>`. Plain-text values from older versions are still read and are
 * encrypted on the next save.
 */
class Settings {
public:
    void load(const std::string& utf8Path);
    bool save() const;

    std::string get(const std::string& key, const std::string& def = "") const;
    // Returns true if the value actually changed.
    bool set(const std::string& key, const std::string& value);

private:
    static bool isSecret(const std::string& key);

    std::unordered_map<std::string, std::string> m_values;
    std::string m_path;
};

} // namespace live

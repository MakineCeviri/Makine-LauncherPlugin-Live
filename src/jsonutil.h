// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Reads of JSON that came from outside (a TV, a phone, the server, Discord): nlohmann's
// value(key, default) throws when the key holds another type, and a throw on a helper thread
// ends the plugin host. These never throw.

#include <nlohmann/json.hpp>

#include <string>

namespace live {

inline std::string jstr(const nlohmann::json& j, const char* key, std::string def = {})
{
    if (!j.is_object()) return def;
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : def;
}

inline double jnum(const nlohmann::json& j, const char* key, double def = 0)
{
    if (!j.is_object()) return def;
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : def;
}

} // namespace live

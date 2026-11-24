// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "pool_list.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace live::pools {

namespace {

std::string folded(const std::string& s)   // ASCII case folding is enough to order names
{
    std::string out = s;
    for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

} // namespace

std::string state(const Entry& e)
{
    if (e.downloading) return "downloading";
    if (!e.error.empty()) return "error";
    if (!e.remote) return e.local > 0 ? "current" : "missing";   // not asked yet: what this PC has
    const int remote = *e.remote;
    if (remote == 0 && e.local == 0) return "empty";
    if (e.local == 0) return "missing";
    if (e.localRev.empty() || e.localRev != e.remoteRev) return "outdated";
    // The same revision has the same rows; the count the server gave at that sync, not the lines kept here
    // (a row without a key is never kept), else the pool would look outdated for ever
    if ((e.syncedCount >= 0 ? e.syncedCount : e.local) != remote) return "outdated";
    return "current";
}

bool wanted(const Entry& e)
{
    const std::string s = state(e);
    return s == "outdated" || (s == "missing" && e.remote && *e.remote > 0);
}

void sort(std::vector<Entry>& items)
{
    const auto rank = [](const Entry& e) {
        if (e.playing) return 0;
        const std::string s = state(e);
        return s == "outdated" || s == "missing" || s == "downloading" ? 1 : 2;
    };
    std::stable_sort(items.begin(), items.end(), [&](const Entry& a, const Entry& b) {
        const int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        if (ra == 1) {
            const int na = a.remote.value_or(0), nb = b.remote.value_or(0);
            if (na != nb) return na > nb;
        }
        const std::string fa = folded(a.name.empty() ? a.slug : a.name), fb = folded(b.name.empty() ? b.slug : b.name);
        return fa != fb ? fa < fb : a.slug < b.slug;
    });
}

json itemJson(const Entry& e)
{
    const std::string s = state(e);
    json item = {{"slug", e.slug},
                 {"name", e.name.empty() ? e.slug : e.name},
                 {"local", e.local},
                 {"remote", e.remote ? json(*e.remote) : json(nullptr)},
                 {"state", s},
                 {"playing", e.playing}};
    if (s == "error") item["error"] = e.error;
    return item;
}

json listJson(std::vector<Entry> items)
{
    sort(items);
    if (items.size() > kMaxItems) items.resize(kMaxItems);
    json out = json::array();
    for (const Entry& e : items) out.push_back(itemJson(e));
    return out;
}

bool hasLetters(const std::string& name)
{
    // UTF-8 lead/continuation bytes count as letters: "Ölüm" and "黑神话" are names
    return std::any_of(name.begin(), name.end(), [](char ch) {
        const auto c = static_cast<unsigned char>(ch);
        return std::isalpha(c) || c >= 0x80;
    });
}

} // namespace live::pools

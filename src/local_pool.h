// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Local pool (port of makine_live/net/local_pool.py): a game's APPROVED translations kept
// on this PC, looked up before any request (exact pool key, then the server's ≥90% rule).
// Files: <dataDir>\pool\<slug>.json and pool\games.json (confirmed games). No network here;
// the worker downloads with /api/live/pool?action=sync and hands the answer to applySync.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace live {

int boundedLevenshtein(const std::u32string& a, const std::u32string& b, int limit);
bool keysSimilar(const std::u32string& a, const std::u32string& b);

struct PoolHit {
    int64_t id = 0;
    std::string translation;
    bool exact = false;
};

class GamePool {
public:
    GamePool(std::string slug, std::string name, std::string rev, const nlohmann::json& items, double syncedAt);

    std::optional<PoolHit> lookup(const std::string& text) const;
    nlohmann::json toJson() const;
    size_t size() const { return m_items.size(); }

    std::string slug, name, rev;
    double syncedAt = 0;

private:
    friend class LocalPool;
    struct Item {
        std::u32string key;
        std::string source, translation;
    };
    std::map<int64_t, Item> m_items;
    std::unordered_map<std::u32string, int64_t> m_byKey;
    std::unordered_map<size_t, std::vector<int64_t>> m_byLen;
    // Similar-line results, "not found" included: OCR reads the same line again and again (0.3.2)
    mutable std::mutex m_fuzzyMutex;
    mutable std::unordered_map<std::u32string, std::optional<int64_t>> m_fuzzy;
};

struct SyncStats {
    int added = 0, changed = 0, removed = 0, total = 0;
    bool unchanged = false;
};

class LocalPool {
public:
    explicit LocalPool(std::wstring dataDir);

    bool isConfirmed(const std::string& slug) const;
    void confirm(const std::string& slug, const std::string& name);
    void forget(const std::string& slug);

    std::shared_ptr<GamePool> get(const std::string& slug);
    std::string rev(const std::string& slug);
    SyncStats applySync(const std::string& slug, const nlohmann::json& resp);
    std::optional<PoolHit> lookup(const std::string& slug, const std::string& text);
    std::map<int64_t, int> takeHits(const std::string& slug);
    void returnHits(const std::string& slug, const std::map<int64_t, int>& hits);

private:
    std::wstring file(const std::string& slug) const;
    void writeGames(const nlohmann::json& games) const;

    std::wstring m_dir;
    mutable std::mutex m_mutex;
    nlohmann::json m_games;   // {"confirmed": {slug: {name, t}}}
    std::map<std::string, std::shared_ptr<GamePool>> m_pools;
    std::map<std::string, std::map<int64_t, int>> m_hits;
};

} // namespace live

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// The Live screen's game pools ("Oyun havuzları", ui.state pools): per game what this PC has, what the
// server has, the state that follows and the order they are listed in. Pure: the engine gathers the
// facts (launcher library, LocalPool, head syncs), this decides. README "Launcher screen API" → pools.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace live::pools {

constexpr size_t kMaxItems = 200;

struct Entry {
    std::string slug, name;
    int local = 0;                 // approved lines on this PC
    std::string localRev;          // the revision they came with ("" = none, or a file from before variants=1)
    int syncedCount = -1;          // the server's count at that sync (-1 = unknown)
    std::optional<int> remote;     // the server's count now (head sync); nullopt = not asked yet
    std::string remoteRev;
    bool downloading = false, playing = false;
    std::string error;             // the last head sync or download failed (Turkish, for the screen)
};

/// "current" | "outdated" | "missing" | "downloading" | "error" | "empty"
std::string state(const Entry& e);
/// Worth a download: missing with a known server count, or outdated.
bool wanted(const Entry& e);
/// Playing first, then outdated / missing (and downloading) by server count, most first, then the rest by name.
void sort(std::vector<Entry>& items);
/// One contract item: {slug, name, local, remote, state, playing[, error]}
nlohmann::json itemJson(const Entry& e);
/// Sorted and capped at kMaxItems.
nlohmann::json listJson(std::vector<Entry> items);
/// A pool needs a name with at least one letter (an exe stem of digits is no game).
bool hasLetters(const std::string& name);

} // namespace live::pools

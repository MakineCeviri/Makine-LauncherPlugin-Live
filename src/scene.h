// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Scene memory, the app's side (Makine-WEB src/live/scene.ts, docs/live-scene-context.md; vectors
// spec/scene_vectors.json). Players see lines in different orders and subsets, so the line before is never
// part of a key: it is only a DeepL context hint the server may use, and the speaker / listener pick a
// stable variant ("f:sen" / "f:siz" from an address rule, "scene:" / "line:" from a script dump).

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace live::text {
class Garbage;
}

namespace live::scene {

constexpr double kContextFreshMs = 8000;   // the line before is context only this long after it was shown
constexpr size_t kMaxContextLine = 200;    // characters; a longer line is dropped, not cut
constexpr size_t kMaxContext = 3;

struct SceneLine {
    std::string line, speaker;
};

struct Scene {
    std::optional<SceneLine> prev;   // the line just before, fresh and from this conversation
    std::string speaker;             // who speaks this line: its label, else the speaker sent
    std::string prevSpeaker;         // who spoke the fresh line before: the likely listener
    std::string lastForm;            // how this speaker addressed someone a line ago: "sen" | "siz" | ""
    std::string sceneId, scriptLineId;
};

struct Relation {   // an address rule of the game's glossary (pool sync "relations")
    std::string speakerKey, listenerKey, form;
};

/// Client text as plain text: no control, line-separator or direction characters, no markup, spaces collapsed.
std::string plain(std::string_view text);
/// A speaker as the rules key it: letters and digits, lowercased ("Officer Diaz" = "officerdiaz").
std::string speakerKey(std::string_view name);
/// Three words or fewer, only pronouns, or a question under five words: the meaning hangs on the line before.
bool isContextDependent(std::string_view line);
/// How a Turkish line addresses its listener: "sen", "siz", or "" (neither, or both).
std::string addressForm(std::string_view turkish);
/// The scene of a request ({speaker?, scene_id?, script_line_id?, context: [{text, speaker?, translated?, age_ms,
/// scene_id?}]}), read as the server reads it.
Scene readScene(const nlohmann::json& inp, const std::string& labelSpeaker, const text::Garbage& garbage);
std::string contextText(const SceneLine& prev);
/// The speaker of the line before, when someone else said it.
std::string listenerOf(const Scene& scene);
/// The address rule for this speaker and listener, else the speaker's rule for anyone ("*").
std::string ruleForm(const std::vector<Relation>& rules, const std::string& speaker, const std::string& listener);
/// The stable signals of a request, most specific first: line:<id>, scene:<id>, f:<form>. Never ctx:.
std::vector<std::string> variantSignals(const Scene& scene, const std::string& form);

} // namespace live::scene

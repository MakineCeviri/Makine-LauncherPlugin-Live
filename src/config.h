// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

#include <string>

namespace live {

/// Screen rectangle in physical pixels of the Windows virtual desktop — the space
/// of DXGI Desktop Duplication, BitBlt on the screen DC and every overlay.* call.
struct Region {
    int x = 0, y = 0, width = 0, height = 0;

    bool valid() const { return width > 0 && height > 0; }
    bool operator==(const Region&) const = default;
};

/// The plugin's settings (manifest.json "contributes.settings"). The worker reads
/// an immutable snapshot; a change swaps in a new one.
struct Config {
    // openai | groq | openrouter | ollama | lmstudio | compatible | deepl | google
    std::string provider = "openai";
    std::string apiKey;
    std::string model;
    std::string baseUrl;          // Custom OpenAI-compatible endpoint; blank by default.
    std::string sourceLang = "en";
    std::string targetLang = "tr";
    int intervalMs = 1000;
    bool showSource = false;
    bool autoStart = false;

    /// Same translation for the same text: the cache stays valid.
    bool sameTranslation(const Config& o) const
    {
        return provider == o.provider && apiKey == o.apiKey && model == o.model
               && baseUrl == o.baseUrl && sourceLang == o.sourceLang && targetLang == o.targetLang;
    }
};

} // namespace live

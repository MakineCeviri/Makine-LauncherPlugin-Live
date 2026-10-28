// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

#include "config.h"

#include <windows.h>
#include <winhttp.h>

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace live {

/// Earlier (source, translation) pairs, oldest first; OpenAI gets them as context.
using History = std::deque<std::pair<std::string, std::string>>;

/// OpenAI-compatible providers, DeepL and Google Cloud Translation over WinHTTP.
/// translate() runs on the worker thread; cancel() may be called from any thread.
class Translator {
public:
    Translator();
    ~Translator();
    Translator(const Translator&) = delete;
    Translator& operator=(const Translator&) = delete;

    /// The translation, or nullopt with a message for the user in `error`.
    std::optional<std::string> translate(const Config& cfg, const std::string& text, const History& history,
                                         std::string& error);

    /// Aborts the request in flight and refuses new ones.
    void cancel();

private:
    struct Response {
        DWORD status = 0;
        std::string body;
    };
    std::optional<Response> post(const std::wstring& url, const std::wstring& headers,
                                 const std::string& body, std::string& error);

    HINTERNET m_session = nullptr;
    std::mutex m_mutex;           // guards the two below
    HINTERNET m_request = nullptr;
    bool m_cancelled = false;
};

} // namespace live

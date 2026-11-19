// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace live {

struct TranslateResult {
    std::string translated;
    std::string error;
    bool success = false;
};

enum class TranslatorEngine {
    Makine,   // Makine Live server (makineceviri.org/api/live): pool + server-side DeepL, account token instead of an API key
    GPT,
    DeepL,
    Google,
    None,
};

/**
 * Translation via HTTP — uses WinHTTP (native Windows, no Qt).
 * Engines: Makine Live server (default; POST {server}/api/live/translate with the user's app token —
 * the server answers from the shared pool or translates once with its own DeepL key), and direct
 * OpenAI GPT / DeepL / Google with a user API key (developer use).
 */
class Translator {
public:
    ~Translator();

    void setEngine(TranslatorEngine engine) { m_engine = engine; }
    void setApiKey(const std::string& key) { m_apiKey = key; }
    void setSourceLang(const std::string& lang) { m_srcLang = lang; }
    void setTargetLang(const std::string& lang) { m_tgtLang = lang; }
    void setModel(const std::string& model) { m_model = model; }
    void setContext(const std::string& ctx) { m_context = ctx; }
    // Makine engine: server base URL ("https://makineceviri.org", "http://127.0.0.1:8899"), app token, game slug
    void setServer(const std::string& url) { m_serverUrl = url; }
    void setAuthToken(const std::string& token) { m_authToken = token; }
    void setGame(const std::string& slug, const std::string& name) { m_gameSlug = slug; m_gameName = name; }

    // Credentials for the selected engine are present (token for Makine, API key otherwise)
    bool ready() const { return m_engine == TranslatorEngine::Makine ? !m_authToken.empty() : !m_apiKey.empty(); }
    // Last Makine response: "pool" or "deepl" (empty for other engines)
    const std::string& lastOrigin() const { return m_lastOrigin; }

    TranslateResult translate(const std::string& text);

    TranslatorEngine engine() const { return m_engine; }
    std::string lastError() const { return m_error; }

private:
    TranslateResult translateGPT(const std::string& text);
    TranslateResult translateDeepL(const std::string& text);
    TranslateResult translateGoogle(const std::string& text);
    TranslateResult translateMakine(const std::string& text);

    // Persistent WinHTTP session — reused across calls (~100ms saved per request).
    // Returns false (and sets m_error) on network errors, timeouts and non-2xx responses.
    bool httpPost(const std::string& host, const std::string& path,
                  const std::string& body, const std::string& authHeader,
                  std::string& response, const std::string& extraHeader = "",
                  int port = 443, bool secure = true);
    void ensureSession();
    void closeConnection();
    void closeSession();

    TranslatorEngine m_engine = TranslatorEngine::Makine;
    std::string m_apiKey;
    std::string m_srcLang = "en";
    std::string m_tgtLang = "tr";
    std::string m_model = "gpt-4o-mini";
    std::string m_error;
    std::string m_context;  // Translation context for GPT (game terminology consistency)
    std::string m_serverUrl = "https://makineceviri.org";
    std::string m_authToken;
    std::string m_gameSlug;
    std::string m_gameName;
    std::string m_lastOrigin;
    long m_lastStatus = 0;

#ifdef _WIN32
    HINTERNET m_hSession = nullptr;  // Persistent session handle
    HINTERNET m_hConnect = nullptr;  // Persistent connection handle
    std::string m_lastHost;          // Track host:port for connection reuse
#endif
};

} // namespace live

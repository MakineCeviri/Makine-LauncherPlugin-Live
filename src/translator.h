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
    GPT,
    DeepL,
    Google,
    None,
};

/**
 * Translation via HTTP APIs — uses WinHTTP (native Windows, no Qt).
 * Supports: OpenAI GPT, DeepL, Google Cloud Translation.
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

    TranslateResult translate(const std::string& text);

    TranslatorEngine engine() const { return m_engine; }
    std::string lastError() const { return m_error; }

private:
    TranslateResult translateGPT(const std::string& text);
    TranslateResult translateDeepL(const std::string& text);
    TranslateResult translateGoogle(const std::string& text);

    // Persistent WinHTTP session — reused across calls (~100ms saved per request)
    std::string httpPost(const std::string& host, const std::string& path,
                         const std::string& body, const std::string& authHeader,
                         const std::string& contentType = "application/json");
    void ensureSession();
    void closeSession();

    TranslatorEngine m_engine = TranslatorEngine::GPT;
    std::string m_apiKey;
    std::string m_srcLang = "en";
    std::string m_tgtLang = "tr";
    std::string m_model = "gpt-4o-mini";
    std::string m_error;
    std::string m_context;  // Translation context for GPT (game terminology consistency)

#ifdef _WIN32
    HINTERNET m_hSession = nullptr;  // Persistent session handle
    HINTERNET m_hConnect = nullptr;  // Persistent connection handle
    std::string m_lastHost;          // Track host for connection reuse
#endif
};

} // namespace live

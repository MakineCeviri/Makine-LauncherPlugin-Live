#include "translator.h"
#include <sstream>

#ifdef _WIN32
#pragma comment(lib, "winhttp.lib")
#endif

namespace live {

// Simple JSON string escaper
static std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 10);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c;
        }
    }
    return out;
}

// Extract value from JSON by key (simple, no nested objects)
static std::string jsonValue(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";

    // Skip whitespace
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        // String value
        pos++;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                pos++;
                switch (json[pos]) {
                    case 'n': val += '\n'; break;
                    case 't': val += '\t'; break;
                    case '"': val += '"'; break;
                    case '\\': val += '\\'; break;
                    default: val += json[pos];
                }
            } else {
                val += json[pos];
            }
            pos++;
        }
        return val;
    }

    // Non-string value
    std::string val;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']')
        val += json[pos++];
    return val;
}

TranslateResult Translator::translate(const std::string& text)
{
    if (text.empty()) return {"", "", true};
    if (m_apiKey.empty()) return {"", "API key not set", false};

    switch (m_engine) {
        case TranslatorEngine::GPT:    return translateGPT(text);
        case TranslatorEngine::DeepL:  return translateDeepL(text);
        case TranslatorEngine::Google: return translateGoogle(text);
        default: return {"", "No translator engine selected", false};
    }
}

TranslateResult Translator::translateGPT(const std::string& text)
{
    // Context-aware prompt — includes previous translations for game terminology consistency
    std::string systemPrompt = "You are a game text translator. Translate from "
                               + m_srcLang + " to " + m_tgtLang
                               + ". Return ONLY the translated text, nothing else."
                               " Be consistent with game terminology.";
    if (!m_context.empty())
        systemPrompt += " Previous translations for context:\n" + m_context;

    std::string body = "{\"model\":\"" + m_model + "\","
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"" + jsonEscape(systemPrompt) + "\"},"
        "{\"role\":\"user\",\"content\":\"" + jsonEscape(text) + "\"}"
        "],\"temperature\":0.2,\"max_tokens\":512}";

    std::string response = httpPost(
        "api.openai.com", "/v1/chat/completions",
        body, "Bearer " + m_apiKey);

    if (response.empty())
        return {"", m_error, false};

    // Parse response — find content field in choices[0].message.content
    std::string content = jsonValue(response, "content");
    if (content.empty())
        return {"", "Failed to parse GPT response", false};

    return {content, "", true};
}

TranslateResult Translator::translateDeepL(const std::string& text)
{
    std::string body = "{\"text\":[\"" + jsonEscape(text) + "\"],"
        "\"source_lang\":\"" + m_srcLang + "\","
        "\"target_lang\":\"" + m_tgtLang + "\"}";

    // DeepL free API uses api-free.deepl.com
    std::string host = "api-free.deepl.com";
    if (m_apiKey.find(":fx") == std::string::npos)
        host = "api.deepl.com"; // Pro API

    std::string response = httpPost(
        host, "/v2/translate",
        body, "DeepL-Auth-Key " + m_apiKey);

    if (response.empty())
        return {"", m_error, false};

    std::string translated = jsonValue(response, "text");
    if (translated.empty())
        return {"", "Failed to parse DeepL response", false};

    return {translated, "", true};
}

TranslateResult Translator::translateGoogle(const std::string& text)
{
    std::string path = "/language/translate/v2?key=" + m_apiKey;

    std::string body = "{\"q\":\"" + jsonEscape(text) + "\","
        "\"source\":\"" + m_srcLang + "\","
        "\"target\":\"" + m_tgtLang + "\","
        "\"format\":\"text\"}";

    // Google Cloud Translation API — key is in URL, no auth header needed
    std::string response = httpPost(
        "translation.googleapis.com", path,
        body, "");

    if (response.empty())
        return {"", m_error, false};

    std::string translated = jsonValue(response, "translatedText");
    if (translated.empty())
        return {"", "Failed to parse Google Translate response", false};

    return {translated, "", true};
}

// -- Persistent WinHTTP Session --

Translator::~Translator() { closeSession(); }

void Translator::ensureSession()
{
#ifdef _WIN32
    if (!m_hSession) {
        m_hSession = WinHttpOpen(L"MakineAI-Live/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    }
#endif
}

void Translator::closeSession()
{
#ifdef _WIN32
    if (m_hConnect) { WinHttpCloseHandle(m_hConnect); m_hConnect = nullptr; }
    if (m_hSession) { WinHttpCloseHandle(m_hSession); m_hSession = nullptr; }
    m_lastHost.clear();
#endif
}

// -- WinHTTP POST (persistent session, ~100ms faster per call) --

std::string Translator::httpPost(
    const std::string& host, const std::string& path,
    const std::string& body, const std::string& authHeader,
    const std::string& contentType)
{
#ifdef _WIN32
    ensureSession();
    if (!m_hSession) { m_error = "WinHttpOpen failed"; return ""; }

    // Reuse connection if same host, otherwise reconnect
    if (host != m_lastHost) {
        if (m_hConnect) WinHttpCloseHandle(m_hConnect);
        std::wstring wHost(host.begin(), host.end());
        m_hConnect = WinHttpConnect(m_hSession, wHost.c_str(),
            INTERNET_DEFAULT_HTTPS_PORT, 0);
        m_lastHost = host;
    }
    if (!m_hConnect) { m_error = "WinHttpConnect failed"; return ""; }

    std::wstring wPath(path.begin(), path.end());
    HINTERNET hRequest = WinHttpOpenRequest(m_hConnect, L"POST", wPath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { m_error = "WinHttpOpenRequest failed"; return ""; }

    // Headers — Authorization is conditional (Google uses key in URL)
    std::string headers = "Content-Type: " + contentType + "\r\n";
    if (!authHeader.empty())
        headers += "Authorization: " + authHeader + "\r\n";
    std::wstring wHeaders(headers.begin(), headers.end());
    WinHttpAddRequestHeaders(hRequest, wHeaders.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        const_cast<char*>(body.c_str()), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);

    if (!ok || !WinHttpReceiveResponse(hRequest, nullptr)) {
        m_error = "HTTP request failed (error " + std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(hRequest);
        return "";
    }

    // Read response into pre-reserved buffer
    std::string response;
    response.reserve(2048);
    DWORD bytesRead = 0;
    char buf[4096];
    while (WinHttpReadData(hRequest, buf, sizeof(buf), &bytesRead) && bytesRead > 0)
        response.append(buf, bytesRead);

    WinHttpCloseHandle(hRequest);
    return response;
#else
    (void)host; (void)path; (void)body; (void)authHeader; (void)contentType;
    m_error = "HTTP not supported on this platform";
    return "";
#endif
}

} // namespace live

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "webos_install.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace live::webos {

namespace {

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

void winsock()
{
    static const bool ready = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)ready;
}

void sshLibrary()
{
    static const int rc = libssh2_init(0);   // once, before any session (not thread-safe itself)
    (void)rc;
}

std::string trim(std::string s)
{
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && space(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && space(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

/// The first n bytes, not cutting a UTF-8 character.
std::string head(const std::string& s, size_t n)
{
    if (s.size() <= n) return s;
    while (n > 0 && continuation(s[n])) --n;
    return s.substr(0, n);
}

/// The last n bytes, not cutting a UTF-8 character.
std::string tail(const std::string& s, size_t n)
{
    if (s.size() <= n) return s;
    size_t i = s.size() - n;
    while (i < s.size() && continuation(s[i])) ++i;
    return s.substr(i);
}

void wipe(std::string& s)
{
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}

void wipe(std::vector<uint8_t>& v)
{
    if (!v.empty()) SecureZeroMemory(v.data(), v.size());
    v.clear();
}

// ── network ─────────────────────────────────────────────────────────────────

SOCKET connectTcp(const std::string& host, int port, int timeoutMs, std::string& error)
{
    winsock();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        error = "adres çözülemedi";
        return INVALID_SOCKET;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        error = "soket açılamadı";
        return INVALID_SOCKET;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    const int ready = select(0, nullptr, &w, &e, &tv);
    if (ready != 1 || FD_ISSET(s, &e)) {
        error = ready == 0 ? "yanıt yok (zaman aşımı)" : "bağlantı reddedildi (" + std::to_string(port) + ". kapı kapalı)";
        closesocket(s);
        return INVALID_SOCKET;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

bool httpGet(const std::string& host, int port, const std::string& path, int timeoutMs, std::string& body,
             std::string& error)
{
    const SOCKET s = connectTcp(host, port, timeoutMs, error);
    if (s == INVALID_SOCKET) return false;
    const DWORD t = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    const std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + ":" + std::to_string(port)
                            + "\r\nConnection: close\r\n\r\n";
    bool ok = ::send(s, req.data(), static_cast<int>(req.size()), 0) == static_cast<int>(req.size());
    std::string resp;
    char buf[4096];
    while (ok && resp.size() < 256 * 1024) {
        const int n = ::recv(s, buf, sizeof buf, 0);
        if (n == 0) break;
        if (n < 0) {
            ok = false;
            break;
        }
        resp.append(buf, static_cast<size_t>(n));
    }
    closesocket(s);
    const size_t split = resp.find("\r\n\r\n");
    if (!ok || split == std::string::npos || resp.compare(0, 5, "HTTP/") != 0) {
        error = "anahtar sunucusu yanıt vermedi";
        return false;
    }
    const size_t sp = resp.find(' ');
    const std::string status = sp == std::string::npos ? std::string() : resp.substr(sp + 1, 3);
    if (status != "200") {
        error = "anahtar sunucusu HTTP " + status + " döndü";
        return false;
    }
    body = resp.substr(split + 4);
    wipe(resp);
    return true;
}

// ── the developer key: traditional PEM, decrypted with CNG ───────────────────

std::vector<uint8_t> base64Decode(std::string_view s)
{
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else continue;   // line breaks
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return out;
}

std::string base64Lines(const std::vector<uint8_t>& in)
{
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string flat;
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t n = (uint32_t(in[i]) << 16) | (i + 1 < in.size() ? uint32_t(in[i + 1]) << 8 : 0)
                           | (i + 2 < in.size() ? uint32_t(in[i + 2]) : 0);
        flat += abc[(n >> 18) & 63];
        flat += abc[(n >> 12) & 63];
        flat += i + 1 < in.size() ? abc[(n >> 6) & 63] : '=';
        flat += i + 2 < in.size() ? abc[n & 63] : '=';
    }
    std::string out;
    for (size_t i = 0; i < flat.size(); i += 64) out += flat.substr(i, 64) + "\n";
    wipe(flat);
    return out;
}

std::vector<uint8_t> md5(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> out(16);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0))
              && BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0))
              && BCRYPT_SUCCESS(BCryptHashData(h, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0))
              && BCRYPT_SUCCESS(BCryptFinishHash(h, out.data(), static_cast<ULONG>(out.size()), 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) throw std::runtime_error("MD5 kullanılamadı");
    return out;
}

/// OpenSSL's EVP_BytesToKey(MD5, count 1): the key of a traditional encrypted PEM.
std::vector<uint8_t> pemKey(const std::string& pass, const std::vector<uint8_t>& salt, size_t len)
{
    std::vector<uint8_t> key, d;
    while (key.size() < len) {
        std::vector<uint8_t> in = d;
        in.insert(in.end(), pass.begin(), pass.end());
        in.insert(in.end(), salt.begin(), salt.end());
        d = md5(in);
        wipe(in);
        key.insert(key.end(), d.begin(), d.end());
    }
    wipe(d);
    key.resize(len);
    return key;
}

bool cbcDecrypt(LPCWSTR algorithm, const std::vector<uint8_t>& key, const std::vector<uint8_t>& iv,
                const std::vector<uint8_t>& data, std::vector<uint8_t>& out)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    ULONG n = 0;
    std::vector<uint8_t> chain = iv;
    out.assign(data.size(), 0);
    const bool ok =
        BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0))
        && BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                                            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                                            sizeof(BCRYPT_CHAIN_MODE_CBC), 0))
        && BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                                     static_cast<ULONG>(key.size()), 0))
        && BCRYPT_SUCCESS(BCryptDecrypt(k, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), nullptr,
                                        chain.data(), static_cast<ULONG>(chain.size()), out.data(),
                                        static_cast<ULONG>(out.size()), &n, BCRYPT_BLOCK_PADDING));
    if (k) BCryptDestroyKey(k);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    out.resize(ok ? n : 0);
    return ok;
}

bool derLength(const std::vector<uint8_t>& d, size_t& pos, size_t& len)
{
    if (pos >= d.size()) return false;
    const uint8_t first = d[pos++];
    if (first < 0x80) {
        len = first;
        return true;
    }
    const int count = first & 0x7F;
    if (count == 0 || count > 4) return false;
    len = 0;
    for (int i = 0; i < count; ++i) {
        if (pos >= d.size()) return false;
        len = (len << 8) | d[pos++];
    }
    return true;
}

/// PKCS#1 RSAPrivateKey: SEQUENCE of 9 INTEGERs that fills the buffer exactly. A wrong passphrase
/// leaves garbage that fails this (or the CBC padding before it).
bool isRsaPrivateKey(const std::vector<uint8_t>& d)
{
    size_t pos = 0, len = 0;
    if (d.empty() || d[pos++] != 0x30 || !derLength(d, pos, len) || pos + len != d.size()) return false;
    for (int i = 0; i < 9; ++i) {
        if (pos >= d.size() || d[pos++] != 0x02 || !derLength(d, pos, len) || pos + len > d.size()) return false;
        pos += len;
    }
    return pos == d.size();
}

std::vector<uint8_t> hexDecode(const std::string& s)
{
    std::vector<uint8_t> out;
    const auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0) return {};
        out.push_back(static_cast<uint8_t>(a * 16 + b));
    }
    return out;
}

// ── .ipk version: ar → control.tar.gz (inflate) → control ─────────────────────

class Inflater {
public:
    Inflater(const uint8_t* in, size_t n) : m_in(in), m_n(n) {}

    std::string run(size_t limit)
    {
        m_limit = limit;
        int last = 0;
        while (!last) {
            last = bits(1);
            const int type = bits(2);
            if (type == 0) stored();
            else if (type == 1) fixed();
            else if (type == 2) dynamic();
            else throw std::runtime_error("deflate");
        }
        return m_out;
    }

private:
    struct Huffman {
        std::array<short, 16> count{};
        std::array<short, 320> symbol{};
    };

    int bits(int need)
    {
        uint32_t val = m_bitBuf;
        while (m_bitCnt < need) {
            if (m_pos >= m_n) throw std::runtime_error("deflate");
            val |= uint32_t(m_in[m_pos++]) << m_bitCnt;
            m_bitCnt += 8;
        }
        m_bitBuf = val >> need;
        m_bitCnt -= need;
        return static_cast<int>(val & ((1u << need) - 1));
    }

    void put(char c)
    {
        if (m_out.size() >= m_limit) throw std::runtime_error("deflate");
        m_out.push_back(c);
    }

    void stored()
    {
        m_bitBuf = 0;
        m_bitCnt = 0;
        if (m_pos + 4 > m_n) throw std::runtime_error("deflate");
        const unsigned len = m_in[m_pos] | (m_in[m_pos + 1] << 8);
        const unsigned nlen = m_in[m_pos + 2] | (m_in[m_pos + 3] << 8);
        m_pos += 4;
        if (len != (~nlen & 0xFFFFu) || m_pos + len > m_n) throw std::runtime_error("deflate");
        for (unsigned i = 0; i < len; ++i) put(static_cast<char>(m_in[m_pos + i]));
        m_pos += len;
    }

    static void build(Huffman& h, const short* lengths, int n)
    {
        h.count.fill(0);
        for (int i = 0; i < n; ++i) h.count[static_cast<size_t>(lengths[i])]++;
        std::array<short, 16> offs{};
        for (int len = 1; len < 15; ++len) offs[static_cast<size_t>(len + 1)] = static_cast<short>(offs[static_cast<size_t>(len)] + h.count[static_cast<size_t>(len)]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) h.symbol[static_cast<size_t>(offs[static_cast<size_t>(lengths[i])]++)] = static_cast<short>(i);
    }

    int decode(const Huffman& h)
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= bits(1);
            const int count = h.count[static_cast<size_t>(len)];
            if (code - count < first) return h.symbol[static_cast<size_t>(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        throw std::runtime_error("deflate");
    }

    void codes(const Huffman& lens, const Huffman& dists)
    {
        static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = decode(lens);
            if (sym < 256) {
                put(static_cast<char>(sym));
                continue;
            }
            if (sym == 256) return;
            sym -= 257;
            if (sym >= 29) throw std::runtime_error("deflate");
            const int len = lbase[sym] + bits(lext[sym]);
            const int d = decode(dists);
            if (d >= 30) throw std::runtime_error("deflate");
            const size_t dist = static_cast<size_t>(dbase[d] + bits(dext[d]));
            if (dist > m_out.size()) throw std::runtime_error("deflate");
            for (int i = 0; i < len; ++i) put(m_out[m_out.size() - dist]);
        }
    }

    void fixed()
    {
        short l[288];
        int i = 0;
        for (; i < 144; ++i) l[i] = 8;
        for (; i < 256; ++i) l[i] = 9;
        for (; i < 280; ++i) l[i] = 7;
        for (; i < 288; ++i) l[i] = 8;
        Huffman lens, dists;
        build(lens, l, 288);
        short d[30];
        std::fill(std::begin(d), std::end(d), short(5));
        build(dists, d, 30);
        codes(lens, dists);
    }

    void dynamic()
    {
        static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (nlen > 286 || ndist > 30) throw std::runtime_error("deflate");
        short lengths[320] = {};
        for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<short>(bits(3));
        Huffman lencode, distcode;
        build(lencode, lengths, 19);
        int index = 0;
        while (index < nlen + ndist) {
            int sym = decode(lencode);
            if (sym < 16) {
                lengths[index++] = static_cast<short>(sym);
                continue;
            }
            short len = 0;
            if (sym == 16) {
                if (index == 0) throw std::runtime_error("deflate");
                len = lengths[index - 1];
                sym = 3 + bits(2);
            } else if (sym == 17) {
                sym = 3 + bits(3);
            } else {
                sym = 11 + bits(7);
            }
            if (index + sym > nlen + ndist) throw std::runtime_error("deflate");
            while (sym--) lengths[index++] = len;
        }
        build(lencode, lengths, nlen);
        build(distcode, lengths + nlen, ndist);
        codes(lencode, distcode);
    }

    const uint8_t* m_in;
    size_t m_n, m_pos = 0, m_limit = 0;
    uint32_t m_bitBuf = 0;
    int m_bitCnt = 0;
    std::string m_out;
};

std::string gunzip(const uint8_t* p, size_t n, size_t limit)
{
    if (n < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) throw std::runtime_error("gzip");
    const uint8_t flags = p[3];
    size_t pos = 10;
    if (flags & 4) {
        if (pos + 2 > n) throw std::runtime_error("gzip");
        pos += 2 + (p[pos] | (p[pos + 1] << 8));
    }
    if (flags & 8)
        while (pos < n && p[pos++]) {}
    if (flags & 16)
        while (pos < n && p[pos++]) {}
    if (flags & 2) pos += 2;
    if (pos >= n) throw std::runtime_error("gzip");
    return Inflater(p + pos, n - pos).run(limit);
}

std::string tarFile(const std::string& tar, const std::string& want)
{
    for (size_t pos = 0; pos + 512 <= tar.size();) {
        const std::string name(tar.c_str() + pos, strnlen(tar.c_str() + pos, 100));
        if (name.empty()) break;
        const size_t size = std::strtoull(std::string(tar, pos + 124, 12).c_str(), nullptr, 8);
        std::string clean = name;
        while (clean.starts_with("./")) clean.erase(0, 2);
        if (clean == want && pos + 512 + size <= tar.size()) return tar.substr(pos + 512, size);
        pos += 512 + (size + 511) / 512 * 512;
    }
    return {};
}

// ── SSH: libssh2 in non-blocking mode, every call with its own deadline ────────

class Ssh {
public:
    ~Ssh()
    {
        if (m_session) {
            libssh2_session_set_timeout(m_session, 2000);
            libssh2_session_set_blocking(m_session, 1);
            libssh2_session_disconnect(m_session, "Makine Live");
            libssh2_session_free(m_session);
        }
        if (m_s != INVALID_SOCKET) closesocket(m_s);
    }

    bool connect(const std::string& host, int port, std::string& error)
    {
        m_s = connectTcp(host, port, 10000, error);
        if (m_s == INVALID_SOCKET) return false;
        m_session = libssh2_session_init();
        if (!m_session) {
            error = "SSH oturumu açılamadı";
            return false;
        }
        libssh2_session_set_blocking(m_session, 0);
        return true;
    }

    LIBSSH2_SESSION* session() const { return m_session; }
    SOCKET sock() const { return m_s; }

    std::string lastError() const
    {
        char* msg = nullptr;
        int len = 0;
        if (m_session) libssh2_session_last_error(m_session, &msg, &len, 0);
        return msg && len > 0 ? std::string(msg, static_cast<size_t>(len)) : std::string("bilinmeyen SSH hatası");
    }

    /// Waits for the socket in the direction libssh2 asked for; false once the deadline passed.
    bool wait(Clock::time_point deadline, int capMs = 250) const
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) return false;
        const long ms = static_cast<long>(std::min<long long>(left, capMs));
        const int dir = libssh2_session_block_directions(m_session);
        fd_set r, w;
        FD_ZERO(&r);
        FD_ZERO(&w);
        if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) FD_SET(m_s, &r);
        if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(m_s, &w);
        if (!(dir & (LIBSSH2_SESSION_BLOCK_INBOUND | LIBSSH2_SESSION_BLOCK_OUTBOUND))) FD_SET(m_s, &r);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        select(0, &r, &w, nullptr, &tv);
        return true;
    }

    template <class F>
    int call(F f, int timeoutMs)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const int rc = static_cast<int>(f());
            if (rc != LIBSSH2_ERROR_EAGAIN) return rc;
            if (!wait(deadline)) return LIBSSH2_ERROR_TIMEOUT;
        }
    }

    template <class F>
    auto callPtr(F f, int timeoutMs) -> decltype(f())
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            if (auto p = f()) return p;
            if (libssh2_session_last_errno(m_session) != LIBSSH2_ERROR_EAGAIN || !wait(deadline)) return nullptr;
        }
    }

    /// A command channel, stderr merged into stdout. No pty: LG's sshd closes the channel on a pty request.
    LIBSSH2_CHANNEL* exec(const std::string& cmd)
    {
        LIBSSH2_CHANNEL* ch = callPtr([&] { return libssh2_channel_open_session(m_session); }, 10000);
        if (!ch) return nullptr;
        call([&] { return libssh2_channel_handle_extended_data2(ch, LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE); }, 10000);
        if (call([&] { return libssh2_channel_exec(ch, cmd.c_str()); }, 10000) != 0) {
            release(ch);
            return nullptr;
        }
        return ch;
    }

    void release(LIBSSH2_CHANNEL* ch)
    {
        call([&] { return libssh2_channel_close(ch); }, 3000);
        call([&] { return libssh2_channel_free(ch); }, 3000);
    }

    /// What the channel has now. 1: read something, 0: nothing yet, -1: end of output (or a broken channel).
    int readSome(LIBSSH2_CHANNEL* ch, std::string& out)
    {
        char buf[8192];
        bool got = false;
        for (;;) {
            const auto n = libssh2_channel_read(ch, buf, sizeof buf);
            if (n > 0) {
                out.append(buf, static_cast<size_t>(n));
                got = true;
                continue;
            }
            if (n == LIBSSH2_ERROR_EAGAIN) return got ? 1 : 0;
            if (n == 0 && !libssh2_channel_eof(ch)) return got ? 1 : 0;
            return -1;
        }
    }

    /// Runs a command to its end (a connection closed early leaves it half-done on the TV); its output.
    std::string run(const std::string& cmd, int timeoutMs)
    {
        LIBSSH2_CHANNEL* ch = exec(cmd);
        if (!ch) return {};
        std::string out;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (readSome(ch, out) >= 0)
            if (!wait(deadline)) break;
        release(ch);
        return out;
    }

    bool sftpPut(const std::vector<uint8_t>& data, const std::string& path, int timeoutMs, std::string& error)
    {
        LIBSSH2_SFTP* sftp = callPtr([&] { return libssh2_sftp_init(m_session); }, 10000);
        if (!sftp) {
            error = lastError();
            return false;
        }
        LIBSSH2_SFTP_HANDLE* h = callPtr(
            [&] {
                return libssh2_sftp_open_ex(sftp, path.c_str(), static_cast<unsigned int>(path.size()),
                                            LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0644,
                                            LIBSSH2_SFTP_OPENFILE);
            },
            10000);
        bool ok = h != nullptr;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        size_t off = 0;
        while (ok && off < data.size()) {
            const auto n = libssh2_sftp_write(h, reinterpret_cast<const char*>(data.data()) + off, data.size() - off);
            if (n == LIBSSH2_ERROR_EAGAIN) ok = wait(deadline);
            else if (n < 0) ok = false;
            else off += static_cast<size_t>(n);
        }
        if (!ok) error = lastError();
        if (h && call([&] { return libssh2_sftp_close_handle(h); }, 10000) != 0) ok = false;
        call([&] { return libssh2_sftp_shutdown(sftp); }, 10000);
        return ok;
    }

    bool catPut(const std::vector<uint8_t>& data, const std::string& path, int timeoutMs, std::string& error)
    {
        LIBSSH2_CHANNEL* ch = exec("cat > " + path);
        if (!ch) {
            error = lastError();
            return false;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        bool ok = true;
        size_t off = 0;
        while (ok && off < data.size()) {
            const auto n = libssh2_channel_write(ch, reinterpret_cast<const char*>(data.data()) + off, data.size() - off);
            if (n == LIBSSH2_ERROR_EAGAIN) ok = wait(deadline);
            else if (n < 0) ok = false;
            else off += static_cast<size_t>(n);
        }
        ok = ok && call([&] { return libssh2_channel_send_eof(ch); }, 10000) == 0;
        std::string out;
        while (ok && readSome(ch, out) >= 0) ok = wait(deadline);   // cat ends once it wrote the file
        if (!ok) error = lastError();
        release(ch);
        return ok;
    }

private:
    SOCKET m_s = INVALID_SOCKET;
    LIBSSH2_SESSION* m_session = nullptr;
};

std::string jsonText(const json& j)
{
    if (j.is_string()) return j.get<std::string>();
    if (j.is_null()) return {};
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string installSteps(Ssh& ssh, const std::vector<uint8_t>& ipk, const Progress& step, const Options& opt)
{
    const std::string want = ipkVersion(ipk);
    const auto installed = [&] {
        // The app's own appinfo.json (applicationManager/getAppInfo is missing on webOS 3.x)
        std::string out = ssh.run(std::string("cat ") + kAppsDir + "/" + kApp + "/appinfo.json 2>/dev/null", 10000);
        std::erase_if(out, [](char c) { return c == ' ' || c == '\t'; });
        return out.find(std::string("\"id\":\"") + kApp + "\"") != std::string::npos
               && (want.empty() || out.find("\"version\":\"" + want + "\"") != std::string::npos);
    };

    step("Uygulama yükleniyor…");
    const int uploadMs = 60000 + static_cast<int>(std::min<size_t>(ipk.size() / 50, 600000));   // ≥ 50 KB/s
    std::string err;
    if (!ssh.sftpPut(ipk, kRemoteIpk, uploadMs, err) && !ssh.catPut(ipk, kRemoteIpk, uploadMs, err))
        return "Uygulama TV'ye yüklenemedi: " + err;

    step("Uygulama kuruluyor…");
    const std::string payload = json{{"id", kApp}, {"ipkUrl", kRemoteIpk}, {"subscribe", true}}.dump();
    LIBSSH2_CHANNEL* ch = ssh.exec("/usr/bin/luna-send-pub -i 'luna://com.webos.appInstallService/dev/install' '" + payload + "'");
    if (!ch) return "TV'de kurulum başlatılamadı: " + ssh.lastError();
    std::string raw, buf, result;
    const auto start = Clock::now();
    const auto end = start + std::chrono::milliseconds(opt.streamTimeoutMs);
    auto nextCheck = start + std::chrono::milliseconds(opt.firstCheckMs);
    while (result.empty() && Clock::now() < end) {
        std::string chunk;
        const int got = ssh.readSome(ch, chunk);
        raw += chunk;
        buf += chunk;
        for (size_t nl; result.empty() && (nl = buf.find('\n')) != std::string::npos;) {
            const std::string line = trim(buf.substr(0, nl));
            buf.erase(0, nl + 1);
            const json msg = json::parse(line, nullptr, false);
            if (msg.is_discarded() || !msg.is_object()) continue;
            const json details = msg.contains("details") && msg["details"].is_object() ? msg["details"] : json::object();
            std::string st = jsonText(details.value("state", json()));
            if (st.empty()) st = jsonText(msg.value("state", json()));
            if ((msg.contains("returnValue") && msg["returnValue"] == false) || upper(st).find("FAIL") != std::string::npos) {
                std::string why = jsonText(msg.value("errorText", json()));
                if (why.empty()) why = jsonText(details.value("reason", json()));
                if (why.empty()) why = st.empty() ? line : st;
                result = "fail:" + why;
            } else if (lower(st) == "installed" || upper(st).find("SUCCESS") != std::string::npos) {
                result = "ok";
            } else if (!st.empty()) {
                step("Uygulama kuruluyor… (" + st + ")");
            }
        }
        if (got < 0) break;
        if (result.empty() && Clock::now() >= nextCheck) {   // a silent stream: look at the app's file
            if (installed()) result = "ok";
            nextCheck = Clock::now() + std::chrono::milliseconds(opt.checkEveryMs);
        }
        if (result.empty() && got == 0) ssh.wait(std::min(end, Clock::now() + std::chrono::milliseconds(500)), 500);
    }
    ssh.release(ch);
    if (result.empty() && installed()) result = "ok";
    ssh.run(std::string("rm -f ") + kRemoteIpk, 15000);
    if (result.empty()) {
        std::string said;
        for (size_t i = 0; i < raw.size();) {   // " ".join(raw.split())
            while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
            size_t j = i;
            while (j < raw.size() && !std::isspace(static_cast<unsigned char>(raw[j]))) ++j;
            if (j > i) said += (said.empty() ? "" : " ") + raw.substr(i, j - i);
            i = j;
        }
        said = tail(said, 300);
        return "TV kurulumu bitirmedi. "
               + (said.empty() ? std::string("TV hiç yanıt vermedi. TV'yi kapatıp açın (fişten çekmeden) ve tekrar deneyin.")
                               : "TV'nin yanıtı: " + said);
    }
    if (result != "ok") return "TV kurulumu reddetti: " + head(result.substr(5), 200);

    step("Makine TV açılıyor…");
    ssh.run("/usr/bin/luna-send-pub -n 1 'luna://com.webos.applicationManager/launch' '" + json{{"id", kApp}}.dump() + "'",
            15000);
    return {};
}

} // namespace

std::string ipkVersion(const std::vector<uint8_t>& ipk)
{
    try {
        if (ipk.size() < 8 || std::memcmp(ipk.data(), "!<arch>\n", 8) != 0) return {};
        for (size_t pos = 8; pos + 60 <= ipk.size();) {
            std::string name(reinterpret_cast<const char*>(ipk.data() + pos), 16);
            name = trim(name);
            if (!name.empty() && name.back() == '/') name.pop_back();
            const size_t size = std::strtoull(std::string(reinterpret_cast<const char*>(ipk.data() + pos + 48), 10).c_str(),
                                              nullptr, 10);
            if (pos + 60 + size > ipk.size()) return {};
            if (name == "control.tar.gz") {
                const std::string control = tarFile(gunzip(ipk.data() + pos + 60, size, 4 * 1024 * 1024), "control");
                size_t at = 0;
                while (at < control.size()) {
                    size_t eol = control.find('\n', at);
                    if (eol == std::string::npos) eol = control.size();
                    const std::string line = control.substr(at, eol - at);
                    if (line.starts_with("Version:")) return trim(line.substr(8));
                    at = eol + 1;
                }
                return {};
            }
            pos += 60 + size + (size % 2);
        }
    } catch (const std::exception&) {
    }
    return {};
}

bool decryptKey(const std::string& pem, const std::string& passphrase, std::string& plainPem, std::string& error)
{
    static const std::string begin = "-----BEGIN RSA PRIVATE KEY-----", end = "-----END RSA PRIVATE KEY-----";
    const size_t b = pem.find(begin), e = pem.find(end);
    if (b == std::string::npos || e == std::string::npos || e < b) {
        error = pem.find("OPENSSH PRIVATE KEY") != std::string::npos || pem.find("BEGIN PRIVATE KEY") != std::string::npos
                    || pem.find("BEGIN ENCRYPTED PRIVATE KEY") != std::string::npos
                    ? "Bu TV'nin geliştirici anahtarı tanımadığımız bir biçimde. \"Dosyayı kaydet\" ile paketi kaydedip "
                      "webOS Dev Manager ile kurun."
                    : "TV'nin geliştirici anahtarı okunamadı. Developer Mode'da Key Server açık mı?";
        return false;
    }
    bool encrypted = false;
    std::string cipher, ivHex, body;
    const std::string block = pem.substr(b + begin.size(), e - b - begin.size());
    for (size_t at = 0; at < block.size();) {
        size_t eol = block.find('\n', at);
        if (eol == std::string::npos) eol = block.size();
        const std::string line = trim(block.substr(at, eol - at));
        at = eol + 1;
        if (line.starts_with("Proc-Type:")) {
            encrypted = line.find("ENCRYPTED") != std::string::npos;
        } else if (line.starts_with("DEK-Info:")) {
            const std::string v = trim(line.substr(9));
            const size_t comma = v.find(',');
            cipher = upper(trim(v.substr(0, comma)));
            ivHex = comma == std::string::npos ? std::string() : trim(v.substr(comma + 1));
        } else if (line.find(':') == std::string::npos) {
            body += line;
        }
    }
    std::vector<uint8_t> der = base64Decode(body);
    wipe(body);
    if (encrypted) {
        LPCWSTR alg = BCRYPT_AES_ALGORITHM;
        size_t keyLen = 0, blockLen = 16;
        if (cipher == "AES-128-CBC") keyLen = 16;
        else if (cipher == "AES-192-CBC") keyLen = 24;
        else if (cipher == "AES-256-CBC") keyLen = 32;
        else if (cipher == "DES-EDE3-CBC") alg = BCRYPT_3DES_ALGORITHM, keyLen = 24, blockLen = 8;
        const std::vector<uint8_t> iv = hexDecode(ivHex);
        if (!keyLen || iv.size() != blockLen) {
            error = "TV'nin geliştirici anahtarı tanımadığımız bir şifreyle (" + (cipher.empty() ? std::string("?") : cipher)
                    + "). \"Dosyayı kaydet\" ile paketi kaydedip webOS Dev Manager ile kurun.";
            return false;
        }
        const std::vector<uint8_t> salt(iv.begin(), iv.begin() + 8);
        std::vector<std::string> tries{trim(passphrase)};
        if (upper(tries[0]) != tries[0]) tries.push_back(upper(tries[0]));
        std::vector<uint8_t> plain;
        bool ok = false;
        for (std::string& pw : tries) {
            std::vector<uint8_t> key = pemKey(pw, salt, keyLen);
            ok = cbcDecrypt(alg, key, iv, der, plain) && isRsaPrivateKey(plain);
            wipe(key);
            wipe(pw);
            if (ok) break;
            wipe(plain);
        }
        wipe(der);
        if (!ok) {
            error = "Parola (Passphrase) yanlış. Developer Mode uygulamasında yazan 6 karakteri girin.";
            return false;
        }
        der = std::move(plain);
    } else if (!isRsaPrivateKey(der)) {
        wipe(der);
        error = "TV'nin geliştirici anahtarı okunamadı. Developer Mode'da Key Server açık mı?";
        return false;
    }
    std::string lines = base64Lines(der);
    wipe(der);
    plainPem = begin + "\n" + lines + end + "\n";
    wipe(lines);
    return true;
}

std::string installIpk(const std::string& hostIn, const std::string& passphrase, const std::vector<uint8_t>& ipk,
                       const Progress& progress, const Options& opt)
{
    const auto step = [&](const std::string& s) {
        if (progress) progress(s);
    };
    const std::string host = trim(hostIn);
    if (ipk.empty()) return "Makine TV'nin LG paketi bulunamadı (makine-tv.ipk).";
    try {
        step("TV'nin geliştirici anahtarı alınıyor…");
        std::string pem, error;
        if (!httpGet(host, opt.keyPort, "/webos_rsa", 6000, pem, error))
            return "TV'ye ulaşılamadı (" + host + "). Developer Mode uygulamasında Dev Mode Status ve Key Server açık mı, "
                   "adres doğru mu? (" + error + ")";
        std::string key;
        const bool keyOk = decryptKey(pem, passphrase, key, error);
        wipe(pem);
        if (!keyOk) return error;
        struct Wipe {
            std::string& s;
            ~Wipe() { wipe(s); }
        } keyWipe{key};

        // Old LG TVs run OpenSSH 6.x: no server-sig-algs, only ssh-rsa (SHA-1) signatures; libssh2 then
        // signs with ssh-rsa by itself. A server that announces rsa-sha2 but still wants ssh-rsa gets a
        // second connection limited to ssh-rsa.
        sshLibrary();
        step("TV'ye bağlanılıyor…");
        std::string refused;
        for (const bool shaOneOnly : {false, true}) {
            Ssh ssh;
            if (!ssh.connect(host, opt.sshPort, error)) return "TV'ye bağlanılamadı: " + error;
            if (shaOneOnly) libssh2_session_method_pref(ssh.session(), LIBSSH2_METHOD_SIGN_ALGO, "ssh-rsa");
            if (ssh.call([&] { return libssh2_session_handshake(ssh.session(), ssh.sock()); }, 15000) != 0)
                return "TV'ye bağlanılamadı: " + ssh.lastError();
            const int rc = ssh.call(
                [&] {
                    return libssh2_userauth_publickey_frommemory(ssh.session(), "prisoner", 8, nullptr, 0, key.data(),
                                                                 key.size(), nullptr);
                },
                15000);
            if (rc == 0) return installSteps(ssh, ipk, step, opt);
            if (rc == LIBSSH2_ERROR_TIMEOUT || rc == LIBSSH2_ERROR_SOCKET_DISCONNECT || rc == LIBSSH2_ERROR_SOCKET_RECV
                || rc == LIBSSH2_ERROR_SOCKET_SEND)
                return "TV'ye bağlanılamadı: " + ssh.lastError();
            refused = ssh.lastError();
        }
        return "TV bağlantıyı kabul etmedi (" + refused + "). Developer Mode'da Key Server'ı kapatıp açın ve ekrandaki "
               "yeni Passphrase ile tekrar deneyin.";
    } catch (const std::exception& e) {
        return std::string("Kurulum yarıda kaldı: ") + e.what();
    }
}

} // namespace live::webos

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "adb.h"

#include "winutil.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace live::adb {

namespace {

constexpr uint32_t A_CNXN = 0x4e584e43, A_AUTH = 0x48545541, A_OPEN = 0x4e45504f, A_OKAY = 0x59414b4f,
                   A_CLSE = 0x45534c43, A_WRTE = 0x45545257;
constexpr uint32_t kVersion = 0x01000000, kMaxData = 256 * 1024;
constexpr uint32_t kAuthToken = 1, kAuthSignature = 2, kAuthPublicKey = 3;
constexpr const char* kRemoteApk = "/data/local/tmp/makine-tv.apk";

void winsock()
{
    static const bool ready = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)ready;
}

std::vector<uint8_t> dpapi(const std::vector<uint8_t>& in, bool protect)
{
    DATA_BLOB src{static_cast<DWORD>(in.size()), const_cast<BYTE*>(in.data())};
    DATA_BLOB out{};
    const BOOL ok = protect ? CryptProtectData(&src, L"Makine Live ADB", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
                            : CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) return {};
    std::vector<uint8_t> v(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return v;
}

struct CngKey {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    ~CngKey()
    {
        if (key) BCryptDestroyKey(key);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
};

struct Msg {
    uint32_t cmd = 0, arg0 = 0, arg1 = 0;
    std::string data;
};

// One ADB transport over TCP: messages, streams with one write in flight (OKAY-paced).
class Conn {
public:
    ~Conn()
    {
        if (m_s != INVALID_SOCKET) closesocket(m_s);
    }

    bool open(const std::string& host, int port)
    {
        winsock();
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return false;
        m_s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        u_long nb = 1;
        ioctlsocket(m_s, FIONBIO, &nb);
        connect(m_s, res->ai_addr, static_cast<int>(res->ai_addrlen));
        freeaddrinfo(res);
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(m_s, &w);
        FD_SET(m_s, &e);
        timeval tv{10, 0};
        if (select(0, nullptr, &w, &e, &tv) != 1 || FD_ISSET(m_s, &e)) return false;
        nb = 0;
        ioctlsocket(m_s, FIONBIO, &nb);
        BOOL nodelay = TRUE;
        setsockopt(m_s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
        return true;
    }

    bool send(uint32_t cmd, uint32_t a0, uint32_t a1, const std::string& data = {})
    {
        uint32_t sum = 0;
        for (const unsigned char c : data) sum += c;
        const uint32_t head[6] = {cmd, a0, a1, static_cast<uint32_t>(data.size()), sum, cmd ^ 0xFFFFFFFFu};
        std::string out(reinterpret_cast<const char*>(head), sizeof head);
        out += data;
        size_t off = 0;
        while (off < out.size()) {
            const int n = ::send(m_s, out.data() + off, static_cast<int>(out.size() - off), 0);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    std::optional<Msg> recv(int timeoutMs)
    {
        DWORD t = static_cast<DWORD>(timeoutMs);
        setsockopt(m_s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
        uint32_t head[6];
        if (!readAll(head, sizeof head) || (head[0] ^ 0xFFFFFFFFu) != head[5] || head[3] > 16 * 1024 * 1024)
            return std::nullopt;
        Msg m{head[0], head[1], head[2], std::string(head[3], '\0')};
        if (head[3] && !readAll(m.data.data(), head[3])) return std::nullopt;
        return m;
    }

    // CNXN, answering AUTH with a signature, then (first time) with the public key the user approves.
    std::string handshake(const Key& key, const std::string& name, const Progress& progress)
    {
        if (!send(A_CNXN, kVersion, kMaxData, std::string("host::") + '\0')) return "TV'ye bağlanılamadı.";
        bool sentSignature = false, sentKey = false;
        for (;;) {
            const auto m = recv(sentKey ? 60000 : 10000);
            if (!m) return sentKey ? "TV bağlantıyı onaylamadı. TV ekranındaki soruya \"İzin ver\" deyip tekrar deneyin."
                                   : "TV yanıt vermedi.";
            if (m->cmd == A_CNXN) {
                m_maxData = std::clamp<uint32_t>(m->arg1, 4096, kMaxData);
                return {};
            }
            if (m->cmd != A_AUTH || m->arg0 != kAuthToken) continue;
            if (!sentSignature) {
                const auto sig = key.sign(std::vector<uint8_t>(m->data.begin(), m->data.end()));
                if (!send(A_AUTH, kAuthSignature, 0, std::string(sig.begin(), sig.end()))) return "TV bağlantısı koptu.";
                sentSignature = true;
            } else if (!sentKey) {
                if (progress) progress("TV ekranında \"USB hata ayıklamaya izin verilsin mi?\" sorusu çıktı: İzin ver'i seçin.");
                if (!send(A_AUTH, kAuthPublicKey, 0, key.androidPublicKey(name) + '\0')) return "TV bağlantısı koptu.";
                sentKey = true;
            } else {
                return "TV bağlantıyı onaylamadı. TV ekranındaki soruya \"İzin ver\" deyip tekrar deneyin.";
            }
        }
    }

    // OPEN a service; returns the device's stream id (0 = refused).
    uint32_t openStream(const std::string& service, uint32_t& local)
    {
        local = ++m_nextId;
        if (!send(A_OPEN, local, 0, service + '\0')) return 0;
        for (;;) {
            const auto m = recv(30000);
            if (!m || m->cmd == A_CLSE) return 0;
            if (m->cmd == A_OKAY && m->arg1 == local) return m->arg0;
        }
    }

    // WRTE in chunks of maxdata, each waiting for its OKAY. Data the device writes meanwhile is kept.
    bool write(uint32_t local, uint32_t remote, const std::string& data)
    {
        for (size_t off = 0; off < data.size(); off += m_maxData) {
            if (!send(A_WRTE, local, remote, data.substr(off, m_maxData))) return false;
            for (;;) {
                const auto m = recv(60000);
                if (!m || m->cmd == A_CLSE) return false;
                if (m->cmd == A_OKAY && m->arg1 == local) break;
                if (m->cmd == A_WRTE && m->arg1 == local) {
                    m_pending += m->data;
                    send(A_OKAY, local, remote);
                }
            }
        }
        return true;
    }

    // Everything the device writes on the stream until it closes it (or until `want` bytes came).
    std::string readStream(uint32_t local, uint32_t remote, int timeoutMs, size_t want = std::string::npos)
    {
        std::string out = std::move(m_pending);
        m_pending.clear();
        while (out.size() < want) {
            const auto m = recv(timeoutMs);
            if (!m) break;
            if (m->cmd == A_WRTE && m->arg1 == local) {
                out += m->data;
                send(A_OKAY, local, remote);
            } else if (m->cmd == A_CLSE && m->arg1 == local) {
                send(A_CLSE, local, remote);
                m_closed.insert(local);
                break;
            }
        }
        return out;
    }

    void close(uint32_t local, uint32_t remote)
    {
        if (!m_closed.count(local)) send(A_CLSE, local, remote);
    }

    std::string shell(const std::string& cmd, int timeoutMs = 120000)
    {
        uint32_t local = 0;
        const uint32_t remote = openStream("shell:" + cmd, local);
        if (!remote) return {};
        std::string out = readStream(local, remote, timeoutMs);
        close(local, remote);
        return out;
    }

    // sync: SEND path,mode / DATA… / DONE mtime → OKAY | FAIL
    std::string push(const std::vector<uint8_t>& file, const std::string& path)
    {
        uint32_t local = 0;
        const uint32_t remote = openStream("sync:", local);
        if (!remote) return "TV dosya aktarımını açmadı.";
        const auto u32 = [](uint32_t v) { return std::string(reinterpret_cast<const char*>(&v), 4); };
        const std::string spec = path + ",33188";   // S_IFREG | 0644
        std::string out = "SEND" + u32(static_cast<uint32_t>(spec.size())) + spec;
        for (size_t off = 0; off < file.size(); off += 64 * 1024) {
            const size_t n = std::min<size_t>(64 * 1024, file.size() - off);
            out += "DATA" + u32(static_cast<uint32_t>(n));
            out.append(reinterpret_cast<const char*>(file.data() + off), n);
            if (out.size() >= m_maxData) {
                const size_t keep = out.size() - out.size() % m_maxData;
                if (!write(local, remote, out.substr(0, keep))) return "TV'ye yükleme kesildi.";
                out.erase(0, keep);
            }
        }
        out += "DONE" + u32(static_cast<uint32_t>(std::time(nullptr)));
        if (!write(local, remote, out)) return "TV'ye yükleme kesildi.";
        const std::string reply = readStream(local, remote, 120000, 8);
        std::string error;
        if (reply.size() < 8 || reply.compare(0, 4, "OKAY") != 0)
            error = "TV dosyayı kabul etmedi" + (reply.size() > 8 ? ": " + reply.substr(8, 200) : std::string()) + ".";
        write(local, remote, "QUIT" + u32(0));
        close(local, remote);
        return error;
    }

private:
    bool readAll(void* buf, size_t n)
    {
        auto* p = static_cast<char*>(buf);
        while (n) {
            const int got = ::recv(m_s, p, static_cast<int>(n), 0);
            if (got <= 0) return false;
            p += got;
            n -= static_cast<size_t>(got);
        }
        return true;
    }

    SOCKET m_s = INVALID_SOCKET;
    uint32_t m_maxData = 4096, m_nextId = 0;
    std::string m_pending;
    std::set<uint32_t> m_closed;
};

// Little-endian 32-bit word big integers (2048-bit modulus = 64 words)
using Words = std::vector<uint32_t>;

bool geq(const Words& a, const Words& b)
{
    for (size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i]) return a[i] > b[i];
    return true;
}

void sub(Words& a, const Words& b)
{
    int64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int64_t d = static_cast<int64_t>(a[i]) - b[i] - borrow;
        borrow = d < 0;
        a[i] = static_cast<uint32_t>(d);
    }
}

} // namespace

std::vector<uint8_t> androidKeyStruct(const std::vector<uint8_t>& mod, uint32_t exponent)
{
    const size_t words = mod.size() / 4;
    Words n(words + 1, 0);
    for (size_t i = 0; i < mod.size(); ++i) {
        const size_t byteFromEnd = mod.size() - 1 - i;
        n[byteFromEnd / 4] |= static_cast<uint32_t>(mod[i]) << (8 * (byteFromEnd % 4));
    }
    // n0inv = -1 / n[0] mod 2^32 (Newton's iteration doubles the correct bits each round)
    uint32_t inv = n[0];
    for (int i = 0; i < 5; ++i) inv *= 2 - n[0] * inv;
    const uint32_t n0inv = 0u - inv;
    // rr = (2^(32·words))^2 mod n, by doubling 1 that many times
    Words r(words + 1, 0);
    r[0] = 1;
    for (size_t i = 0; i < 64 * words; ++i) {
        uint32_t carry = 0;
        for (auto& w : r) {
            const uint32_t next = w >> 31;
            w = (w << 1) | carry;
            carry = next;
        }
        if (geq(r, n)) sub(r, n);
    }
    std::vector<uint8_t> out;
    const auto put = [&](uint32_t v) {
        for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * k)));
    };
    put(static_cast<uint32_t>(words));
    put(n0inv);
    for (size_t i = 0; i < words; ++i) put(n[i]);
    for (size_t i = 0; i < words; ++i) put(r[i]);
    put(exponent);
    return out;
}

bool Key::loadOrCreate(const std::wstring& dir, std::string& error)
{
    const std::wstring path = dir + L"\\adbkey.bin";
    if (const auto stored = win::readFile(path)) m_blob = dpapi(std::vector<uint8_t>(stored->begin(), stored->end()), false);
    CngKey k;
    if (BCryptOpenAlgorithmProvider(&k.alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0) {
        error = "Windows şifreleme hizmeti açılamadı.";
        return false;
    }
    if (m_blob.empty()) {
        ULONG size = 0;
        if (BCryptGenerateKeyPair(k.alg, &k.key, 2048, 0) != 0 || BCryptFinalizeKeyPair(k.key, 0) != 0
            || BCryptExportKey(k.key, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, nullptr, 0, &size, 0) != 0) {
            error = "ADB anahtarı oluşturulamadı.";
            return false;
        }
        m_blob.resize(size);
        BCryptExportKey(k.key, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, m_blob.data(), size, &size, 0);
        const auto sealed = dpapi(m_blob, true);
        if (sealed.empty()
            || !win::writeFileAtomic(path, std::string_view(reinterpret_cast<const char*>(sealed.data()), sealed.size()))) {
            error = "ADB anahtarı kaydedilemedi.";
            return false;
        }
    }
    if (m_blob.size() < sizeof(BCRYPT_RSAKEY_BLOB)) {
        error = "ADB anahtarı bozuk.";
        return false;
    }
    BCRYPT_RSAKEY_BLOB head;
    std::memcpy(&head, m_blob.data(), sizeof head);
    const uint8_t* p = m_blob.data() + sizeof head;
    exponent = 0;
    for (ULONG i = 0; i < head.cbPublicExp; ++i) exponent = (exponent << 8) | p[i];
    modulus.assign(p + head.cbPublicExp, p + head.cbPublicExp + head.cbModulus);
    return true;
}

std::vector<uint8_t> Key::sign(const std::vector<uint8_t>& token) const
{
    CngKey k;
    std::vector<uint8_t> sig(modulus.size());
    BCRYPT_PKCS1_PADDING_INFO pad{BCRYPT_SHA1_ALGORITHM};
    ULONG size = 0;
    if (BCryptOpenAlgorithmProvider(&k.alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0
        || BCryptImportKeyPair(k.alg, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, &k.key, const_cast<UCHAR*>(m_blob.data()),
                               static_cast<ULONG>(m_blob.size()), 0) != 0
        || BCryptSignHash(k.key, &pad, const_cast<UCHAR*>(token.data()), static_cast<ULONG>(token.size()), sig.data(),
                          static_cast<ULONG>(sig.size()), &size, BCRYPT_PAD_PKCS1) != 0)
        return {};
    sig.resize(size);
    return sig;
}

std::string Key::androidPublicKey(const std::string& name) const
{
    const auto raw = androidKeyStruct(modulus, exponent);
    DWORD len = 0;
    CryptBinaryToStringA(raw.data(), static_cast<DWORD>(raw.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::string b64(len, '\0');
    CryptBinaryToStringA(raw.data(), static_cast<DWORD>(raw.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &len);
    b64.resize(len);
    return b64 + " " + name;
}

std::vector<std::string> scanHosts(int port, int timeoutMs)
{
    winsock();
    std::vector<std::string> mine, candidates;
    char hostName[256] = {};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (gethostname(hostName, sizeof hostName) == 0 && getaddrinfo(hostName, nullptr, &hints, &res) == 0) {
        for (addrinfo* a = res; a; a = a->ai_next) {
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr, ip, sizeof ip);
            const std::string s = ip;
            if (s.rfind("127.", 0) == 0 || s.rfind("169.254.", 0) == 0) continue;
            mine.push_back(s);
            const std::string net = s.substr(0, s.rfind('.') + 1);
            for (int h = 1; h < 255; ++h)
                if (std::find(candidates.begin(), candidates.end(), net + std::to_string(h)) == candidates.end())
                    candidates.push_back(net + std::to_string(h));
        }
        freeaddrinfo(res);
    }
    std::vector<std::string> open;
    for (size_t start = 0; start < candidates.size(); start += 60) {   // FD_SETSIZE is 64 on Windows
        std::vector<std::pair<SOCKET, std::string>> batch;
        fd_set w;
        FD_ZERO(&w);
        for (size_t i = start; i < std::min(candidates.size(), start + 60); ++i) {
            if (std::find(mine.begin(), mine.end(), candidates[i]) != mine.end()) continue;
            const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(static_cast<u_short>(port));
            inet_pton(AF_INET, candidates[i].c_str(), &to.sin_addr);
            connect(s, reinterpret_cast<sockaddr*>(&to), sizeof to);
            FD_SET(s, &w);
            batch.emplace_back(s, candidates[i]);
        }
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (!batch.empty() && select(0, nullptr, &w, nullptr, &tv) > 0)
            for (const auto& [s, ip] : batch)
                if (FD_ISSET(s, &w)) open.push_back(ip);
        for (const auto& [s, ip] : batch) closesocket(s);
    }
    return open;
}

std::string installApk(const std::string& host, int port, const std::vector<uint8_t>& apk, const Key& key,
                       const std::string& keyName, const Progress& progress, std::string& note)
{
    const auto step = [&](const std::string& s) {
        if (progress) progress(s);
    };
    if (apk.empty()) return "Makine TV paketi bulunamadı (makine-tv.apk).";
    Conn c;
    step("TV'ye bağlanılıyor… TV'de \"hata ayıklamaya izin ver\" sorusu çıkarsa İzin ver'i seçin.");
    if (!c.open(host, port))
        return "TV'ye bağlanılamadı (" + host + ":" + std::to_string(port)
               + "). TV'de \"Ağ üzerinden hata ayıklama\" (Fire TV: ADB hata ayıklama) açık mı, adres doğru mu?";
    if (std::string e = c.handshake(key, keyName, progress); !e.empty()) return e;
    step("Uygulama yükleniyor…");
    if (std::string e = c.push(apk, kRemoteApk); !e.empty()) return e;
    step("Uygulama kuruluyor…");
    std::string out = c.shell(std::string("pm install -r ") + kRemoteApk);
    if (out.find("UPDATE_INCOMPATIBLE") != std::string::npos
        || out.find("INSTALL_FAILED_VERSION_DOWNGRADE") != std::string::npos) {
        // Another signature (a development build) or a newer version: remove and install again.
        // Makine TV keeps only its pairing code; a new one appears on the TV.
        step("Eski Makine TV kaldırılıyor…");
        c.shell(std::string("pm uninstall ") + kPackage);
        out = c.shell(std::string("pm install ") + kRemoteApk);
    }
    c.shell(std::string("rm -f ") + kRemoteApk);
    if (out.find("Success") == std::string::npos) {
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
        return "TV kurulumu reddetti: " + (out.empty() ? std::string("yanıt yok") : out.substr(0, 200));
    }
    step("Altyazı katmanı izni veriliyor…");
    c.shell(std::string("appops set ") + kPackage + " SYSTEM_ALERT_WINDOW allow");
    std::string mode = c.shell(std::string("appops get ") + kPackage + " SYSTEM_ALERT_WINDOW");
    std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    step("Makine TV açılıyor…");
    c.shell(std::string("am start -n ") + kPackage + "/.MainActivity");
    if (mode.find("allow") == std::string::npos) {
        const bool lowRam = c.shell("getprop ro.config.low_ram").find("true") != std::string::npos;
        note = std::string("Makine TV kuruldu, ama bu TV altyazı katmanına izin vermedi") + (lowRam ? " (düşük bellekli cihaz)" : "")
               + ". Altyazıyı telefonda okumak için konsol modunda çıktıyı Telefon yapın.";
    }
    return {};
}

} // namespace live::adb

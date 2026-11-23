// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// Discord activity, the stored gzip, the ADB key (signature checked the way adbd checks it),
// the phone / OBS web server over real sockets and the Makine TV link against a fake TV.
//   live_outputs_test <case>     cases: discord discord_pipe gzip adb_key web tv_link

#include "adb.h"
#include "discord.h"
#include "gzip_store.h"
#include "subtitle_web.h"
#include "tv_link.h"

#include <nlohmann/json.hpp>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using namespace live;

namespace {

int g_fails = 0;

void check(bool cond, const std::string& label)
{
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", label.c_str());
    if (!cond) ++g_fails;
}

void testDiscord()
{
    const json a = discordActivity("Grand Theft Auto VI", 1700000000, 124, true, "", "grand-theft-auto-vi");
    check(a["details"] == "Grand Theft Auto VI · konsolda Türkçe oynuyor", "details");
    check(a["state"] == "Bu oturumda 124 diyalog çevrildi", "state");
    check(a["assets"]["large_image"].get<std::string>().find("assets/discord/gta6.png") != std::string::npos,
          "GTA VI: Makine × GTA VI logo");
    check(a["timestamps"]["start"] == 1700000000 && a["buttons"][0]["url"] == "https://makineceviri.org/live/", "time and button");
    const json b = discordActivity("", 1, 0, false, "1174180", "red-dead-redemption-2");
    check(b["details"] == "Bir oyun · Türkçe oynuyor" && b["state"] == "Türkçe altyazı açık", "no game, no lines");
    check(b["assets"]["large_image"] == "https://cdn.cloudflare.steamstatic.com/steam/apps/1174180/header.jpg", "Steam cover");
    check(discordActivity("X", 1, 0, false, "", "x")["assets"]["large_image"] == "https://makineceviri.org/images/icon-512.png",
          "other games: the logo");
}

// A fake Discord desktop app on its IPC pipe: handshake → READY, SET_ACTIVITY → reply.
void testDiscordPipe()
{
    const std::string base = "\\\\.\\pipe\\makine-live-test-" + std::to_string(GetCurrentProcessId()) + "-";
    const HANDLE pipe = CreateNamedPipeA((base + "0").c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 65536,
                                         65536, 0, nullptr);
    std::vector<json> frames;
    std::thread discord([&] {
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
        const auto readFrame = [&](uint32_t& op, json& body) {
            uint32_t head[2];
            DWORD got = 0;
            if (!ReadFile(pipe, head, 8, &got, nullptr) || got != 8) return false;
            std::string data(head[1], '\0');
            DWORD total = 0;
            while (total < head[1] && ReadFile(pipe, data.data() + total, head[1] - total, &got, nullptr) && got) total += got;
            op = head[0];
            body = json::parse(data, nullptr, false);
            return true;
        };
        const auto writeFrame = [&](uint32_t op, const json& body) {
            const std::string data = body.dump();
            const uint32_t head[2] = {op, static_cast<uint32_t>(data.size())};
            DWORD put = 0;
            WriteFile(pipe, head, 8, &put, nullptr);
            WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), &put, nullptr);
        };
        uint32_t op = 0;
        json body;
        for (int i = 0; i < 2 && readFrame(op, body); ++i) {
            frames.push_back(body);
            writeFrame(1, i == 0 ? json{{"evt", "READY"}} : json{{"cmd", "SET_ACTIVITY"}, {"evt", nullptr}});
        }
    });
    auto presence = std::make_shared<DiscordPresence>(base);
    presence->start();
    presence->set(discordActivity("Grand Theft Auto VI", 1700000000, 3, true, "", "grand-theft-auto-vi"));
    discord.join();
    check(frames.size() == 2 && frames[0]["client_id"] == kDiscordAppId && frames[0]["v"] == 1, "handshake with the app id");
    check(frames.size() == 2 && frames[1]["cmd"] == "SET_ACTIVITY"
              && frames[1]["args"]["activity"]["state"] == "Bu oturumda 3 diyalog çevrildi"
              && frames[1]["args"]["pid"] == GetCurrentProcessId(),
          "SET_ACTIVITY with this process and the activity");
    check(presence->connected(), "connected");
    presence->stop();
    CloseHandle(pipe);
}

void testGzip()
{
    check(crc32("hello") == 0x3610A686u, "CRC-32 of \"hello\"");
    const std::string big(200000, 'x');
    const std::string gz = gzipStored(big);
    check(gz.size() == 10 + 4 * 5 + big.size() + 8 && static_cast<unsigned char>(gz[0]) == 0x1f
              && static_cast<unsigned char>(gz[1]) == 0x8b,
          "stored gzip: header, 4 blocks, trailer");
    check(static_cast<unsigned char>(gz[10]) == 0 && static_cast<unsigned char>(gz[10 + 5 + 65535]) == 0
              && static_cast<unsigned char>(gz[10 + 3 * (5 + 65535)]) == 1,
          "only the last block is final");
    check(gzipStored("").size() == 10 + 5 + 8, "empty input");
}

// adbd's check (mincrypt RSA_verify, e = 65537): Montgomery arithmetic with n0inv and rr from the
// key struct, then PKCS#1 v1.5 with the SHA-1 DigestInfo around the token.
struct AndroidKey {
    uint32_t len, n0inv;
    std::vector<uint32_t> n, rr;
};

void subM(const AndroidKey& k, std::vector<uint32_t>& a)
{
    int64_t A = 0;
    for (uint32_t i = 0; i < k.len; ++i) {
        A += static_cast<int64_t>(a[i]) - k.n[i];
        a[i] = static_cast<uint32_t>(A);
        A >>= 32;
    }
}

bool geM(const AndroidKey& k, const std::vector<uint32_t>& a)
{
    for (uint32_t i = k.len; i;) {
        --i;
        if (a[i] < k.n[i]) return false;
        if (a[i] > k.n[i]) return true;
    }
    return true;
}

void montMulAdd(const AndroidKey& k, std::vector<uint32_t>& c, uint32_t a, const std::vector<uint32_t>& b)
{
    uint64_t A = static_cast<uint64_t>(a) * b[0] + c[0];
    const uint32_t d0 = static_cast<uint32_t>(A) * k.n0inv;
    uint64_t B = static_cast<uint64_t>(d0) * k.n[0] + static_cast<uint32_t>(A);
    uint32_t i = 1;
    for (; i < k.len; ++i) {
        A = (A >> 32) + static_cast<uint64_t>(a) * b[i] + c[i];
        B = (B >> 32) + static_cast<uint64_t>(d0) * k.n[i] + static_cast<uint32_t>(A);
        c[i - 1] = static_cast<uint32_t>(B);
    }
    A = (A >> 32) + (B >> 32);
    c[i - 1] = static_cast<uint32_t>(A);
    if (A >> 32) subM(k, c);
}

std::vector<uint32_t> montMul(const AndroidKey& k, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
{
    std::vector<uint32_t> c(k.len, 0);
    for (uint32_t i = 0; i < k.len; ++i) montMulAdd(k, c, a[i], b);
    return c;
}

void testAdbKey()
{
    const std::wstring dir = std::filesystem::temp_directory_path().wstring() + L"makine_adb_test_"
                             + std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(dir);
    adb::Key key;
    std::string error;
    check(key.loadOrCreate(dir, error), "key made (" + error + ")");
    check(key.modulus.size() == 256 && key.exponent == 65537, "RSA-2048, e = 65537");
    adb::Key again;
    check(again.loadOrCreate(dir, error) && again.modulus == key.modulus, "the same key comes back from adbkey.bin");

    std::vector<uint8_t> token(20);
    for (size_t i = 0; i < token.size(); ++i) token[i] = static_cast<uint8_t>(i * 37 + 11);
    const auto sig = key.sign(token);
    check(sig.size() == 256, "signature of the token");

    const auto raw = adb::androidKeyStruct(key.modulus, key.exponent);
    check(raw.size() == 524, "Android RSAPublicKey is 524 bytes");
    const auto word = [&](size_t i) {
        return static_cast<uint32_t>(raw[4 * i]) | raw[4 * i + 1] << 8 | raw[4 * i + 2] << 16 | static_cast<uint32_t>(raw[4 * i + 3]) << 24;
    };
    AndroidKey k{word(0), word(1), {}, {}};
    for (uint32_t i = 0; i < 64; ++i) k.n.push_back(word(2 + i));
    for (uint32_t i = 0; i < 64; ++i) k.rr.push_back(word(66 + i));
    check(k.len == 64 && word(130) == 65537 && static_cast<uint32_t>(k.n0inv * k.n[0]) == 0xFFFFFFFFu, "len, e, n0inv");

    std::vector<uint32_t> a(64);
    for (uint32_t i = 0; i < 64; ++i) {
        const size_t p = (63 - i) * 4;
        a[i] = static_cast<uint32_t>(sig[p]) << 24 | sig[p + 1] << 16 | sig[p + 2] << 8 | sig[p + 3];
    }
    std::vector<uint32_t> aR = montMul(k, a, k.rr);
    for (int i = 0; i < 16; i += 2) {
        const auto aaR = montMul(k, aR, aR);
        aR = montMul(k, aaR, aaR);
    }
    auto m = montMul(k, aR, a);
    if (geM(k, m)) subM(k, m);
    std::vector<uint8_t> em(256);
    for (uint32_t i = 0; i < 64; ++i)
        for (int b = 0; b < 4; ++b) em[255 - (4 * i + b)] = static_cast<uint8_t>(m[i] >> (8 * b));
    static const uint8_t digestInfo[] = {0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14};
    bool padded = em[0] == 0 && em[1] == 1;
    const size_t tail = sizeof digestInfo + token.size();
    for (size_t i = 2; i < 256 - tail - 1; ++i) padded = padded && em[i] == 0xFF;
    padded = padded && em[256 - tail - 1] == 0;
    check(padded && std::memcmp(em.data() + 256 - tail, digestInfo, sizeof digestInfo) == 0
              && std::memcmp(em.data() + 256 - token.size(), token.data(), token.size()) == 0,
          "adbd's Montgomery check accepts the signature (n0inv and rr right)");
    check(key.androidPublicKey("makine-live@test").find(" makine-live@test") != std::string::npos, "public key text");
    std::filesystem::remove_all(dir);
}

SOCKET dial(int port)
{
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    DWORD t = 10000;   // generous: the machine may be busy with other builds
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

std::string readUntil(SOCKET s, const std::string& marker, size_t limit = 1 << 16)
{
    std::string out;
    char buf[4096];
    while (out.find(marker) == std::string::npos && out.size() < limit) {
        const int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

void testWeb()
{
    auto web = std::make_shared<SubtitleWeb>("LOGO");
    web->configure(true, false);
    check(web->running() && !web->lan() && web->port() >= SubtitleWeb::kPort, "listens on 127.0.0.1");
    const int port = web->port();
    SOCKET s = dial(port);
    std::string req = "GET /obs HTTP/1.1\r\nHost: x\r\n\r\n";
    send(s, req.data(), static_cast<int>(req.size()), 0);
    const std::string page = readUntil(s, "</html>");
    check(page.rfind("HTTP/1.1 200", 0) == 0 && page.find("Makine Live · OBS") != std::string::npos
              && page.find("Content-Security-Policy: default-src 'self'") != std::string::npos,
          "/obs page with its CSP");
    closesocket(s);

    s = dial(port);
    req = "GET /events HTTP/1.1\r\n\r\n";
    send(s, req.data(), static_cast<int>(req.size()), 0);
    const std::string first = readUntil(s, "\n\n");
    check(first.find("text/event-stream") != std::string::npos && first.find("\"t\":\"state\"") != std::string::npos,
          "/events sends the state at once");
    web->sendLine(7, "Seni bulacağım.", "verified");
    const std::string next = readUntil(s, "\n\n");
    check(next.find("Seni bulacağım.") != std::string::npos && next.find("\"kind\":\"verified\"") != std::string::npos,
          "a new line is pushed");
    check(web->clients() == 1, "one client counted");
    web->setStyle({{"size", 40}, {"opacity", 50}, {"pos", "ust"}});
    check(readUntil(s, "\n\n").find("\"size\":40") != std::string::npos, "style is pushed");
    web->stop();
    check(!web->running(), "stopped");
    closesocket(s);
    check(dial(port) == INVALID_SOCKET, "port closed after stop");
}

void testTvLink()
{
    // A fake Makine TV: UDP discovery and the TCP line stream with code 4321
    const SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const SOCKET tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(udp, reinterpret_cast<sockaddr*>(&a), sizeof a);
    bind(tcp, reinterpret_cast<sockaddr*>(&a), sizeof a);
    listen(tcp, 2);
    int len = sizeof a;
    getsockname(udp, reinterpret_cast<sockaddr*>(&a), &len);
    const int udpPort = ntohs(a.sin_port);
    len = sizeof a;
    getsockname(tcp, reinterpret_cast<sockaddr*>(&a), &len);
    const int tcpPort = ntohs(a.sin_port);
    std::string received;
    std::thread tv([&] {
        char buf[2048];
        sockaddr_in from{};
        int flen = sizeof from;
        const int n = recvfrom(udp, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n > 0 && std::string(buf, static_cast<size_t>(n)).find("discover") != std::string::npos) {
            const std::string here = json({{"t", "here"}, {"v", 1}, {"id", "tv-1"}, {"name", "Salon TV"}, {"port", tcpPort}}).dump();
            sendto(udp, here.data(), static_cast<int>(here.size()), 0, reinterpret_cast<sockaddr*>(&from), flen);
        }
        const SOCKET c = accept(tcp, nullptr, nullptr);
        DWORD t = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
        const std::string hello = readUntil(c, "\n");
        const std::string reply = hello.find("\"code\":\"4321\"") != std::string::npos ? "{\"t\":\"ok\",\"tv\":\"Salon TV\"}\n"
                                                                                       : "{\"t\":\"bad_code\"}\n";
        send(c, reply.data(), static_cast<int>(reply.size()), 0);
        received = readUntil(c, "Merhaba");
        closesocket(c);
    });
    const auto found = discoverTvs(1500, udpPort, {"127.0.0.1"});
    check(found.size() == 1 && found[0].name == "Salon TV" && found[0].port == tcpPort && found[0].host == "127.0.0.1",
          "discovery finds the TV");
    auto link = std::make_shared<TvLink>("TEST-PC", udpPort);
    link->start();
    if (!found.empty()) link->pair(found[0], "4321");
    for (int i = 0; i < 50 && link->state().first != "connected"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(link->state().first == "connected" && link->state().second == "Salon TV", "paired with the code");
    link->setStyle(30, "alt", 80);
    link->sendLine(1, "Merhaba", "api");
    tv.join();
    check(received.find("\"t\":\"style\"") != std::string::npos && received.find("\"text\":\"Merhaba\"") != std::string::npos,
          "style and line arrive");
    link->close();
    closesocket(udp);
    closesocket(tcp);
}

} // namespace

int main(int argc, char** argv)
{
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    const std::map<std::string, std::function<void()>> cases{
        {"discord", testDiscord}, {"discord_pipe", testDiscordPipe}, {"gzip", testGzip}, {"adb_key", testAdbKey}, {"web", testWeb}, {"tv_link", testTvLink}};
    const auto it = argc > 1 ? cases.find(argv[1]) : cases.end();
    if (it == cases.end()) {
        std::printf("usage: live_outputs_test discord|gzip|adb_key|web|tv_link\n");
        return 2;
    }
    it->second();
    std::printf("%s: %s\n", argv[1], g_fails ? (std::to_string(g_fails) + " FAILED").c_str() : "passed");
    return g_fails ? 1 : 0;
}

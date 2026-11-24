// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

// LG webOS install (src/webos_install.cpp). The fake TV lives in tests/test_webos_install_cpp.py,
// which drives this program:
//   live_webos_test ipk_version <ipk> <version>        the .ipk's control Version (ctest)
//   live_webos_test key <pem file> <passphrase>         decrypts the TV's key: "key: ok" / "key: error: …"
//   live_webos_test install <host> <keyPort> <sshPort> <passphrase> <ipk> [streamTimeoutMs]
//                                                       "step: …" per step, then "result: ok" / "result: error: …"

#include "webos_install.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> readAll(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void line(const std::string& s)
{
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "ipk_version" && argc == 4) {
        const std::string got = live::webos::ipkVersion(readAll(argv[2]));
        line("ipk_version: " + got + (got == argv[3] ? " passed" : std::string(" FAILED, expected ") + argv[3]));
        return got == argv[3] ? 0 : 1;
    }
    if (mode == "key" && argc == 4) {
        const std::vector<uint8_t> pem = readAll(argv[2]);
        std::string plain, error;
        if (live::webos::decryptKey(std::string(pem.begin(), pem.end()), argv[3], plain, error))
            line(plain.starts_with("-----BEGIN RSA PRIVATE KEY-----\n") ? "key: ok" : "key: error: bad output");
        else
            line("key: error: " + error);
        return 0;
    }
    if (mode == "install" && (argc == 7 || argc == 8)) {
        live::webos::Options opt;
        opt.keyPort = std::atoi(argv[3]);
        opt.sshPort = std::atoi(argv[4]);
        if (argc == 8) opt.streamTimeoutMs = std::atoi(argv[7]);
        const std::string error = live::webos::installIpk(argv[2], argv[5], readAll(argv[6]),
                                                          [](const std::string& s) { line("step: " + s); }, opt);
        line(error.empty() ? "result: ok" : "result: error: " + error);
        return 0;
    }
    line("usage: live_webos_test ipk_version <ipk> <version> | key <pem> <passphrase> | "
         "install <host> <keyPort> <sshPort> <passphrase> <ipk> [streamTimeoutMs]");
    return 2;
}

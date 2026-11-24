// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// "TV'ye kur" for Android TV / Google TV / Fire TV: a minimal ADB client over the network
// (port of makine_live/tv/tv_install.py install()). No adb.exe is shipped or started: the
// plugin speaks the ADB wire protocol itself (CNXN / AUTH / OPEN / WRTE / OKAY / CLSE, the
// sync: service to push the APK, shell: to install it). The PC's ADB key is an RSA-2048 key
// made with Windows CNG, kept DPAPI-encrypted in <dataDir>\adbkey.bin; the TV asks once
// "Allow USB debugging from this computer?" and remembers it.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace live::adb {

constexpr int kPort = 5555;
inline const char* const kPackage = "org.makineceviri.tv";

class Key {
public:
    /// Loads <dir>\adbkey.bin or makes a new key there. false: CNG / DPAPI failed (error says why).
    bool loadOrCreate(const std::wstring& dir, std::string& error);
    /// PKCS#1 v1.5 signature of the device's 20-byte token (the token is used as a SHA-1 digest).
    std::vector<uint8_t> sign(const std::vector<uint8_t>& token) const;
    /// The public key as adbd expects it: base64(Android RSAPublicKey) + " " + name.
    std::string androidPublicKey(const std::string& name) const;

    std::vector<uint8_t> modulus;   // big endian
    uint32_t exponent = 0;

private:
    std::vector<uint8_t> m_blob;    // BCRYPT_RSAFULLPRIVATE_BLOB
};

/// The 524-byte Android RSAPublicKey (len, n0inv, n[], rr[], e) for a 2048-bit modulus.
std::vector<uint8_t> androidKeyStruct(const std::vector<uint8_t>& modulusBigEndian, uint32_t exponent);

/// Hosts on this PC's /24 networks with `port` open (the install screen fills in the TV's address).
std::vector<std::string> scanHosts(int port, int timeoutMs = 350);

using Progress = std::function<void(const std::string&)>;

/// Pushes and installs the APK, grants the overlay permission and opens Makine TV. Returns ""
/// on success, else a Turkish message for the user. `note` receives a warning that does not
/// fail the install (the TV refused the overlay permission).
std::string installApk(const std::string& host, int port, const std::vector<uint8_t>& apk, const Key& key,
                       const std::string& keyName, const Progress& progress, std::string& note);

} // namespace live::adb

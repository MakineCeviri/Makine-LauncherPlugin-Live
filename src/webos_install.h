// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// "TV'ye kur" for LG webOS (port of makine_live/tv/tv_install.py install_webos()). The TV runs
// LG's Developer Mode app with Dev Mode Status and Key Server on. The plugin does what LG's
// ares-install does, with no ares-cli, ssh.exe or Python on the PC:
//   1. GET http://<tv>:9991/webos_rsa: the TV's developer key, an encrypted PEM whose passphrase
//      is the 6 characters on the Developer Mode screen (decrypted here with Windows CNG),
//   2. SSH prisoner@<tv>:9922 with that key (libssh2, WinCNG backend),
//   3. upload the .ipk to /media/developer/temp (SFTP, else `cat > file`),
//   4. luna-send-pub appInstallService/dev/install, following its JSON stream,
//   5. remove the upload and open Makine TV.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace live::webos {

constexpr int kKeyPort = 9991;
constexpr int kSshPort = 9922;
inline const char* const kApp = "org.makineceviri.tv";
inline const char* const kRemoteIpk = "/media/developer/temp/makine-tv.ipk";
inline const char* const kAppsDir = "/media/developer/apps/usr/palm/applications";   // developer-mode installs

struct Options {
    int keyPort = kKeyPort;
    int sshPort = kSshPort;
    int streamTimeoutMs = 150000;   // the whole install stream
    int firstCheckMs = 15000;       // a silent stream: first look at the app's appinfo.json
    int checkEveryMs = 10000;       // ... and then every this often
};

using Progress = std::function<void(const std::string&)>;

/// "Version:" from the .ipk's control file (ar → control.tar.gz → control); "" if unreadable.
std::string ipkVersion(const std::vector<uint8_t>& ipk);

/// The TV's key server PEM ("BEGIN RSA PRIVATE KEY", plain or Proc-Type 4,ENCRYPTED with
/// AES-128/192/256-CBC or DES-EDE3-CBC) as a plain PKCS#1 PEM for libssh2. The passphrase is tried
/// as typed and upper-cased. false: wrong passphrase or an unsupported key (error says which).
bool decryptKey(const std::string& pem, const std::string& passphrase, std::string& plainPem, std::string& error);

/// Installs and opens Makine TV on a developer-mode LG TV. Returns "" on success, else a Turkish
/// message for the user.
std::string installIpk(const std::string& host, const std::string& passphrase, const std::vector<uint8_t>& ipk,
                       const Progress& progress, const Options& options = {});

} // namespace live::webos

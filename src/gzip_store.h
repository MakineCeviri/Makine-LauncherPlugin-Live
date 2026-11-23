// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// A valid gzip stream without compression (deflate "stored" blocks): the server takes the
// support log as application/gzip and the plugin needs no zlib for a few hundred kilobytes.

#include <cstdint>
#include <string>
#include <string_view>

namespace live {

uint32_t crc32(std::string_view data);
std::string gzipStored(std::string_view data);

} // namespace live

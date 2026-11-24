// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#include "gzip_store.h"

#include <algorithm>
#include <array>

namespace live {

uint32_t crc32(std::string_view data)
{
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (const unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string gzipStored(std::string_view data)
{
    std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);   // deflate, no name, OS unknown
    size_t off = 0;
    do {
        const size_t n = std::min<size_t>(65535, data.size() - off);
        const bool last = off + n == data.size();
        out += static_cast<char>(last ? 1 : 0);   // BFINAL, BTYPE 00 (stored)
        out += static_cast<char>(n & 0xFF);
        out += static_cast<char>(n >> 8);
        out += static_cast<char>(~n & 0xFF);
        out += static_cast<char>((~n >> 8) & 0xFF);
        out.append(data.substr(off, n));
        off += n;
    } while (off < data.size());
    const auto put32 = [&](uint32_t v) {
        for (int k = 0; k < 4; ++k) out += static_cast<char>((v >> (8 * k)) & 0xFF);
    };
    put32(crc32(data));
    put32(static_cast<uint32_t>(data.size()));
    return out;
}

} // namespace live

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// K3 (OCR_KARAR): the OCR gate of console mode. A Remote Play / capture stream never repeats a
// frame, so the frame hash wakes the OCR ten times a second; what does not change is the subtitle
// itself. The mask marks subtitle-like pixels — a light fill with a dark outline next to it, both
// relative to the region's own brightness range (no absolute "bright" threshold: a washed-out HDR
// preview or a night scene breaks those) — on a coarse cell grid that compression noise barely
// moves. The mask only ever says "skip": when in doubt the OCR runs.

#include <cstdint>
#include <vector>

namespace live {

class TextMask {
public:
    static TextMask of(const uint8_t* bgra, int width, int height);

    /// Share of the cells that differ, relative to the larger mask (0 = identical, 1 = nothing shared).
    double difference(const TextMask& other) const;
    /// Same subtitle (or both empty): the OCR can be skipped.
    bool sameText(const TextMask& other) const;
    int cells() const { return m_count; }
    bool empty() const { return m_w == 0; }

    // D-C (tests/cpp/mask_gate.cpp): the same subtitle over a fully re-drawn background differs by up to ~1.5,
    // a changed subtitle by 0.82 at least; 0.25 keeps a wide margin under the changed side (G-MISS = 0)
    static constexpr double kSameText = 0.25;

private:
    int m_w = 0, m_h = 0, m_count = 0;
    std::vector<uint8_t> m_bits;
};

} // namespace live

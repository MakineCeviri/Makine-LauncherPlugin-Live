// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// One subtitle-finder sample from a whole window: the frame downscaled to finder::kMaxWidth, the text
// detector over it, each line's fill score, and the head of the `recognize` most subtitle-like lines
// (read at full resolution). Shared by the engine and tests/cpp/finder_gate.cpp, so the gate measures
// the code that runs.

#include "ocr_ort.h"
#include "subtitle_finder.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace live::finder {

/// `own(text)`: true for a line that is our own overlay read back, which is dropped.
std::vector<Observed> observe(OcrOrt& ocr, const uint8_t* bgra, int width, int height, std::string& error,
                              int recognize = 1, const std::function<bool(const std::string&)>& own = {});

} // namespace live::finder

// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 Makine Çeviri. Commons Clause applies — see LICENSE.

#pragma once

// Python str semantics on UTF-32 text. The text rules (garbage.cpp, subtitle_timing.cpp,
// local_pool.cpp) are ports of the Python harness and count, classify and lowercase
// characters exactly as CPython does; the tables come from tools/gen_unicode.py.

#include <string>
#include <string_view>
#include <vector>

namespace live::text {

std::u32string fromUtf8(std::string_view utf8);   // invalid bytes become U+FFFD
std::string toUtf8(std::u32string_view text);

// str.isalpha() / isdecimal() / isdigit() / isnumeric() / islower() / isupper() / isspace() of one character
bool isAlpha(char32_t c);
bool isDecimal(char32_t c);   // re \d
bool isDigit(char32_t c);
bool isNumeric(char32_t c);
bool isLower(char32_t c);
bool isUpper(char32_t c);
bool isTitle(char32_t c);     // category Lt
bool isSpace(char32_t c);     // re \s, str.split()
bool isNumber(char32_t c);    // category N*
inline bool isAlnum(char32_t c) { return isAlpha(c) || isDecimal(c) || isDigit(c) || isNumeric(c); }
inline bool isWord(char32_t c) { return isAlnum(c) || c == U'_'; }                         // re \w
inline bool isWordLetter(char32_t c) { return isWord(c) && !isDecimal(c) && c != U'_'; }  // re [^\W\d_]

std::u32string lower(std::u32string_view s);       // str.lower(), full mapping (İ → i̇, final sigma)
std::u32string strip(std::u32string_view s);       // str.strip()
std::u32string strip(std::u32string_view s, std::u32string_view chars);   // str.strip(chars)
std::vector<std::u32string> split(std::u32string_view s);                  // str.split()
std::vector<std::u32string> split(std::u32string_view s, char32_t sep);    // str.split(sep)
std::u32string join(const std::vector<std::u32string>& parts, std::u32string_view sep);
bool strIsAlpha(std::u32string_view s);   // str.isalpha()
bool strIsUpper(std::u32string_view s);   // str.isupper()
size_t count(std::u32string_view s, char32_t c);
bool startsWith(std::u32string_view s, std::u32string_view prefix);
std::u32string asciiLower(std::u32string_view s);

/// difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()
double sequenceRatio(std::u32string_view a, std::u32string_view b);

} // namespace live::text

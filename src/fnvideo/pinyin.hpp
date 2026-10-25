// ps5-homebrew-ui - Offline Han -> pinyin, for the search box's Latin keyboard.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console's on-screen keyboard types Latin letters, so a Han title can never
// be typed directly. This turns a title into the pinyin a viewer can type in
// stead: the full syllables ("jidaоcheshen") and their initials ("jdcs"), which
// fnvideo.cpp matches against the query. The codepoint table lives in
// pinyin_data.inc, generated once at authoring time by _gen_pinyin.py (pypinyin)
// and compiled into the app, so there is no runtime dictionary or network call.

#pragma once

#include <string>
#include <string_view>

namespace hui::fnvideo
{
// Lowercase pinyin for a UTF-8 string. Han characters become their default
// toneless syllable; ASCII letters and digits pass through lowercased; spaces,
// punctuation and non-CJK scripts are dropped. When `initials` is non-null it
// receives the first letter of each syllable, so "极盗车神" yields "jidaоcheshen"
// and "jdcs". Deterministic and allocation-light: safe to call per list row.
std::string pinyin_of(std::string_view utf8, std::string *initials = nullptr);

} // namespace hui::fnvideo

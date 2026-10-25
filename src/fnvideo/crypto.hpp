// ps5-homebrew-ui - MD5 and SHA-256, the two digests the 飞牛影视 API asks for.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console's libc ships no OpenSSL and the app links no crypto archive, so
// both digests are spelled out here. They are used on short strings only - a
// canonical query, a JSON body, a password - so a straightforward
// block-at-a-time implementation is fast enough and keeps the code auditable.
//
//   * MD5 signs every request (`authx`) and the API's own `sign` parameter.
//   * SHA-256 hashes the account password before it leaves the console; the
//     server stores nothing else, and 0.9.5+ refuses a plain-text password.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hui::fnvideo
{

// Lower-case hex, 32 characters.
std::string md5_hex(std::string_view data);

// Lower-case hex, 64 characters.
std::string sha256_hex(std::string_view data);

} // namespace hui::fnvideo

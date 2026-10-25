// ps5-homebrew-ui - The 飞牛影视 request signature. See authx.hpp for the why.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnvideo/authx.hpp"

#include "fnvideo/crypto.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace hui::fnvideo
{
namespace
{

// Fixed by the shipped web client; not a per-install value, so it is a
// constant here rather than something the settings page has to collect.
constexpr const char *kSecret = "NDzZTVxnRKP8Z0jXg1VAMonaG8akvh";

// Stored XOR-169 in the bundle to keep it out of a plain grep. Decodes to
// "16CCEB3D-AB42-077D-36A1-F355324E4237".
constexpr const char *kAppKey = "16CCEB3D-AB42-077D-36A1-F355324E4237";

bool is_get(const SignRequest &request)
{
    return request.method == "GET" || request.method == "get";
}

// What the API digests for this request: the query for a GET, the body bytes
// for anything else.
std::string payload_digest(const SignRequest &request)
{
    if (is_get(request))
    {
        return md5_hex(canonical_query(request.query));
    }
    return md5_hex(request.body);
}

std::string random_nonce()
{
    // Six digits, 100000..999999, matching the web client's range. The nonce
    // only has to be fresh within the timestamp's window, not unguessable.
    static unsigned state = 0;
    if (state == 0)
    {
        state = static_cast<unsigned>(std::chrono::system_clock::now().time_since_epoch().count() &
                                      0x7fffffffu);
        if (state == 0)
        {
            state = 0x9e3779b9u;
        }
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const unsigned value = 100000u + (state % 900000u);
    return std::to_string(value);
}

} // namespace

std::string canonical_query(const std::vector<std::pair<std::string, std::string>> &query)
{
    std::vector<std::pair<std::string, std::string>> sorted(query);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });

    std::string out;
    for (const auto &entry : sorted)
    {
        if (!out.empty())
        {
            out.push_back('&');
        }
        out += entry.first;
        out.push_back('=');
        out += entry.second;
    }
    return out;
}

std::string authx_header(const SignRequest &request, const std::string &nonce,
                         const std::string &timestamp)
{
    std::string payload(kSecret);
    payload.push_back('_');
    payload += request.path;
    payload.push_back('_');
    payload += nonce;
    payload.push_back('_');
    payload += timestamp;
    payload.push_back('_');
    payload += payload_digest(request);
    payload.push_back('_');
    payload += kAppKey;

    std::string header("nonce=");
    header += nonce;
    header += "&timestamp=";
    header += timestamp;
    header += "&sign=";
    header += md5_hex(payload);
    return header;
}

std::string authx_header(const SignRequest &request)
{
    return authx_header(request, random_nonce(), now_millis());
}

std::string password_digest(const std::string &plain)
{
    return sha256_hex(plain);
}

std::string now_millis()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(millis);
}

} // namespace hui::fnvideo

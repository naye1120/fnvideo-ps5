// ps5-homebrew-ui - The 飞牛影视 request signature (`authx`).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every call except `sys/version` and `server/info` is refused with
// {"code":5000,"msg":"invalid sign"} unless it carries
//
//     authx: nonce=<6 digits>&timestamp=<unix ms>&sign=<md5>
//
// where the digest is taken over a secret, the path, the nonce, the timestamp,
// a digest of the request's payload, and an app key. The web client builds the
// payload digest differently per verb: a GET hashes its sorted query string, a
// POST hashes the exact JSON bytes it is about to send. Both forms are
// reproduced here, because a single wrong byte in either is indistinguishable
// from a wrong password on the wire.
//
// This was reverse-engineered from the shipped web bundle; _research/
// fnvideo_routes.md records how, and _research/fnvideo_probe.py is the Python
// twin of this file that was checked against the live NAS.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace hui::fnvideo
{

// The parts of a request the signature covers.
struct SignRequest
{
    std::string method; // "GET" or "POST"
    std::string path;   // pathname only, e.g. "/v/api/v1/mediadb/sum"
    // Decoded query pairs. Order does not matter: the canonical form sorts.
    std::vector<std::pair<std::string, std::string>> query;
    // The bytes actually sent. Ignored for GET, where `query` is hashed.
    std::string body;
};

// "a=1&b=2" with the keys sorted. This is what the API hashes for a GET: the
// web client serialises the query, percent-encodes it, then decodes it again
// before digesting, so the digest sees the plain values.
std::string canonical_query(const std::vector<std::pair<std::string, std::string>> &query);

// The `authx` header value for a caller-supplied nonce and timestamp. Kept
// separate from the clock so tests can pin both and compare a fixed digest.
std::string authx_header(const SignRequest &request, const std::string &nonce,
                         const std::string &timestamp);

// Same, with a fresh nonce and the current wall clock.
std::string authx_header(const SignRequest &request);

// The value the account password is sent as: sha256 of the plain text, hex.
// Version 0.9.5 and later reject a plain-text password outright.
std::string password_digest(const std::string &plain);

// Milliseconds since the Unix epoch, as the decimal string the header wants.
std::string now_millis();

} // namespace hui::fnvideo

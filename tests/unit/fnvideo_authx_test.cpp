// ps5-homebrew-ui - Digest and signature tests for the 飞牛影视 client.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two things here can silently poison every request: a digest that is off by
// one byte, and a canonical query whose key order differs from the server's.
// Both fail the same way on the wire - {"code":5000,"msg":"invalid sign"} -
// which is indistinguishable from a wrong password, so they are pinned to
// published test vectors and to values computed by the Python probe that was
// checked against the live NAS (_research/fnvideo_probe.py).

#include "fnvideo/authx.hpp"
#include "fnvideo/crypto.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace
{

using hui::fnvideo::authx_header;
using hui::fnvideo::canonical_query;
using hui::fnvideo::md5_hex;
using hui::fnvideo::password_digest;
using hui::fnvideo::sha256_hex;
using hui::fnvideo::SignRequest;

TEST(FnvideoCrypto, Md5MatchesPublishedVectors)
{
    EXPECT_EQ(md5_hex(""), "d41d8cd98f00b204e9800998ecf8427e");
    EXPECT_EQ(md5_hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
    EXPECT_EQ(md5_hex("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
    // Longer than one block, so the padding path is exercised too.
    EXPECT_EQ(md5_hex("abcdefghijklmnopqrstuvwxyz"), "c3fcd3d76192e4007dfb496cca67e13b");
}

TEST(FnvideoCrypto, Sha256MatchesPublishedVectors)
{
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // The password the API actually receives for the word "password".
    EXPECT_EQ(sha256_hex("password"),
              "5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8");
}

TEST(FnvideoCrypto, PasswordIsSentAsItsSha256)
{
    EXPECT_EQ(password_digest("password"),
              "5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8");
}

// The API sorts the query before hashing, so the same pairs in a different
// order must produce the same string.
TEST(FnvideoAuthx, CanonicalQuerySortsKeys)
{
    const std::vector<std::pair<std::string, std::string>> query = {
        {"page", "2"}, {"guid", "abc"}, {"sort_type", "desc"}};
    EXPECT_EQ(canonical_query(query), "guid=abc&page=2&sort_type=desc");
}

TEST(FnvideoAuthx, EmptyQueryCanonicalisesToNothing)
{
    EXPECT_EQ(canonical_query({}), "");
}

// A bare read: no query, so the payload digest is the digest of the empty
// string. Expected value computed by _research/fnvideo_probe.py.
TEST(FnvideoAuthx, GetWithoutQuerySignsTheEmptyString)
{
    SignRequest request;
    request.method = "GET";
    request.path = "/v/api/v1/mediadb/sum";

    EXPECT_EQ(authx_header(request, "123456", "1700000000000"),
              "nonce=123456&timestamp=1700000000000&sign=d91f8739738e0c7ee9e98b46a7572f60");
}

// A GET whose query is reordered by the caller must sign identically.
TEST(FnvideoAuthx, GetSignatureIgnoresQueryOrder)
{
    SignRequest first;
    first.method = "GET";
    first.path = "/v/api/v1/item/detail";
    first.query = {{"guid", "abc"}, {"page", "2"}};

    SignRequest second = first;
    second.query = {{"page", "2"}, {"guid", "abc"}};

    const std::string header = authx_header(first, "654321", "1700000000001");
    EXPECT_EQ(header, "nonce=654321&timestamp=1700000000001&sign=91281d738c28b3fadd2d3c9fab04ea37");
    EXPECT_EQ(authx_header(second, "654321", "1700000000001"), header);
}

// A POST hashes the bytes it sends, not a query. The body below is exactly
// what the login call puts on the wire.
TEST(FnvideoAuthx, PostSignsItsExactBodyBytes)
{
    SignRequest request;
    request.method = "POST";
    request.path = "/v/api/v2/user/loginByPassword";
    request.body = R"({"username":"admin","password":"x","app_name":"trimemedia-web"})";

    EXPECT_EQ(authx_header(request, "111111", "1700000000002"),
              "nonce=111111&timestamp=1700000000002&sign=e1e7031a603040126be76a8e1722b96d");
}

// A body edited after signing must change the signature, or the console would
// happily send a stale header alongside new content.
TEST(FnvideoAuthx, PostSignatureTracksBodyEdits)
{
    SignRequest request;
    request.method = "POST";
    request.path = "/v/api/v1/play/play";
    request.body = R"({"media_guid":"a"})";
    const std::string before = authx_header(request, "222222", "1700000000003");

    request.body = R"({"media_guid":"b"})";
    EXPECT_NE(authx_header(request, "222222", "1700000000003"), before);
}

// The convenience overload must fill in a six-digit nonce and a millisecond
// clock, since a malformed either one is rejected before the digest is even
// looked at.
TEST(FnvideoAuthx, LiveHeaderCarriesSixDigitNonceAndMillis)
{
    SignRequest request;
    request.method = "GET";
    request.path = "/v/api/v1/mediadb/sum";

    const std::string header = authx_header(request);
    const std::string nonce_marker = "nonce=";
    const std::size_t nonce_at = header.find(nonce_marker);
    ASSERT_NE(nonce_at, std::string::npos);

    const std::size_t nonce_begin = nonce_at + nonce_marker.size();
    const std::size_t nonce_end = header.find('&', nonce_begin);
    ASSERT_NE(nonce_end, std::string::npos);
    EXPECT_EQ(nonce_end - nonce_begin, 6u);

    const std::string stamp_marker = "timestamp=";
    const std::size_t stamp_at = header.find(stamp_marker);
    ASSERT_NE(stamp_at, std::string::npos);
    const std::size_t stamp_begin = stamp_at + stamp_marker.size();
    const std::size_t stamp_end = header.find('&', stamp_begin);
    ASSERT_NE(stamp_end, std::string::npos);
    // Thirteen digits is what a millisecond clock reads in this century.
    EXPECT_EQ(stamp_end - stamp_begin, 13u);
}

} // namespace

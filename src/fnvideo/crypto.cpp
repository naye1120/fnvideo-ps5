// ps5-homebrew-ui - MD5 and SHA-256. See crypto.hpp for why these live here.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnvideo/crypto.hpp"

#include <array>

namespace hui::fnvideo
{
namespace
{

// Little-endian byte access: MD5 is defined on little-endian words, SHA-256 on
// big-endian ones, and the console's SPU has no unaligned load to lean on.
std::uint32_t load_le32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint32_t load_be32(const std::uint8_t *p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::uint32_t rotl(std::uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32u - n));
}

std::uint32_t rotr(std::uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

std::string to_hex(const std::uint8_t *bytes, std::size_t count)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(count * 2, '0');
    for (std::size_t i = 0; i < count; ++i)
    {
        out[i * 2] = kDigits[bytes[i] >> 4];
        out[i * 2 + 1] = kDigits[bytes[i] & 0x0f];
    }
    return out;
}

// Appends the 1-bit, the zero padding and the 64-bit bit length. `big_endian`
// selects where the length goes, which is the only structural difference
// between the two digests.
std::string pad_message(std::string_view data, bool big_endian)
{
    std::string padded(data);
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8u;
    padded.push_back(static_cast<char>(0x80));
    while (padded.size() % 64 != 56)
    {
        padded.push_back('\0');
    }
    for (int i = 0; i < 8; ++i)
    {
        const int shift = big_endian ? (7 - i) * 8 : i * 8;
        padded.push_back(static_cast<char>((bit_length >> shift) & 0xff));
    }
    return padded;
}

constexpr std::array<std::uint32_t, 64> kMd5K = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
    0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
    0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
    0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
    0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
    0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
    0xeb86d391u,
};

constexpr std::array<unsigned, 64> kMd5Shift = {
    7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
    14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
    4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21,
};

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

} // namespace

std::string md5_hex(std::string_view data)
{
    std::uint32_t a0 = 0x67452301u;
    std::uint32_t b0 = 0xefcdab89u;
    std::uint32_t c0 = 0x98badcfeu;
    std::uint32_t d0 = 0x10325476u;

    const std::string padded = pad_message(data, false);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(padded.data());

    for (std::size_t offset = 0; offset < padded.size(); offset += 64)
    {
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i)
        {
            m[i] = load_le32(bytes + offset + i * 4);
        }

        std::uint32_t a = a0;
        std::uint32_t b = b0;
        std::uint32_t c = c0;
        std::uint32_t d = d0;

        for (unsigned i = 0; i < 64; ++i)
        {
            std::uint32_t f = 0;
            unsigned g = 0;
            if (i < 16)
            {
                f = (b & c) | (~b & d);
                g = i;
            }
            else if (i < 32)
            {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            }
            else if (i < 48)
            {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            }
            else
            {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }

            const std::uint32_t temp = d;
            d = c;
            c = b;
            b = b + rotl(a + f + kMd5K[i] + m[g], kMd5Shift[i]);
            a = temp;
        }

        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    std::uint8_t digest[16];
    const std::uint32_t state[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; ++i)
    {
        digest[i * 4] = static_cast<std::uint8_t>(state[i] & 0xff);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 8) & 0xff);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state[i] >> 16) & 0xff);
        digest[i * 4 + 3] = static_cast<std::uint8_t>((state[i] >> 24) & 0xff);
    }
    return to_hex(digest, sizeof(digest));
}

std::string sha256_hex(std::string_view data)
{
    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    const std::string padded = pad_message(data, true);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(padded.data());

    for (std::size_t offset = 0; offset < padded.size(); offset += 64)
    {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
        {
            w[i] = load_be32(bytes + offset + i * 4);
        }
        for (int i = 16; i < 64; ++i)
        {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0];
        std::uint32_t b = h[1];
        std::uint32_t c = h[2];
        std::uint32_t d = h[3];
        std::uint32_t e = h[4];
        std::uint32_t f = h[5];
        std::uint32_t g = h[6];
        std::uint32_t hh = h[7];

        for (int i = 0; i < 64; ++i)
        {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;

            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::uint8_t digest[32];
    for (int i = 0; i < 8; ++i)
    {
        digest[i * 4] = static_cast<std::uint8_t>((h[i] >> 24) & 0xff);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xff);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xff);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xff);
    }
    return to_hex(digest, sizeof(digest));
}

} // namespace hui::fnvideo

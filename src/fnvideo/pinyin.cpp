// ps5-homebrew-ui - Han -> pinyin lookup over the generated codepoint table.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnvideo/pinyin.hpp"

#include <cctype>
#include <cstddef>

namespace hui::fnvideo
{
namespace
{

struct Entry
{
    unsigned cp;
    char py[8];
};

// Rows are emitted codepoint-ascending by _gen_pinyin.py, so a binary search
// finds a syllable in ~15 steps regardless of library size.
const Entry kTable[] = {
#include "pinyin_data.inc"
};

constexpr std::size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);

const char *syllable_of(unsigned cp)
{
    std::size_t low = 0;
    std::size_t high = kTableSize;
    while (low < high)
    {
        const std::size_t mid = low + (high - low) / 2;
        if (kTable[mid].cp < cp)
        {
            low = mid + 1;
        }
        else if (kTable[mid].cp > cp)
        {
            high = mid;
        }
        else
        {
            return kTable[mid].py;
        }
    }
    return nullptr;
}

// Decode the UTF-8 codepoint at `i` and advance `i` past it. A bad lead byte or
// a truncated sequence yields 0 and advances one, so malformed input can never
// loop or read past the end.
unsigned decode(const std::string &text, std::size_t &i)
{
    const unsigned lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80u)
    {
        ++i;
        return lead;
    }
    std::size_t length = 0;
    unsigned cp = 0;
    if ((lead & 0xE0u) == 0xC0u)
    {
        length = 2;
        cp = lead & 0x1Fu;
    }
    else if ((lead & 0xF0u) == 0xE0u)
    {
        length = 3;
        cp = lead & 0x0Fu;
    }
    else if ((lead & 0xF8u) == 0xF0u)
    {
        length = 4;
        cp = lead & 0x07u;
    }
    else
    {
        ++i;
        return 0;
    }
    if (i + length > text.size())
    {
        ++i;
        return 0;
    }
    for (std::size_t k = 1; k < length; ++k)
    {
        const unsigned cont = static_cast<unsigned char>(text[i + k]);
        if ((cont & 0xC0u) != 0x80u)
        {
            ++i;
            return 0;
        }
        cp = (cp << 6) | (cont & 0x3Fu);
    }
    i += length;
    return cp;
}

} // namespace

std::string pinyin_of(std::string_view utf8, std::string *initials)
{
    const std::string text(utf8);
    std::string out;
    std::string init;
    out.reserve(text.size() * 2u);
    if (initials != nullptr)
    {
        init.reserve(text.size());
    }
    std::size_t i = 0;
    while (i < text.size())
    {
        const unsigned cp = decode(text, i);
        if (cp == 0)
        {
            continue;
        }
        if (cp < 0x80u)
        {
            // ASCII letters and digits fold into the haystack lowercased; spaces
            // and punctuation are dropped so "Baby Driver" reads "babydriver".
            const char ch = static_cast<char>(cp);
            if (std::isdigit(static_cast<unsigned char>(ch)))
            {
                out.push_back(ch);
                if (initials != nullptr)
                {
                    init.push_back(ch);
                }
            }
            else if (std::isalpha(static_cast<unsigned char>(ch)))
            {
                const char low = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                out.push_back(low);
                if (initials != nullptr)
                {
                    init.push_back(low);
                }
            }
            continue;
        }
        if (const char *py = syllable_of(cp); py != nullptr)
        {
            out.append(py);
            if (initials != nullptr)
            {
                init.push_back(py[0]);
            }
        }
        // A Han outside the table, or any other non-ASCII script, contributes
        // nothing to the Latin-typed query.
    }
    if (initials != nullptr)
    {
        *initials = std::move(init);
    }
    return out;
}

} // namespace hui::fnvideo

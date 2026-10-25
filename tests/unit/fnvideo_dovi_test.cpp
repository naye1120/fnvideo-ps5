// ps5-homebrew-ui - Dolby Vision profile 5 RPU parsing and LUT tests.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The captured RPU is a real frame from 《抓特务》. It is the only check that
// the bitstream reader and the colour step still agree after a refactor: a
// wrong offset or a BT.2100 matrix both produce a plausible-looking picture
// that is simply the wrong colour, which no screenshot test can catch.

#include "core/save_file.hpp"
#include "fnvideo/dovi.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#ifndef HUI_SOURCE_DIR
#define HUI_SOURCE_DIR "."
#endif

namespace
{

std::vector<std::uint8_t> read_rpu(const char *name)
{
    std::vector<std::uint8_t> bytes;
    const std::string path = std::string(HUI_SOURCE_DIR) + "/tests/fixtures/" + name;
    std::string data;
    if (!hui::save::read_file(path, &data, 1u << 20))
    {
        return bytes;
    }
    bytes.assign(data.begin(), data.end());
    return bytes;
}

// The captures are bare NAL units; put a start code in front so the same
// annex-B path the console uses finds the type-62 header.
std::vector<std::uint8_t> with_start_code(std::vector<std::uint8_t> nal)
{
    std::vector<std::uint8_t> framed;
    framed.reserve(nal.size() + 4);
    framed.push_back(0);
    framed.push_back(0);
    framed.push_back(0);
    framed.push_back(1);
    framed.insert(framed.end(), nal.begin(), nal.end());
    return framed;
}

} // namespace

TEST(FnvideoDovi, ParsesProfileFiveColourStep)
{
    const std::vector<std::uint8_t> raw = read_rpu("rpu_4e78b627.bin");
    ASSERT_FALSE(raw.empty()) << "captured RPU is missing";
    const std::vector<std::uint8_t> packet = with_start_code(raw);

    hui::fnvideo::DoviRpu rpu;
    rpu.parse_packet(packet.data(), packet.size(), true);

    ASSERT_TRUE(rpu.ready());
    const hui::fnvideo::DoviFrame &frame = rpu.frame();
    EXPECT_TRUE(frame.full_range);
    EXPECT_EQ(frame.bit_depth, 10);
    ASSERT_TRUE(frame.has_colour_step);
    ASSERT_TRUE(frame.has_ycc_to_rgb);

    // The RPU stores these as integers scaled by 2^28. Dolby's neutral is
    // (0, 0.5, 0.5), not (0, 0, 0): the old code added the raw integer to a
    // normalised code and pushed every chroma sample half a unit off.
    EXPECT_NEAR(frame.ycc_to_rgb_offset[0], 0.0f, 1.0e-5f);
    EXPECT_NEAR(frame.ycc_to_rgb_offset[1], 0.5f, 1.0e-5f);
    EXPECT_NEAR(frame.ycc_to_rgb_offset[2], 0.5f, 1.0e-5f);

    // Dolby's rgb_to_lms is composed into the HPE inverse, so the result is
    // close to, but not exactly, the bare 3.064/1.786/1.030 base matrix. What
    // matters is that it stays on Dolby's side of the divide: BT.2100's ICtCp
    // inverse has a 4.1 diagonal here, which is the magenta cast.
    ASSERT_TRUE(frame.has_rgb_to_lms);
    EXPECT_GT(frame.rgb_from_lms[0], 2.9f);
    EXPECT_LT(frame.rgb_from_lms[0], 3.6f);
    EXPECT_GT(frame.rgb_from_lms[4], 1.5f);
    EXPECT_LT(frame.rgb_from_lms[4], 2.2f);
    EXPECT_GT(frame.rgb_from_lms[8], 0.9f);
    EXPECT_LT(frame.rgb_from_lms[8], 1.3f);

    float luma[1024] = {};
    float ct[1024] = {};
    float cp[1024] = {};
    ASSERT_TRUE(hui::fnvideo::dovi_reshape_lut(frame, 0, luma, 1024));
    ASSERT_TRUE(hui::fnvideo::dovi_reshape_lut(frame, 1, ct, 1024));
    ASSERT_TRUE(hui::fnvideo::dovi_reshape_lut(frame, 2, cp, 1024));
    // Neutral chroma is near 461/409 in this frame's own coded range. The
    // exact neutral belongs in the RPU offset, but the curve has to agree:
    // if it maps those codes to zero instead of 0.5, even the correct offset
    // cannot recover the colour.
    EXPECT_NEAR(ct[461], 0.5f, 2.0e-2f);
    EXPECT_NEAR(cp[409], 0.5f, 2.0e-2f);
}

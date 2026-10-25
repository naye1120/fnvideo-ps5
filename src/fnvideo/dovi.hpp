// ps5-homebrew-ui - Dolby Vision RPU decoding for profile 5.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Profile 5 has no HDR10 layer to fall back on: its single HEVC layer is
// IPTPQc2 whose samples have been *reshaped*, and the polynomial that undoes
// that lives in a per-frame RPU (NAL unit type 62) that nothing else in this
// app reads. Without it the film is squeezed into the bottom third of the PQ
// range - measured on a real frame, the code values sat between 70 and 572 of
// 0..1023, a median of 0.13 nits - which is why it looked dark, grey and
// slightly wrong in hue no matter what matrix the shader used.
//
// The console decodes the picture in hardware and never looks at the RPU, so
// it is parsed here, out of the packets, before they are handed over.

#pragma once

#include <cstddef>
#include <cstdint>

namespace hui::fnvideo
{

// One frame's worth of reshaping, flattened out of the RPU into plain numbers
// so the renderer never has to know about the bitstream.
//
// The curves are piece-wise polynomials over the *coded* component values: for
// component c, find the piece whose pivot interval contains the sample, then
// evaluate coef[c][piece][0..2] as x^0, x^1, x^2 and shift right by
// coef_log2_denom. That result is what the colour matrix then turns into RGB.
//
// Two details cost a day to get right and are not obvious from any header:
//
//   * x is the *absolute* normalised code value (code / 1023), not the offset
//     inside the piece's own pivot interval.
//   * coef[0] is the constant term, and the coefficients arrive in that order.
//
// Checked against a real profile 5 RPU: evaluated this way the eight pieces of
// the luma curve meet at every pivot to within a thousandth. Evaluated with
// x relative to the piece they do not meet at all.
struct DoviFrame
{
    bool valid = false;
    bool full_range = false; // bl_video_full_range_flag
    int bit_depth = 10;      // bl_bit_depth
    int coef_log2_denom = 0;
    int nlq_method = -1; // -1 none, 0 linear dead zone
    int num_pivots[3] = {0, 0, 0};
    std::uint16_t pivots[3][9] = {};
    std::uint8_t order[3][8] = {};   // 0 means "MMR, unsupported here"
    std::int64_t coef[3][8][3] = {}; // fixed point, x^0 x^1 x^2
    std::uint16_t nlq_offset[3] = {};
    std::uint64_t vdr_in_max[3] = {};
    std::uint64_t nlq_slope[3] = {};
    std::uint64_t nlq_threshold[3] = {};
    // Dolby's own IPT matrices, when the RPU carries them. The comment in
    // libavutil/dovi_meta.h is explicit that these replace the frame's colour
    // tags, and that the LMS -> RGB step is a Hunt-Pointer-Estevez transform
    // *without* crosstalk - so the BT.2100 ICtCp matrix is not a substitute.
    bool has_ycc_to_rgb = false;
    float ycc_to_rgb[9] = {}; // row major
    float ycc_to_rgb_offset[3] = {};
    bool has_rgb_to_lms = false;
    float rgb_to_lms[9] = {}; // row major
    // The colour step, pre-composed for the shader. libavutil/dovi_meta.h is
    // explicit about how the two matrices are used, and it is not the obvious
    // way round:
    //
    //   ycc_to_rgb_matrix  /* before PQ linearization */
    //   ycc_to_rgb_offset  /* input offset of neutral value */
    //   rgb_to_lms_matrix  /* after PQ linearization */
    //
    // so
    //
    //   L'M'S' = lms_from_ycc * ((I, Ct, Cp) - ycc_to_rgb_offset)
    //   RGB    = rgb_from_lms * LMS        (after the PQ curve, in nits)
    //
    // with lms_from_ycc = ycc_to_rgb and rgb_from_lms = hpe_lms_to_rgb *
    // rgb_to_lms. Neither is BT.2100's ICtCp matrix, and using that one is
    // what kept the hue wrong after the reshaping was fixed: Dolby's
    // ycc_to_rgb sends Ct to S' at 0.03 where BT.2100 sends it at 0.56, and
    // its LMS base is Hunt-Pointer-Estevez *without crosstalk*
    // (L = 0.4408R + 0.5354G + 0.0238B) whose inverse has a 3.06 diagonal,
    // against BT.2100's 4.10.
    float lms_from_ycc[9] = {};
    float rgb_from_lms[9] = {};
    bool has_colour_step = false;
    int source_min_pq = 0;
    int source_max_pq = 0;
};

class DoviRpu
{
  public:
    DoviRpu();
    ~DoviRpu();
    DoviRpu(const DoviRpu &) = delete;
    DoviRpu &operator=(const DoviRpu &) = delete;

    // Feeds one access unit and keeps the last RPU in it, which is what the
    // reference decoders do. `annex_b` says whether the NAL units are split by
    // start codes (TS, MKV) or by a length prefix (MP4).
    void parse_packet(const std::uint8_t *data, std::size_t size, bool annex_b);

    // True once a mapping has been parsed; the player reads this to decide
    // whether the shader has anything to apply.
    bool ready() const
    {
        return frame_.valid;
    }
    const DoviFrame &frame() const
    {
        return frame_;
    }

    // One line, for the open log: what the RPU actually said.
    void describe(char *out, std::size_t capacity) const;

    // Counters for the probe: a stream with no RPU at all and a stream whose
    // RPU fails to parse look identical from the outside, and they mean very
    // different things.
    void stats(char *out, std::size_t capacity) const;
    int probe_packets() const
    {
        return packets_;
    }

  private:
    bool parse_one(const std::uint8_t *rpu, std::size_t size);
    void note_type(int type);

    DoviFrame frame_{};
    int seen_ = 0;
    // Probe counters (see stats()).
    int packets_ = 0;
    int nals_ = 0;
    int rpu_nals_ = 0;
    int parsed_ = 0;
    int failed_ = 0;
    int nal_types_[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // first four NAL types seen
    // Why the parse failed, so the probe can say it instead of "failed".
    int last_parse_ret_ = 0;
    int rpu_first_size_ = 0;
    unsigned char rpu_first_[16] = {};
};

// Fills `out` with the reshaped value of one component for every input code
// 0..count-1, in the 0..1 PQ domain the colour matrix expects. The encoder
// squeezes the film's own range into the code range, so without this the
// picture sits in the bottom third of PQ: measured on a real frame the luma
// codes ran 70..572 of 0..1023 and the median came out at 0.13 nits instead
// of 0.9.
//
// All three components carry a curve, not just luma: on the same film the
// chroma curves are single linear segments that stretch 0..1 into 0..1.19, a
// mild saturation lift. `component` is 0 for I, 1 for Ct, 2 for Cp.
//
// False when the RPU carries no usable curve for that component; the caller
// then leaves those samples as they are.
bool dovi_reshape_lut(const DoviFrame &frame, int component, float *out, int count);

} // namespace hui::fnvideo

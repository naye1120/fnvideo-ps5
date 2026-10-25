// ps5-homebrew-ui - Dolby Vision RPU decoding for profile 5. See dovi.hpp.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnvideo/dovi.hpp"

#include <cstdio>
#include <cstring>

namespace
{
// NAL unit type that carries a Dolby Vision RPU in HEVC.
constexpr int kRpuNalType = 62;

// An RPU is a few hundred bytes; anything past this is not one.
constexpr std::size_t kMaxRbsp = 4096;

// HEVC stores its payload as an EBSP: every 0x0000 in the RBSP is followed by
// an 0x03 so that no start code can appear inside a NAL. Parsing the EBSP
// directly shifts the whole bit stream by eight bits at the first escape -
// which is exactly what made FFmpeg's parser report signal_bit_depth = 0 here.
std::size_t unescape(const std::uint8_t *in, std::size_t size, std::uint8_t *out,
                     std::size_t capacity)
{
    std::size_t written = 0;
    for (std::size_t i = 0; i < size && written < capacity; ++i)
    {
        if (i + 2 < size && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 3)
        {
            out[written++] = 0;
            out[written++] = 0;
            i += 2;
        }
        else
        {
            out[written++] = in[i];
        }
    }
    return written;
}

// MSB-first bit reader. `ok` goes false on any overrun, and every caller checks
// it once at the end instead of after each field.
struct Bits
{
    const std::uint8_t *data;
    std::size_t size; // bytes
    std::size_t pos;  // bits
    bool ok;

    std::size_t left() const
    {
        return size * 8 - pos;
    }

    std::uint32_t bits(int count)
    {
        if (count < 0 || left() < static_cast<std::size_t>(count))
        {
            ok = false;
            pos = size * 8;
            return 0;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i)
        {
            const std::uint8_t byte = data[pos >> 3];
            value = (value << 1) | ((byte >> (7 - (pos & 7))) & 1u);
            ++pos;
        }
        return value;
    }

    std::int32_t sbits(int count)
    {
        const std::uint32_t raw = bits(count);
        if (count <= 0 || count > 31)
        {
            return 0;
        }
        const std::uint32_t sign = 1u << (count - 1);
        return static_cast<std::int32_t>(raw & sign ? raw - (sign << 1) : raw);
    }

    void skip(int count)
    {
        if (left() < static_cast<std::size_t>(count < 0 ? 0 : count))
        {
            ok = false;
            pos = size * 8;
            return;
        }
        pos += static_cast<std::size_t>(count);
    }

    // Unsigned Exp-Golomb, as HEVC writes it.
    std::uint32_t ue()
    {
        int zeros = 0;
        while (ok && bits(1) == 0)
        {
            if (++zeros > 31)
            {
                ok = false;
                return 0;
            }
        }
        if (!ok)
        {
            return 0;
        }
        return zeros == 0 ? 0 : ((1u << zeros) - 1u + bits(zeros));
    }

    // Signed Exp-Golomb: 0, 1, -1, 2, -2, ...
    std::int32_t se()
    {
        const std::uint32_t k = ue();
        if (!ok)
        {
            return 0;
        }
        return (k & 1u) != 0 ? static_cast<std::int32_t>((k + 1) / 2)
                             : -static_cast<std::int32_t>(k / 2);
    }
};

// A coefficient is a signed integer part followed by coef_log2_denom fractional
// bits, both read as one fixed-point number.
std::int64_t se_coef(Bits &bits, int denom)
{
    const std::int64_t ipart = bits.se();
    const std::int64_t fpart = denom > 0 ? bits.bits(denom) : 0;
    return ipart * (std::int64_t{1} << denom) + fpart;
}

std::uint64_t ue_coef(Bits &bits, int denom)
{
    const std::uint64_t ipart = bits.ue();
    const std::uint64_t fpart = denom > 0 ? bits.bits(denom) : 0;
    return (ipart << denom) + fpart;
}

// 3x3 row-major helpers. The colour step is three of these, all small.
void mat_mul(const float *a, const float *b, float *out)
{
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
        {
            out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] +
                             a[r * 3 + 2] * b[2 * 3 + c];
        }
    }
}

// Dolby's base LMS -> RGB for BT.2020: the inverse of its own IPT crosstalk
//
//     L = 0.4408 R + 0.5354 G + 0.0238 B
//     M = 0.1620 R + 0.7587 G + 0.0794 B
//     S = 0.0000 R + 0.0258 G + 0.9741 B
//
// which is Hunt-Pointer-Estevez *without crosstalk*, as libavutil/dovi_meta.h
// says. BT.2100's ICtCp uses a different matrix (0.3592/0.4054/0.2354, the
// one with crosstalk) whose inverse has a 4.10 diagonal instead of 3.06;
// feeding Dolby's LMS through that one is what left the picture with the
// right brightness and the wrong hue. The RPU's rgb_to_lms_matrix is a small
// per-stream correction applied on top of this base, not a replacement for it.
constexpr float kHpeLmsToRgb[9] = {
    3.06441879f,  -2.16597676f, 0.10155818f,  -0.65612108f, 1.78554118f,
    -0.12943749f, 0.01736321f,  -0.04725154f, 1.03004253f,
};

} // namespace

namespace hui::fnvideo
{

DoviRpu::DoviRpu() = default;
DoviRpu::~DoviRpu() = default;

bool DoviRpu::parse_one(const std::uint8_t *rpu, std::size_t size)
{
    ++rpu_nals_;
    if (rpu_first_size_ == 0)
    {
        rpu_first_size_ = static_cast<int>(size);
        for (int i = 0; i < 16 && static_cast<std::size_t>(i) < size; ++i)
        {
            rpu_first_[i] = rpu[i];
        }
    }
    if (rpu == nullptr || size < 8 || size > kMaxRbsp)
    {
        ++failed_;
        last_parse_ret_ = -1;
        return false;
    }

    std::uint8_t rbsp[kMaxRbsp];
    const std::size_t rbsp_size = unescape(rpu, size, rbsp, sizeof(rbsp));
    Bits bits{rbsp, rbsp_size, 0, true};

    if (bits.bits(8) != 25) // nal_prefix
    {
        ++failed_;
        last_parse_ret_ = -2;
        return false;
    }
    if (bits.bits(6) != 2) // rpu_type: only type 2 carries this syntax
    {
        ++failed_;
        last_parse_ret_ = -3;
        return false;
    }
    const std::uint32_t rpu_format = bits.bits(11);
    bits.skip(4); // vdr_rpu_profile
    bits.skip(4); // vdr_rpu_level

    DoviFrame next;
    int denom = 0;
    int coef_data_type = 0;
    bool disable_residual = true;
    int el_bit_depth = 10;
    int vdr_bit_depth = 12;

    // Every RPU in a profile 5 stream repeats the sequence info. Without it the
    // bit depths default to zero and nothing downstream can be interpreted.
    if (bits.bits(1) != 1)
    {
        ++failed_;
        last_parse_ret_ = -4;
        return false;
    }
    bits.skip(1); // chroma_resampling_explicit_filter_flag
    coef_data_type = static_cast<int>(bits.bits(2));
    if (coef_data_type == 0)
    {
        const std::uint32_t value = bits.ue();
        if (value < 13 || value > 32)
        {
            ++failed_;
            last_parse_ret_ = -5;
            return false;
        }
        denom = static_cast<int>(value);
    }
    else
    {
        denom = 32;
    }
    bits.skip(2); // vdr_rpu_normalized_idc
    next.full_range = bits.bits(1) != 0;
    if ((rpu_format & 0x700u) == 0)
    {
        const std::uint32_t bl = bits.ue();
        const std::uint32_t el = bits.ue();
        const std::uint32_t vdr = bits.ue();
        if (!bits.ok || bl > 8 || el > 8 || vdr > 8)
        {
            ++failed_;
            last_parse_ret_ = -6;
            return false;
        }
        next.bit_depth = static_cast<int>(bl) + 8;
        el_bit_depth = static_cast<int>(el) + 8;
        vdr_bit_depth = static_cast<int>(vdr) + 8;
        bits.skip(1); // spatial_resampling_filter_flag
        bits.skip(3); // reserved_zero_3bits
        bits.skip(1); // el_spatial_resampling_filter_flag
        disable_residual = bits.bits(1) != 0;
    }
    const bool use_nlq = (rpu_format & 0x700u) == 0 && !disable_residual;

    const bool dm_present = bits.bits(1) != 0;
    const bool use_prev = bits.bits(1) != 0;
    const bool header_full_range = next.full_range;

    if (use_prev)
    {
        // The RPU reuses the previous mapping and only (maybe) replaces the
        // colour metadata. Keeping the previous frame alive is what lets that
        // common case work: returning failure here made every such frame fall
        // back to the generic matrices, which is visible as a colour flicker.
        const std::uint32_t previous_id = bits.ue();
        if (!bits.ok || previous_id != 0 || !frame_.valid)
        {
            ++failed_;
            last_parse_ret_ = -7;
            return false;
        }
        next = frame_;
        next.full_range = header_full_range;
    }
    else
    {
        bits.ue(); // vdr_rpu_id
        bits.ue(); // mapping_color_space
        bits.ue(); // mapping_chroma_format_idc

        for (int c = 0; c < 3 && bits.ok; ++c)
        {
            const std::uint32_t count = bits.ue() + 2;
            if (count < 2 || count > 9)
            {
                bits.ok = false;
                break;
            }
            next.num_pivots[c] = static_cast<int>(count);
            std::uint32_t accumulated = 0;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                accumulated += bits.bits(next.bit_depth);
                next.pivots[c][i] = static_cast<std::uint16_t>(accumulated);
            }
        }
        if (!bits.ok)
        {
            ++failed_;
            last_parse_ret_ = -8;
            return false;
        }

        if (use_nlq)
        {
            const std::uint32_t method = bits.bits(3);
            if (method > 1)
            {
                ++failed_;
                last_parse_ret_ = -9;
                return false;
            }
            next.nlq_method = static_cast<int>(method);
        }
        bits.ue(); // num_x_partitions
        bits.ue(); // num_y_partitions

        for (int c = 0; c < 3 && bits.ok; ++c)
        {
            for (int i = 0; i < next.num_pivots[c] - 1 && bits.ok; ++i)
            {
                const std::uint32_t mapping_idc = bits.ue();
                if (mapping_idc == 0)
                {
                    const std::uint32_t poly_order_minus1 = bits.ue();
                    if (poly_order_minus1 > 1)
                    {
                        bits.ok = false;
                        break;
                    }
                    const int order = static_cast<int>(poly_order_minus1) + 1;
                    if (poly_order_minus1 == 0 && bits.bits(1) != 0)
                    {
                        // Linear interpolation between the two pivots. Nothing
                        // in this library uses it, and guessing would be worse
                        // than falling back to the plain curve.
                        bits.ok = false;
                        break;
                    }
                    next.order[c][i] = static_cast<std::uint8_t>(order);
                    for (int k = 0; k <= order && k < 3; ++k)
                    {
                        next.coef[c][i][k] = se_coef(bits, denom);
                    }
                }
                else if (mapping_idc == 1)
                {
                    // MMR: a 3D lookup the shader cannot evaluate. Recorded as
                    // order 0, which the LUT builder treats as a pass-through.
                    const std::uint32_t mmr_order = bits.bits(2) + 1;
                    if (mmr_order > 3)
                    {
                        bits.ok = false;
                        break;
                    }
                    next.order[c][i] = 0;
                    se_coef(bits, denom); // mmr_constant
                    for (std::uint32_t j = 0; j < mmr_order; ++j)
                    {
                        for (int k = 0; k < 7; ++k)
                        {
                            se_coef(bits, denom);
                        }
                    }
                }
                else
                {
                    bits.ok = false;
                    break;
                }
            }
        }
        if (!bits.ok)
        {
            ++failed_;
            last_parse_ret_ = -10;
            return false;
        }

        if (use_nlq)
        {
            for (int c = 0; c < 3 && bits.ok; ++c)
            {
                next.nlq_offset[c] = static_cast<std::uint16_t>(bits.bits(el_bit_depth));
                next.vdr_in_max[c] = ue_coef(bits, denom);
                if (next.nlq_method == 1)
                {
                    next.nlq_slope[c] = ue_coef(bits, denom);
                    next.nlq_threshold[c] = ue_coef(bits, denom);
                }
            }
        }
    }

    if (dm_present)
    {
        bits.ue(); // affected_dm_id
        bits.ue(); // current_dm_id
        bits.ue(); // scene_refresh_flag
        for (int i = 0; i < 9; ++i)
        {
            next.ycc_to_rgb[i] = static_cast<float>(bits.sbits(16)) / 8192.0f;
        }
        for (int i = 0; i < 3; ++i)
        {
            const std::uint32_t raw = bits.bits(32);
            // Profile 5's offsets are unsigned 2^28 fixed point. FFmpeg halves
            // the denominator for values past INT_MAX to fit an AVRational,
            // which leaves the ratio unchanged; a double needs no such dance.
            next.ycc_to_rgb_offset[i] = static_cast<float>(raw / 268435456.0);
        }
        for (int i = 0; i < 9; ++i)
        {
            next.rgb_to_lms[i] = static_cast<float>(bits.sbits(16)) / 16384.0f;
        }
        bits.skip(16); // signal_eotf
        bits.skip(16); // signal_eotf_param0
        bits.skip(16); // signal_eotf_param1
        bits.skip(32); // signal_eotf_param2
        bits.skip(5);  // signal_bit_depth
        bits.skip(2);  // signal_color_space
        bits.skip(2);  // signal_chroma_format
        bits.skip(2);  // signal_full_range_flag
        next.source_min_pq = static_cast<int>(bits.bits(12));
        next.source_max_pq = static_cast<int>(bits.bits(12));
        bits.skip(10); // source_diagonal
        next.has_ycc_to_rgb = true;
        next.has_rgb_to_lms = true;
    }
    else if (frame_.has_ycc_to_rgb && frame_.has_rgb_to_lms)
    {
        // dm_present = 0 means "reuse the current Dolby metadata", not "there
        // is none". Copying the previous step keeps neutrals neutral from one
        // RPU to the next; clearing it was the flicker between good colour and
        // the generic BT.2100 fallback.
        for (int i = 0; i < 9; ++i)
        {
            next.ycc_to_rgb[i] = frame_.ycc_to_rgb[i];
            next.rgb_to_lms[i] = frame_.rgb_to_lms[i];
        }
        for (int i = 0; i < 3; ++i)
        {
            next.ycc_to_rgb_offset[i] = frame_.ycc_to_rgb_offset[i];
        }
        next.has_ycc_to_rgb = true;
        next.has_rgb_to_lms = true;
    }

    if (next.has_ycc_to_rgb && next.has_rgb_to_lms)
    {
        // libavutil/dovi_meta.h spells out the order, and it is easy to get
        // backwards: ycc_to_rgb_matrix goes in *before* PQ linearisation with
        // ycc_to_rgb_offset as the "input offset of neutral value", and
        // rgb_to_lms_matrix goes in *after* linearisation, feeding a BT.2020
        // Hunt-Pointer-Estevez LMS -> RGB matrix. So the shader gets the
        // first matrix as-is (the offset is folded into its constant term
        // there) and the second composed with the HPE inverse.
        for (int i = 0; i < 9; ++i)
        {
            next.lms_from_ycc[i] = next.ycc_to_rgb[i];
        }
        mat_mul(kHpeLmsToRgb, next.rgb_to_lms, next.rgb_from_lms);
        next.has_colour_step = true;
    }

    if (!bits.ok)
    {
        ++failed_;
        last_parse_ret_ = -11;
        return false;
    }
    (void)coef_data_type;
    (void)vdr_bit_depth;
    next.valid = true;
    next.coef_log2_denom = denom;
    frame_ = next;
    ++seen_;
    ++parsed_;
    last_parse_ret_ = 0;
    return true;
}

bool dovi_reshape_lut(const DoviFrame &frame, int component, float *out, int count)
{
    if (out == nullptr || count < 2 || component < 0 || component > 2 || !frame.valid ||
        frame.coef_log2_denom <= 0 || frame.coef_log2_denom > 32 || frame.bit_depth < 8 ||
        frame.bit_depth > 16)
    {
        return false;
    }
    const int c = component;
    const int pivots = frame.num_pivots[c];
    if (pivots < 2)
    {
        return false;
    }
    // The polynomial's argument is the *absolute* normalised code value, not
    // the position inside its own pivot interval, and coef[0] is the constant
    // term. Checked against a real RPU: evaluated that way the eight pieces of
    // a profile 5 luma curve meet at every pivot to within a thousandth, and
    // evaluated any other way they do not meet at all.
    const double denom = static_cast<double>(std::int64_t{1} << frame.coef_log2_denom);
    const double top = static_cast<double>((1 << frame.bit_depth) - 1);
    for (int code = 0; code < count; ++code)
    {
        int piece = 0;
        while (piece + 2 < pivots && code > frame.pivots[c][piece + 1])
        {
            ++piece;
        }
        const int order = frame.order[c][piece];
        double value = 0.0;
        if (order == 0)
        {
            value = static_cast<double>(code) / top; // MMR: pass through
        }
        else
        {
            double power = 1.0;
            const double x = static_cast<double>(code) / top;
            for (int k = 0; k <= order && k < 3; ++k)
            {
                value += static_cast<double>(frame.coef[c][piece][k]) / denom * power;
                power *= x;
            }
        }
        out[code] = static_cast<float>(value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value));
    }
    return true;
}

void DoviRpu::parse_packet(const std::uint8_t *data, std::size_t size, bool annex_b)
{
    if (data == nullptr || size < 4)
    {
        return;
    }
    ++packets_;
    std::size_t pos = 0;
    while (pos + 2 <= size)
    {
        std::size_t length = 0;
        if (annex_b)
        {
            // Skip to the next start code, then measure the NAL that follows.
            std::size_t start = pos;
            while (start + 3 <= size &&
                   !(data[start] == 0 && data[start + 1] == 0 && data[start + 2] == 1))
            {
                ++start;
            }
            if (start + 3 > size)
            {
                return;
            }
            const std::size_t payload = start + 3;
            std::size_t end = payload;
            while (end + 3 <= size && !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 1))
            {
                ++end;
            }
            if (end + 3 > size)
            {
                end = size;
            }
            // Trim the four-byte start code's trailing zero from the previous
            // NAL when it is present.
            while (end > payload && data[end - 1] == 0 && end >= 2 && data[end - 2] == 0)
            {
                --end;
            }
            if (end > payload + 2)
            {
                const int type = (data[payload] >> 1) & 0x3f;
                note_type(type);
                if (type == kRpuNalType)
                {
                    parse_one(data + payload + 2, end - payload - 2);
                }
            }
            pos = end;
        }
        else
        {
            if (pos + 4 > size)
            {
                return;
            }
            length = (static_cast<std::size_t>(data[pos]) << 24) |
                     (static_cast<std::size_t>(data[pos + 1]) << 16) |
                     (static_cast<std::size_t>(data[pos + 2]) << 8) |
                     static_cast<std::size_t>(data[pos + 3]);
            if (length == 0 || length > size - pos - 4)
            {
                return;
            }
            const std::uint8_t *nal = data + pos + 4;
            if (length > 2)
            {
                const int type = (nal[0] >> 1) & 0x3f;
                note_type(type);
                if (type == kRpuNalType)
                {
                    parse_one(nal + 2, length - 2);
                }
            }
            pos += 4 + length;
        }
    }
}

void DoviRpu::note_type(int type)
{
    ++nals_;
    for (int i = 0; i < 8; ++i)
    {
        if (nal_types_[i] == type || nal_types_[i] == 0)
        {
            if (nal_types_[i] == 0)
            {
                nal_types_[i] = type == 0 ? -1 : type; // -1 marks "type 0"
            }
            return;
        }
    }
}

void DoviRpu::stats(char *out, std::size_t capacity) const
{
    if (out == nullptr || capacity == 0)
    {
        return;
    }
    std::snprintf(out, capacity,
                  "dovi probe: packets=%d nals=%d rpu_nals=%d parsed=%d failed=%d "
                  "types=%d,%d,%d,%d,%d,%d,%d,%d",
                  packets_, nals_, rpu_nals_, parsed_, failed_, nal_types_[0], nal_types_[1],
                  nal_types_[2], nal_types_[3], nal_types_[4], nal_types_[5], nal_types_[6],
                  nal_types_[7]);
    if (capacity > 160)
    {
        char extra[192];
        std::snprintf(extra, sizeof(extra),
                      " parse_ret=%d rpu0_size=%d "
                      "rpu0=%02x%02x%02x%02x%02x%02x%02x%02x",
                      last_parse_ret_, rpu_first_size_, rpu_first_[0], rpu_first_[1], rpu_first_[2],
                      rpu_first_[3], rpu_first_[4], rpu_first_[5], rpu_first_[6], rpu_first_[7]);
        const std::size_t used = std::strlen(out);
        std::snprintf(out + used, capacity - used, "%s", extra);
    }
}

void DoviRpu::describe(char *out, std::size_t capacity) const
{
    if (out == nullptr || capacity == 0)
    {
        return;
    }
    if (!frame_.valid)
    {
        std::snprintf(out, capacity, "rpu: none seen (%d parsed, %d failed)", parsed_, failed_);
        return;
    }
    std::snprintf(out, capacity,
                  "rpu: full=%d depth=%d log2denom=%d nlq=%d pivots=%d/%d/%d order=%d/%d/%d "
                  "ycc=%d lms=%d pq=%d..%d",
                  frame_.full_range ? 1 : 0, frame_.bit_depth, frame_.coef_log2_denom,
                  frame_.nlq_method, frame_.num_pivots[0], frame_.num_pivots[1],
                  frame_.num_pivots[2], frame_.order[0][0], frame_.order[1][0], frame_.order[2][0],
                  frame_.has_ycc_to_rgb ? 1 : 0, frame_.has_rgb_to_lms ? 1 : 0,
                  frame_.source_min_pq, frame_.source_max_pq);
}

} // namespace hui::fnvideo

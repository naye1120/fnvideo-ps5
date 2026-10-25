// ps5-homebrew-ui - OpenGL backend for the 2D draw list.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gfx/draw_list.hpp"
#include "gfx/font.hpp"

#include <GL/glcorearb.h>

#include <cstddef>
#include <cstdint>

namespace hui::gfx
{

// Draws a DrawList with one instanced program: all instances are uploaded
// once per frame into an orphaned stream buffer, then each run is one
// glDrawArraysInstancedBaseInstance call (triangle lists only).
class GlBatch
{
  public:
    GlBatch() = default;
    GlBatch(const GlBatch &) = delete;
    GlBatch &operator=(const GlBatch &) = delete;
    ~GlBatch();

    bool init();
    // Deletes the programs, vertex arrays and buffers; init() recreates them.
    void release();
    // Uploads a font atlas as a single-level R8 texture and gives it one of
    // the font slots; returns the handle DrawList::text takes. With every
    // slot taken it returns a plain texture name, which also works but starts
    // a new draw call wherever that font is used.
    std::uint32_t create_font_texture(const Font &font);
    // Uploads RGBA8 pixels as a single-level texture; returns its name.
    std::uint32_t create_texture(int width, int height, const std::uint8_t *rgba);
    // Replaces the pixels of a texture made by create_texture, at the same
    // size. A video frame arrives sixty times a second, and allocating a new
    // texture for each one would leave the GPU holding a queue of them.
    void update_texture(std::uint32_t texture, int width, int height, const std::uint8_t *rgba);
    // Frees a texture made by create_texture.
    void destroy_texture(std::uint32_t texture);

    // ---- video ----
    // A film's picture kept as the planes the decoder produced, so the shader
    // does the YUV to RGB conversion and the scaling. Uploading RGBA instead
    // meant an sws pass and a full-size copy per picture on the CPU, which is
    // what kept 4K films at 1080p and still stuttered.
    //
    // One video at a time (only one film plays). ten_bit selects 16-bit plane
    // storage. Where the ten bits sit depends on the decoder: the console
    // leaves them low-aligned, FFmpeg's P010LE puts them in the top of the
    // word; set_video_colorspace carries that difference to the shader.
    // Recreates the storage when the geometry or the bit depth changes.
    void create_video_texture(int width, int height, bool ten_bit);
    // planar_chroma = false: u holds interleaved UV (NV12/P010) and v is
    // ignored. true: u is the U plane and v the V plane (planar 4:2:0).
    void update_video_texture(const std::uint8_t *y, int y_pitch, const std::uint8_t *u,
                              int u_pitch, const std::uint8_t *v, int v_pitch, bool planar_chroma);
    void destroy_video_texture();
    // Dolby Vision profile 5's inverse reshaping, as a lookup table: `lut` holds
    // `count` samples in 0..1, one per input code value, with three rows - I,
    // Ct and Cp - laid out one after another. Built on the CPU from the RPU
    // and uploaded once per scene, because the curve is a piece-wise
    // polynomial and evaluating it per pixel is both slower and branchier than
    // one texel fetch. Passing a null table leaves the samples as a ramp.
    void set_dovi_reshape(const float *lut, int count);
    // The same RPU's colour step, as two row-major 3x3 matrices and the input
    // offset of neutral:
    //
    //     L'M'S' = from_ycc * ((I, Ct, Cp) - offset)
    //     RGB    = to_rgb * LMS        (after the PQ curve, in nits)
    //
    // `from_ycc` is Dolby's ycc_to_rgb, applied before the PQ curve; `to_rgb`
    // is Dolby's Hunt-Pointer-Estevez LMS -> RGB composed with the RPU's own
    // rgb_to_lms correction, applied after it. Both are nothing like the
    // BT.2100 matrices a plain ICtCp decode would use - Dolby's Ct reaches S'
    // at 0.03 rather than 0.56, and its LMS inverse has a 3.06 diagonal rather
    // than 4.10 - so a generic matrix over-saturates and shifts the hue.
    // valid = false keeps the BT.2100 fallback, for a film whose RPU carries
    // no colour metadata.
    void set_dovi_colour(const float *from_ycc, const float *offset, const float *to_rgb,
                         bool valid);
    // Multiplies the video's final RGB; the UI is untouched. 1.0 is neutral,
    // 0.5 is as dark as the toolbar allows and 1.5 is a bright panel.
    void set_brightness(float brightness)
    {
        brightness_ = brightness < 0.5f ? 0.5f : (brightness > 1.5f ? 1.5f : brightness);
    }
    // HDR10 scanout output. Turn it on only when the framebuffer really is a
    // 10-bit PQ surface; then the whole frame is written in the PQ domain, SDR
    // white (UI and any held-back film) sits at ref_white_nits and the peak
    // hints the target TV's headroom. Off (the default) leaves the SDR path
    // byte for byte untouched, so this never changes what a normal run draws.
    void set_hdr_output(bool on, float ref_white_nits = 203.0f, float tv_peak_nits = 1000.0f)
    {
        hdr_out_[0] = on ? 1.0f : 0.0f;
        hdr_out_[1] = ref_white_nits;
        hdr_out_[2] = tv_peak_nits;
        hdr_out_[3] = 0.0f;
    }
    // How a film's planes become the right colours. Chosen once, when the
    // film opens, from the stream's tags.
    //
    // Sdr is a plain studio-range matrix. Pq and Hlg are HDR: their samples
    // are light, not brightness, so they go through the transfer curve, a
    // BT.2390 tone map and BT.2020 to BT.709 primaries. Shown through the SDR
    // curve instead, an HDR film comes out pale and grey - which is exactly
    // the "colours are off on this one" report, and no matrix change fixes it.
    enum class VideoTransfer
    {
        Sdr = 0,
        Pq = 1,  // HDR10, SMPTE ST 2084 on BT.2020
        Hlg = 2, // ARIB STD-B67 on BT.2020
    };
    // matrix: 0 BT.601, 1 BT.709, 2 BT.2020. peak_nits is the film's own
    // brightest light for PQ, from its mastering metadata; wide_gamut says the
    // primaries are BT.2020 and must come down to BT.709.
    //
    // ten_bit_low_aligned says where the decoder put its ten bits: true for
    // the console's Videodec2 output and FFmpeg's planar 10-bit formats, false
    // for P010LE, which stores them in the top of the word. It is only read
    // when the texture is ten-bit, and it changes the shader's sample scale by
    // 64x, so a frame tagged the wrong way is not slightly tinted.
    // ictcp says the planes are Dolby Vision profile 5's IPTPQc2 base layer,
    // not Y'CbCr: the three components are I, Ct and Cp, and reading them as
    // luma and two chroma differences is what turns a P5 film purple.
    void set_video_colorspace(VideoTransfer transfer, int matrix, bool full_range, float peak_nits,
                              bool wide_gamut, bool ten_bit_low_aligned = true, bool ictcp = false)
    {
        video_transfer_ = transfer;
        video_matrix_ = matrix;
        video_full_range_ = full_range;
        video_peak_nits_ = peak_nits;
        video_wide_gamut_ = wide_gamut;
        video_ten_bit_low_aligned_ = ten_bit_low_aligned;
        video_ictcp_ = ictcp;
    }
    std::uint32_t video_texture() const
    {
        return video_display_slot_ >= 0 && video_y_[video_display_slot_] != 0 ? 1u : 0u;
    }

    // Draws into the currently bound framebuffer of the given size.
    void draw(const DrawList &list, const Viewport &viewport, int surface_width,
              int surface_height);

    std::size_t last_draw_calls() const
    {
        return draw_calls_;
    }

  private:
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint buffer_ = 0;
    std::size_t capacity_ = 0; // instances
    GLuint mesh_program_ = 0;
    GLuint mesh_vao_ = 0;
    GLuint mesh_buffer_ = 0;
    std::size_t mesh_capacity_ = 0; // vertices
    std::size_t draw_calls_ = 0;
    GLuint font_textures_[kFontSlots] = {};
    std::uint32_t font_count_ = 0;
    // A small ring of plane sets so the CPU never overwrites a texture the GPU
    // may still be sampling: writing a 4K picture into the very texture the last
    // frame read stalls on the read-after-write, which measured ~100 ms a pop.
    static constexpr int kVideoSlots = 3;
    GLuint video_y_[kVideoSlots] = {};
    GLuint video_uv_[kVideoSlots] = {};
    GLuint video_v_[kVideoSlots] = {};
    GLuint video_pbo_y_[kVideoSlots] = {}; // pixel-unpack buffers, one per ring slot
    GLuint video_pbo_uv_[kVideoSlots] = {};
    GLuint video_pbo_v_[kVideoSlots] = {};
    bool video_pbo_ok_ = true;    // cleared once glMapBuffer on the unpack buffer proves unusable
    int video_slot_ = -1;         // ring position the next update writes
    int video_display_slot_ = -1; // ring position draw() samples
    int upload_probe_ = 0;        // low-frequency counter for the plane split probe
    int video_width_ = 0;
    int video_height_ = 0;
    bool video_ten_bit_ = false;
    bool video_ten_bit_low_aligned_ = true;
    bool video_planar_ = false;
    int video_matrix_ = 1;
    bool video_full_range_ = false;
    float video_peak_nits_ = 1000.0f;
    float brightness_ = 1.0f;
    // u_hdr_out: x on/off, y SDR reference white in nits, z target TV peak in
    // nits, w unused. Defaults to off so a frame drawn before main() opts in is
    // still ordinary SDR.
    float hdr_out_[4] = {0.0f, 203.0f, 1000.0f, 0.0f};
    bool video_wide_gamut_ = false;
    bool video_ictcp_ = false;
    // 1024x1, one texel per input code, holding the reshaped luma in 0..1.
    // Only Dolby Vision profile 5 binds it.
    GLuint dovi_lut_ = 0;
    // A 1x1 GL_TEXTURE_RECTANGLE parked on units 9..11 whenever no film is up.
    // The batch2d program always declares three sampler2DRect uniforms; left at
    // their default they reference unit 0, which holds a 2D texture. That target
    // mismatch makes Mesa reject the entire instanced draw with
    // GL_INVALID_OPERATION, so the whole UI silently disappears on a cold boot
    // (backdrop, a separate program, still paints). Binding a real rectangle
    // here keeps the samplers well-formed until the video block swaps in planes.
    GLuint dummy_rect_ = 0;
    // The RPU's colour step, as uploaded. Rows are 3 wide, plus the offset in
    // the fourth slot; the shader reads them as vec4 dots.
    float dovi_from_ycc_[3][4] = {};
    float dovi_to_rgb_[3][4] = {};
    bool dovi_colour_valid_ = false;
    VideoTransfer video_transfer_ = VideoTransfer::Sdr;
};

} // namespace hui::gfx

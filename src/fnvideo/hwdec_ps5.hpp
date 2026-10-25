// ps5-homebrew-ui - Hardware video decoding on the PS5 (libSceVideodec2):
// H.264, HEVC (8 and 10-bit) and VP9 up to 3840x2176, decoded by the console's
// video block instead of the CPU. Anything this refuses (other codecs, tiled
// HEVC, a decoder error) is decoded in software by FFmpeg, which the player
// still keeps as its fallback.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Sp9nky/unofficial-stremio-ps5-port (src/hwdec_ps5.cpp,
// GPL-3.0-or-later), which ports Nuvio PS5's evo_vdec_native.c
// (Copyright (C) 2026 Husam Osman) and carries the details that were found on
// hardware: the memory pool sizes, the access-unit ring, the chroma offset,
// and the "tolerate 32 refusals after a seek" rule.

#pragma once

#include <cstdint>
#include <string>

struct AVCodecParameters;

namespace hui::fnvideo
{

class HwDecoder
{
  public:
    // A decoded picture: NV12 (8-bit) or two-plane 4:2:0 with 16-bit samples
    // (10-bit). Valid until the next send/receive/flush.
    //
    // The 10-bit samples are low-aligned in their 16-bit words (the value sits
    // in the low ten bits), which is not how any FFmpeg 10-bit format stores
    // them; the player lifts them before the scaler sees them.
    struct Picture
    {
        const std::uint8_t *y = nullptr;
        const std::uint8_t *uv = nullptr;
        int pitch = 0;        // bytes per row, both planes
        int width = 0;        // visible
        int height = 0;       // visible
        int coded_height = 0; // luma rows in the buffer, padding included
        bool ten_bit = false;
        std::int64_t pts_us = INT64_MIN;
    };

    // Loads the decoder module; call once before the first film opens.
    static void load_module();

    // A decoder for this stream, or nullptr (why says so) when the hardware
    // cannot take it.
    static HwDecoder *open(const AVCodecParameters *par, std::string *why);
    ~HwDecoder();
    HwDecoder(const HwDecoder &) = delete;
    HwDecoder &operator=(const HwDecoder &) = delete;

    // One access unit (pts in microseconds, INT64_MIN unknown); data = nullptr
    // at the end of the stream. 0 taken, 1 receive() first, -1 failed.
    int send(const std::uint8_t *data, int size, std::int64_t pts_us);
    // 1 a picture, 0 none yet, -1 failed.
    int receive(Picture *out);
    // After a seek.
    void flush();

    const char *name() const;

    struct Impl;

  private:
    HwDecoder() = default;
    Impl *d_ = nullptr;
};

} // namespace hui::fnvideo

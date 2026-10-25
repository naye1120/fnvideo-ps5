// ps5-homebrew-ui - OpenGL backend for the 2D draw list.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/gl_batch.hpp"

#include "gfx/gl_program.hpp"

#include "platform/ps5/system.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hui::gfx
{

namespace
{

constexpr const char *kVertex = R"(
layout(location = 0) in vec4 a_rect;
layout(location = 1) in vec4 a_top;
layout(location = 2) in vec4 a_bottom;
layout(location = 3) in vec4 a_border;
layout(location = 4) in vec4 a_params;
layout(location = 5) in vec4 a_extra;
layout(location = 0) uniform vec4 u_viewport; // scale, offset x, offset y
layout(location = 1) uniform vec2 u_surface;
out vec2 v_local;
out vec2 v_virtual;
out vec2 v_uv;
out float v_t;
flat out vec2 v_half;
flat out vec4 v_top;
flat out vec4 v_bottom;
flat out vec4 v_border;
flat out vec4 v_params;
flat out vec4 v_extra;
const vec2 kCorners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                 vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
void main()
{
    vec2 corner = kCorners[gl_VertexID];
    int shape = int(a_params.w + 0.5);
    vec2 size = a_rect.zw;
    // Grow shapes by their softness plus one output pixel for anti-aliasing;
    // glyph and image quads map texels exactly and are not grown.
    float pad = (shape == 1 || shape == 3) ? 0.0 : a_params.z + 1.0 / u_viewport.x;
    vec2 local = mix(vec2(-pad), size + vec2(pad), corner);
    vec2 centred = local - 0.5 * size;
    vec2 virt = a_rect.xy + local;
    // Rounded rectangles, shadows and triangles may turn about their centre: the quad
    // rotates, the distance field keeps working in the unrotated frame.
    bool box = shape == 0 || shape == 2 || shape == 5;
    if (box && a_extra.x != 0.0)
    {
        float c = cos(a_extra.x);
        float s = sin(a_extra.x);
        virt = a_rect.xy + 0.5 * size + vec2(c * centred.x - s * centred.y,
                                             s * centred.x + c * centred.y);
    }
    v_virtual = virt;
    v_local = centred;
    v_half = 0.5 * size;
    v_t = (shape == 0 && a_extra.y > 0.5) ? clamp(local.x / max(size.x, 1e-3), 0.0, 1.0)
                                          : clamp(local.y / max(size.y, 1e-3), 0.0, 1.0);
    v_uv = mix(a_extra.xy, a_extra.zw, corner);
    v_top = a_top;
    v_bottom = a_bottom;
    v_border = a_border;
    v_params = a_params;
    v_extra = a_extra;
    vec2 surface = virt * u_viewport.x + u_viewport.yz;
    vec2 ndc = surface / u_surface * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)";

constexpr const char *kFragment = R"(
layout(location = 0) uniform vec4 u_viewport;
layout(location = 2) uniform sampler2D u_texture;
layout(location = 3) uniform sampler2D u_fonts[8];
// Fixed units of their own, like the font atlases: a video instance never has
// to break a run to bind anything. Locations 3..10 are the eight font slots.
// Rectangle textures, not 2D: ps5-opengl uploads a rectangle (linear, no mip
// chain, no tiling) through a fast path, while a 2D glTexSubImage2D of a 4K
// plane crawls at ~13 ns per texel. The reference player uses rectangles too.
layout(location = 11) uniform sampler2DRect u_video_y;
layout(location = 12) uniform sampler2DRect u_video_uv;
layout(location = 13) uniform sampler2DRect u_video_v;
layout(location = 25) uniform vec2 u_video_size; // the luma plane in texels
layout(location = 14) uniform vec4 u_video;  // x: planar chroma, y: matrix, z: sample -> 10-bit, w: transfer
layout(location = 15) uniform vec4 u_video2; // x: full range, y: peak nits (PQ), z: BT.2020 primaries, w: ICtCp (DV profile 5)
// Dolby Vision profile 5 only: 1024 texels, the reshaped luma for every input
// code. The RPU's inverse reshaping is a piece-wise polynomial, so it is baked
// into a lookup once per scene instead of being evaluated per pixel.
layout(location = 16) uniform sampler2D u_dovi_lut;
// Dolby Vision profile 5's colour step, three rows at a time. a* is
// rgb_to_lms * ycc_to_rgb with the chroma offset folded into w; i* is the
// inverse of rgb_to_lms, applied after the PQ curve. Both come from the RPU.
layout(location = 17) uniform vec4 u_dovi_a0;
layout(location = 18) uniform vec4 u_dovi_a1;
layout(location = 19) uniform vec4 u_dovi_a2;
layout(location = 20) uniform vec4 u_dovi_i0;
layout(location = 21) uniform vec4 u_dovi_i1;
layout(location = 22) uniform vec4 u_dovi_i2;
// Video-only brightness. The UI keeps its contrast; only the film dims.
layout(location = 23) uniform float u_brightness;
// HDR10 scanout output. x: 1 when the framebuffer is a 10-bit PQ BT.2020
// surface and the whole frame is written in the PQ domain; y: the SDR reference
// white in nits that places UI grey and SDR video inside HDR; z: the target
// TV's peak in nits. With x 0 the ordinary SDR path runs, byte for byte.
layout(location = 24) uniform vec4 u_hdr_out;
in vec2 v_local;
in vec2 v_virtual;
in vec2 v_uv;
in float v_t;
flat in vec2 v_half;
flat in vec4 v_top;
flat in vec4 v_bottom;
flat in vec4 v_border;
flat in vec4 v_params;
flat in vec4 v_extra;
out vec4 frag_color;

float round_box(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float triangle(vec2 p, vec2 p0, vec2 p1, vec2 p2)
{
    vec2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    vec2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
    vec2 q0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    vec2 q1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    vec2 q2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    vec2 d = min(min(vec2(dot(q0, q0), s * (v0.x * e0.y - v0.y * e0.x)),
                     vec2(dot(q1, q1), s * (v1.x * e1.y - v1.y * e1.x))),
                 vec2(dot(q2, q2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

float star5(vec2 p, float r, float rf)
{
    const vec2 k1 = vec2(0.809016994375, -0.587785252292);
    const vec2 k2 = vec2(-k1.x, k1.y);
    p.x = abs(p.x);
    p -= 2.0 * max(dot(k1, p), 0.0) * k1;
    p -= 2.0 * max(dot(k2, p), 0.0) * k2;
    p.x = abs(p.x);
    p.y -= r;
    vec2 ba = rf * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
    float h = clamp(dot(p, ba) / dot(ba, ba), 0.0, r);
    return length(p - ba * h) * sign(p.y * ba.x - p.x * ba.y);
}

// ---- video colour --------------------------------------------------------
// A film's planes arrive as 10-bit code values, and everything below works on
// those, because that is how the matrices and the curves are defined. The
// chain is VLC's PS5 port (vlc-ps5 shaders/video.frag and src/player.cc,
// which in turn mirror mpv), checked there against real frames; Kodi's PS5
// port carries the same constants. The SDR matrix coefficients are the
// full-range ones, so limited range expands chroma by 255/224 first - that is
// what VLC does, and it is the step this shader used to get backwards.

const float SDR_WHITE = 203.0; // HDR's reference white (BT.2408) = the TV's white
const float PQ_M1 = 0.1593017578125, PQ_M2 = 78.84375;
const float PQ_C1 = 0.8359375, PQ_C2 = 18.8515625, PQ_C3 = 18.6875;

vec3 pq_to_nits(vec3 e)
{
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / PQ_M2));
    return 10000.0 * pow(max(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), vec3(1.0 / PQ_M1));
}

float nits_to_pq(float nits)
{
    float y = pow(clamp(nits / 10000.0, 0.0, 1.0), PQ_M1);
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), PQ_M2);
}

vec3 nits_to_pq3(vec3 nits)
{
    return vec3(nits_to_pq(nits.r), nits_to_pq(nits.g), nits_to_pq(nits.b));
}

// HLG: scene light, then the system gamma of a 1000-nit TV (BT.2100)
vec3 hlg_to_nits(vec3 e)
{
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    e = clamp(e, 0.0, 1.0);
    vec3 s = mix(e * e / 3.0, (exp((e - c) / a) + b) / 12.0, step(vec3(0.5), e));
    float ys = max(dot(s, vec3(0.2627, 0.6780, 0.0593)), 1e-6);
    return 1000.0 * pow(ys, 0.2) * s;
}

// HDR to SDR, from linear light in nits. Without this an HDR film is not
// merely a bit off, it is pale and grey: its samples are light, not
// brightness. `peak` is the film's own brightest light.
vec3 nits_to_sdr(vec3 nits, float peak, bool to709)
{
    // BT.2390's roll-off on the brightest channel, so the hue survives: up to
    // about half of SDR white nothing changes, above it the highlights bend
    // down to fit under white.
    float sig = max(nits.r, max(nits.g, nits.b));
    if (sig > 0.0 && peak > SDR_WHITE)
    {
        float src = nits_to_pq(peak);
        float e1 = min(nits_to_pq(sig) / src, 1.0);
        float maxl = nits_to_pq(SDR_WHITE) / src;
        float ks = 1.5 * maxl - 0.5;
        float e2 = e1;
        if (e1 > ks)
        {
            float t = (e1 - ks) / (1.0 - ks), t2 = t * t, t3 = t2 * t;
            e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) +
                 (-2.0 * t3 + 3.0 * t2) * maxl;
        }
        nits *= pq_to_nits(vec3(e2 * src)).r / sig;
    }
    vec3 lin = nits / SDR_WHITE;
    if (to709)
    {
        // BT.2020 to BT.709 primaries, in linear light (GLSL matrices are by
        // column)
        lin = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006, -0.0728, -0.0083,
                   1.1187) *
              lin;
    }
    // back to a TV's signal (BT.1886, gamma 2.4)
    return pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.4));
}

// Two-arg form: honours the wide-gamut flag, for HDR10 / HLG, whose linear RGB
// really is BT.2020 and must be brought into BT.709 for an SDR screen. Dolby
// Vision profile 5's RPU also ends in BT.2020 - its HPE LMS->RGB matrix is the
// inverse of the BT.2020 luminance matrix - so it needs the same step.
vec3 nits_to_sdr(vec3 nits, float peak)
{
    return nits_to_sdr(nits, peak, u_video2.z > 0.5);
}

vec3 hdr_to_sdr(vec3 e, bool hlg)
{
    return nits_to_sdr(hlg ? hlg_to_nits(e) : pq_to_nits(e), hlg ? 1000.0 : u_video2.y);
}

// The RPU's inverse reshaping for all three components at once, as one texel
// fetch each: the table is 1024 wide (one texel per input code) and three rows
// tall (I, Ct, Cp). `code` is the ten-bit sample.
vec3 dovi_reshape(vec3 code)
{
    vec3 t = (clamp(code, 0.0, 1023.0) + 0.5) * (1.0 / 1024.0);
    return vec3(texture(u_dovi_lut, vec2(t.x, 0.5 / 3.0)).r,
                texture(u_dovi_lut, vec2(t.y, 1.5 / 3.0)).r,
                texture(u_dovi_lut, vec2(t.z, 2.5 / 3.0)).r);
}

// Dolby Vision profile 5's base layer is IPTPQc2: the three planes are
// ICtCp-like, not Y'CbCr, and they already carry the PQ curve. After the RPU's
// inverse reshaping gives back (I, Ct, Cp), one matrix rebuilds L'M'S', the PQ
// curve turns those into linear LMS in nits, and a second matrix gives RGB.
//
// Both matrices are Dolby's, from the RPU, and they are not BT.2100's. The
// difference is what kept the hue wrong after the reshaping was fixed:
//
//   * Dolby's ycc_to_rgb sends Ct into S' at 0.03 where BT.2100 sends it at
//     0.56, and its Ct/Cp axes are not even symmetric between L' and M'.
//   * its LMS base is Hunt-Pointer-Estevez *without crosstalk*, whose inverse
//     has a 3.06 diagonal against BT.2100's 4.10, so a generic decode
//     over-saturates every colour and rotates the hue.
//
// `e` is (I, Ct, Cp) as normalised codes, 0..1 each; the chroma centring is
// folded into the offset.
vec3 ictcp_to_nits(vec3 e)
{
    if (u_dovi_i0.w > 0.5)
    {
        // The RPU's own matrices: u_dovi_a* is ycc_to_rgb with its input
        // offset folded into w, u_dovi_i* is HPE LMS -> RGB composed with the
        // RPU's rgb_to_lms correction.
        vec3 lms = vec3(dot(u_dovi_a0.xyz, e) + u_dovi_a0.w,
                        dot(u_dovi_a1.xyz, e) + u_dovi_a1.w,
                        dot(u_dovi_a2.xyz, e) + u_dovi_a2.w);
        return mat3(u_dovi_i0.xyz, u_dovi_i1.xyz, u_dovi_i2.xyz) * pq_to_nits(lms);
    }
    // No RPU colour metadata: BT.2100's generic ICtCp, chroma centred.
    float i = e.x, ct = e.y - 0.5, cp = e.z - 0.5;
    vec3 lms = vec3(i + 0.0086090370 * ct + 0.1110296250 * cp,
                    i - 0.0086090370 * ct - 0.1110296250 * cp,
                    i + 0.5600311257 * ct - 0.3206271750 * cp);
    // Columns are L, M and S (GLSL matrices are by column). Dolby's P5 base
    // layer is HPE LMS, so this is Dolby's inverse, not BT.2100's: the latter
    // over-saturates every colour and rotates skin towards magenta. It is the
    // inverse of the IPT RGB -> LMS matrix (rows 0.4408/0.5354/0.0238, ...),
    // whose rows sum to 1 so a neutral code stays neutral.
    return mat3(3.06441879, -0.65612108, 0.01736321,
                -2.16597676, 1.78554118, -0.04725154,
                0.10155818, -0.12943749, 1.03004253) *
           pq_to_nits(lms);
}

// Y'CbCr code values to the 0..1 luma and -0.5..0.5 chroma the matrices are
// written for.
vec3 video_signal(float y10, float u10, float v10)
{
    // Full range puts black at 0 and white at 1023 and centres chroma at
    // 511.5; studio range puts black at 64, white at 940 and centres chroma at
    // 512. Both expand to the same 0..1 luma and -0.5..0.5 chroma.
    //
    // The stream's own tag decides, and player.cpp already forces full range
    // for Dolby Vision profile 5 - ICtCp is defined on full-range codes, and
    // the RPU's bl_video_full_range_flag says so as well.
    const bool full = u_video2.x > 0.5;
    float y = full ? y10 * (1.0 / 1023.0) : (y10 - 64.0) * (1.0 / 876.0);
    float u = full ? (u10 - 511.5) * (1.0 / 1023.0) : (u10 - 512.0) * (1.0 / 896.0);
    float v = full ? (v10 - 511.5) * (1.0 / 1023.0) : (v10 - 512.0) * (1.0 / 896.0);
    return vec3(y, u, v);
}

// Y'CbCr to R'G'B', 10-bit code values in, 0..1 out. u_video.y picks the
// matrix: 0 BT.601, 1 BT.709, 2 BT.2020.
vec3 video_rgb(float y10, float u10, float v10)
{
    vec3 s = video_signal(y10, u10, v10);
    int m = int(u_video.y + 0.5);
    if (m == 2)
        return vec3(s.x + 1.4746 * s.z, s.x - 0.16455 * s.y - 0.57135 * s.z,
                    s.x + 1.8814 * s.y);
    if (m == 1)
        return vec3(s.x + 1.5748 * s.z, s.x - 0.1873 * s.y - 0.4681 * s.z,
                    s.x + 1.8556 * s.y);
    return vec3(s.x + 1.402 * s.z, s.x - 0.344136 * s.y - 0.714136 * s.z,
                s.x + 1.772 * s.y);
}

// BT.2020 primaries to BT.709 in linear light, then back to the SDR signal.
// An SDR film graded in BT.2020 is not HDR, so it never reaches hdr_to_sdr,
// but its reds and greens are still outside the panel's gamut: without this
// they stay oversaturated and skin tones lean orange.
vec3 bt2020_to_709(vec3 e)
{
    vec3 lin = pow(clamp(e, 0.0, 1.0), vec3(2.4));
    lin = mat3(1.6605, -0.1246, -0.0182, -0.5876, 1.1329, -0.1006, -0.0728, -0.0083, 1.1187) *
          lin;
    return pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.4));
}

// Bring an SDR BT.709 gamma 2.4 signal - the UI, glyphs, borders, and any film
// held in SDR - onto the HDR scanout: decode to linear, widen 709 -> 2020
// primaries, place SDR white at u_hdr_out.y nits, re-encode on the PQ curve.
// The matrix is the inverse of the 2020 -> 709 above, the same one VLC uses in
// its PQ output (vlc-ps5 video.frag to_pq_output).
vec3 sdr709_to_pq2020(vec3 sdr)
{
    vec3 lin = pow(clamp(sdr, 0.0, 1.0), vec3(2.4));
    lin = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956) * lin;
    return nits_to_pq3(lin * u_hdr_out.y);
}

void main()
{
    float px = 1.0 / u_viewport.x; // virtual units per output pixel
    int shape = int(v_params.w + 0.5);
    vec4 fill = mix(v_top, v_bottom, v_t);
    vec4 color;
    if (shape == 0)
    {
        float radius = min(v_params.x, min(v_half.x, v_half.y));
        float d = round_box(v_local, v_half, radius);
        color = fill;
        if (v_params.y > 0.0)
        {
            float inner = clamp(0.5 - (d + v_params.y) / px, 0.0, 1.0);
            color = mix(v_border, fill, inner);
        }
        color.a *= clamp(0.5 - d / px, 0.0, 1.0);
    }
    else if (shape == 1)
    {
        // The font atlases stay bound to their own units; the slot picks one.
        int slot = int(v_params.z + 0.5);
        float sdf;
        if (slot == 1)
            sdf = texture(u_fonts[0], v_uv).r;
        else if (slot == 2)
            sdf = texture(u_fonts[1], v_uv).r;
        else if (slot == 3)
            sdf = texture(u_fonts[2], v_uv).r;
        else if (slot == 4)
            sdf = texture(u_fonts[3], v_uv).r;
        else if (slot == 5)
            sdf = texture(u_fonts[4], v_uv).r;
        else if (slot == 6)
            sdf = texture(u_fonts[5], v_uv).r;
        else if (slot == 7)
            sdf = texture(u_fonts[6], v_uv).r;
        else if (slot == 8)
            sdf = texture(u_fonts[7], v_uv).r;
        else
            sdf = texture(u_texture, v_uv).r;
        float distance = (sdf - 0.5) * 2.0 * v_params.x;
        color = vec4(fill.rgb, fill.a * clamp(distance / px + 0.5, 0.0, 1.0));
    }
    else if (shape == 2)
    {
        float radius = min(v_params.x, min(v_half.x, v_half.y));
        float d = round_box(v_local, v_half, radius);
        color = vec4(fill.rgb, fill.a * (1.0 - smoothstep(-v_params.z, v_params.z, d)));
    }
    else if (shape == 3)
    {
        color = texture(u_texture, v_uv) * fill;
        if (v_params.x > 0.0)
        {
            float radius = min(v_params.x, min(v_half.x, v_half.y));
            color.a *= clamp(0.5 - round_box(v_local, v_half, radius) / px, 0.0, 1.0);
        }
    }
    else if (shape == 9)
    {
        // A video frame straight from its planes, in 10-bit code values.
        // u_video.z is the factor that turns a texture sample back into a
        // code: 65535 for low-aligned ten-bit words (value/65535 * 65535), a
        // shade under 1024 for P010's high-aligned ones (value/65535 *
        // 65535/64), and 1020 for an 8-bit plane (value/255 * 1020, which is
        // the same code four times over). One set of curves then serves all
        // three depths.
        // Rectangle samplers take texel coordinates, so map the quad's 0..1 uv
        // onto the plane size; the chroma planes are half in each axis.
        const vec2 ycoord = v_uv * u_video_size;
        const vec2 ccoord = v_uv * u_video_size * 0.5;
        float luma = texture(u_video_y, ycoord).r * u_video.z;
        vec2 chroma;
        if (u_video.x > 0.5)
            chroma = vec2(texture(u_video_uv, ccoord).r, texture(u_video_v, ccoord).r) * u_video.z;
        else
            chroma = texture(u_video_uv, ccoord).rg * u_video.z;
        vec3 rgb;
        bool native_pq = false;
        if (u_video2.w > 0.5)
        {
            // Dolby Vision profile 5: the planes are ICtCp (IPTPQc2), so the
            // Y'CbCr matrix and transfer switch below do not apply. All three
            // components are reshaped - the encoder squeezes the film's range
            // into the code range, so the raw code is not the light level - and
            // the RPU's matrices rebuild linear RGB in nits. That base is
            // BT.2020 (SMPTE ST 2128; dovi_meta.h's HPE-without-crosstalk, the
            // RGB side is 2020 just like BT.2100's), so an SDR panel needs the
            // 2020 -> 709 step - skipping it over-saturates green (图1) - while
            // an HDR panel takes the BT.2020 straight onto the PQ curve.
            vec3 nits = ictcp_to_nits(dovi_reshape(vec3(luma, chroma.x, chroma.y)));
            if (u_hdr_out.x > 0.5)
            {
                rgb = nits_to_pq3(nits);
                native_pq = true;
            }
            else
                rgb = nits_to_sdr(nits, u_video2.y, true);
        }
        else if (u_hdr_out.x > 0.5 && u_video.w > 0.5)
        {
            // HDR10 / HLG straight to the HDR scanout: no SDR roll-off, no
            // gamma. BT.2020 PQ codes are already the signal the panel wants;
            // HLG is carried through linear nits onto the PQ curve.
            rgb = video_rgb(luma, chroma.x, chroma.y);
            if (u_video.w > 1.5)
                rgb = nits_to_pq3(hlg_to_nits(rgb));
            native_pq = true;
        }
        else
        {
            rgb = video_rgb(luma, chroma.x, chroma.y);
            if (u_video.w > 1.5)
                rgb = hdr_to_sdr(rgb, true);
            else if (u_video.w > 0.5)
                rgb = hdr_to_sdr(rgb, false);
            else if (u_video2.z > 0.5)
                rgb = bt2020_to_709(rgb);
        }
        rgb *= u_brightness;
        // An SDR result (a plain film, or DV P5 held in SDR) on the HDR panel
        // still has to reach the PQ domain; native HDR10 / HLG is already there.
        if (u_hdr_out.x > 0.5 && !native_pq)
            rgb = sdr709_to_pq2020(rgb);
        color = vec4(clamp(rgb, 0.0, 1.0), 1.0) * fill;
        if (v_params.x > 0.0)
        {
            float radius = min(v_params.x, min(v_half.x, v_half.y));
            color.a *= clamp(0.5 - round_box(v_local, v_half, radius) / px, 0.0, 1.0);
        }
    }
    else if (shape == 4)
    {
        vec2 pa = v_virtual - v_extra.xy;
        vec2 ba = v_extra.zw - v_extra.xy;
        float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
        float d = length(pa - ba * h) - 0.5 * v_params.y;
        color = vec4(fill.rgb, fill.a * clamp(0.5 - d / px, 0.0, 1.0));
    }
    else if (shape == 8)
    {
        // Box with 45 degree corner cuts: the box distance, limited by the
        // diagonal plane through each corner.
        float cut = min(v_params.x, min(v_half.x, v_half.y));
        vec2 q = abs(v_local) - v_half;
        float box = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
        float d = max(box, (q.x + q.y + cut) * 0.70710678);
        color = fill;
        if (v_params.y > 0.0)
        {
            float inner = clamp(0.5 - (d + v_params.y) / px, 0.0, 1.0);
            color = mix(v_border, fill, inner);
        }
        color.a *= clamp(0.5 - d / px, 0.0, 1.0);
    }
    else if (shape == 7)
    {
        // Ring sector, after Inigo Quilez's arc: turn the sector upright,
        // fold it on its axis, then measure to the ring or to its end.
        float rb = 0.5 * v_params.y;
        float ra = min(v_half.x, v_half.y) - rb;
        float half_sweep = 0.5 * v_extra.x;
        float mid = v_params.x + half_sweep;
        float cm = cos(mid);
        float sm = sin(mid);
        vec2 p = vec2(v_local.x, -v_local.y);
        p = vec2(abs(p.x * cm - p.y * sm), p.x * sm + p.y * cm);
        vec2 sc = vec2(sin(half_sweep), cos(half_sweep));
        float ring = abs(length(p) - ra);
        float d;
        if (v_extra.y > 0.5)
            d = max(ring - rb, p.x * sc.y - p.y * sc.x);
        else
            d = ((sc.y * p.x > sc.x * p.y) ? length(p - sc * ra) : ring) - rb;
        color = vec4(fill.rgb, fill.a * clamp(0.5 - d / px, 0.0, 1.0));
    }
    else if (shape == 6)
    {
        vec2 p = vec2(v_local.x, -v_local.y);
        float d = star5(p, min(v_half.x, v_half.y), 0.45);
        if (v_params.y > 0.0)
            d = abs(d + 0.5 * v_params.y) - 0.5 * v_params.y;
        color = vec4(fill.rgb, fill.a * clamp(0.5 - d / px, 0.0, 1.0));
    }
    else
    {
        float d = triangle(v_local, vec2(0.0, -v_half.y), vec2(-v_half.x, v_half.y),
                           vec2(v_half.x, v_half.y));
        if (v_params.y > 0.0)
            d = abs(d + 0.5 * v_params.y) - 0.5 * v_params.y;
        color = vec4(fill.rgb, fill.a * clamp(0.5 - d / px, 0.0, 1.0));
    }
    // On an HDR scanout every non-video fragment (UI, glyphs, borders) was
    // built as SDR gamma 2.4 BT.709 and must sit in the PQ domain. Alpha is
    // coverage, not light, so it stays as-is and the panel blends it over the
    // film as it did in SDR (a translucent black stays black, not grey).
    if (shape != 9 && u_hdr_out.x > 0.5)
        color = vec4(sdr709_to_pq2020(color.rgb), color.a);
    if (color.a <= 0.0)
        discard;
    frag_color = color;
}
)";

constexpr const char *kMeshVertex = R"(
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) uniform vec4 u_viewport;
layout(location = 1) uniform vec2 u_surface;
out vec4 v_color;
void main()
{
    vec2 surface = a_position * u_viewport.x + u_viewport.yz;
    vec2 ndc = surface / u_surface * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    v_color = a_color;
}
)";

constexpr const char *kMeshFragment = R"(
in vec4 v_color;
out vec4 frag_color;
void main()
{
    frag_color = v_color;
}
)";

} // namespace

GlBatch::~GlBatch()
{
    release();
}

void GlBatch::release()
{
    if (buffer_ != 0)
        glDeleteBuffers(1, &buffer_);
    if (vao_ != 0)
        glDeleteVertexArrays(1, &vao_);
    if (program_ != 0)
        glDeleteProgram(program_);
    if (mesh_buffer_ != 0)
        glDeleteBuffers(1, &mesh_buffer_);
    if (mesh_vao_ != 0)
        glDeleteVertexArrays(1, &mesh_vao_);
    if (mesh_program_ != 0)
        glDeleteProgram(mesh_program_);
    if (font_count_ != 0)
        glDeleteTextures(static_cast<GLsizei>(font_count_), font_textures_);
    for (GLuint &texture : font_textures_)
        texture = 0;
    font_count_ = 0;
    destroy_video_texture();
    if (dovi_lut_ != 0)
    {
        glDeleteTextures(1, &dovi_lut_);
        dovi_lut_ = 0;
    }
    if (dummy_rect_ != 0)
    {
        glDeleteTextures(1, &dummy_rect_);
        dummy_rect_ = 0;
    }
    buffer_ = vao_ = program_ = mesh_buffer_ = mesh_vao_ = mesh_program_ = 0;
    capacity_ = 0;
    mesh_capacity_ = 0;
}

bool GlBatch::init()
{
    program_ = build_program("batch2d", kVertex, kFragment);
    mesh_program_ = build_program("mesh2d", kMeshVertex, kMeshFragment);
    if (program_ == 0 || mesh_program_ == 0)
        return false;
    glGenVertexArrays(1, &mesh_vao_);
    glGenBuffers(1, &mesh_buffer_);
    glBindVertexArray(mesh_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, mesh_buffer_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                          reinterpret_cast<const void *>(static_cast<std::uintptr_t>(8)));
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &buffer_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    constexpr GLsizei stride = sizeof(Instance);
    for (GLuint attribute = 0; attribute < 6; ++attribute)
    {
        glEnableVertexAttribArray(attribute);
        glVertexAttribPointer(
            attribute, 4, GL_FLOAT, GL_FALSE, stride,
            reinterpret_cast<const void *>(static_cast<std::uintptr_t>(attribute * 16)));
        glVertexAttribDivisor(attribute, 1);
    }
    glBindVertexArray(0);
    // A valid ramp before any RPU arrives. Sampling texture 0 is black, and a
    // profile 5 film whose first packets are still in flight would start on a
    // black frame instead of the generic decode. The player replaces the ramp
    // as soon as it has the RPU's curve.
    set_dovi_reshape(nullptr, 1024);
    set_dovi_colour(nullptr, nullptr, nullptr, false);
    // A single opaque texel of its own, so the sampler2DRect uniforms always
    // have a rectangle to look at even before a film opens (see header note).
    glGenTextures(1, &dummy_rect_);
    glBindTexture(GL_TEXTURE_RECTANGLE, dummy_rect_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const std::uint8_t black[4] = {0, 0, 0, 255};
    glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    return true;
}

std::uint32_t GlBatch::create_font_texture(const Font &font)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, font.atlas_width(), font.atlas_height(), 0, GL_RED,
                 GL_UNSIGNED_BYTE, font.atlas().data());
    // One level only: mip chains leave ps5-opengl's batched fast path.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (font_count_ < kFontSlots)
    {
        font_textures_[font_count_] = texture;
        ++font_count_;
        return kFontHandleBase | font_count_;
    }
    return texture;
}

std::uint32_t GlBatch::create_texture(int width, int height, const std::uint8_t *rgba)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

void GlBatch::update_texture(std::uint32_t texture, int width, int height, const std::uint8_t *rgba)
{
    if (texture == 0 || width <= 0 || height <= 0)
        return;
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    // A sub-image rather than a fresh glTexImage2D: the storage is already the
    // right size, and re-specifying it every frame is what makes a driver
    // allocate a new copy per upload.
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

void GlBatch::destroy_texture(std::uint32_t texture)
{
    GLuint name = texture;
    if (name != 0)
        glDeleteTextures(1, &name);
}

void GlBatch::set_dovi_reshape(const float *lut, int count)
{
    // 16-bit unorm keeps the curve to 1/65535, which is finer than the ten-bit
    // samples it is applied to. Linear filtering then interpolates between
    // neighbouring codes instead of snapping to them.
    //
    // One row per component - I, Ct, Cp - because the RPU reshapes all three.
    constexpr int kRows = 3;
    std::uint16_t samples[1024 * kRows];
    if (count <= 0 || count > static_cast<int>(sizeof(samples) / sizeof(samples[0]) / kRows))
    {
        count = static_cast<int>(sizeof(samples) / sizeof(samples[0]) / kRows);
    }
    for (int row = 0; row < kRows; ++row)
    {
        for (int i = 0; i < count; ++i)
        {
            // No table means no RPU yet: fall back to a straight ramp, which
            // is what the shader did before there was one. Black would be
            // worse - a missing RPU must not turn the film off.
            const float value =
                lut != nullptr ? lut[row * count + i] : static_cast<float>(i) / (count - 1);
            const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
            samples[row * count + i] = static_cast<std::uint16_t>(clamped * 65535.0f + 0.5f);
        }
    }
    if (dovi_lut_ == 0)
    {
        glGenTextures(1, &dovi_lut_);
    }
    glBindTexture(GL_TEXTURE_2D, dovi_lut_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16, count, kRows, 0, GL_RED, GL_UNSIGNED_SHORT, samples);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    // The rows are separate curves, not a gradient: clamp rather than blend
    // into the neighbouring component.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GlBatch::set_dovi_colour(const float *from_ycc, const float *offset, const float *to_rgb,
                              bool valid)
{
    dovi_colour_valid_ = valid && from_ycc != nullptr && to_rgb != nullptr;
    for (int row = 0; row < 3; ++row)
    {
        // The offset is the input value of neutral, so it is subtracted from
        // the signal - which folds into the matrix's constant term:
        //   A * (x - offset) == A * x - A * offset
        float bias = 0.0f;
        for (int col = 0; col < 3; ++col)
        {
            dovi_from_ycc_[row][col] = dovi_colour_valid_ ? from_ycc[row * 3 + col] : 0.0f;
            if (dovi_colour_valid_ && offset != nullptr)
            {
                bias -= dovi_from_ycc_[row][col] * offset[col];
            }
            // Upload as columns: the shader rebuilds the matrix with
            // mat3(u_dovi_i0.xyz, u_dovi_i1.xyz, u_dovi_i2.xyz), and GLSL
            // constructs mat3 from its arguments as columns. Storing the
            // row-major to_rgb transposed here makes the shader reconstruct
            // the correct matrix (not its transpose) - the transpose rendered
            // every neutral as magenta (#FF009C).
            dovi_to_rgb_[col][row] = dovi_colour_valid_ ? to_rgb[row * 3 + col] : 0.0f;
        }
        dovi_from_ycc_[row][3] = bias;
        // The fourth slot of the inverse rows is the shader's "the RPU
        // supplied a colour step" flag; without it the generic BT.2100
        // matrices are used instead.
        dovi_to_rgb_[row][3] = dovi_colour_valid_ && row == 0 ? 1.0f : 0.0f;
    }
}

void GlBatch::destroy_video_texture()
{
    for (int s = 0; s < kVideoSlots; ++s)
    {
        GLuint names[3] = {video_y_[s], video_uv_[s], video_v_[s]};
        for (GLuint &name : names)
        {
            if (name != 0)
            {
                glDeleteTextures(1, &name);
                name = 0;
            }
        }
        video_y_[s] = video_uv_[s] = video_v_[s] = 0;
        GLuint pbos[3] = {video_pbo_y_[s], video_pbo_uv_[s], video_pbo_v_[s]};
        for (GLuint &name : pbos)
        {
            if (name != 0)
            {
                glDeleteBuffers(1, &name);
                name = 0;
            }
        }
        video_pbo_y_[s] = video_pbo_uv_[s] = video_pbo_v_[s] = 0;
    }
    video_slot_ = -1;
    video_display_slot_ = -1;
    video_width_ = video_height_ = 0;
    video_ten_bit_ = false;
    video_planar_ = false;
    video_pbo_ok_ = true;
}

void GlBatch::create_video_texture(int width, int height, bool ten_bit)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }
    if (video_y_[0] != 0 && width == video_width_ && height == video_height_ &&
        ten_bit == video_ten_bit_)
    {
        return; // the ring already holds the right shape
    }
    destroy_video_texture();
    video_width_ = width;
    video_height_ = height;
    video_ten_bit_ = ten_bit;

    const GLenum internal = ten_bit ? GL_R16 : GL_R8;
    const GLenum type = ten_bit ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
    const int chroma_w = std::max(1, width / 2);
    const int chroma_h = std::max(1, height / 2);

    const auto make = [&](GLuint *name, GLenum iformat, GLenum format, int w, int h)
    {
        glGenTextures(1, name);
        glBindTexture(GL_TEXTURE_RECTANGLE, *name);
        glTexImage2D(GL_TEXTURE_RECTANGLE, 0, static_cast<GLint>(iformat), w, h, 0, format, type,
                     nullptr);
        // A rectangle texture has no mip chain (part of why the driver uploads
        // it fast); linear filtering is the whole of the GPU-side scaling.
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    for (int s = 0; s < kVideoSlots; ++s)
    {
        make(&video_y_[s], internal, GL_RED, width, height);
        make(&video_uv_[s], ten_bit ? GL_RG16 : GL_RG8, GL_RG, chroma_w, chroma_h);
        make(&video_v_[s], internal, GL_RED, chroma_w, chroma_h);
    }
    // One unpack buffer per ring slot per plane, sized to the largest plane so
    // the per-frame orphan+refill never has to reallocate storage.
    const int pbo_bytes = ten_bit ? 2 : 1;
    const std::size_t max_plane_bytes = static_cast<std::size_t>(width) *
                                        static_cast<std::size_t>(height) *
                                        static_cast<std::size_t>(pbo_bytes);
    const auto make_pbo = [&](GLuint *name)
    {
        glGenBuffers(1, name);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, *name);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(max_plane_bytes), nullptr,
                     GL_STREAM_DRAW);
    };
    for (int s = 0; s < kVideoSlots; ++s)
    {
        make_pbo(&video_pbo_y_[s]);
        make_pbo(&video_pbo_uv_[s]);
        make_pbo(&video_pbo_v_[s]);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
}

void GlBatch::update_video_texture(const std::uint8_t *y, int y_pitch, const std::uint8_t *u,
                                   int u_pitch, const std::uint8_t *v, int v_pitch,
                                   bool planar_chroma)
{
    if (video_y_[0] == 0 || y == nullptr || y_pitch <= 0)
    {
        return;
    }
    // Write the next ring slot and sample it this frame. The slot about to be
    // filled was last read kVideoSlots-1 frames ago, so the GPU has retired it
    // and the overwrite no longer stalls on a read-after-write.
    video_slot_ = (video_slot_ + 1) % kVideoSlots;
    video_display_slot_ = video_slot_;
    video_planar_ = planar_chroma;
    const int bytes = video_ten_bit_ ? 2 : 1;
    const GLenum type = video_ten_bit_ ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
    const int chroma_w = std::max(1, video_width_ / 2);
    const int chroma_h = std::max(1, video_height_ / 2);

    // ROW_LENGTH only when the rows really carry padding. A lean GL driver can
    // keep its batched upload fast path solely for the default pixel-store
    // state, and forcing a strided copy of already-contiguous rows (pitch ==
    // width, the common 4K case) is exactly the ~100 ms we are measuring away.
    const auto set_unpack =
        [](int row_components, int texel_width, int bytes_per_texel, int bytes_per_sample)
    {
        if (row_components == texel_width)
        {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            const std::size_t row_bytes =
                static_cast<std::size_t>(texel_width) * static_cast<std::size_t>(bytes_per_texel);
            glPixelStorei(GL_UNPACK_ALIGNMENT, row_bytes % 4u == 0u ? 4 : bytes_per_sample);
        }
        else
        {
            glPixelStorei(GL_UNPACK_ALIGNMENT, bytes_per_sample);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, row_components);
        }
    };

    // Prefer an async pixel-unpack buffer (PBO): orphan it, memcpy the plane in
    // while the GPU runs, then TexSubImage reads from the buffer offset so the
    // driver can DMA without stalling this thread on the previous frame. If the
    // driver cannot map an unpack buffer we fall back to a direct client-pointer
    // upload and stop retrying PBO for the rest of the session.
    const auto upload_plane = [&](GLuint pbo, const std::uint8_t *src, int pitch, int w, int h,
                                  GLenum format, int bytes_per_texel) -> bool
    {
        if (pbo == 0 || src == nullptr)
        {
            return false;
        }
        const std::size_t row_bytes =
            static_cast<std::size_t>(w) * static_cast<std::size_t>(bytes_per_texel);
        const std::size_t tight = row_bytes * static_cast<std::size_t>(h);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(tight), nullptr,
                     GL_STREAM_DRAW);
        void *dst = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
        if (dst == nullptr)
        {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            return false;
        }
        if (pitch == static_cast<int>(row_bytes))
        {
            std::memcpy(dst, src, tight);
        }
        else
        {
            auto *d = static_cast<std::uint8_t *>(dst);
            for (int r = 0; r < h; ++r)
            {
                std::memcpy(d + static_cast<std::size_t>(r) * row_bytes,
                            src + static_cast<std::size_t>(r) * pitch, row_bytes);
            }
        }
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, row_bytes % 4u == 0u ? 4 : bytes);
        glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 0, 0, w, h, format, type,
                        reinterpret_cast<const void *>(0));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return true;
    };

    const std::int64_t y_begin_us = sys::monotonic_us();
    glBindTexture(GL_TEXTURE_RECTANGLE, video_y_[video_slot_]);
    if (!(video_pbo_ok_ && upload_plane(video_pbo_y_[video_slot_], y, y_pitch, video_width_,
                                        video_height_, GL_RED, bytes)))
    {
        video_pbo_ok_ = false;
        set_unpack(y_pitch / bytes, video_width_, bytes, bytes);
        glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 0, 0, video_width_, video_height_, GL_RED, type,
                        y);
    }
    const std::int64_t y_end_us = sys::monotonic_us();

    std::int64_t uv_end_us = y_end_us;
    if (u != nullptr && u_pitch > 0)
    {
        glBindTexture(GL_TEXTURE_RECTANGLE, video_uv_[video_slot_]);
        // Interleaved UV is two components per texel; a planar U plane is one.
        const int uv_texel = planar_chroma ? bytes : 2 * bytes;
        if (!(video_pbo_ok_ && upload_plane(video_pbo_uv_[video_slot_], u, u_pitch, chroma_w,
                                            chroma_h, planar_chroma ? GL_RED : GL_RG, uv_texel)))
        {
            video_pbo_ok_ = false;
            set_unpack(u_pitch / uv_texel, chroma_w, uv_texel, bytes);
            glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 0, 0, chroma_w, chroma_h,
                            planar_chroma ? GL_RED : GL_RG, type, u);
        }
        uv_end_us = sys::monotonic_us();
    }
    std::int64_t v_end_us = uv_end_us;
    if (planar_chroma && v != nullptr && v_pitch > 0)
    {
        glBindTexture(GL_TEXTURE_RECTANGLE, video_v_[video_slot_]);
        if (!(video_pbo_ok_ && upload_plane(video_pbo_v_[video_slot_], v, v_pitch, chroma_w,
                                            chroma_h, GL_RED, bytes)))
        {
            video_pbo_ok_ = false;
            set_unpack(v_pitch / bytes, chroma_w, bytes, bytes);
            glTexSubImage2D(GL_TEXTURE_RECTANGLE, 0, 0, 0, chroma_w, chroma_h, GL_RED, type, v);
        }
        v_end_us = sys::monotonic_us();
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_RECTANGLE, 0);
    // Plane split: if each plane's cost tracks its byte count (Y ~2x UV) the
    // driver is throughput-bound and only fewer bytes or a DMA path helps; if
    // every call costs a similar fixed chunk it is a per-call stall and fewer
    // uploads is the lever.
    if (++upload_probe_ % 60 == 0)
    {
        sys::log("[HUI] upload planes y_us=%lld uv_us=%lld v_us=%lld pbo=%d %dx%d planar=%d",
                 static_cast<long long>(y_end_us - y_begin_us),
                 static_cast<long long>(uv_end_us - y_end_us),
                 static_cast<long long>(v_end_us - uv_end_us), video_pbo_ok_ ? 1 : 0, video_width_,
                 video_height_, planar_chroma ? 1 : 0);
    }
}

void GlBatch::draw(const DrawList &list, const Viewport &viewport, int surface_width,
                   int surface_height)
{
    draw_calls_ = 0;
    const auto &instances = list.instances();
    const auto &mesh = list.mesh_vertices();
    if (instances.empty() && mesh.empty())
        return;
    // Orphan, then fill: re-specifying storage avoids waiting on the GPU's
    // reads of the previous frame (ps5-opengl drains on same-size reuse).
    if (!instances.empty())
    {
        glBindBuffer(GL_ARRAY_BUFFER, buffer_);
        capacity_ = std::max(capacity_, instances.size());
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capacity_ * sizeof(Instance)),
                     nullptr, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(instances.size() * sizeof(Instance)),
                        instances.data());
    }
    if (!mesh.empty())
    {
        glBindBuffer(GL_ARRAY_BUFFER, mesh_buffer_);
        mesh_capacity_ = std::max(mesh_capacity_, mesh.size());
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh_capacity_ * sizeof(MeshVertex)),
                     nullptr, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(mesh.size() * sizeof(MeshVertex)), mesh.data());
    }

    glViewport(0, 0, surface_width, surface_height);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    for (GLuint program : {program_, mesh_program_})
    {
        glUseProgram(program);
        glUniform4f(0, viewport.scale, viewport.offset_x, viewport.offset_y, 0.0f);
        glUniform2f(1, static_cast<float>(surface_width), static_cast<float>(surface_height));
    }
    glUseProgram(program_);
    glUniform1i(2, 0);
    // Font atlases on units 1..kFontSlots for the whole frame; unit 0 changes per run.
    const GLint font_units[kFontSlots] = {1, 2, 3, 4, 5, 6, 7, 8};
    glUniform1iv(3, static_cast<GLsizei>(kFontSlots), font_units);
    for (std::uint32_t slot = 0; slot < font_count_; ++slot)
    {
        glActiveTexture(GL_TEXTURE1 + slot);
        glBindTexture(GL_TEXTURE_2D, font_textures_[slot]);
    }
    // Default the three video rectangle samplers onto dedicated units that hold
    // a 1x1 rectangle. A sampler2DRect left at its default unit 0 (a 2D texture)
    // is a sampler-target mismatch that makes Mesa reject this program's whole
    // instanced draw with GL_INVALID_OPERATION, blanking the UI on a cold boot
    // before any film opens. The block below overwrites these with the real
    // planes whenever a film is actually up.
    glUniform1i(11, 9);
    glUniform1i(12, 10);
    glUniform1i(13, 11);
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_RECTANGLE, dummy_rect_);
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_RECTANGLE, dummy_rect_);
    glActiveTexture(GL_TEXTURE11);
    glBindTexture(GL_TEXTURE_RECTANGLE, dummy_rect_);
    glActiveTexture(GL_TEXTURE0);
    if (video_display_slot_ >= 0 && video_y_[video_display_slot_] != 0)
    {
        // Fixed units, bound once for the frame: a video instance then costs
        // no state change at all inside the run.
        glUniform1i(11, 9);
        glUniform1i(12, 10);
        glUniform1i(13, 11);
        // Rectangle samplers need texel coordinates; hand the shader the luma
        // plane size so it can turn the quad's 0..1 uv into them.
        glUniform2f(25, static_cast<float>(video_width_), static_cast<float>(video_height_));
        // A sample of an R16 plane is value/65535 and of an R8 plane
        // value/255; these scale factors turn either into the 10-bit code the
        // shader's curves are defined on. The console's decoder leaves ten
        // bits low-aligned, P010LE puts them in the top six bits, so the
        // sample is 64 times larger there and the factor 64 times smaller.
        const float video_scale =
            !video_ten_bit_ ? 1020.0f : (video_ten_bit_low_aligned_ ? 65535.0f : 65535.0f / 64.0f);
        glUniform4f(14, video_planar_ ? 1.0f : 0.0f, static_cast<float>(video_matrix_), video_scale,
                    static_cast<float>(video_transfer_));
        glUniform4f(15, video_full_range_ ? 1.0f : 0.0f, video_peak_nits_,
                    video_wide_gamut_ ? 1.0f : 0.0f, video_ictcp_ ? 1.0f : 0.0f);
        glActiveTexture(GL_TEXTURE9);
        glBindTexture(GL_TEXTURE_RECTANGLE, video_y_[video_display_slot_]);
        glActiveTexture(GL_TEXTURE10);
        glBindTexture(GL_TEXTURE_RECTANGLE, video_uv_[video_display_slot_]);
        glActiveTexture(GL_TEXTURE11);
        glBindTexture(GL_TEXTURE_RECTANGLE, video_planar_ ? video_v_[video_display_slot_]
                                                          : video_uv_[video_display_slot_]);
        // Profile 5 only. GlBatch::init() creates a straight-ramp table, so a
        // film whose first RPU is still in flight reshapes to itself instead
        // of sampling texture 0 (black) or undefined memory.
        glUniform1i(16, 12);
        glActiveTexture(GL_TEXTURE12);
        glBindTexture(GL_TEXTURE_2D, dovi_lut_);
        // Three rows of each colour matrix, offsets folded into w. i0.w is the
        // validity flag; a film without an RPU keeps the BT.2100 fallback.
        glUniform4fv(17, 1, dovi_from_ycc_[0]);
        glUniform4fv(18, 1, dovi_from_ycc_[1]);
        glUniform4fv(19, 1, dovi_from_ycc_[2]);
        glUniform4fv(20, 1, dovi_to_rgb_[0]);
        glUniform4fv(21, 1, dovi_to_rgb_[1]);
        glUniform4fv(22, 1, dovi_to_rgb_[2]);
        glUniform1f(23, brightness_);
    }
    // HDR scanout flag is uploaded unconditionally: a UI-only frame (no video
    // planes bound yet) still has to land in the PQ domain, and the shader
    // reads u_hdr_out for every non-video fragment. program_ stays current
    // here, so this sits with the other fixed-location uploads.
    glUniform4f(24, hdr_out_[0], hdr_out_[1], hdr_out_[2], hdr_out_[3]);
    glActiveTexture(GL_TEXTURE0);

    GLuint bound_texture = 0;
    bool scissor = false;
    bool mesh_bound = false;
    glBindVertexArray(vao_);
    for (const Run &run : list.runs())
    {
        if (run.count == 0)
            continue;
        if (run.mesh != mesh_bound)
        {
            mesh_bound = run.mesh;
            glUseProgram(mesh_bound ? mesh_program_ : program_);
            glBindVertexArray(mesh_bound ? mesh_vao_ : vao_);
        }
        if (run.texture != 0 && run.texture != bound_texture)
        {
            glBindTexture(GL_TEXTURE_2D, run.texture);
            bound_texture = run.texture;
        }
        if (run.clipped)
        {
            const float x0 = std::floor(run.clip.x * viewport.scale + viewport.offset_x);
            const float y0 = std::floor(run.clip.y * viewport.scale + viewport.offset_y);
            const float x1 =
                std::ceil((run.clip.x + run.clip.w) * viewport.scale + viewport.offset_x);
            const float y1 =
                std::ceil((run.clip.y + run.clip.h) * viewport.scale + viewport.offset_y);
            if (!scissor)
                glEnable(GL_SCISSOR_TEST);
            scissor = true;
            glScissor(static_cast<GLint>(x0), surface_height - static_cast<GLint>(y1),
                      static_cast<GLsizei>(std::max(0.0f, x1 - x0)),
                      static_cast<GLsizei>(std::max(0.0f, y1 - y0)));
        }
        else if (scissor)
        {
            glDisable(GL_SCISSOR_TEST);
            scissor = false;
        }
        if (run.mesh)
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(run.first),
                         static_cast<GLsizei>(run.count));
        else
            glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(run.count),
                                              run.first);
        ++draw_calls_;
    }
    if (scissor)
        glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
}

} // namespace hui::gfx

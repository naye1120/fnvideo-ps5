// ps5-homebrew-ui - Album artwork: decoding on the worker, textures on the
// render thread.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Covers arrive as JPEG or PNG bytes from the NAS; FFmpeg turns them into
// RGBA8 on the network thread, and the frame that polls the answer stages the
// pixels. Only the render thread touches GL, so upload() is called once per
// frame from the app loop rather than from the screen.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hui::gfx
{
class Renderer;
}

namespace hui::fnos
{

// The edge a cover is kept at: enough for a 300-pixel tile and a 512-pixel
// now-playing square, small enough that forty of them are not a megabyte each.
constexpr int kCoverEdge = 512;

// Turns JPEG/PNG/WebP bytes into RGBA8, its longest edge at most `edge`. False
// when the bytes are not a picture the console can decode (always on the PC
// preview host, which has no FFmpeg).
bool decode_image(const std::string &bytes, int edge, std::vector<std::uint8_t> *rgba, int *width,
                  int *height);

// Holds the covers the screen has drawn, oldest-recached first: a tile that
// left the grid is dropped before a new one is uploaded, so a whole album list
// never sits in memory.
class ArtCache
{
  public:
    // `keep` is how many textures stay resident. The music shelf draws forty
    // covers at most, so that is the default; the video wall shows two rows of
    // bigger posters plus a hero and a still, and asks for its own number.
    explicit ArtCache(std::size_t keep = kKeep) : keep_(keep)
    {
    }

    // The pixels a finished answer brought. Waiting to be uploaded.
    void stage(const std::string &key, std::vector<std::uint8_t> rgba, int width, int height);
    // Uploads what arrived since the last call. Render thread only.
    void upload(gfx::Renderer &renderer);
    // The texture, 0 until it is drawn-able. Stamps the entry as used.
    std::uint32_t texture(const std::string &key);
    bool size(const std::string &key, int *width, int *height) const;
    // False when the cover is held or on its way, so the same bytes are never
    // fetched twice in a row.
    bool wants(const std::string &key) const;
    // Frees every texture (signing out).
    void clear();
    std::size_t held() const
    {
        return textures_.size();
    }

    static constexpr std::size_t kKeep = 40;

  private:
    struct Entry
    {
        std::uint32_t texture = 0;
        int width = 0;
        int height = 0;
        std::uint64_t used = 0;
    };
    // Decoded bytes waiting for the render thread to hand them to GL.
    struct Incoming
    {
        std::string key;
        std::vector<std::uint8_t> pixels;
        int width = 0;
        int height = 0;
    };

    std::unordered_map<std::string, Entry> textures_;
    std::vector<Incoming> incoming_;
    std::size_t keep_ = kKeep;
    std::uint64_t clock_ = 0;
};

} // namespace hui::fnos

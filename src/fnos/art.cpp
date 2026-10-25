// ps5-homebrew-ui - Album artwork implementation: FFmpeg decode, GL upload.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnos/art.hpp"

#ifdef HUI_PS5
#include "gfx/renderer.hpp"
#include "platform/ps5/system.hpp"

#include <GL/glcorearb.h>
#endif

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace hui::fnos
{

#ifdef HUI_PS5

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
// fnOS serves posters as .webp; this FFmpeg build cannot decode them, so they
// are handled with libwebp below and only the remaining formats reach FFmpeg.
#include <webp/decode.h>
}

namespace
{

// The bytes the server sent, read through a custom AVIO.
struct Memory
{
    const std::uint8_t *at = nullptr;
    const std::uint8_t *end = nullptr;
};

int read_memory(void *user, std::uint8_t *buffer, int size)
{
    auto *source = static_cast<Memory *>(user);
    const std::size_t left = static_cast<std::size_t>(source->end - source->at);
    const std::size_t want = std::min(left, static_cast<std::size_t>(size > 0 ? size : 0));
    if (want == 0)
    {
        // FFmpeg's custom-IO contract: a read that has run out of bytes must
        // report end-of-stream as a negative AVERROR, never as 0. Returning 0
        // makes the container probe treat "no bytes left" as "temporarily
        // nothing, try again", which spins avformat_find_stream_info on some
        // JPEGs (the .jpg posters that left grey tiles on the wall) and can
        // stall the single art worker behind them.
        return AVERROR_EOF;
    }
    std::memcpy(buffer, source->at, want);
    source->at += want;
    return static_cast<int>(want);
}

} // namespace

bool decode_image(const std::string &bytes, int edge, std::vector<std::uint8_t> *rgba, int *width,
                  int *height)
{
    if (bytes.empty() || rgba == nullptr || width == nullptr || height == nullptr)
        return false;

    // fnOS posters are .webp, which this FFmpeg build has no decoder for. Catch
    // the "RIFF....WEBP" header and route them through libwebp, which the
    // PacBrew sysroot ships. Everything else (png/jpeg) still falls through to
    // FFmpeg below.
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
        std::memcmp(bytes.data() + 8, "WEBP", 4) == 0)
    {
        WebPDecoderConfig config;
        if (!WebPInitDecoderConfig(&config))
        {
            return false;
        }
        const std::uint8_t *data = reinterpret_cast<const std::uint8_t *>(bytes.data());
        if (WebPGetFeatures(data, bytes.size(), &config.input) != VP8_STATUS_OK)
        {
            return false;
        }
        const int w = config.input.width;
        const int h = config.input.height;
        if (w <= 0 || h <= 0)
        {
            return false;
        }
        // libwebp can scale while decoding. Asking for the full poster first
        // made a 4K source allocate ~32 MB per tile before the texture upload
        // shrank it to 512, which is exactly the kind of spike the console
        // heap cannot afford on a wall of artwork.
        int out_w = w;
        int out_h = h;
        if (edge > 0)
        {
            const int longest = std::max(w, h);
            if (longest > edge)
            {
                const double scale = static_cast<double>(edge) / static_cast<double>(longest);
                out_w = std::max(1, static_cast<int>(static_cast<double>(w) * scale + 0.5));
                out_h = std::max(1, static_cast<int>(static_cast<double>(h) * scale + 0.5));
            }
        }
        config.output.colorspace = MODE_RGBA;
        if (out_w != w || out_h != h)
        {
            config.options.use_scaling = 1;
            config.options.scaled_width = out_w;
            config.options.scaled_height = out_h;
        }
        if (WebPDecode(data, bytes.size(), &config) != VP8_STATUS_OK)
        {
            WebPFreeDecBuffer(&config.output);
            return false;
        }
        const int decoded_w = config.output.width;
        const int decoded_h = config.output.height;
        const int stride = config.output.u.RGBA.stride;
        const std::uint8_t *pixels = config.output.u.RGBA.rgba;
        if (decoded_w <= 0 || decoded_h <= 0 || stride <= 0 || pixels == nullptr)
        {
            WebPFreeDecBuffer(&config.output);
            return false;
        }
        rgba->resize(static_cast<std::size_t>(decoded_w) * decoded_h * 4u);
        for (int row = 0; row < decoded_h; ++row)
        {
            std::memcpy(rgba->data() + static_cast<std::size_t>(row) * decoded_w * 4u,
                        pixels + static_cast<std::size_t>(row) * stride,
                        static_cast<std::size_t>(decoded_w) * 4u);
        }
        *width = decoded_w;
        *height = decoded_h;
        WebPFreeDecBuffer(&config.output);
        return true;
    }

    Memory source{reinterpret_cast<const std::uint8_t *>(bytes.data()),
                  reinterpret_cast<const std::uint8_t *>(bytes.data()) + bytes.size()};
    constexpr int kIoSize = 16 * 1024;
    // Zeroed, and AVPROBE_PADDING_SIZE bytes longer than the size given to the
    // avio context: a container probe reads past the bytes it was given and is
    // promised zeros there.
    std::uint8_t *io = static_cast<std::uint8_t *>(av_mallocz(kIoSize + AVPROBE_PADDING_SIZE));
    if (io == nullptr)
        return false;
    AVIOContext *avio = avio_alloc_context(io, kIoSize, 0, &source, read_memory, nullptr, nullptr);
    if (avio == nullptr)
    {
        av_free(io);
        return false;
    }
    AVFormatContext *format = avformat_alloc_context();
    if (format == nullptr)
    {
        avio_context_free(&avio);
        return false;
    }
    format->pb = avio;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;

    bool ok = false;
    AVFrame *frame = nullptr;
    AVFrame *scaled = nullptr;
    AVPacket *packet = av_packet_alloc();
    SwsContext *sws = nullptr;
    int out_w = 0;
    int out_h = 0;

    // The name is empty on purpose: the demuxer is chosen from the bytes.
    if (avformat_open_input(&format, "", nullptr, nullptr) >= 0 &&
        avformat_find_stream_info(format, nullptr) >= 0)
    {
        const int stream_index =
            av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream_index >= 0)
        {
            AVCodecParameters *parameters = format->streams[stream_index]->codecpar;
            const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
            AVCodecContext *context = codec == nullptr ? nullptr : avcodec_alloc_context3(codec);
            if (context != nullptr && avcodec_parameters_to_context(context, parameters) >= 0 &&
                avcodec_open2(context, codec, nullptr) >= 0)
            {
                frame = av_frame_alloc();
                while (av_read_frame(format, packet) >= 0)
                {
                    if (packet->stream_index == stream_index &&
                        avcodec_send_packet(context, packet) >= 0 &&
                        avcodec_receive_frame(context, frame) >= 0)
                        break;
                    av_packet_unref(packet);
                }
                if (frame != nullptr && frame->width > 0 && frame->height > 0)
                {
                    out_w = frame->width;
                    out_h = frame->height;
                    if (edge > 0 && std::max(out_w, out_h) > edge)
                    {
                        const double scale = static_cast<double>(edge) / std::max(out_w, out_h);
                        out_w = std::max(1, static_cast<int>(out_w * scale));
                        out_h = std::max(1, static_cast<int>(out_h * scale));
                    }
                    scaled = av_frame_alloc();
                    if (scaled != nullptr && av_image_alloc(scaled->data, scaled->linesize, out_w,
                                                            out_h, AV_PIX_FMT_RGBA, 32) >= 0)
                    {
                        sws = sws_getContext(
                            frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                            out_w, out_h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                        if (sws != nullptr)
                            sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                                      scaled->data, scaled->linesize);
                    }
                }
                if (sws != nullptr && scaled != nullptr)
                {
                    // Rows, because a plane's stride is padded past its width.
                    rgba->resize(static_cast<std::size_t>(out_w) * out_h * 4u);
                    for (int row = 0; row < out_h; ++row)
                    {
                        std::memcpy(rgba->data() + static_cast<std::size_t>(row) * out_w * 4u,
                                    scaled->data[0] +
                                        static_cast<std::size_t>(row) * scaled->linesize[0],
                                    static_cast<std::size_t>(out_w) * 4u);
                    }
                    *width = out_w;
                    *height = out_h;
                    ok = true;
                }
                avcodec_free_context(&context);
            }
            else if (context != nullptr)
                avcodec_free_context(&context);
        }
    }

    if (sws != nullptr)
        sws_freeContext(sws);
    if (scaled != nullptr)
    {
        av_freep(&scaled->data[0]);
        av_frame_free(&scaled);
    }
    if (frame != nullptr)
        av_frame_free(&frame);
    if (packet != nullptr)
        av_packet_free(&packet);
    if (format != nullptr)
        avformat_close_input(&format);
    if (avio != nullptr)
        avio_context_free(&avio);
    return ok;
}

#else // the PC preview host has no FFmpeg; the fake server sends no artwork.

bool decode_image(const std::string &, int, std::vector<std::uint8_t> *, int *, int *)
{
    return false;
}

#endif

void ArtCache::stage(const std::string &key, std::vector<std::uint8_t> rgba, int width, int height)
{
    if (key.empty() || rgba.empty() || width <= 0 || height <= 0)
        return;
    if (textures_.find(key) != textures_.end())
        return; // it is on the GPU already
    for (auto &waiting : incoming_)
    {
        if (waiting.key == key)
        {
            waiting.pixels = std::move(rgba);
            waiting.width = width;
            waiting.height = height;
            return;
        }
    }
    incoming_.push_back({key, std::move(rgba), width, height});
}

#ifdef HUI_PS5
void ArtCache::upload(gfx::Renderer &renderer)
{
    for (const Incoming &waiting : incoming_)
    {
        if (textures_.find(waiting.key) != textures_.end())
            continue;
        // Room for one more: drop the cover drawn longest ago.
        while (textures_.size() + 1 > keep_)
        {
            auto oldest = textures_.begin();
            for (auto it = textures_.begin(); it != textures_.end(); ++it)
            {
                if (it->second.used < oldest->second.used)
                    oldest = it;
            }
            if (oldest->second.texture != 0)
                glDeleteTextures(1, &oldest->second.texture);
            textures_.erase(oldest);
        }
        Entry entry;
        entry.width = waiting.width;
        entry.height = waiting.height;
        entry.used = ++clock_;
        entry.texture =
            renderer.batch().create_texture(entry.width, entry.height, waiting.pixels.data());
        // The log's only way to tell "decoded" from "on the GPU". A picture
        // that exists here and still does not appear points at the draw path,
        // not at the fetch. Raised to 400 so every card in a library can be
        // traced per key for one clean diagnostic round: a fetched key with no
        // matching texture line is stuck in the upload/draw path, not the network.
        static int logged = 0;
        if (logged < 400)
        {
            ++logged;
            sys::log("[HUI] art texture key=%s id=%u %dx%d", waiting.key.c_str(), entry.texture,
                     entry.width, entry.height);
        }
        textures_.emplace(waiting.key, entry);
    }
    incoming_.clear();
}
#else
// The PC preview never decodes a cover (there is no FFmpeg), so this queue is
// always empty; the tests have no GL context to upload into.
void ArtCache::upload(gfx::Renderer &)
{
    // Named so the host build, where no texture is ever uploaded, does not
    // report the budget as an unused field.
    (void)keep_;
    incoming_.clear();
}
#endif

std::uint32_t ArtCache::texture(const std::string &key)
{
    const auto found = textures_.find(key);
    if (found == textures_.end())
        return 0;
    found->second.used = ++clock_;
    return found->second.texture;
}

bool ArtCache::size(const std::string &key, int *width, int *height) const
{
    const auto found = textures_.find(key);
    if (found == textures_.end())
        return false;
    *width = found->second.width;
    *height = found->second.height;
    return true;
}

bool ArtCache::wants(const std::string &key) const
{
    if (key.empty())
        return false;
    if (textures_.find(key) != textures_.end())
        return false;
    for (const Incoming &waiting : incoming_)
    {
        if (waiting.key == key)
            return false;
    }
    return true;
}

void ArtCache::clear()
{
#ifdef HUI_PS5
    for (auto &entry : textures_)
    {
        if (entry.second.texture != 0)
            glDeleteTextures(1, &entry.second.texture);
    }
#endif
    textures_.clear();
    incoming_.clear();
}

} // namespace hui::fnos

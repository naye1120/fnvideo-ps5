// ps5-homebrew-ui - streaming decoder implementation.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// On the console the bytes come from a ranged HTTP reader and ffmpeg turns
// them into sound. The PC snapshot host has no ffmpeg, so the same class is
// inert there and only keeps its state for the preview screens.

#include "fnos/player.hpp"

#include "audio/mixer.hpp"
#include "fnos/http.hpp"
#include "platform/ps5/system.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace hui::fnos
{

const char *player_error_text(PlayerError error)
{
    switch (error)
    {
    case PlayerError::none:
        return "";
    case PlayerError::network:
        return "网络中断，请重试";
    case PlayerError::denied:
        return "服务器拒绝访问，请重新登录";
    case PlayerError::missing:
        return "音频文件已不在服务器上";
    case PlayerError::unsupported:
        return "这个格式暂时播不了";
    case PlayerError::decode:
        return "音频解码失败";
    }
    return "播放失败";
}

Player::Player() = default;

#ifdef HUI_PS5

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

    int sceKernelDebugOutText(int channel, const char *text);
    int sceKernelUsleep(unsigned int microseconds);
}

#include <errno.h>

namespace
{

void wait_us(unsigned int microseconds)
{
    sceKernelUsleep(microseconds);
}

void note(const char *text)
{
    // Through sys::log, not just klog: the on-disk log is the only channel the
    // next launch can read back, and this is the line that names why a stream
    // was refused or a container rejected.
    sys::log("[HUI] %s", text);
}

std::string av_text(int code)
{
    char message[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(code, message, sizeof(message)) == 0)
        return message;
    return "error " + std::to_string(code);
}

// Ranged HTTP reads behind an AVIOContext: ffmpeg asks, ByteReader fetches
// exactly the bytes it wants.
struct Source
{
    ByteReader reader;
    std::uint64_t offset = 0;
    // How many callbacks have been described in the log: enough of each to see
    // where a probe walks, small enough to leave the tail readable.
    int reads = 0;
    int seeks = 0;
};

constexpr int kAvioBuffer = 64 * 1024;
// Demuxers look at the bytes past what they asked for when they score a
// container (avformat.h: the probe buffer "must have AVPROBE_PADDING_SIZE of
// extra allocated bytes filled with zero"). Handing them fresh av_malloc
// memory is not innocent: this block is very often the one the cover decoder
// just released, so those bytes still look like compressed media and a probe
// walks out of the buffer looking for a header that is not there.
constexpr int kAvioIoSize = kAvioBuffer + AVPROBE_PADDING_SIZE;

// How much stack this thread really got. pthread_attr_setstacksize() answers
// "yes" here, but nothing has ever proved the SDK handed out the 4 MB it
// agreed to, and the app dies inside the first deep ffmpeg call on this very
// thread. Each level owns a 64 KiB frame and touches one byte of every page in
// it, so the memory is really written rather than optimised away; the caller
// logs each depth before descending into it, and the last line names the
// ceiling.
__attribute__((noinline)) int stack_reach(int levels)
{
    volatile char frame[64 * 1024];
    int touched = 0;
    for (std::size_t page = 0; page < sizeof(frame); page += 4096)
    {
        frame[page] = static_cast<char>(levels);
        touched += frame[page] == '\0';
    }
    if (levels > 0)
        touched += stack_reach(levels - 1);
    return touched;
}

int source_read(void *opaque, unsigned char *buffer, int size)
{
    auto *source = static_cast<Source *>(opaque);
    // Written before the bytes are asked for: a crash inside the transfer then
    // still leaves this line, and the ladder says "died on the wire" instead of
    // "died somewhere after the first read".
    if (source->reads < 20)
    {
        ++source->reads;
        sys::log("[HUI] decoder io get off=%llu want=%d",
                 static_cast<unsigned long long>(source->offset), size);
    }
    const std::size_t got =
        source->reader.read(source->offset, buffer, static_cast<std::size_t>(size));
    if (got == 0)
    {
        // The file running out is EOF whatever an earlier transfer complained
        // about: ffmpeg has to hear it in its own words, or the end of a song
        // looks like a dead network and the queue stops.
        const long long total = source->reader.size();
        if (total >= 0 && static_cast<long long>(source->offset) >= total)
            return AVERROR_EOF;
        if (source->reads < 20 && !source->reader.error().empty())
            sys::log("[HUI] decoder io read failed: %s", source->reader.error().c_str());
        return source->reader.error().empty() ? AVERROR_EOF : AVERROR(EIO);
    }
    source->offset += got;
    return static_cast<int>(got);
}

int64_t source_seek(void *opaque, int64_t offset, int whence)
{
    auto *source = static_cast<Source *>(opaque);
    if (source->seeks < 20)
    {
        ++source->seeks;
        sys::log("[HUI] decoder io seek ask off=%lld whence=%d", static_cast<long long>(offset),
                 whence);
    }
    const long long total = source->reader.size();
    long long target = static_cast<long long>(offset);
    switch (whence & ~AVSEEK_FORCE)
    {
    case AVSEEK_SIZE:
        return total;
    case SEEK_SET:
        break;
    case SEEK_CUR:
        target = static_cast<long long>(source->offset) + offset;
        break;
    case SEEK_END:
        if (total < 0)
            return -1;
        target = total + offset;
        break;
    default:
        return -1;
    }
    if (target < 0 || (total >= 0 && target > total))
        return -1;
    source->offset = static_cast<std::uint64_t>(target);
    return target;
}

} // namespace

Player::~Player()
{
    keep_running_.store(false);
    if (worker_running_)
    {
        pthread_join(worker_, nullptr);
        worker_running_ = false;
    }
    if (mixer_ != nullptr)
    {
        // The audio thread must stop touching the rings before they go away;
        // the swap lands on its command queue a few grains later.
        mixer_->set_stream_gain(kSlot, 0.0f, 0.0f);
        mixer_->swap_stream(kSlot, nullptr);
        wait_us(200000);
    }
    delete ring_;
    delete last_ring_;
}

void Player::attach(audio::Mixer &mixer)
{
    mixer_ = &mixer;
    if (ring_ != nullptr)
        return;
    ring_ = new (std::nothrow) audio::StreamRing(kRingFrames);
    if (ring_ == nullptr)
        return;
    // The menu music owns slot 0, so the NAS stream takes slot 1.
    mixer.attach_stream(kSlot, ring_);
}

bool Player::play(const std::string &url, const std::vector<std::string> &headers)
{
    if (mixer_ == nullptr || ring_ == nullptr)
        return false;
    stop();
    // The ring still holds up to 1.4 s of whatever was playing before this. The
    // mixer reads it every grain whether a song is decoded into it or not, so
    // without a fresh one a chosen track opens with the tail of the last, and
    // while the new address is still being probed that tail is all there is.
    retarget();
    url_ = url;
    headers_ = headers;
    error_.store(static_cast<int>(PlayerError::none));
    position_.store(0);
    duration_.store(0);
    paused_.store(false);
    seek_request_.store(-1);
    state_.store(static_cast<int>(PlayerState::opening));
    mixer_->set_stream_gain(kSlot, kStreamGain, 0.0f);
    keep_running_.store(true);
    // ffmpeg's demuxer runs on this thread, and avformat_find_stream_info alone
    // wants more stack than the SDK gives a default thread: the app dies the
    // moment a song is chosen. If 4 MB cannot be had, fall back to the default.
    pthread_attr_t attr;
    bool sized = pthread_attr_init(&attr) == 0;
    if (sized)
        sized = pthread_attr_setstacksize(&attr, 4u * 1024u * 1024u) == 0;
    int created = pthread_create(&worker_, sized ? &attr : nullptr, &Player::run, this);
    if (created != 0 && sized)
        created = pthread_create(&worker_, nullptr, &Player::run, this);
    if (sized)
        pthread_attr_destroy(&attr);
    if (created != 0)
    {
        keep_running_.store(false);
        state_.store(static_cast<int>(PlayerState::stopped));
        error_.store(static_cast<int>(PlayerError::network));
        return false;
    }
    worker_running_ = true;
    sys::log("[HUI] decoder stack=%d", sized ? 4096 : 0);
    return true;
}

void Player::pause()
{
    if (state_.load(std::memory_order_relaxed) == static_cast<int>(PlayerState::playing))
    {
        paused_.store(true);
        state_.store(static_cast<int>(PlayerState::paused));
    }
}

void Player::resume()
{
    if (state() == PlayerState::paused)
    {
        paused_.store(false);
        state_.store(static_cast<int>(PlayerState::playing));
    }
}

void Player::stop()
{
    keep_running_.store(false);
    if (worker_running_)
    {
        pthread_join(worker_, nullptr);
        worker_running_ = false;
    }
    paused_.store(false);
    seek_request_.store(-1);
    state_.store(static_cast<int>(PlayerState::stopped));
    if (mixer_ != nullptr)
        mixer_->set_stream_gain(kSlot, 0.0f, 0.1f);
}

void Player::seek(int seconds)
{
    if (seconds < 0)
        seconds = position() + seconds;
    const int length = duration();
    if (length > 0 && seconds >= length)
        seconds = length > 1 ? length - 1 : 0;
    if (seconds < 0)
        seconds = 0;
    seek_request_.store(seconds);
    position_.store(seconds);
    if (state() == PlayerState::ended || state() == PlayerState::paused)
    {
        paused_.store(false);
        state_.store(static_cast<int>(PlayerState::playing));
    }
}

void Player::set_gain(float gain)
{
    if (mixer_ != nullptr)
        mixer_->set_stream_gain(kSlot, gain, 0.05f);
}

void Player::fail(PlayerError error)
{
    error_.store(static_cast<int>(error));
    state_.store(static_cast<int>(PlayerState::stopped));
    keep_running_.store(false);
}

void Player::retarget()
{
    if (mixer_ == nullptr)
        return;
    auto *next = new (std::nothrow) audio::StreamRing(kRingFrames);
    if (next == nullptr)
        return;
    mixer_->swap_stream(kSlot, next);
    // last_ring_ left the mixer's slot at the seek before this one, which is
    // seconds ago, so the audio thread can no longer be reading it.
    delete last_ring_;
    last_ring_ = ring_;
    ring_ = next;
}

void *Player::run(void *user)
{
    auto *player = static_cast<Player *>(user);
    player->decode(player->url_, player->headers_);
    return nullptr;
}

void Player::decode(const std::string &url, const std::vector<std::string> &headers)
{
    // Measure this thread before trusting it with ffmpeg. Stopping at 2 MB on
    // purpose: a song needs less than that, so if the fill survives the answer
    // is "stack is not the culprit" and a title that would have played still
    // plays. If it dies here, the last line names the real ceiling.
    for (const int levels : {4, 16, 32})
    {
        sys::log("[HUI] decoder stack fill %d", levels * 64);
        stack_reach(levels);
    }
    sys::log("[HUI] decoder stack fill done");
    Source source;
    std::string reason;
    if (!source.reader.open(url, headers, &reason))
    {
        // Under the "decoder " prefix, so the mirror that survives a broken
        // stdout catches it too: this is the line that says why the stream was
        // refused, and it is often the only one.
        sys::log("[HUI] decoder open failed: %s", reason.c_str());
        fail(PlayerError::network);
        return;
    }
    // The first line the second network thread writes: if this one is missing
    // while "play decoder thread=1" arrived, curl died on the way. The name at
    // the end says which container is about to be probed, which the ladder had
    // no way to tell.
    {
        std::string name = url.substr(0, url.find_first_of("?#"));
        const std::size_t slash = name.rfind('/');
        if (slash != std::string::npos)
            name.erase(0, slash + 1);
        sys::log("[HUI] decoder bytes ready %s", name.c_str());
    }

    AVFormatContext *format = nullptr;
    AVCodecContext *context = nullptr;
    SwrContext *resample = nullptr;
    AVIOContext *avio = nullptr;
    bool ok = true;
    // Which step gave up: the failure text the pill shows is this word, since
    // ffmpeg's own answer here would be one generic placeholder for five
    // different causes.
    const char *stage = "io buffer";

    // The address the server gives ends in "/stream", so there is no file name
    // to read a container from, and a probe that dies never says which demuxer
    // it died in. Score the first bytes here, once, out loud: the ladder then
    // names the container even when the open below never returns, and the open
    // itself is handed that container instead of running the same scan again
    // over its own buffer.
    const AVInputFormat *guessed = nullptr;
    {
        unsigned char *head =
            static_cast<unsigned char *>(av_mallocz(kAvioBuffer + AVPROBE_PADDING_SIZE));
        if (head != nullptr)
        {
            sys::log("[HUI] decoder guess start");
            const std::size_t got = source.reader.read(0, head, kAvioBuffer);
            sys::log("[HUI] decoder guess of %d", static_cast<int>(got));
            AVProbeData probe{};
            // Never NULL: several demuxers' scoring looks at the name (the
            // image and subtitle ones match an extension before they look at a
            // single byte), and ffmpeg's own probe only ever hands them a name
            // it got from avformat_open_input -- "" at worst, never 0.
            probe.filename = "";
            probe.mime_type = "";
            probe.buf = head;
            probe.buf_size = static_cast<int>(got);
            guessed = av_probe_input_format(&probe, 0);
            sys::log("[HUI] decoder guess fmt=%s", guessed != nullptr ? guessed->name : "none");
            av_free(head);
        }
    }

    unsigned char *buffer = static_cast<unsigned char *>(av_mallocz(kAvioIoSize));
    if (buffer == nullptr)
        ok = false;
    if (ok)
    {
        avio =
            avio_alloc_context(buffer, kAvioBuffer, 0, &source, source_read, nullptr, source_seek);
        ok = avio != nullptr;
        if (!ok)
            av_free(buffer);
    }
    if (ok)
    {
        stage = "container";
        format = avformat_alloc_context();
        ok = format != nullptr;
    }
    if (ok)
    {
        stage = "probe";
        format->pb = avio;
        // Without this the container's own cleanup avio_closes *our* AVIOContext
        // and its 64 KiB buffer, and the avio_context_free below frees them a
        // second time: the heap takes the hit on the way out of a song, which
        // reads as the title dying shortly after a press. Say the io is ours,
        // exactly as the cover-art decoder already does.
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        // The address carries no file name, so the container is probed from
        // the bytes themselves.
        sys::log("[HUI] decoder probe");
        const int opened = avformat_open_input(&format, "", guessed, nullptr);
        ok = opened >= 0;
        if (!ok)
        {
            // open_input freed the context it was given.
            format = nullptr;
            sys::log("[HUI] decoder probe rc=%d", opened);
        }
        else
            sys::log("[HUI] decoder probe ok fmt=%s", format->iformat->name);
    }
    if (ok)
    {
        stage = "stream info";
        sys::log("[HUI] decoder stream info");
        ok = avformat_find_stream_info(format, nullptr) >= 0;
    }
    int stream_index = -1;
    if (ok)
    {
        stage = "stream pick";
        stream_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        ok = stream_index >= 0;
    }
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    if (ok)
    {
        stage = "codec";
        codec_id = format->streams[stream_index]->codecpar->codec_id;
        const AVCodec *codec = avcodec_find_decoder(codec_id);
        ok = codec != nullptr;
        if (ok)
        {
            context = avcodec_alloc_context3(codec);
            ok = context != nullptr;
        }
        if (ok)
        {
            avcodec_parameters_to_context(context, format->streams[stream_index]->codecpar);
            ok = avcodec_open2(context, codec, nullptr) >= 0;
            sys::log("[HUI] decoder codec=%d open=%d", static_cast<int>(codec_id), ok ? 1 : 0);
        }
    }
    if (ok)
    {
        stage = "resample";
        AVChannelLayout stereo;
        av_channel_layout_default(&stereo, 2);
        ok = swr_alloc_set_opts2(&resample, &stereo, AV_SAMPLE_FMT_FLT, audio::kSampleRate,
                                 &context->ch_layout,
                                 static_cast<AVSampleFormat>(context->sample_fmt),
                                 context->sample_rate, 0, nullptr) >= 0;
        if (ok)
            ok = swr_init(resample) >= 0;
    }
    if (!ok)
    {
        // Names the step instead of ffmpeg's one-size-fits-all string, and says
        // it before the frees: if the title dies on the way out, this is the
        // last line in the log.
        sys::log("[HUI] decoder failed at %s", stage);
        if (resample != nullptr)
            swr_free(&resample);
        if (context != nullptr)
            avcodec_free_context(&context);
        if (format != nullptr)
            avformat_close_input(&format);
        if (avio != nullptr)
            avio_context_free(&avio);
        fail(codec_id == AV_CODEC_ID_NONE ? PlayerError::unsupported : PlayerError::decode);
        return;
    }

    AVStream *stream = format->streams[stream_index];
    // ffmpeg's duration covers the whole container, the header's offset
    // included. The clock above counts from where the music starts, so the two
    // have to be cut the same way or the bar's total outlives the song.
    int seconds = 0;
    if (format->duration > 0)
    {
        const std::int64_t from = format->start_time != AV_NOPTS_VALUE ? format->start_time : 0;
        seconds = static_cast<int>((format->duration - from) / AV_TIME_BASE);
        if (seconds < 0)
            seconds = 0;
    }
    duration_.store(seconds);
    // What the container turned out to be: the number a wrong-sounding track is
    // named by, since a 44.1 kHz file and a mis-picked stream look the same
    // until someone reads the rate back.
    sys::log("[HUI] decoder open %s rate=%d ch=%d len=%d start=%lld", avcodec_get_name(codec_id),
             context->sample_rate, context->ch_layout.nb_channels, seconds,
             static_cast<long long>(format->start_time));
    state_.store(static_cast<int>(PlayerState::playing));

    std::vector<float> pcm;
    std::int64_t counted = 0;
    const int input_rate = context->sample_rate > 0 ? context->sample_rate : audio::kSampleRate;
    // One marker for the first samples that reach the mixer: a death after this
    // line belongs to the audio thread, a death before it to the decoder.
    bool first_audio = true;

    // One decoded frame to the ring, blocking while the audio thread catches up.
    auto emit = [&](AVFrame *decoded)
    {
        const int room = static_cast<int>(
            av_rescale_rnd(swr_get_delay(resample, input_rate) + decoded->nb_samples,
                           audio::kSampleRate, input_rate, AV_ROUND_UP));
        if (room <= 0)
            return;
        pcm.resize(static_cast<std::size_t>(room) * 2);
        // A copy, because uint8_t ** does not convert to const uint8_t ** in C++.
        const uint8_t *input[AV_NUM_DATA_POINTERS] = {};
        for (int plane = 0; plane < AV_NUM_DATA_POINTERS; ++plane)
            input[plane] = decoded->data[plane];
        uint8_t *output[1] = {reinterpret_cast<uint8_t *>(pcm.data())};
        const int produced = swr_convert(resample, output, room, input, decoded->nb_samples);
        if (produced <= 0)
            return;
        if (first_audio)
        {
            first_audio = false;
            sys::log("[HUI] decoder audio %d", produced);
        }
        std::size_t written = 0;
        while (written < static_cast<std::size_t>(produced))
        {
            if (!keep_running_.load())
                return;
            const std::size_t got =
                ring_ == nullptr ? 0
                                 : ring_->write(pcm.data() + written * 2,
                                                static_cast<std::size_t>(produced) - written);
            written += got;
            if (got == 0)
                wait_us(10000);
        }
        counted += produced;
        // What the speaker has reached, not what the decoder has: the ring is
        // up to 1.4 s deep and the decoder keeps it full, so a clock read off
        // the decoded frames runs that far ahead of the music.
        const int heard = static_cast<int>(ring_ == nullptr ? 0 : ring_->available()) /
                          static_cast<int>(audio::kSampleRate);
        // A container's clock need not start at zero (an mp4 edit list, a long
        // ID3 header): the seconds shown are the seconds since the music does.
        const std::int64_t base = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
        const int stamp = decoded->best_effort_timestamp != AV_NOPTS_VALUE
                              ? static_cast<int>(av_rescale_q(decoded->best_effort_timestamp - base,
                                                              stream->time_base, AV_TIME_BASE_Q) /
                                                 AV_TIME_BASE)
                              : static_cast<int>(counted / audio::kSampleRate);
        position_.store(std::max(0, stamp - heard));
    };

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool ended = false;
    while (keep_running_.load(std::memory_order_relaxed) && packet != nullptr && frame != nullptr)
    {
        const int wanted = seek_request_.exchange(-1, std::memory_order_acq_rel);
        if (wanted >= 0)
        {
            // av_rescale_q reads its input as ticks of the rational it is
            // given, and AV_TIME_BASE_Q is a microsecond: the seconds have to
            // be multiplied up first. Handing it the bare seconds asked the
            // demuxer for a fraction of one, which every container answered
            // with its first byte -- so every drag of the bar restarted the
            // song however far it had been pulled.
            const long long target = av_rescale_q(static_cast<std::int64_t>(wanted) * AV_TIME_BASE,
                                                  AV_TIME_BASE_Q, stream->time_base);
            const int sought = avformat_seek_file(format, stream_index, INT64_MIN, target, target,
                                                  AVSEEK_FLAG_BACKWARD);
            // Whether the stream took the jump is the whole answer to a fast
            // forward that moves the bar and nothing else, and a finger has to
            // ask for one before this line can be written.
            sys::log("[HUI] decoder seek %d -> %d", wanted, sought);
            if (sought >= 0)
            {
                avcodec_flush_buffers(context);
                retarget();
                counted = static_cast<std::int64_t>(wanted) * audio::kSampleRate;
            }
            else
            {
                note("seek refused by the stream");
            }
        }
        if (paused_.load(std::memory_order_relaxed))
        {
            wait_us(40000);
            continue;
        }
        const int read = av_read_frame(format, packet);
        if (read == AVERROR_EOF)
        {
            ended = true;
            break;
        }
        if (read < 0)
        {
            // A server that answers the last request of a file with an error
            // instead of an empty body still ended the song: once the bytes have
            // run out and sound has been flowing, that is the tape finishing, and
            // the queue has to be allowed to move on.
            const long long total = source.reader.size();
            if (counted > 0 && total >= 0 && static_cast<long long>(source.offset) >= total)
            {
                note("read ran past the end of the file");
                ended = true;
                break;
            }
            note(av_text(read).c_str());
            fail(PlayerError::network);
            break;
        }
        if (packet->stream_index == stream_index && avcodec_send_packet(context, packet) == 0)
        {
            while (avcodec_receive_frame(context, frame) == 0)
            {
                emit(frame);
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }
    // Either side of the frees, so a death in the release chain is no longer
    // mistaken for a death in the decode loop.
    sys::log("[HUI] decoder loop done");
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&resample);
    avcodec_free_context(&context);
    avformat_close_input(&format);
    avio_context_free(&avio);
    sys::log("[HUI] decoder memory freed");

    sys::log("[HUI] decoder stop ended=%d frames=%lld pos=%d err=%d", ended ? 1 : 0,
             static_cast<long long>(counted), position_.load(), static_cast<int>(error()));
    if (ended && error() == PlayerError::none)
    {
        // EOF arrives while up to 1.4 s of the song is still in the ring, and the
        // next play() swaps that ring out: every track lost its last second, and
        // a file that yields nothing ended so fast that the queue walked itself
        // back to song one. Let the speaker catch up before calling it over.
        for (int spin = 0; spin < 90 && keep_running_.load(std::memory_order_relaxed); ++spin)
        {
            if (ring_ == nullptr || ring_->available() == 0)
                break;
            wait_us(50000);
        }
        if (counted <= 0)
            // Nothing ever reached the speaker. Say so instead of calling that a
            // finished track, which the queue would walk past in silence.
            fail(PlayerError::unsupported);
        else
            state_.store(static_cast<int>(PlayerState::ended));
    }
    else if (!ended)
        state_.store(static_cast<int>(PlayerState::stopped));
    keep_running_.store(false);
}

#else // PC snapshot host: no ffmpeg, so nothing decodes and nothing is heard.

Player::~Player() = default;

void Player::attach(audio::Mixer &mixer)
{
    mixer_ = &mixer;
}

bool Player::play(const std::string &url, const std::vector<std::string> &)
{
    url_ = url;
    state_.store(static_cast<int>(PlayerState::playing));
    return true;
}

void Player::pause()
{
    paused_.store(true);
    state_.store(static_cast<int>(PlayerState::paused));
}

void Player::resume()
{
    paused_.store(false);
    state_.store(static_cast<int>(PlayerState::playing));
}

void Player::stop()
{
    paused_.store(false);
    state_.store(static_cast<int>(PlayerState::stopped));
}

void Player::seek(int seconds)
{
    position_.store(seconds < 0 ? 0 : seconds);
}

void Player::set_gain(float)
{
}

void Player::fail(PlayerError error)
{
    error_.store(static_cast<int>(error));
    state_.store(static_cast<int>(PlayerState::stopped));
}

void Player::retarget()
{
}

void *Player::run(void *)
{
    return nullptr;
}

void Player::decode(const std::string &, const std::vector<std::string> &)
{
}

#endif

} // namespace hui::fnos

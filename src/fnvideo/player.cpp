// ps5-homebrew-ui - The video player. See player.hpp for why it is shaped
// this way.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnvideo/player.hpp"

#include "fnvideo/dovi.hpp"
#include "fnvideo/hwdec_ps5.hpp"
#include "platform/ps5/system.hpp"

#ifdef HUI_PS5
// ffmpeg and the renderer are included here, at file scope: a header pulled in
// inside a namespace declares its types in that namespace, and the ones
// player.hpp forward-declares are not.
#include "gfx/renderer.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dovi_meta.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

extern "C" int sceKernelUsleep(unsigned int microseconds);
// The heap counters, so a crash in the middle of a film can be told apart
// from a decoder fault: `failures` climbing means the heap ran out, which is
// what a 4K film with several 18 MB pictures queued does. See the stats line.
extern "C" void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes,
                               std::size_t *blocks, std::size_t *failures);
extern "C" std::size_t hui_heap_capacity();
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace hui::fnvideo
{

namespace
{

void wait_us(unsigned int microseconds)
{
#ifdef HUI_PS5
    sceKernelUsleep(microseconds);
#else
    (void)microseconds;
#endif
}

#ifdef HUI_PS5

// Packets waiting to be decoded, filled by one thread and drained by another.
//
// This is the whole difference between a picture that stutters and one that
// does not. Reading and decoding used to share a thread, so a slow read - a
// stalled range request, a TCP that dropped - stopped the decoder, and a full
// audio buffer stopped the reader. Separated, a slow read only makes this
// queue deeper; the picture carries on from what is already here.
//
// The queue is measured in bytes, because that is what the heap cares about,
// and it is capped well below what the reference implementation uses: 256 MB
// of packets is affordable when the heap is a gigabyte, not when it is 256 MB
// and a single 4K picture is 18 MB of it.
struct PacketQueue
{
    std::deque<AVPacket *> items;
    mutable std::mutex m;
    std::condition_variable cv;
    std::size_t bytes = 0;
    bool eof = false;
    bool abort = false;
    // Bumped by every flush. A consumer that remembers the value it last saw
    // knows its decoder is holding state from before a seek, and flushes it
    // itself - which is the only safe place to do it, since the decoder is
    // that consumer's to touch.
    int serial = 0;

    void put(AVPacket *pkt)
    {
        {
            std::lock_guard<std::mutex> lock(m);
            bytes += static_cast<std::size_t>(pkt->size);
            items.push_back(pkt);
        }
        cv.notify_one();
    }
    // 1 a packet, 0 nothing yet, -1 stopped, -2 end of file.
    int get(AVPacket **out, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(m);
        if (items.empty() && !eof && !abort)
        {
            cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return !items.empty() || eof || abort; });
        }
        if (abort)
        {
            return -1;
        }
        if (items.empty())
        {
            return eof ? -2 : 0;
        }
        *out = items.front();
        items.pop_front();
        bytes -= static_cast<std::size_t>((*out)->size);
        lock.unlock();
        // The producer may be waiting for room.
        cv.notify_all();
        return 1;
    }
    int serial_value() const
    {
        std::lock_guard<std::mutex> lock(m);
        return serial;
    }
    void flush()
    {
        std::lock_guard<std::mutex> lock(m);
        for (AVPacket *pkt : items)
        {
            av_packet_free(&pkt);
        }
        items.clear();
        bytes = 0;
        eof = false;
        ++serial;
        cv.notify_all();
    }
    void finish(bool stopped)
    {
        {
            std::lock_guard<std::mutex> lock(m);
            if (stopped)
            {
                abort = true;
            }
            else
            {
                eof = true;
            }
        }
        cv.notify_all();
    }
    std::size_t held_bytes() const
    {
        std::lock_guard<std::mutex> lock(m);
        return bytes;
    }
};

// A pthread whose body is a lambda. pthread_create takes a plain function, and
// everything these loops need lives in the caller's frame, so the lambda is
// handed over on the heap.
struct ThreadBody
{
    std::function<void()> body;
};

void *thread_body_main(void *raw)
{
    std::unique_ptr<ThreadBody> start(static_cast<ThreadBody *>(raw));
    start->body();
    return nullptr;
}

bool start_thread(pthread_t *thread, std::function<void()> body, std::size_t stack_bytes)
{
    auto *start = new (std::nothrow) ThreadBody{std::move(body)};
    if (start == nullptr)
    {
        sys::log("[HUI] thread refused: no room for the body");
        return false;
    }
    pthread_attr_t attr;
    bool sized = pthread_attr_init(&attr) == 0;
    if (sized)
    {
        sized = pthread_attr_setstacksize(&attr, stack_bytes) == 0;
    }
    int created = pthread_create(thread, sized ? &attr : nullptr, &thread_body_main, start);
    if (created != 0 && sized)
    {
        // A custom stack size is refused on this platform often enough that
        // the worker creation already retries without it (see start()); the
        // same fallback belongs here, or the audio thread silently never runs.
        pthread_attr_destroy(&attr);
        sized = false;
        created = pthread_create(thread, nullptr, &thread_body_main, start);
    }
    if (sized)
    {
        pthread_attr_destroy(&attr);
    }
    if (created != 0)
    {
        sys::log("[HUI] thread refused: pthread_create rc=%d stack=%zuKB", created,
                 stack_bytes >> 10);
        delete start;
        return false;
    }
    return true;
}

#endif // HUI_PS5

} // namespace

Player::Player() = default;

Player::~Player()
{
    stop();
    if (mixer_ != nullptr)
    {
        // The audio thread must stop reading the ring before it goes away; the
        // swap lands on its command queue a few grains later.
        mixer_->set_stream_gain(kSlot, 0.0f, 0.0f);
        mixer_->swap_stream(kSlot, nullptr);
        wait_us(200000);
    }
    delete ring_;
    delete last_ring_;
}

#ifdef HUI_PS5

namespace
{

// A film is read over the wire as it plays. Fifteen seconds without a byte is
// a NAS that went away, not a slow one.
constexpr int kReadTimeoutUs = 15000000;
// Backstop for the open phase: even with rw_timeout set, `reconnect` can keep
// re-establishing a half-dead socket, so a stalled source is abandoned after
// this much wall-clock. It only applies while a film opens; once the header is
// read the reader is cancelled by keep_running_ alone.
constexpr std::int64_t kOpenDeadlineUs = 20 * 1000 * 1000;
// How many past RPU curves to keep tagged by pts. A scene changes a handful of
// times a film; this only has to cover the demuxer's lookahead over the frame
// queue and the B-frame reorder depth, so 24 is generous and its memory cost
// (24 x ~12 KB) is negligible.
constexpr std::size_t kMaxCurveStamps = 24;
// How much of the head of the file ffmpeg may read to work out what it is,
// and how long it may spend doing so. Both are budgets, not targets: a normal
// MP4 answers from its first few hundred KB, and anything that would need
// more than this is a file whose header is not at the front, which is a
// different problem than a bigger probe would solve.
constexpr long long kProbeSize = 8 * 1024 * 1024;
constexpr long long kAnalyzeUsec = 5 * 1000 * 1000;
// A picture this far behind the sound is not worth showing. Generous on
// purpose: a 4K picture costs a decode, a plane copy and an upload, and any
// jitter around that used to cross a 150 ms line and drop the frame, which
// reads as a stutter rather than as a picture that is a little late. Kodi
// allows 500 ms before it looks for a keyframe.
constexpr double kLateSeconds = 0.30;
// The largest handoff buffer, in bytes: a 4K ten-bit picture is 25 MB of
// planes (3840x2176 luma at two bytes a sample plus half that again for
// chroma). This is the heap's backstop - exceptions are off, so an oversized
// request would take the title down instead of dropping a frame.
constexpr std::size_t kMaxFrameBytes = 40u << 20;

// How much decoded picture the queue may hold, and how much may sit in the
// spare cache, given the heap this build actually got. A 4K picture is 18 MB
// of planes, so on a small heap the queue is one frame and there is no spare:
// buffering fewer pictures costs a little smoothness, buffering none at all
// costs the film. The heap reports its own size because the request for a big
// one is a request, not a promise (see app_heap.c).
std::size_t queue_budget()
{
    // Room for three 4K pictures, two, or one. A queue that holds a single
    // picture cannot put a 24 fps film on a 60 Hz panel evenly: the frame that
    // is due next is still being decoded when the refresh that needs it
    // arrives, so it slips a refresh and the 3:2 cadence becomes 2,3,2,3 at
    // random - which reads as a stutter from a decoder that is keeping up
    // perfectly (late=0 in the stats line while it happens).
    //
    // Two, not three, on a 256 MB heap: a 4K picture is 19 MB, and the queue
    // is not the only place one lives - one is on its way to the GPU and one
    // may be parked. Measured on the console, five alive at once is 204 MB of
    // a 256 MB heap, and the sixth is the crash.
    const std::size_t capacity = hui_heap_capacity();
    if (capacity >= (384u << 20))
        return 60u << 20;
    if (capacity >= (224u << 20))
        return 40u << 20; // two 4K pictures
    return 20u << 20;
}
std::size_t spare_budget()
{
    // Parking a 19 MB picture is only worth it where the heap can afford to
    // park it. On a 256 MB heap it is the difference between three pictures
    // alive and four, and four is where it died.
    return hui_heap_capacity() >= (384u << 20) ? std::size_t(22u << 20) : std::size_t(0);
}

// A stream timestamp in seconds, or -1 when it has none.
double seconds_of(const AVRational &base, std::int64_t timestamp)
{
    if (timestamp == AV_NOPTS_VALUE || base.num == 0 || base.den == 0)
    {
        return -1.0;
    }
    return static_cast<double>(timestamp) * av_q2d(base);
}

// ASS carries eight comma-separated fields before the words. The words may
// themselves contain commas, so only the first eight separators count.
std::string ass_text(const char *ass)
{
    if (ass == nullptr)
    {
        return std::string();
    }
    const char *at = ass;
    int commas = 0;
    while (*at != '\0' && commas < 8)
    {
        if (*at == ',')
        {
            ++commas;
        }
        ++at;
    }
    return std::string(at);
}

// Turn a subtitle payload into something the UI font can draw: drop override
// blocks, line-break escapes and the handful of HTML-ish tags ffmpeg leaves in
// SubRip. Bitmap subtitles never reach this function.
std::string clean_subtitle_text(const std::string &source)
{
    std::string out;
    out.reserve(source.size());
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        const char c = source[i];
        if (c == '{')
        {
            const std::size_t close = source.find('}', i + 1);
            if (close == std::string::npos)
            {
                break;
            }
            i = close;
            continue;
        }
        if (c == '<')
        {
            const std::size_t close = source.find('>', i + 1);
            if (close != std::string::npos)
            {
                i = close;
                continue;
            }
        }
        if (c == '\\' && i + 1 < source.size())
        {
            const char next = source[i + 1];
            if (next == 'N' || next == 'n')
            {
                out += '\n';
                ++i;
                continue;
            }
            if (next == 'h')
            {
                out += ' ';
                ++i;
                continue;
            }
        }
        out += c;
    }
    // Trim the outer whitespace, including the \r some servers leave behind.
    std::size_t begin = 0;
    while (begin < out.size() &&
           (out[begin] == ' ' || out[begin] == '\t' || out[begin] == '\r' || out[begin] == '\n'))
    {
        ++begin;
    }
    std::size_t end = out.size();
    while (end > begin && (out[end - 1] == ' ' || out[end - 1] == '\t' || out[end - 1] == '\r' ||
                           out[end - 1] == '\n'))
    {
        --end;
    }
    return out.substr(begin, end - begin);
}

} // namespace

void Player::attach(audio::Mixer &mixer)
{
    mixer_ = &mixer;
    if (ring_ != nullptr)
    {
        return;
    }
    ring_ = new (std::nothrow) audio::StreamRing(kRingFrames);
    if (ring_ == nullptr)
    {
        return;
    }
    mixer.attach_stream(kSlot, ring_);
}

// A subtitle stream the viewer here reads as Chinese: the container's ISO
// language tag (the official 飞牛影视 TV app matches tracks by a subtitle
// language code too), or a Chinese name in the track title. Ordinals stay in
// the container's own order, so the first hit is "the first Chinese track".
static bool track_lang_is_chinese(const SubtitleTrack &track)
{
    std::string lang = track.language;
    for (char &c : lang)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lang == "zh" || lang == "zho" || lang == "chi" || lang == "chs" || lang == "cht" ||
        lang == "zho-cn" || lang == "zho-hk" || lang == "zho-tw" || lang == "cn")
    {
        return true;
    }
    const std::string hay = track.language + track.title;
    return hay.find("\xe4\xb8\xad\xe6\x96\x87") != std::string::npos    // 中文
           || hay.find("\xe5\x9b\xbd\xe8\xaf\xad") != std::string::npos // 国语
           || hay.find("\xe7\xb2\xa4\xe8\xaf\xad") != std::string::npos // 粤语
           || hay.find("Chinese") != std::string::npos ||
           hay.find("chinese") != std::string::npos || hay.find("Chs") != std::string::npos ||
           hay.find("Chi") != std::string::npos;
}

bool Player::start(const std::string &url, const std::vector<std::string> &headers,
                   int resume_seconds, int audio_track, int subtitle_track, bool subtitle_auto)
{
    if (ring_ == nullptr || url.empty())
    {
        state_.store(static_cast<int>(PlayState::failed));
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_ = ring_ == nullptr ? "没有音频缓冲" : "播放地址为空";
        return false;
    }
    std::lock_guard<std::mutex> control(control_mutex_);
    stop_locked();
    // Whatever the previous film left in the ring would be heard under this
    // one while its header is being read.
    retarget();
    url_ = url;
    headers_ = headers;
    resume_seconds_ = std::max(0, resume_seconds);
    selected_audio_.store(audio_track, std::memory_order_relaxed);
    selected_subtitle_.store(subtitle_track, std::memory_order_relaxed);
    subtitle_auto_.store(subtitle_auto, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        audio_tracks_.clear();
        subtitle_tracks_.clear();
    }
    {
        // A subtitle from the last film must not sit over this one's opening.
        // The external cues go too: a reopen is a fresh container, and if the
        // viewer still wants the external file the caller re-loads it after.
        std::lock_guard<std::mutex> lock(subtitle_mutex_);
        subtitle_cues_.clear();
        subtitle_bitmaps_.clear();
        external_cues_.clear();
        external_active_.store(false, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        // The first frame of the new film must not be mistaken for the last
        // frame of the old one.
        ++session_;
        pending_.clear();
        // The stale RGBA of the previous film is dropped here as well: the
        // render thread must not upload it under the new session.
        upload_.clear();
        upload_width_ = 0;
        upload_height_ = 0;
        upload_luma_ = 0;
        upload_chroma_ = 0;
        upload_ten_bit_ = false;
        upload_ten_bit_low_ = false;
        upload_planar_ = false;
        uploads_ = 0;
        // The previous film's reshaping table and colour step must not survive
        // into this one; an empty version forces the render thread to clear
        // them.
        dovi_lut_valid_ = false;
        dovi_colour_valid_ = false;
        dovi_curve_.clear();
        dovi_lut_version_.fetch_add(1, std::memory_order_relaxed);
    }
    position_.store(resume_seconds_);
    position_ms_.store(resume_seconds_ * 1000);
    duration_.store(0);
    paused_.store(false);
    seek_request_.store(-1);
    audio_finished_.store(false, std::memory_order_release);
    sync_sum_ms_.store(0, std::memory_order_relaxed);
    sync_count_.store(0, std::memory_order_relaxed);
    state_.store(static_cast<int>(PlayState::opening));
    mixer_->set_stream_gain(kSlot, gain_.load(std::memory_order_relaxed), 0.0f);
    mixer_->set_stream_paused(kSlot, false);
    keep_running_.store(true);

    // avformat_find_stream_info alone wants more stack than the SDK hands a
    // default thread, so the demuxer gets 4 MB of its own.
    pthread_attr_t attr;
    bool sized = pthread_attr_init(&attr) == 0;
    if (sized)
    {
        sized = pthread_attr_setstacksize(&attr, 4u * 1024u * 1024u) == 0;
    }
    int created = pthread_create(&worker_, sized ? &attr : nullptr, &Player::run, this);
    if (created != 0 && sized)
    {
        created = pthread_create(&worker_, nullptr, &Player::run, this);
    }
    if (sized)
    {
        pthread_attr_destroy(&attr);
    }
    if (created != 0)
    {
        keep_running_.store(false);
        state_.store(static_cast<int>(PlayState::failed));
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_ = "起不了播放线程";
        return false;
    }
    worker_running_ = true;
    return true;
}

void Player::pause()
{
    if (state() == PlayState::playing)
    {
        paused_.store(true);
        state_.store(static_cast<int>(PlayState::paused));
        // Stop the consumer, not just the picture. The audio ring's played()
        // count is the clock; letting it drain in the background made the
        // progress bar advance and queued seconds vanish while "paused".
        if (mixer_ != nullptr)
        {
            mixer_->set_stream_paused(kSlot, true);
        }
    }
}

void Player::resume()
{
    if (state() == PlayState::paused)
    {
        paused_.store(false);
        state_.store(static_cast<int>(PlayState::playing));
        if (mixer_ != nullptr)
        {
            mixer_->set_stream_paused(kSlot, false);
        }
    }
}

void Player::stop()
{
    std::lock_guard<std::mutex> control(control_mutex_);
    stop_locked();
}

void Player::stop_locked()
{
    keep_running_.store(false);
    if (worker_running_)
    {
        pthread_join(worker_, nullptr);
        worker_running_ = false;
    }
    paused_.store(false);
    seek_request_.store(-1);
    if (mixer_ != nullptr)
    {
        mixer_->set_stream_paused(kSlot, false);
    }
    {
        std::lock_guard<std::mutex> lock(subtitle_mutex_);
        subtitle_cues_.clear();
        subtitle_bitmaps_.clear();
        // A stopped player holds no subtitle of any kind; the next film starts
        // clean. Seek does not reach here, so an external file survives a seek.
        external_cues_.clear();
        external_active_.store(false, std::memory_order_relaxed);
    }
    if (state() != PlayState::failed)
    {
        state_.store(static_cast<int>(PlayState::idle));
    }
    {
        // The handoff buffers belong to the film that just stopped; leaving
        // them full would let a later frame() upload a picture from it.
        std::lock_guard<std::mutex> lock(frame_mutex_);
        pending_.clear();
        upload_.clear();
        upload_width_ = 0;
        upload_height_ = 0;
        upload_luma_ = 0;
        upload_chroma_ = 0;
        upload_ten_bit_ = false;
        upload_ten_bit_low_ = false;
        upload_planar_ = false;
        // The reshaping curves belong to the film that just stopped; a later
        // frame must not pick one up under the next session.
        dovi_curve_.clear();
    }
    // The controller light starts over with the next film too: a stale tint
    // from the old one would flash on the bar before the first new picture.
    video_light_.store(0, std::memory_order_relaxed);
    audio_level_.store(0.0f, std::memory_order_relaxed);
    if (mixer_ != nullptr)
    {
        mixer_->set_stream_gain(kSlot, 0.0f, 0.1f);
    }
}

void Player::seek(int seconds)
{
    if (seconds < 0)
    {
        seconds = position() + seconds;
    }
    const int length = duration();
    if (length > 0 && seconds >= length)
    {
        seconds = length > 1 ? length - 1 : 0;
    }
    seconds = std::max(0, seconds);
    position_.store(seconds);
    position_ms_.store(seconds * 1000);
    {
        // The new position is not where the queued cues were aimed.
        std::lock_guard<std::mutex> lock(subtitle_mutex_);
        subtitle_cues_.clear();
        subtitle_bitmaps_.clear();
    }
    // Settle-based seek: the user asked for "先不管实际播放内容，进度条停下后
    // 把声音和视频同步调过去". The first press pauses playback and stores the
    // target; subsequent presses only move the target and reset the deadline.
    // The video main loop fires one real `avformat_seek_file` 400 ms after
    // the last press, and the seek_applied handler resumes playback. No
    // intermediate seeks, no clock thrash: sound and picture come back
    // together at the position the progress bar is pointing at.
    if (seek_debounce_paused_.load(std::memory_order_relaxed))
    {
        // Mid-debounce: just move the target and extend the window.
        pending_seek_target_.store(seconds, std::memory_order_relaxed);
        pending_seek_deadline_us_.store(sys::monotonic_us() + 400'000, std::memory_order_relaxed);
        return;
    }
    if (state() == PlayState::ended)
    {
        // Seeking after the end restores the play state; go through the
        // immediate path because there is no burst to coalesce here.
        paused_.store(false);
        state_.store(static_cast<int>(PlayState::playing));
        if (mixer_ != nullptr)
        {
            mixer_->set_stream_paused(kSlot, false);
        }
        seek_request_.store(seconds);
        return;
    }
    if (state() != PlayState::playing)
    {
        // User-paused scrubbing keeps the paused state (pre-v26 behaviour)
        // and fires the seek immediately so the paused picture refreshes.
        seek_request_.store(seconds);
        return;
    }
    // Playing: enter the settle window.
    seek_debounce_paused_.store(true, std::memory_order_relaxed);
    pending_seek_target_.store(seconds, std::memory_order_relaxed);
    pending_seek_deadline_us_.store(sys::monotonic_us() + 400'000, std::memory_order_relaxed);
    pause();
}

void Player::set_gain(float gain)
{
    gain = std::clamp(gain, 0.0f, 1.5f);
    gain_.store(gain, std::memory_order_relaxed);
    if (mixer_ != nullptr)
    {
        mixer_->set_stream_gain(kSlot, gain, 0.05f);
    }
}

std::vector<AudioTrack> Player::audio_tracks() const
{
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    return audio_tracks_;
}

std::vector<SubtitleTrack> Player::subtitle_tracks() const
{
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    return subtitle_tracks_;
}

std::string Player::subtitle_at(int position_ms) const
{
    std::lock_guard<std::mutex> lock(subtitle_mutex_);
    std::string text;
    // With an external file on screen its cues are the only text drawn; the
    // container may still be decoding an embedded track underneath, but showing
    // both would stack two languages. Otherwise read the container's cues.
    const bool use_external = external_active_.load(std::memory_order_relaxed);
    const std::deque<SubtitleCue> &cues = use_external ? external_cues_ : subtitle_cues_;
    for (const SubtitleCue &cue : cues)
    {
        if (position_ms >= cue.start_ms && position_ms < cue.end_ms)
        {
            if (!text.empty())
            {
                text += '\n';
            }
            text += cue.text;
        }
    }
    return text;
}

bool Player::subtitle_bitmap_at(int position_ms, SubtitleBitmap *out) const
{
    if (out == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(subtitle_mutex_);
    const SubtitleBitmap *best = nullptr;
    for (const SubtitleBitmap &bm : subtitle_bitmaps_)
    {
        if (position_ms >= bm.start_ms && position_ms < bm.end_ms)
        {
            best = &bm; // the newest matching frame wins (later in the deque)
        }
    }
    if (best == nullptr)
    {
        return false;
    }
    *out = *best; // copy the pixels out; the render thread uploads them
    return true;
}

bool Player::set_external_subtitle(const std::string &bytes, const std::string &format)
{
    std::string text = bytes;
    // /subtitle/dl hands back the file verbatim, which for a Windows-authored
    // SRT means a UTF-8 BOM in front. The demuxers choke on it, so drop it.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    {
        text.erase(0, 3);
    }
    if (text.empty())
    {
        clear_external_subtitle();
        return false;
    }

    // A fixed cursor over the bytes in memory: FFmpeg demuxes through an
    // AVIOContext, so a subtitle file needs no reader and no temp file.
    struct Reader
    {
        const std::uint8_t *data;
        std::size_t size;
        std::size_t pos;
    } reader{reinterpret_cast<const std::uint8_t *>(text.data()), text.size(), 0};

    const auto io_read = +[](void *opaque, std::uint8_t *buf, int buf_size) -> int
    {
        Reader *r = static_cast<Reader *>(opaque);
        if (buf_size <= 0)
        {
            return 0;
        }
        const std::size_t remaining = r->size - r->pos;
        const int n =
            static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(buf_size), remaining));
        std::memcpy(buf, r->data + r->pos, static_cast<std::size_t>(n));
        r->pos += static_cast<std::size_t>(n);
        return n;
    };
    const auto io_seek = +[](void *opaque, std::int64_t offset, int whence) -> std::int64_t
    {
        Reader *r = static_cast<Reader *>(opaque);
        if (whence == AVSEEK_SIZE)
        {
            return static_cast<std::int64_t>(r->size);
        }
        const std::int64_t base = (whence == SEEK_SET)   ? 0
                                  : (whence == SEEK_CUR) ? static_cast<std::int64_t>(r->pos)
                                                         : static_cast<std::int64_t>(r->size);
        const std::int64_t next = base + offset;
        if (next < 0 || next > static_cast<std::int64_t>(r->size))
        {
            return -1;
        }
        r->pos = static_cast<std::size_t>(next);
        return next;
    };

    // Prefer the format the server named; otherwise sniff the magic so a
    // non-seekable probe still lands on the right demuxer.
    std::string key;
    key.reserve(format.size());
    for (char c : format)
    {
        key.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    const char *demuxer = nullptr;
    if (key == "ass" || key == "ssa")
    {
        demuxer = "ass";
    }
    else if (key == "vtt" || key == "webvtt")
    {
        demuxer = "webvtt";
    }
    else if (key == "srt" || key == "subrip")
    {
        demuxer = "srt";
    }
    if (demuxer == nullptr)
    {
        if (text.rfind("WEBVTT", 0) == 0)
        {
            demuxer = "webvtt";
        }
        else if (text.find("[Script Info]") != std::string::npos ||
                 text.find("[V4") != std::string::npos)
        {
            demuxer = "ass";
        }
        else
        {
            demuxer = "srt";
        }
    }

    constexpr int kIoBuf = 4096;
    std::uint8_t *io_buf = static_cast<std::uint8_t *>(av_malloc(kIoBuf));
    if (io_buf == nullptr)
    {
        return false;
    }
    AVIOContext *avio = avio_alloc_context(io_buf, kIoBuf, 0, &reader, io_read, nullptr, io_seek);
    if (avio == nullptr)
    {
        av_free(io_buf);
        return false;
    }
    AVFormatContext *fmt = avformat_alloc_context();
    if (fmt == nullptr)
    {
        avio_context_free(&avio);
        return false;
    }
    fmt->pb = avio;
    // Keep the AVIOContext ours: with this flag neither a failed open nor
    // avformat_close_input frees it, so we free it once, here, both paths.
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    const AVInputFormat *ifmt = av_find_input_format(demuxer);
    AVDictionary *open_opts = nullptr;
    int rc = avformat_open_input(&fmt, "", ifmt, &open_opts);
    av_dict_free(&open_opts);
    if (rc < 0)
    {
        // avformat_open_input nulled fmt on failure but left the custom io ours.
        avio_context_free(&avio);
        clear_external_subtitle();
        return false;
    }

    int sub_index = -1;
    if (avformat_find_stream_info(fmt, nullptr) >= 0)
    {
        for (unsigned i = 0; i < fmt->nb_streams; ++i)
        {
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                sub_index = static_cast<int>(i);
                break;
            }
        }
    }

    std::deque<SubtitleCue> decoded;
    if (sub_index >= 0)
    {
        const AVCodec *codec = avcodec_find_decoder(fmt->streams[sub_index]->codecpar->codec_id);
        AVCodecContext *avctx = codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
        if (avctx != nullptr)
        {
            avcodec_parameters_to_context(avctx, fmt->streams[sub_index]->codecpar);
            avctx->pkt_timebase = fmt->streams[sub_index]->time_base;
            if (avcodec_open2(avctx, codec, nullptr) >= 0)
            {
                AVPacket *pkt = av_packet_alloc();
                while (pkt != nullptr && av_read_frame(fmt, pkt) >= 0)
                {
                    if (pkt->stream_index == sub_index)
                    {
                        AVSubtitle parsed{};
                        int got = 0;
                        if (avcodec_decode_subtitle2(avctx, &parsed, &got, pkt) >= 0 && got > 0)
                        {
                            std::string line;
                            for (unsigned i = 0; i < parsed.num_rects; ++i)
                            {
                                const AVSubtitleRect *rect = parsed.rects[i];
                                std::string part;
                                if (rect->type == SUBTITLE_ASS)
                                {
                                    part = clean_subtitle_text(ass_text(rect->ass));
                                }
                                else if (rect->type == SUBTITLE_TEXT && rect->text != nullptr)
                                {
                                    part = clean_subtitle_text(rect->text);
                                }
                                if (!part.empty())
                                {
                                    if (!line.empty())
                                    {
                                        line += '\n';
                                    }
                                    line += part;
                                }
                            }
                            if (!line.empty())
                            {
                                // The demuxer stamps each packet in its own
                                // time_base; convert with the same helper the
                                // container path uses, then add the rect's
                                // display window.
                                std::int64_t start_ms = 0;
                                const double seconds =
                                    seconds_of(fmt->streams[sub_index]->time_base, pkt->pts);
                                if (seconds >= 0.0)
                                {
                                    start_ms = static_cast<std::int64_t>(seconds * 1000.0);
                                }
                                const int duration =
                                    parsed.end_display_time > 0 ? parsed.end_display_time : 2500;
                                SubtitleCue cue;
                                cue.start_ms =
                                    static_cast<int>(std::max<std::int64_t>(0, start_ms)) +
                                    static_cast<int>(parsed.start_display_time);
                                cue.end_ms = cue.start_ms + std::max(500, duration);
                                cue.text = std::move(line);
                                decoded.push_back(std::move(cue));
                            }
                        }
                        avsubtitle_free(&parsed);
                    }
                    av_packet_unref(pkt);
                }
                if (pkt != nullptr)
                {
                    av_packet_free(&pkt);
                }
            }
            avcodec_free_context(&avctx);
        }
    }

    avformat_close_input(&fmt);
    avio_context_free(&avio);

    std::lock_guard<std::mutex> lock(subtitle_mutex_);
    if (decoded.empty())
    {
        external_cues_.clear();
        external_active_.store(false, std::memory_order_relaxed);
        return false;
    }
    external_cues_ = std::move(decoded);
    external_active_.store(true, std::memory_order_relaxed);
    return true;
}

void Player::clear_external_subtitle()
{
    external_active_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(subtitle_mutex_);
    external_cues_.clear();
}

void Player::set_frame_edge(int edge)
{
    // 0 means automatic (no cap): keep it so the decode loop uploads the frame
    // at its real resolution. Positive edges are clamped to the manual tiers.
    if (edge > 0)
        edge = std::min(kMaxFrameEdge, std::max(kMinFrameEdge, edge));
    frame_edge_.store(edge, std::memory_order_relaxed);
}

std::string Player::error_text() const
{
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
}

void Player::fail(const char *error)
{
    sys::log("[HUI] video failed: %s", error);
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        error_ = error;
    }
    state_.store(static_cast<int>(PlayState::failed));
    keep_running_.store(false);
}

void Player::retarget()
{
    if (mixer_ == nullptr)
    {
        return;
    }
    auto *next = new (std::nothrow) audio::StreamRing(kRingFrames);
    if (next == nullptr)
    {
        return;
    }
    mixer_->swap_stream(kSlot, next);
    {
        // The audio thread writes through `ring_`; it must not be looking at
        // the old one while it moves to `last_ring_`.
        std::lock_guard<std::mutex> lock(ring_mutex_);
        // last_ring_ left the mixer's slot at the seek before this one, so the
        // audio thread can no longer be reading it.
        delete last_ring_;
        last_ring_ = ring_;
        ring_ = next;
    }
}

int Player::av_interrupt(void *opaque)
{
    auto *self = static_cast<Player *>(opaque);
    if (self == nullptr)
    {
        return 0;
    }
    // A stop() or seek sets keep_running_ false. Returning non-zero here makes
    // any blocking ffmpeg call (the open handshake, a stalled read) abort so
    // the decode thread returns and the next play's pthread_join cannot hang
    // the single Service worker behind a source that stopped answering.
    if (!self->keep_running_.load(std::memory_order_relaxed))
    {
        return 1;
    }
    // A seek the settle path just fired has to reach a reader that is parked
    // inside a blocking av_read_frame near the tail; otherwise the request
    // waits out the rw_timeout before the format context is repositioned. Abort
    // the in-flight read so the loop returns to its top and performs
    // avformat_seek_file - the exchange at that top clears this flag.
    if (self->seek_request_.load(std::memory_order_relaxed) >= 0)
    {
        return 1;
    }
    const std::int64_t deadline = self->interrupt_deadline_us_.load(std::memory_order_relaxed);
    if (deadline != 0 && sys::monotonic_us() >= deadline)
    {
        return 1;
    }
    return 0;
}

void *Player::run(void *user)
{
    static_cast<Player *>(user)->decode();
    return nullptr;
}

void Player::decode()
{
    const std::string url = url_;
    const std::vector<std::string> headers = headers_;
    int resume = resume_seconds_;
    sys::log("[HUI] decode begin build=%s %s url_len=%d", __DATE__, __TIME__,
             static_cast<int>(url.size()));

    AVDictionary *options = nullptr;
    std::string header_block;
    for (const std::string &line : headers)
    {
        header_block += line;
        header_block += "\r\n";
    }
    if (!header_block.empty())
    {
        av_dict_set(&options, "headers", header_block.c_str(), 0);
    }
    // The bytes are the original file, so the range request asks for the file
    // itself; ffmpeg's http protocol does the seeking.
    av_dict_set(&options, "rw_timeout", std::to_string(kReadTimeoutUs).c_str(), 0);
    // These films are original files, 1.8 GB and up, served over the local
    // network. Without a cap, avformat_find_stream_info reads forward until it
    // is confident - which on a big MP4 can mean megabytes of seeking back and
    // forth across the wire before the first frame, and on a stalled read it
    // never returns at all. A fixed budget keeps the open bounded, and the
    // reconnect options let a dropped TCP come back instead of failing the film.
    av_dict_set(&options, "probesize", std::to_string(kProbeSize).c_str(), 0);
    av_dict_set(&options, "analyzeduration", std::to_string(kAnalyzeUsec).c_str(), 0);
    av_dict_set(&options, "reconnect", "1", 0);
    av_dict_set(&options, "reconnect_streamed", "1", 0);
    av_dict_set(&options, "reconnect_on_network_error", "1", 0);
    av_dict_set(&options, "reconnect_delay_max", "5", 0);

    AVFormatContext *format = avformat_alloc_context();
    if (format == nullptr)
    {
        av_dict_free(&options);
        fail("内存不足");
        return;
    }
    // Make the open cancellable. Without this a source that stalls mid-handshake
    // leaves the decode thread unreachable inside ffmpeg, and the next film's
    // player_.stop() blocks in pthread_join - the cascade that took the whole
    // player down after one bad item. The deadline bounds reconnect loops.
    format->interrupt_callback.callback = &Player::av_interrupt;
    format->interrupt_callback.opaque = this;
    interrupt_deadline_us_.store(sys::monotonic_us() + kOpenDeadlineUs, std::memory_order_relaxed);
    const int opened = avformat_open_input(&format, url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (opened < 0)
    {
        // open_input frees the context it was given, even on failure.
        sys::log("[HUI] video open failed rc=%d", opened);
        fail("打不开这个文件");
        return;
    }
    // A film that fails here is usually still playable. An MP4 keeps its index
    // at the end of the file, so one slow read while probing is enough to make
    // stream info give up while the picture itself is perfectly fine - and
    // giving up with it is what turned "this film opens slowly" into "this
    // film will not play". The reference implementation this port follows
    // logs it and carries on; so does this now. The streams below either come
    // back or the failure is real and says so a few lines later.
    if (avformat_find_stream_info(format, nullptr) < 0)
    {
        sys::log("[HUI] video stream info failed; carrying on anyway");
    }
    // The open is bounded and done. A healthy read runs for the whole film, so
    // drop the wall-clock backstop and let only keep_running_ cancel from here.
    interrupt_deadline_us_.store(0, std::memory_order_relaxed);

    const int video_index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index < 0)
    {
        avformat_close_input(&format);
        fail("这个文件没有画面");
        return;
    }

    // List the container's tracks before picking one. The toolbar can then
    // offer exactly what this file has, and a switch reopens the same ticket
    // at the current second with another ordinal.
    std::vector<AudioTrack> audio_tracks;
    std::vector<SubtitleTrack> subtitle_tracks;
    int audio_index = -1;
    int subtitle_index = -1;
    int chosen_audio = -1;
    int chosen_subtitle = -1;
    int audio_ordinal = 0;
    int subtitle_ordinal = 0;
    for (unsigned i = 0; i < format->nb_streams; ++i)
    {
        const AVStream *stream = format->streams[i];
        const AVCodecParameters *par = stream->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            AudioTrack track;
            track.ordinal = audio_ordinal;
            track.channels = par->ch_layout.nb_channels;
            if (const AVDictionaryEntry *entry =
                    av_dict_get(stream->metadata, "language", nullptr, 0);
                entry != nullptr)
            {
                track.language = entry->value;
            }
            if (const AVDictionaryEntry *entry = av_dict_get(stream->metadata, "title", nullptr, 0);
                entry != nullptr)
            {
                track.title = entry->value;
            }
            audio_tracks.push_back(track);
            ++audio_ordinal;
        }
        else if (par->codec_type == AVMEDIA_TYPE_SUBTITLE)
        {
            SubtitleTrack track;
            track.ordinal = subtitle_ordinal;
            track.codec = avcodec_get_name(par->codec_id);
            switch (par->codec_id)
            {
            case AV_CODEC_ID_SUBRIP:
            case AV_CODEC_ID_ASS:
            case AV_CODEC_ID_SSA:
            case AV_CODEC_ID_TEXT:
            case AV_CODEC_ID_MOV_TEXT:
            case AV_CODEC_ID_WEBVTT:
                track.text = true;
                break;
            default:
                break;
            }
            if (const AVDictionaryEntry *entry =
                    av_dict_get(stream->metadata, "language", nullptr, 0);
                entry != nullptr)
            {
                track.language = entry->value;
            }
            if (const AVDictionaryEntry *entry = av_dict_get(stream->metadata, "title", nullptr, 0);
                entry != nullptr)
            {
                track.title = entry->value;
            }
            subtitle_tracks.push_back(track);
            ++subtitle_ordinal;
        }
    }

    const int wanted_audio = selected_audio_.load(std::memory_order_relaxed);
    if (wanted_audio >= 0 && wanted_audio < static_cast<int>(audio_tracks.size()))
    {
        audio_ordinal = 0;
        for (unsigned i = 0; i < format->nb_streams; ++i)
        {
            if (format->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            {
                continue;
            }
            if (audio_ordinal == wanted_audio)
            {
                audio_index = static_cast<int>(i);
                chosen_audio = audio_ordinal;
                break;
            }
            ++audio_ordinal;
        }
    }
    if (audio_index < 0)
    {
        audio_index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audio_index >= 0)
        {
            audio_ordinal = 0;
            for (unsigned i = 0; i < format->nb_streams; ++i)
            {
                if (format->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
                {
                    continue;
                }
                if (static_cast<int>(i) == audio_index)
                {
                    chosen_audio = audio_ordinal;
                    break;
                }
                ++audio_ordinal;
            }
        }
    }

    const int wanted_subtitle = selected_subtitle_.load(std::memory_order_relaxed);
    if (wanted_subtitle >= 0 && wanted_subtitle < static_cast<int>(subtitle_tracks.size()))
    {
        subtitle_ordinal = 0;
        for (unsigned i = 0; i < format->nb_streams; ++i)
        {
            if (format->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
            {
                continue;
            }
            if (subtitle_ordinal == wanted_subtitle)
            {
                subtitle_index = static_cast<int>(i);
                chosen_subtitle = subtitle_ordinal;
                break;
            }
            ++subtitle_ordinal;
        }
    }
    else if (wanted_subtitle < 0 && subtitle_auto_.load(std::memory_order_relaxed))
    {
        // No track chosen and this is a fresh open, not a track-change
        // reopen: default to the first Chinese subtitle, what a viewer here
        // expects without reaching for the menu.
        for (const SubtitleTrack &track : subtitle_tracks)
        {
            if (track_lang_is_chinese(track))
            {
                subtitle_ordinal = 0;
                for (unsigned i = 0; i < format->nb_streams; ++i)
                {
                    if (format->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
                    {
                        continue;
                    }
                    if (subtitle_ordinal == track.ordinal)
                    {
                        subtitle_index = static_cast<int>(i);
                        chosen_subtitle = subtitle_ordinal;
                        break;
                    }
                    ++subtitle_ordinal;
                }
                break;
            }
        }
        subtitle_auto_.store(false, std::memory_order_relaxed);
    }

    selected_audio_.store(chosen_audio, std::memory_order_relaxed);
    selected_subtitle_.store(chosen_subtitle, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(tracks_mutex_);
        audio_tracks_ = audio_tracks;
        subtitle_tracks_ = subtitle_tracks;
    }
    sys::log("[HUI] video tracks audio=%d subtitle=%d selected_audio=%d index=%d "
             "selected_subtitle=%d index=%d",
             static_cast<int>(audio_tracks.size()), static_cast<int>(subtitle_tracks.size()),
             chosen_audio, audio_index, chosen_subtitle, subtitle_index);
    // What the demuxer actually settled on. This is the line that answers
    // "is the file readable at all, and what codec is inside it" without a
    // debugger on the console.
    {
        const AVStream *vs = format->streams[video_index];
        const AVCodecParameters *cp = vs->codecpar;
        const char *codec = avcodec_get_name(cp->codec_id);
        const long long seconds = format->duration > 0 ? format->duration / AV_TIME_BASE : -1;
        // Which colour matrix this film was graded in. The shader has all
        // three and used to be told BT.601 always, because the setter was
        // never called: every BT.709 film - which is every HD and UHD one -
        // came out with the wrong matrix, and that reads as "the colours are
        // off on this one" while the next film looks right.
        //
        // The tag decides when it is specific; BT.2020 is honoured because a
        // 4K SDR disc really can be graded in it and reading it as BT.709 is
        // visibly desaturated. Only "unspecified" falls back to the size rule,
        // which is what VLC's PS5 port does (MATRIX_AUTO: 709 at 720 lines and
        // up, 601 below).
        const int space = cp->color_space;
        int matrix = cp->height >= 720 ? 1 : 0;
        if (space == AVCOL_SPC_BT2020_NCL || space == AVCOL_SPC_BT2020_CL)
            matrix = 2;
        else if (space == AVCOL_SPC_BT709)
            matrix = 1;
        else if (space == AVCOL_SPC_BT470BG || space == AVCOL_SPC_SMPTE170M)
            matrix = 0;

        // The transfer function is the other half of the colour, and the one
        // that used to be ignored: a PQ or HLG film shown through the SDR
        // curve is not slightly off, it is washed out and grey, because its
        // samples are light rather than brightness. The rule below is the
        // reference ports' (vlc-ps5 src/player.cc, stremio-ps5 src/player.cpp),
        // including its two awkward cases:
        //
        //  - Dolby Vision profile 8 carries an HDR10 or HLG picture in the
        //    base layer, but the stream's own trc tag often says nothing, so
        //    the DV configuration record is what identifies it.
        //  - An untagged 10-bit stream with a wide gamut or mastering metadata
        //    is HDR10 in practice; those files are common and are exactly the
        //    ones that came out grey.
        const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(cp->format));
        const bool deep = pd != nullptr && pd->comp[0].depth >= 10;
        const bool wide_gamut =
            cp->color_primaries == AVCOL_PRI_BT2020 || cp->color_space == AVCOL_SPC_BT2020_NCL;
        int dv_profile = -1;
        int dv_compat = -1;
        if (const AVPacketSideData *side = av_packet_side_data_get(
                cp->coded_side_data, cp->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
            side != nullptr && side->data != nullptr &&
            side->size >= sizeof(AVDOVIDecoderConfigurationRecord))
        {
            const auto *record =
                reinterpret_cast<const AVDOVIDecoderConfigurationRecord *>(side->data);
            dv_profile = record->dv_profile;
            dv_compat = record->dv_bl_signal_compatibility_id;
        }
        const bool has_mastering =
            av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data,
                                    AV_PKT_DATA_MASTERING_DISPLAY_METADATA) != nullptr;
        AVColorTransferCharacteristic trc = cp->color_trc;
        if (dv_profile == 8 && dv_compat == 1)
        {
            trc = AVCOL_TRC_SMPTE2084; // an HDR10 picture inside
        }
        else if (dv_profile == 8 && dv_compat == 4)
        {
            trc = AVCOL_TRC_ARIB_STD_B67; // an HLG picture inside
        }
        else if (dv_profile == 5)
        {
            // Profile 5 is the one with no HDR10 base layer to fall back on:
            // its single layer is IPTPQc2 (ICtCp) with a PQ curve, and the
            // container almost never says so - the film on this console is
            // tagged "unspecified" and plain BT.709, which read literally
            // means "SDR". An SDR reading of a PQ picture is the pale grey
            // picture that started this. It is PQ, and the shader decodes the
            // planes as ICtCp rather than Y'CbCr, which is what made them
            // purple. The RPU's reshaping and colour matrices are applied by
            // the shader; without them the hue and contrast are both wrong.
            trc = AVCOL_TRC_SMPTE2084;
        }
        const bool untagged_hdr =
            trc == AVCOL_TRC_UNSPECIFIED && deep && (wide_gamut || has_mastering);
        gfx::GlBatch::VideoTransfer transfer = gfx::GlBatch::VideoTransfer::Sdr;
        const char *colour = matrix == 2 ? "bt2020" : matrix == 1 ? "bt709" : "bt601";
        if (trc == AVCOL_TRC_SMPTE2084 || untagged_hdr)
        {
            transfer = gfx::GlBatch::VideoTransfer::Pq;
            matrix = 2; // HDR is BT.2020 by definition
            colour = dv_profile == 5   ? "hdr10 (dv profile 5, ictcp)"
                     : dv_profile >= 0 ? "hdr10 (dolby vision)"
                     : untagged_hdr    ? "hdr10 (untagged)"
                                       : "hdr10";
        }
        else if (trc == AVCOL_TRC_ARIB_STD_B67)
        {
            transfer = gfx::GlBatch::VideoTransfer::Hlg;
            matrix = 2;
            colour = "hlg";
        }
        // How bright the film's own light gets. The tone map rolls everything
        // above the SDR reference white down under it, and where that starts
        // depends on the grade: MaxCLL says it outright, the mastering display
        // says what it was graded on, and 1000 nits is the usual answer when
        // neither is tagged.
        float peak_nits = 1000.0f;
        if (const AVPacketSideData *side = av_packet_side_data_get(
                cp->coded_side_data, cp->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
            side != nullptr && side->data != nullptr &&
            side->size >= sizeof(AVContentLightMetadata))
        {
            const auto *light = reinterpret_cast<const AVContentLightMetadata *>(side->data);
            if (light->MaxCLL >= 100)
                peak_nits = static_cast<float>(light->MaxCLL);
        }
        else if (const AVPacketSideData *side =
                     av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data,
                                             AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
                 side != nullptr && side->data != nullptr &&
                 side->size >= sizeof(AVMasteringDisplayMetadata))
        {
            const auto *display = reinterpret_cast<const AVMasteringDisplayMetadata *>(side->data);
            if (display->has_luminance)
            {
                const double nits = av_q2d(display->max_luminance);
                if (nits >= 100.0)
                    peak_nits = static_cast<float>(nits);
            }
        }
        peak_nits = std::min(peak_nits, 10000.0f);
        // The range tag is honoured, with one exception: Dolby Vision profile 5
        // stores ICtCp, and ICtCp code values are full range by definition
        // (black 0, white 1023, chroma centred on 511.5) whatever the container
        // happens to claim. The RPU agrees - its bl_video_full_range_flag reads
        // 1 on every title in this library, and its ycc_to_rgb offset is
        // (0, 0.5, 0.5), which is the ICtCp convention. Reading those samples as
        // studio range subtracts 64 from a signal whose black is 0; PQ turns
        // that into a fourfold loss in the shadows and leaves the chroma 14%
        // too strong, which is exactly a dark picture with a cast.
        //
        // Profiles 8 and 9 keep the container tag: their base layer really is
        // studio-range HDR10, and believing a full-range tag on one lifts every
        // black in the film. Everything else that says full range - JPEG-range
        // H.264, screen captures, some animation masters - really is full range,
        // and reading it as studio range crushes its blacks and clips its white.
        const bool full_range =
            dv_profile == 5 || (cp->color_range == AVCOL_RANGE_JPEG && dv_profile < 0);
        matrix_.store(matrix);
        transfer_.store(static_cast<int>(transfer));
        full_range_.store(full_range);
        ictcp_.store(dv_profile == 5);
        peak_nits_.store(static_cast<int>(peak_nits));
        wide_gamut_.store(wide_gamut || transfer != gfx::GlBatch::VideoTransfer::Sdr);
        sys::log("[HUI] video opened: %s %dx%d %llds audio=%d colours=%s%s trc=%d space=%d "
                 "primaries=%d range=%d depth=%d peak=%dnits dv=%d/%d mastering=%d",
                 codec != nullptr ? codec : "?", cp->width, cp->height, seconds, audio_index,
                 colour, full_range ? " full" : "", static_cast<int>(cp->color_trc), space,
                 static_cast<int>(cp->color_primaries), static_cast<int>(cp->color_range),
                 pd != nullptr ? pd->comp[0].depth : 0, static_cast<int>(peak_nits), dv_profile,
                 dv_compat, has_mastering ? 1 : 0);
    }

    AVCodecContext *video = nullptr;
    AVCodecContext *audio = nullptr;
    AVCodecContext *subtitle = nullptr;
    SwrContext *resample = nullptr;
    SwsContext *scaler = nullptr;
    int scaler_w = 0;
    int scaler_h = 0;
    int scaler_fmt = 0;
    AVFrame *decoded = nullptr;
    HwDecoder *hw = nullptr;
    AVFrame *hw_frame = nullptr;
    bool ok = true;
    // The demux thread and the queue it fills. Declared here, not inside the
    // playback block, so every way out of this function can stop the thread
    // before the format context it reads from is closed.
    PacketQueue packets;
    pthread_t demux_thread{};
    bool demux_running = false;
    // The sound gets its own queue and its own thread: resampling a 5.1 track
    // is microseconds of work, but it is microseconds that used to happen
    // between two pictures.
    PacketQueue audio_packets;
    pthread_t audio_thread{};
    bool audio_running = false;
    // Set when the sound thread could not be created: the demuxer then drops
    // audio packets instead of queueing them for nobody, which would fill the
    // queue and stall the reader.
    std::atomic<bool> audio_disabled{false};
    // Dolby Vision profile 5 keeps the polynomial that undoes its reshaping in
    // a per-frame RPU, and the hardware decoder ignores it, so it is read here
    // out of the packets. Everything else has nothing to reshape.
    std::unique_ptr<DoviRpu> dovi;
    int dovi_annex_b = -1;
    bool dovi_reported = false;
    bool dovi_probed = false;
    std::uint32_t dovi_signature = 0; // reshaping curve, as last turned into a table

    // The software decoder, opened either as the primary path or as the
    // fallback when the hardware one refuses the stream or fails mid-film.
    const auto open_software = [&]() -> bool
    {
        AVStream *stream = format->streams[video_index];
        const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr)
        {
            sys::log("[HUI] video software: no decoder for %s",
                     avcodec_get_name(stream->codecpar->codec_id));
            return false;
        }
        video = avcodec_alloc_context3(codec);
        if (video == nullptr || avcodec_parameters_to_context(video, stream->codecpar) < 0)
        {
            sys::log("[HUI] video software: context refused");
            avcodec_free_context(&video);
            return false;
        }
        // Frame threading is the big win on Zen2, but every thread holds a
        // whole picture: at 4K 10-bit that is about 18 MB each, and asking for
        // six of them is how opening a 4K film used to fail with nothing on
        // screen but "this format will not play". A 1080p stream gets the six
        // cores; a 4K one gets four and keeps the rest of the heap.
        const int w = stream->codecpar->width, h = stream->codecpar->height;
        const bool big = w > 1920 || h > 1088;
        video->thread_count = big ? 4 : 6;
        video->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        const int rc = avcodec_open2(video, codec, nullptr);
        if (rc < 0)
        {
            sys::log("[HUI] video software: open %dx%d threads=%d rc=%d", w, h, video->thread_count,
                     rc);
            avcodec_free_context(&video);
            return false;
        }
        return true;
    };

    // Hardware first: the console's video block decodes H.264/HEVC/VP9 up to
    // 4K in milliseconds, which is the only way a 4K HEVC film plays on this
    // CPU. Whatever it refuses falls through to FFmpeg.
    std::string why; // why the hardware decoder said no, for the error line
    if (decoder() != 0)
    {
        static bool module_loaded = false;
        if (!module_loaded)
        {
            HwDecoder::load_module();
            module_loaded = true;
        }
        hw = HwDecoder::open(format->streams[video_index]->codecpar, &why);
        if (hw == nullptr)
        {
            sys::log("[HUI] hwdec refused: %s", why.c_str());
        }
        else
        {
            sys::log("[HUI] hwdec on: %s %dx%d", hw->name(),
                     format->streams[video_index]->codecpar->width,
                     format->streams[video_index]->codecpar->height);
            hw_frame = av_frame_alloc();
            ok = hw_frame != nullptr;
        }
    }
    if (ok && hw == nullptr)
    {
        ok = open_software();
        sys::log("[HUI] video software decoder %s", ok ? "on" : "refused");
    }
    if (ok && audio_index >= 0)
    {
        AVStream *stream = format->streams[audio_index];
        const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec != nullptr)
        {
            audio = avcodec_alloc_context3(codec);
            if (audio != nullptr && avcodec_parameters_to_context(audio, stream->codecpar) >= 0 &&
                avcodec_open2(audio, codec, nullptr) >= 0)
            {
                AVChannelLayout stereo;
                av_channel_layout_default(&stereo, 2);
                if (swr_alloc_set_opts2(&resample, &stereo, AV_SAMPLE_FMT_FLT, audio::kSampleRate,
                                        &audio->ch_layout,
                                        static_cast<AVSampleFormat>(audio->sample_fmt),
                                        audio->sample_rate, 0, nullptr) < 0 ||
                    swr_init(resample) < 0)
                {
                    // A film with a track this build cannot resample still has
                    // pictures; drop the sound rather than the whole thing.
                    swr_free(&resample);
                    avcodec_free_context(&audio);
                }
            }
            else if (audio != nullptr)
            {
                avcodec_free_context(&audio);
            }
        }
    }
    if (ok && subtitle_index >= 0)
    {
        AVStream *stream = format->streams[subtitle_index];
        bool text_subtitle = false;
        for (const SubtitleTrack &track : subtitle_tracks)
        {
            if (track.ordinal == chosen_subtitle)
            {
                text_subtitle = track.text;
                break;
            }
        }
        (void)text_subtitle; // the menu's text/graphical label is set elsewhere
        // Text and bitmap (PGS/DVD) streams both get a decoder now: the
        // bitmap frames are converted to RGBA and handed to the renderer as a
        // sub-picture, so the picture is no longer thrown away. The `text`
        // flag on the track still tells the menu which kind a stream is.
        {
            const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (codec != nullptr)
            {
                subtitle = avcodec_alloc_context3(codec);
                if (subtitle != nullptr &&
                    (avcodec_parameters_to_context(subtitle, stream->codecpar) < 0 ||
                     avcodec_open2(subtitle, codec, nullptr) < 0))
                {
                    avcodec_free_context(&subtitle);
                }
                sys::log("[HUI] video subtitle decoder %s (%s) canvas %dx%d",
                         subtitle != nullptr ? "on" : "refused",
                         avcodec_get_name(stream->codecpar->codec_id),
                         subtitle != nullptr ? subtitle->width : 0,
                         subtitle != nullptr ? subtitle->height : 0);
            }
        }
    }
    if (ok)
    {
        decoded = av_frame_alloc();
        ok = decoded != nullptr;
    }
    if (!ok)
    {
        // The line on screen carries the reason, not just "cannot play": a
        // refused film is either a hardware limit or a software one, and the
        // two are fixed in completely different places.
        std::string message = "这个格式暂时播不了";
        if (!why.empty())
        {
            message += "（";
            message += why;
            message += "）";
        }
        fail(message.c_str());
        goto cleanup;
    }

    {
        duration_.store(static_cast<int>(format->duration / AV_TIME_BASE));
        if (resume > 0)
        {
            const std::int64_t target = static_cast<std::int64_t>(resume) * AV_TIME_BASE;
            // Backward seek to the cluster at or before `target`. The audio
            // thread will drop any sample whose pts < target before it
            // pushes, so both streams end up starting at target. Video
            // already filters early pictures via `drop_before`. This is the
            // Kodi/VLC model: audio is master, both sides drop pre-target
            // content, media anchored at target.
            audio_skip_until_seconds_.store(static_cast<double>(resume), std::memory_order_release);
            int rc = avformat_seek_file(format, -1, INT64_MIN, target, target, 0);
            if (rc < 0)
            {
                rc = av_seek_frame(format, -1, target, AVSEEK_FLAG_BACKWARD);
            }
            sys::log("[HUI] video resume seek to %ds rc=%d", resume, rc);
            if (video != nullptr)
            {
                avcodec_flush_buffers(video);
            }
            if (hw != nullptr)
            {
                hw->flush();
            }
            if (audio != nullptr)
            {
                avcodec_flush_buffers(audio);
            }
        }
        // Where the sound in this ring starts, in the file's own seconds.
        double clock_base = static_cast<double>(resume);
        bool clock_anchored = resume > 0;
        const std::int64_t started_us = sys::monotonic_us();
        // When the last picture went into the queue. A software decoder that
        // cannot keep up (4K HEVC on Zen2) produces frames far behind the audio
        // clock; dropping every one of them leaves the screen on the previous
        // picture and only the sound moves. Once nothing has been shown for a
        // while, a late frame is better than no frame.
        std::int64_t last_queued_us = 0;
        // Diagnostics, mirrored into steps.log through the `[HUI] video` line
        // every few seconds: a film with sound but no picture was previously
        // indistinguishable from a healthy one until someone attached a
        // debugger.
        long long seen_frames = 0;
        long long late_drops = 0;
        long long scale_fails = 0;
        long long scaled_frames = 0;
        long long clock_resets = 0;
        // True when the last picture was dropped as late, so the next one is
        // shown whatever the clock says.
        bool dropped_last = false;
        // After a seek, pictures older than the target are decoded but not
        // shown. A container can only seek to a keyframe, and the keyframe
        // before 25:00 can easily be at 24:48 - twelve seconds of film the
        // viewer asked to skip past. Without this the skip button looked
        // broken on any film with sparse keyframes, and worked on the ones
        // that happen to have one every second: "it works on some films but
        // not all" was exactly this. The reference implementation does the
        // same thing with `vdrop_`.
        double drop_before = -1.0;
        std::int64_t last_stats_us = started_us;
        // Seek hold-and-commit. The demux thread has already found the new
        // keyframe and flushed both packet queues by the time `seek_applied`
        // lands here, but the audio thread still has to wait for a fresh HTTP
        // range response, flush its own decoder, resample and write. That gap
        // is 100-500 ms and it varies per seek. Meanwhile the ring is drained
        // and silent, so `played()` does not move and the media clock reads
        // `wanted` in a frozen way. If video keeps queueing pictures during
        // that window, each one is scheduled against the stale clock and the
        // frame rate is effectively halved; when audio finally resumes, all
        // those pictures come due at once and the screen bursts ahead of the
        // sound - the exact "FF 后声音明显比图像慢、进度抖、延迟不固定" pattern
        // on any networked file. Hold the video loop until `played()` moves
        // for the first time after the seek, then let both clocks walk forward
        // from the same tick. Three seconds of no audio is treated as a dead
        // or missing track and the hold releases so the film carries on.
        bool seek_awaiting_audio = false;
        std::size_t played_at_seek = 0;
        std::int64_t seek_started_us = 0;
        int held_wanted = 0;

        // The demuxer, on its own thread.
        //
        // Reading and decoding used to share one thread, so a stalled range
        // request stopped the decoder, and a full audio ring stopped the
        // reader - which is the same thread as the picture. Two threads and a
        // queue between them fix both: a slow read only makes the queue
        // deeper, and the decoder keeps working from what is already here.
        // `packets`, `demux_thread` and `demux_running` live at function
        // scope so that cleanup can always stop the reader before the format
        // context it is reading from goes away.
        std::atomic<int> seek_applied{-1};
        std::atomic<int> read_us_shared{0};
        std::atomic<int> read_us_peak{0};
        // Packets in memory, in bytes. A 4K remux is about 7 MB a second, so
        // this is a little over a second of film - enough that a read hiccup
        // never reaches the screen. It used to be 24 MB, and that was 24 MB of
        // a 256 MB heap spent before the first picture was even decoded: the
        // measured baseline went 54 MB idle to 147 MB at the first frame, and
        // the film then had to fit in what was left.
        //
        // This title now gets a much larger heap; a high-bitrate remux can
        // spend 8 MB in less than a second, and that left no room for a range
        // request to hiccup. Keep the old small budget on small heaps, and
        // buffer four seconds where the heap can afford it.
        const std::size_t kMaxPacketBytes =
            hui_heap_capacity() >= (512u << 20) ? std::size_t(32u << 20) : std::size_t(8u << 20);
        // Roughly half a minute of a high-bitrate track. Sound is cheap per
        // second but not free, and this is the only thing standing between a
        // stalled ring and an unbounded queue.
        const std::size_t kMaxAudioPacketBytes =
            hui_heap_capacity() >= (512u << 20) ? std::size_t(8u << 20) : std::size_t(2u << 20);
        // Subtitles are decoded on the demux thread, not queued. There are
        // only a few packets a minute, and a queue would add a third thread
        // and a lifetime rule for a payload that is already in hand.
        const auto decode_subtitle = [&](AVPacket *pkt)
        {
            if (subtitle == nullptr)
            {
                return;
            }
            AVSubtitle parsed{};
            int got = 0;
            if (avcodec_decode_subtitle2(subtitle, &parsed, &got, pkt) < 0 || got == 0)
            {
                avsubtitle_free(&parsed);
                return;
            }
            std::string text;
            for (unsigned i = 0; i < parsed.num_rects; ++i)
            {
                const AVSubtitleRect *rect = parsed.rects[i];
                std::string part;
                if (rect->type == SUBTITLE_ASS)
                {
                    part = clean_subtitle_text(ass_text(rect->ass));
                }
                else if (rect->type == SUBTITLE_TEXT && rect->text != nullptr)
                {
                    part = clean_subtitle_text(rect->text);
                }
                // SUBTITLE_BITMAP is PGS/DVD: a picture, not text. The track
                // stays listed as 图形 in the menu, and this renderer leaves
                // the picture clean rather than drawing the wrong thing.
                if (!part.empty())
                {
                    if (!text.empty())
                    {
                        text += '\n';
                    }
                    text += part;
                }
            }
            if (!text.empty())
            {
                std::int64_t start_ms = 0;
                if (parsed.pts != AV_NOPTS_VALUE)
                {
                    start_ms = parsed.pts / 1000;
                }
                else if (subtitle_index >= 0)
                {
                    const double seconds =
                        seconds_of(format->streams[subtitle_index]->time_base, pkt->pts);
                    if (seconds >= 0.0)
                    {
                        start_ms = static_cast<std::int64_t>(seconds * 1000.0);
                    }
                }
                const int duration = parsed.end_display_time > 0 ? parsed.end_display_time : 2500;
                SubtitleCue cue;
                cue.start_ms = static_cast<int>(std::max<std::int64_t>(0, start_ms)) +
                               static_cast<int>(parsed.start_display_time);
                cue.end_ms = cue.start_ms + std::max(500, duration);
                cue.text = std::move(text);
                std::lock_guard<std::mutex> lock(subtitle_mutex_);
                subtitle_cues_.push_back(std::move(cue));
                // The demuxer reads well ahead of the playhead, so this queue
                // holds future cues, not history. 256 was only a few minutes
                // of dialogue: on a long film with a large packet queue it
                // popped cues before they were due and the subtitles simply
                // stopped. 8192 covers several hours while staying under a
                // megabyte of text.
                while (subtitle_cues_.size() > 8192)
                {
                    subtitle_cues_.pop_front();
                }
            }
            // Bitmap subtitles (PGS/DVD). Convert every SUBTITLE_BITMAP rect
            // of this frame into one RGBA sub-picture and hand it up with its
            // placement in the stream's own canvas. A SUBTITLE_RECT bitmap is a
            // PAL8 buffer: data[0] is the 8-bit index plane (pitch linesize[0])
            // and data[1] the uint32 palette. We never read that palette by hand
            // - we hand data/linesize straight to libswscale, exactly like ffplay
            // (fftools/ffplay.c), so the layout is interpreted by the same library
            // that produced it. x/y/w/h are in the subtitle stream's frame, the
            // canvas we scale against.
            {
                int minx = 1 << 30, miny = 1 << 30, maxx = 0, maxy = 0;
                unsigned bitmaps = 0;
                for (unsigned i = 0; i < parsed.num_rects; ++i)
                {
                    const AVSubtitleRect *r = parsed.rects[i];
                    if (r == nullptr || r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0 ||
                        r->data[0] == nullptr || r->data[1] == nullptr)
                    {
                        continue;
                    }
                    ++bitmaps;
                    minx = std::min(minx, r->x);
                    miny = std::min(miny, r->y);
                    maxx = std::max(maxx, r->x + r->w);
                    maxy = std::max(maxy, r->y + r->h);
                }
                if (bitmaps > 0 && maxx > minx && maxy > miny)
                {
                    const int bx = minx, by = miny;
                    const int bw = maxx - minx, bh = maxy - miny;
                    // A hard ceiling: a corrupt frame claiming a 4000x4000
                    // picture would spend 64 MB of the play heap on nothing.
                    if (bw <= 3840 && bh <= 2160)
                    {
                        SubtitleBitmap bm;
                        bm.x = bx;
                        bm.y = by;
                        bm.w = bw;
                        bm.h = bh;
                        bm.canvas_w = subtitle->width > 0
                                          ? subtitle->width
                                          : format->streams[video_index]->codecpar->width;
                        bm.canvas_h = subtitle->height > 0
                                          ? subtitle->height
                                          : format->streams[video_index]->codecpar->height;
                        bm.rgba.assign(static_cast<std::size_t>(bw) * bh * 4, 0);
                        int opaque = 0;    // pixels the palette left visible
                        int max_alpha = 0; // brightest alpha across them
                        // Expand each rect's PAL8 buffer to RGBA with libswscale -
                        // the way ffplay does it. sws knows the palette lives in
                        // data[1] and the indices in data[0]; hand-rolling that
                        // read is where a swapped data[0]/data[1] produced a
                        // silently all-black sub-picture (the "字幕没出来" bug).
                        for (unsigned i = 0; i < parsed.num_rects; ++i)
                        {
                            const AVSubtitleRect *r = parsed.rects[i];
                            if (r == nullptr || r->type != SUBTITLE_BITMAP || r->w <= 0 ||
                                r->h <= 0 || r->data[0] == nullptr || r->data[1] == nullptr)
                            {
                                continue;
                            }
                            std::vector<std::uint8_t> rect_rgba(
                                static_cast<std::size_t>(r->w) * r->h * 4, 0);
                            std::uint8_t *dst_planes[4] = {rect_rgba.data(), nullptr, nullptr,
                                                           nullptr};
                            int dst_linesize[4] = {r->w * 4, 0, 0, 0};
                            struct SwsContext *sws = sws_getContext(
                                r->w, r->h, AV_PIX_FMT_PAL8, r->w, r->h, AV_PIX_FMT_RGBA, SWS_POINT,
                                nullptr, nullptr, nullptr);
                            if (sws == nullptr)
                            {
                                continue;
                            }
                            sws_scale(sws, reinterpret_cast<const uint8_t *const *>(r->data),
                                      r->linesize, 0, r->h, dst_planes, dst_linesize);
                            sws_freeContext(sws);
                            // Blit the converted rect into the sub-picture at its
                            // offset inside the bounding box, clipping to the box.
                            const int rx = std::max(r->x, bx);
                            const int ry = std::max(r->y, by);
                            const int rr = std::min(r->x + r->w, bx + bw);
                            const int rb = std::min(r->y + r->h, by + bh);
                            for (int y = ry; y < rb; ++y)
                            {
                                const int oy = y - by;
                                const std::size_t srow = static_cast<std::size_t>(y - r->y) * r->w;
                                for (int x = rx; x < rr; ++x)
                                {
                                    const std::uint8_t *s =
                                        &rect_rgba[(srow + static_cast<std::size_t>(x - r->x)) * 4];
                                    std::uint8_t *d = &bm.rgba[(static_cast<std::size_t>(oy) * bw +
                                                                static_cast<std::size_t>(x - bx)) *
                                                               4];
                                    d[0] = s[0];
                                    d[1] = s[1];
                                    d[2] = s[2];
                                    d[3] = s[3];
                                    if (s[3] > 0)
                                    {
                                        ++opaque;
                                        if (static_cast<int>(s[3]) > max_alpha)
                                        {
                                            max_alpha = static_cast<int>(s[3]);
                                        }
                                    }
                                }
                            }
                        }
                        std::int64_t base_ms = 0;
                        if (parsed.pts != AV_NOPTS_VALUE)
                        {
                            base_ms = parsed.pts / 1000;
                        }
                        else if (subtitle_index >= 0)
                        {
                            const double seconds =
                                seconds_of(format->streams[subtitle_index]->time_base, pkt->pts);
                            if (seconds >= 0.0)
                            {
                                base_ms = static_cast<std::int64_t>(seconds * 1000.0);
                            }
                        }
                        base_ms = std::max<std::int64_t>(0, base_ms);
                        // PGS carries no real end: it holds the frame until the
                        // next composition set, and its end_display_time sentinel
                        // can be 0xFFFFFFFF, which as int is -1 (this made every
                        // cue window empty, so nothing ever showed). Treat a
                        // missing/absurd end as a 5-minute tail; a newer frame
                        // supersedes it and a seek clears the queue.
                        const std::int64_t rel_start =
                            static_cast<std::int64_t>(parsed.start_display_time);
                        std::int64_t rel_end = static_cast<std::int64_t>(parsed.end_display_time);
                        if (rel_end <= rel_start || rel_end > 3600000)
                        {
                            rel_end = rel_start + 300000;
                        }
                        bm.start_ms = static_cast<int>(base_ms + rel_start);
                        bm.end_ms = static_cast<int>(base_ms + rel_end);
                        bm.serial = ++subtitle_serial_;
                        // Diagnostic, now reading the real palette (data[1]).
                        // nzpal>0 means the decoder populated a palette and sws
                        // will expand it; nzpal=0 means the PGS decoder has no
                        // palette-definition segment yet (a feed/seek matter, not
                        // an alpha or a pointer one). opaque/maxa come from sws.
                        int nzpal = 0;
                        std::uint32_t palmax = 0;
                        if (parsed.num_rects > 0 && parsed.rects[0] != nullptr &&
                            parsed.rects[0]->data[1] != nullptr)
                        {
                            const std::uint32_t *pal =
                                reinterpret_cast<const std::uint32_t *>(parsed.rects[0]->data[1]);
                            for (int k = 0; k < 256; ++k)
                            {
                                if (pal[k] != 0u)
                                {
                                    ++nzpal;
                                    if (pal[k] > palmax)
                                    {
                                        palmax = pal[k];
                                    }
                                }
                            }
                        }
                        sys::log("[HUI] pgs frame serial=%d rect=%dx%d+%d+%d canvas=%dx%d "
                                 "t=%d-%d opaque=%d maxa=%d nzpal=%d palmax=%08x",
                                 bm.serial, bm.w, bm.h, bm.x, bm.y, bm.canvas_w, bm.canvas_h,
                                 bm.start_ms, bm.end_ms, opaque, max_alpha, nzpal, palmax);
                        std::lock_guard<std::mutex> lock(subtitle_mutex_);
                        subtitle_bitmaps_.push_back(std::move(bm));
                        while (subtitle_bitmaps_.size() > 32)
                        {
                            subtitle_bitmaps_.pop_front();
                        }
                    }
                }
            }
            avsubtitle_free(&parsed);
        };
        {
            const auto demux_loop = [&]()
            {
                AVPacket *in = av_packet_alloc();
                if (in == nullptr)
                {
                    packets.finish(true);
                    return;
                }
                int errors = 0;
                int peak_us = 0;
                int packets_read = 0;
                for (;;)
                {
                    if (!keep_running_.load())
                    {
                        break;
                    }
                    const int wanted = seek_request_.exchange(-1);
                    if (wanted >= 0)
                    {
                        // Seeking has to happen here: the format context is
                        // this thread's to touch. The decoder is told through
                        // `seek_applied`, because flushing its buffers from
                        // this thread would be a race against its own decode.
                        const std::int64_t target =
                            static_cast<std::int64_t>(wanted) * AV_TIME_BASE;
                        // Backward seek + audio drop pre-target (Kodi/VLC
                        // model): the audio thread consumes `skip_until`
                        // before it publishes its anchor and discards any
                        // frame whose pts is less than the target. Video
                        // already drops frames < `drop_before` on the emit
                        // side. Both streams then start at exactly `wanted`
                        // - the moment the progress bar is pointing at.
                        // Publish BEFORE the queue flush so the audio
                        // thread's next catch_up sees it (release/acquire
                        // pairs with the exchange inside catch_up).
                        audio_skip_until_seconds_.store(static_cast<double>(wanted),
                                                        std::memory_order_release);
                        int rc = avformat_seek_file(format, -1, INT64_MIN, target, target, 0);
                        if (rc < 0)
                        {
                            rc = av_seek_frame(format, -1, target, AVSEEK_FLAG_BACKWARD);
                        }
                        if (rc >= 0)
                        {
                            packets.flush();
                            audio_packets.flush();
                            seek_applied.store(wanted);
                        }
                        else
                        {
                            // Say so rather than dropping the request: a seek
                            // that reports success and does nothing is the one
                            // bug that looks like a frozen remote.
                            sys::log("[HUI] video seek to %ds failed rc=%d", wanted, rc);
                        }
                    }
                    if (paused_.load())
                    {
                        wait_us(10000);
                        continue;
                    }
                    // Backpressure: the queue is deep enough, let the decoder
                    // catch up rather than growing without bound. The sound is
                    // capped on its own, because the video budget says nothing
                    // about a track that is stuck behind a full ring.
                    if (packets.held_bytes() > kMaxPacketBytes ||
                        audio_packets.held_bytes() > kMaxAudioPacketBytes)
                    {
                        wait_us(5000);
                        continue;
                    }
                    const std::int64_t read_started = sys::monotonic_us();
                    const int read = av_read_frame(format, in);
                    const int took_us = static_cast<int>(sys::monotonic_us() - read_started);
                    if (took_us > peak_us)
                    {
                        peak_us = took_us;
                        read_us_peak.store(peak_us);
                    }
                    read_us_shared.store(took_us);
                    if (read < 0)
                    {
                        const bool at_end =
                            read == AVERROR_EOF || (format->pb != nullptr && avio_feof(format->pb));
                        if (!at_end && ++errors < 60)
                        {
                            if (errors <= 3)
                            {
                                sys::log("[HUI] video read rc=%d, retry %d", read, errors);
                            }
                            wait_us(100000);
                            continue;
                        }
                        if (at_end)
                        {
                            // The reader ran out of file, but this is not the
                            // end of playback: the decoder may still be working
                            // through a full queue, and the viewer can still
                            // seek back into the tail. Mark the streams ended
                            // (an undisturbed film backs out to the menu once
                            // its own queue drains) yet KEEP THIS THREAD ALIVE:
                            // a seek clears `eof` on the queues via flush(),
                            // re-positions the format context and reading
                            // resumes. Returning here is what made 快退/快进 die
                            // in the last minutes - the settle-fired seek had no
                            // reader left to run avformat_seek_file.
                            packets.finish(false);
                            audio_packets.finish(false);
                            wait_us(20000);
                            continue;
                        }
                        sys::log("[HUI] video read rc=%d, giving up after %d retries", read,
                                 errors);
                        break;
                    }
                    errors = 0;
                    if (packets_read < 30)
                    {
                        sys::log("[HUI] video pkt #%d stream=%d size=%d", packets_read,
                                 in->stream_index, in->size);
                    }
                    ++packets_read;
                    // Sound and picture go their separate ways here. Anything
                    // else - subtitles, data, cover art - is dropped: nothing
                    // on screen reads it, and it would only cost queue room.
                    PacketQueue *target = nullptr;
                    if (in->stream_index == video_index)
                    {
                        target = &packets;
                    }
                    else if (audio_index >= 0 && in->stream_index == audio_index &&
                             !audio_disabled.load())
                    {
                        target = &audio_packets;
                    }
                    else if (subtitle_index >= 0 && in->stream_index == subtitle_index)
                    {
                        decode_subtitle(in);
                        av_packet_unref(in);
                        continue;
                    }
                    if (target == nullptr)
                    {
                        av_packet_unref(in);
                        continue;
                    }
                    AVPacket *queued = av_packet_alloc();
                    if (queued == nullptr)
                    {
                        av_packet_unref(in);
                        continue;
                    }
                    // The buffer moves across, no copy: `in` is empty again and
                    // ready for the next read.
                    av_packet_move_ref(queued, in);
                    target->put(queued);
                }
                av_packet_free(&in);
                const bool stopped = !keep_running_.load();
                packets.finish(stopped);
                audio_packets.finish(stopped);
            };
            demux_running = start_thread(&demux_thread, demux_loop, 4u * 1024u * 1024u);
        }
        if (!demux_running)
        {
            // Without it there is nothing to decode from, and a silent fall
            // back to the old single-threaded path would bring the stutter
            // back without saying so.
            fail("播放线程创建失败");
            goto cleanup;
        }
        // One picture, written to the log directory as a PPM when asked for,
        // and its source planes sampled: a green screen is the picture
        // arriving at the scaler as zeros, and that is indistinguishable from
        // a colour-space bug without looking at the bytes.
        //
        // On request rather than once: the first picture of a film is a logo
        // on a dark field, where a plane taken from the wrong row looks like
        // nothing at all. What has to be looked at is a picture with a face in
        // it, and that is whatever is playing when the request arrives.

        // Once per session: the luma plane the shader is handed, as a PPM, and
        // its first four thousand bytes averaged. A picture that arrives as
        // zeros is indistinguishable from a colour-space bug without looking at
        // the bytes, and a flat green screen is exactly what both produce.
        // The picture the decoder actually handed over, colour-converted here
        // with the same matrix and the same ten-bit handling the shader uses,
        // written where it survives the app closing. Comparing this file with
        // a screenshot of the screen is what separates "the data is wrong"
        // from "the data is right and the upload is wrong", which is the whole
        // question when a picture looks doubled.
        const auto dump_picture = [](const AVFrame *src)
        {
            const bool ten =
                src->format == AV_PIX_FMT_P010LE || src->format == AV_PIX_FMT_YUV420P10LE;
            const int w = src->width;
            const int h = src->height;
            if (w < 2 || h < 2 || src->data[0] == nullptr || src->data[1] == nullptr)
                return;
            FILE *file = std::fopen("/data/homebrew/PPSA99102/frame.ppm", "wb");
            if (file == nullptr)
                return;
            // The console leaves ten-bit samples low-aligned in each 16-bit
            // word (black 64, mid-grey 512, white 940), so the value is the low
            // ten bits, not the top eight.
            const auto sample = [ten](const std::uint8_t *plane, int index) -> int
            {
                if (!ten)
                    return plane[index];
                const std::uint16_t word = reinterpret_cast<const std::uint16_t *>(plane)[index];
                return int(word & 0x3ffu) * 255 / 1023;
            };
            const auto clamp255 = [](double value) -> unsigned char {
                return static_cast<unsigned char>(value < 0.0     ? 0.0
                                                  : value > 255.0 ? 255.0
                                                                  : value);
            };
            std::fprintf(file, "P6\n%d %d\n255\n", w / 2, h / 2);
            for (int row = 0; row < h; row += 2)
            {
                const std::uint8_t *luma = src->data[0] + std::size_t(row) * src->linesize[0];
                const std::uint8_t *chroma = src->data[1] + std::size_t(row / 2) * src->linesize[1];
                const std::uint8_t *chroma_v =
                    src->data[2] == nullptr
                        ? nullptr
                        : src->data[2] + std::size_t(row / 2) * src->linesize[2];
                for (int column = 0; column < w; column += 2)
                {
                    const double y = (sample(luma, column) - 16.0) * (255.0 / 219.0);
                    // Interleaved chroma is two samples per texel, planar one.
                    const double u =
                        (sample(chroma, chroma_v == nullptr ? column : column / 2) - 128.0) *
                        (255.0 / 224.0);
                    const double v = (sample(chroma_v == nullptr ? chroma : chroma_v,
                                             chroma_v == nullptr ? column + 1 : column / 2) -
                                      128.0) *
                                     (255.0 / 224.0);
                    const unsigned char pixel[3] = {clamp255(y + 1.596 * v),
                                                    clamp255(y - 0.391 * u - 0.813 * v),
                                                    clamp255(y + 2.018 * u)};
                    std::fwrite(pixel, 1, 3, file);
                }
            }
            std::fclose(file);
            unsigned luma = 0;
            unsigned chroma = 0;
            for (int i = 0; i < 4096; ++i)
            {
                luma += src->data[0][i];
                chroma += src->data[1][i];
            }
            // Where the samples actually sit answers the range question the
            // container tag gets wrong on Dolby Vision: real studio-range data
            // keeps black at 64 and white under 940, full-range data reaches
            // 0 and 1023.
            const auto raw = [ten](const std::uint8_t *plane, int index) -> int
            {
                if (!ten)
                    return int(plane[index]) * 4;
                return int(reinterpret_cast<const std::uint16_t *>(plane)[index] & 0x3ffu);
            };
            int low = 1023, high = 0;
            for (int row = 0; row < h; row += 8)
            {
                const std::uint8_t *plane = src->data[0] + std::size_t(row) * src->linesize[0];
                for (int column = 0; column < w; column += 8)
                {
                    const int value = raw(plane, column);
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
            }
            sys::log("[HUI] video dump %dx%d fmt=%d pitch=%d/%d avg_y=%u avg_uv=%u y=%d..%d", w, h,
                     src->format, src->linesize[0], src->linesize[1], luma / 4096, chroma / 4096,
                     low, high);
        };
        state_.store(static_cast<int>(PlayState::playing));

        // Dropping an empty ".dump" next to the package asks for the next
        // picture the decoder hands over, written once and the request
        // consumed. On a timer instead of this it cost 200 ms every five
        // seconds - a stutter in the middle of the very measurement it was
        // there to make, and one that looked like a decoder that could not
        // keep up.
        const auto dump_wanted = []() -> bool
        {
            const char *path = "/data/homebrew/PPSA99102/.dump";
            if (std::fopen(path, "rb") == nullptr)
                return false;
            std::remove(path);
            return true;
        };

        // One decoded picture into the queue. The software decoder hands over
        // an AVFrame, the hardware one NV12/P010 planes wrapped in one; both
        // end up here, so the clock, the late-drop rule and the queue stay in
        // a single place. `from_hardware` says the planes are the console
        // decoder's, whose ten bits sit in the low end of each word; FFmpeg's
        // P010LE is high-aligned and its planar YUV420P10LE is low-aligned,
        // which `ten_low_aligned` works out from the format below.
        const auto emit_picture = [&](const AVFrame *src, bool from_hardware, double pts)
        {
            ++seen_frames;
            // Everything the decoder produces from the keyframe before the
            // target belongs to the moment the viewer skipped away from.
            if (drop_before >= 0.0 && pts >= 0.0)
            {
                if (pts < drop_before - 0.02)
                {
                    return;
                }
                drop_before = -1.0;
            }
            if (!clock_anchored && pts >= 0.0)
            {
                // The file does not start at zero, or it started mid-stream:
                // the first picture is where the sound begins.
                clock_base = pts;
                clock_anchored = true;
            }

            // 自动 keeps the picture at its own resolution: the shader does
            // the scaling now, so a 4K film costs one plane upload and nothing
            // else. A manual tier is applied below - to luma and chroma alike
            // and, on the hardware path, without handing low-aligned ten-bit
            // words to swscale, which expects P010's high-aligned ones and
            // turns them into a green picture. That is why this used to ignore
            // the tier entirely on hardware; the picker is honoured now.
            const int edge = frame_edge();
            int out_w = src->width;
            int out_h = src->height;
            if (out_w <= 0 || out_h <= 0)
            {
                return;
            }
            if (edge > 0 && std::max(out_w, out_h) > edge)
            {
                const double scale = static_cast<double>(edge) / std::max(out_w, out_h);
                // Even dimensions: chroma is 4:2:0, and an odd edge would
                // leave the last chroma column covering half a texel.
                out_w = std::max(2, static_cast<int>(out_w * scale)) & ~1;
                out_h = std::max(2, static_cast<int>(out_h * scale)) & ~1;
            }
            // A manual tier is the one case where the picture is rescaled on
            // the CPU. The software path uses swscale; the hardware path
            // cannot, because swscale expects P010's ten bits in the high end
            // of each word and the console puts them in the low end (see the
            // plane scaler below).
            const bool rescale = out_w != src->width || out_h != src->height;
            if (rescale && !from_hardware)
            {
                if (scaler == nullptr || scaler_w != out_w || scaler_h != out_h ||
                    scaler_fmt != src->format)
                {
                    if (scaler != nullptr)
                    {
                        sws_freeContext(scaler);
                    }
                    scaler = sws_getContext(src->width, src->height,
                                            static_cast<AVPixelFormat>(src->format), out_w, out_h,
                                            static_cast<AVPixelFormat>(src->format),
                                            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    scaler_w = scaler != nullptr ? out_w : 0;
                    scaler_h = scaler != nullptr ? out_h : 0;
                    scaler_fmt = scaler != nullptr ? src->format : 0;
                    if (scaler == nullptr && scale_fails++ < 3)
                    {
                        sys::log("[HUI] video sws %dx%d fmt=%d refused", src->width, src->height,
                                 src->format);
                    }
                }
                if (scaler == nullptr)
                {
                    return;
                }
            }

            // Where the sound is, in the file's own seconds. Both the late
            // rule and the schedule below are measured against this.
            const auto media_seconds = [&]() -> double
            {
                // Once the sound has played to its end but the picture still
                // has seconds left (a track shorter than the video), the ring's
                // played() count freezes and would strand those frames forever.
                // Fall back to the wall clock, which tracks the sound closely
                // enough that the switch stays continuous.
                if (audio != nullptr && !audio_finished_.load(std::memory_order_acquire))
                {
                    return clock_base + static_cast<double>(ring_->played()) /
                                            static_cast<double>(audio::kSampleRate);
                }
                return clock_base + static_cast<double>(sys::monotonic_us() - started_us) / 1.0e6;
            };

            // The hardware decoder pairs each output picture with the smallest
            // timestamp it has not used yet. That is right while input and
            // output alternate, and wrong the moment they drift apart - a
            // reordered B frame, the flush after a seek - because the picture
            // then inherits a timestamp several frames old and every frame
            // after it stays that far behind the sound. Measured on hardware:
            // two thirds of the pictures were dropped as late and the screen
            // ran at 8 fps from a decoder that was keeping up perfectly.
            //
            // Half a second behind is not "a little late", it is a timestamp
            // that has come loose: re-anchor the clock to this picture and
            // carry on from where the sound actually is.
            if (pts >= 0.0 && audio != nullptr && media_seconds() - pts > 0.5)
            {
                clock_base = pts - static_cast<double>(ring_->played()) /
                                       static_cast<double>(audio::kSampleRate);
                ++clock_resets;
            }

            // A late picture is not worth the copy: the frame after it is
            // already on its way. Three exceptions, all about not leaving the
            // screen empty: nothing has been shown for half a second, nothing
            // is waiting for the render thread at all, and the previous
            // picture was dropped too. The last one is the backstop that
            // matters - dropping a run of frames is what turns "a little
            // behind" into a slideshow.
            const std::int64_t late_now_us = sys::monotonic_us();
            bool queue_empty;
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                queue_empty = pending_.empty();
            }
            const bool starving = queue_empty || late_now_us - last_queued_us > 500000;
            if (!starving && !dropped_last && audio != nullptr && pts >= 0.0 &&
                pts + kLateSeconds < media_seconds())
            {
                ++late_drops;
                dropped_last = true;
                return;
            }
            dropped_last = false;

            // When this picture is due on screen. The sound under it is the
            // clock (the audio thread's play-out counter, or the wall clock
            // when there is no sound), mapped to monotonic time at the instant
            // the frame comes out of the decoder; the render thread holds a
            // frame back until then, so a queue deep enough to absorb a slow
            // frame cannot push the picture permanently behind the sound.
            const std::int64_t now_us = sys::monotonic_us();
            const double media_now = media_seconds();
            const std::int64_t show_by =
                pts >= 0.0 ? now_us + static_cast<std::int64_t>((pts - media_now) * 1.0e6) : now_us;

            // The decoder's planes, copied once into the handoff buffer. The
            // hardware decoder's picture points into a pool slot it reuses a
            // few frames later, and the software decoder's AVFrame is unref'd
            // as soon as this returns, so one copy is the price of keeping it.
            // No colour conversion happens here: the shader reads these planes
            // directly.
            const bool ten_p010 = src->format == AV_PIX_FMT_P010LE;
            const bool ten_planar = src->format == AV_PIX_FMT_YUV420P10LE;
            const bool ten = ten_p010 || ten_planar;
            const bool planar = src->data[2] != nullptr;
            const int sample_bytes = ten ? 2 : 1;
            const int luma_pitch = src->linesize[0];
            const int chroma_pitch = src->linesize[1];
            const int v_pitch = planar ? src->linesize[2] : 0;
            if (luma_pitch <= 0 || chroma_pitch <= 0 || (!planar && src->data[1] == nullptr))
            {
                return;
            }
            // Where the ten bits sit. The console's decoder writes them low;
            // FFmpeg writes P010 high and its planar 10-bit formats low. The
            // shader's sample scale depends on this by a factor of 64, so
            // getting it wrong is not a slight tint but a colour wreck.
            const bool low_aligned = ten && (ten_planar || from_hardware);

            const int chroma_w = std::max(1, out_w / 2);
            const int chroma_h = std::max(1, out_h / 2);
            // No rescale: keep the decoder's row padding, so a plane copy is
            // one memcpy. Rescaled: write tight rows, which is what both sws
            // and the plane scaler below are given as their destination.
            const int stored_luma_pitch = rescale ? out_w * sample_bytes : luma_pitch;
            const int stored_chroma_pitch =
                rescale ? chroma_w * (planar ? sample_bytes : 2 * sample_bytes) : chroma_pitch;
            const int stored_v_pitch = planar ? (rescale ? chroma_w * sample_bytes : v_pitch) : 0;
            const std::size_t luma_bytes = static_cast<std::size_t>(stored_luma_pitch) * out_h;
            const std::size_t chroma_bytes =
                static_cast<std::size_t>(stored_chroma_pitch) * chroma_h;
            const std::size_t v_bytes =
                planar ? static_cast<std::size_t>(stored_v_pitch) * chroma_h : std::size_t(0);
            const std::size_t bytes = luma_bytes + chroma_bytes + v_bytes;
            if (bytes > kMaxFrameBytes)
            {
                if (scale_fails++ < 3)
                {
                    sys::log("[HUI] video frame %zu MB refused", bytes >> 20);
                }
                return;
            }
            // Never ask for a picture the heap cannot hold. With exceptions off
            // the request that fails is an abort, not an error return, and the
            // whole title goes with it; a dropped frame is a stutter.
            {
                std::size_t live = 0, peak = 0, blocks = 0, fails = 0;
                hui_heap_stats(&live, &peak, &blocks, &fails);
                const std::size_t capacity = hui_heap_capacity();
                if (capacity != 0 && live + bytes > capacity)
                {
                    if (scale_fails++ < 3)
                    {
                        sys::log("[HUI] video frame %zuMB would not fit: heap %zu/%zuMB",
                                 bytes >> 20, live >> 20, capacity >> 20);
                    }
                    return;
                }
            }

            // The first few pictures of every film, with the heap as it stands
            // right before the biggest allocation the app makes. A crash in the
            // first second of playback is either this allocation or the upload
            // that follows it, and this says which one the log reached.
            if (seen_frames <= 4)
            {
                std::size_t live = 0, peak = 0, blocks = 0, fails = 0;
                hui_heap_stats(&live, &peak, &blocks, &fails);
                sys::log("[HUI] video emit #%lld %dx%d %zuMB heap=%zu/%zuMB fails=%zu", seen_frames,
                         out_w, out_h, bytes >> 20, live >> 20, hui_heap_capacity() >> 20, fails);
            }
            Frame frame;
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                if (!spare_.empty())
                {
                    frame.pixels = std::move(spare_.back());
                    spare_.pop_back();
                }
            }
            const std::int64_t pack_started_us = sys::monotonic_us();
            frame.pixels.resize(bytes);
            if (rescale && from_hardware)
            {
                // A plane scaler that does not care where the ten bits are.
                // Luma is a box average, so a downscale keeps detail instead
                // of shimmering. Interleaved chroma is scaled in whole U/V
                // pairs: treating it as a flat word array is exactly what
                // turned V into a repeated U before.
                const int src_chroma_h = std::max(1, src->height / 2);
                const auto scale_plane = [&](const std::uint8_t *from, int from_pitch,
                                             int src_texels, int src_rows, std::uint8_t *to,
                                             int to_pitch, int dst_texels, int dst_rows,
                                             int components)
                {
                    if (src_texels <= 0 || src_rows <= 0)
                    {
                        return;
                    }
                    for (int y = 0; y < dst_rows; ++y)
                    {
                        const int sy0 = y * src_rows / dst_rows;
                        int sy1 = (y + 1) * src_rows / dst_rows;
                        if (sy1 <= sy0)
                        {
                            sy1 = sy0 + 1;
                        }
                        if (sy1 > src_rows)
                        {
                            sy1 = src_rows;
                        }
                        std::uint8_t *out = to + static_cast<std::size_t>(to_pitch) * y;
                        for (int x = 0; x < dst_texels; ++x)
                        {
                            const int sx0 = x * src_texels / dst_texels;
                            int sx1 = (x + 1) * src_texels / dst_texels;
                            if (sx1 <= sx0)
                            {
                                sx1 = sx0 + 1;
                            }
                            if (sx1 > src_texels)
                            {
                                sx1 = src_texels;
                            }
                            const unsigned count = static_cast<unsigned>((sy1 - sy0) * (sx1 - sx0));
                            for (int c = 0; c < components; ++c)
                            {
                                unsigned sum = 0;
                                for (int sy = sy0; sy < sy1; ++sy)
                                {
                                    const std::uint8_t *row =
                                        from + static_cast<std::size_t>(sy) * from_pitch;
                                    if (ten)
                                    {
                                        const auto *words =
                                            reinterpret_cast<const std::uint16_t *>(row);
                                        for (int sx = sx0; sx < sx1; ++sx)
                                        {
                                            sum += words[sx * components + c];
                                        }
                                    }
                                    else
                                    {
                                        for (int sx = sx0; sx < sx1; ++sx)
                                        {
                                            sum += row[sx * components + c];
                                        }
                                    }
                                }
                                const unsigned value = (sum + count / 2) / count;
                                if (ten)
                                {
                                    reinterpret_cast<std::uint16_t *>(out)[x * components + c] =
                                        static_cast<std::uint16_t>(value);
                                }
                                else
                                {
                                    out[x * components + c] = static_cast<std::uint8_t>(value);
                                }
                            }
                        }
                    }
                };
                std::uint8_t *base = frame.pixels.data();
                scale_plane(src->data[0], luma_pitch, luma_pitch / sample_bytes, src->height, base,
                            stored_luma_pitch, out_w, out_h, 1);
                scale_plane(src->data[1], chroma_pitch, chroma_pitch / (2 * sample_bytes),
                            src_chroma_h, base + luma_bytes, stored_chroma_pitch, chroma_w,
                            chroma_h, 2);
                if (planar)
                {
                    scale_plane(src->data[2], v_pitch, v_pitch / sample_bytes, src_chroma_h,
                                base + luma_bytes + chroma_bytes, stored_v_pitch, chroma_w,
                                chroma_h, 1);
                }
            }
            else if (rescale)
            {
                // The software path's manual tier: sws writes each plane in
                // place, at the source's own format and the requested size.
                std::uint8_t *planes[4] = {
                    frame.pixels.data(), frame.pixels.data() + luma_bytes,
                    planar ? frame.pixels.data() + luma_bytes + chroma_bytes : nullptr, nullptr};
                int strides[4] = {stored_luma_pitch, stored_chroma_pitch, stored_v_pitch, 0};
                sws_scale(scaler, src->data, src->linesize, 0, src->height, planes, strides);
            }
            else
            {
                std::memcpy(frame.pixels.data(), src->data[0], luma_bytes);
                std::memcpy(frame.pixels.data() + luma_bytes, src->data[1], chroma_bytes);
                if (planar)
                {
                    std::memcpy(frame.pixels.data() + luma_bytes + chroma_bytes, src->data[2],
                                v_bytes);
                }
            }
            ++scaled_frames;
            pack_us_.store(static_cast<int>(sys::monotonic_us() - pack_started_us),
                           std::memory_order_relaxed);
            if (scaled_frames % 30 == 0 && dump_wanted())
            {
                dump_picture(src);
            }
            frame.luma_bytes = luma_bytes;
            frame.chroma_bytes = chroma_bytes;
            frame.y_pitch = stored_luma_pitch;
            frame.c_pitch = stored_chroma_pitch;
            frame.v_pitch = stored_v_pitch;
            frame.ten_bit = ten;
            frame.ten_bit_low_aligned = low_aligned;
            frame.planar_chroma = planar;
            frame.width = out_w;
            frame.height = out_h;
            frame.show_by_us = show_by;
            frame.pts_us = pts >= 0.0 ? static_cast<std::int64_t>(pts * 1.0e6) : 0;
            last_queued_us = now_us;
            if (seen_frames <= 4)
            {
                sys::log("[HUI] video queued #%lld in=%lldms", seen_frames,
                         static_cast<long long>((show_by - now_us) / 1000));
            }
            {
                std::unique_lock<std::mutex> lock(frame_mutex_);
                // A full queue means the render thread has not taken the last
                // pictures yet - it does not mean these are worth throwing
                // away. The decoder runs in bursts (the only thing pacing it
                // is a second of sound), and the queue is two 4K frames deep,
                // so a burst loses most of itself here. Measured on hardware:
                // 24 fps decoded, 14 fps on screen, and every counter saying
                // nothing was dropped, because `scaled` counts this line and
                // the picture was thrown away a few lines later.
                //
                // Waiting instead keeps the two threads at one pace. The byte
                // budget matters more at 4K, where three frames alone are
                // fifty megabytes, so both limits are checked.
                const auto room_for_one_more = [&]() -> bool
                {
                    if (pending_.size() >= kMaxQueued)
                        return false;
                    std::size_t held = 0;
                    for (const Frame &queued : pending_)
                    {
                        held += queued.pixels.size();
                    }
                    return held + bytes <= queue_budget();
                };
                // A picture that does not fit even in an empty queue would make
                // the wait below a deadlock; drop it and say so.
                if (!room_for_one_more() && pending_.empty())
                {
                    if (scale_fails++ < 3)
                    {
                        sys::log("[HUI] video frame %zuMB over the queue budget", bytes >> 20);
                    }
                    recycle_locked(std::move(frame.pixels));
                    return;
                }
                while (keep_running_.load() && !paused_.load() && !room_for_one_more())
                {
                    // Timed, not indefinite: a film that is closed while the
                    // worker waits has to be able to leave.
                    frame_cv_.wait_for(lock, std::chrono::milliseconds(4));
                }
                if (!keep_running_.load() || paused_.load())
                {
                    recycle_locked(std::move(frame.pixels));
                    return;
                }
                pending_.push_back(std::move(frame));
            }
        };

        // One picture is held back before it is scaled. The decoder's write
        // into the caller's frame slot is not guaranteed to have reached the
        // CPU by the time sceVideodec2Decode returns, and scaling it right
        // away is how a film comes out as a green screen - Y=0, U=V=0 is what
        // an untouched slot converts to. Handing over the previous picture
        // costs one frame of latency and reads a slot nobody is writing.
        struct HeldPicture
        {
            HwDecoder::Picture pic{};
            double pts = -1.0;
            bool used = false;
        } held;
        // The console's video block leaves 10-bit samples in the low bits of
        // each 16-bit word (black is 64, mid-grey is 512, white is 940) rather
        // than the high bits P010 uses. Nothing is done about that here: the
        // planes go to the GPU as they are, tagged low-aligned so the shader's
        // R16 sample scale turns them back into code values (see GlBatch). A
        // CPU pass over nine million words per picture is exactly the cost
        // this path exists to avoid.
        const auto emit_hw = [&](const HwDecoder::Picture &pic, double pts)
        {
            if (hw_frame == nullptr || pic.y == nullptr)
            {
                return;
            }
            hw_frame->width = pic.width;
            hw_frame->height = pic.height;
            hw_frame->format = pic.ten_bit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
            hw_frame->data[0] = const_cast<std::uint8_t *>(pic.y);
            hw_frame->data[1] = const_cast<std::uint8_t *>(pic.uv);
            hw_frame->linesize[0] = pic.pitch;
            hw_frame->linesize[1] = pic.pitch;
            hw_frame->data[2] = nullptr;
            hw_frame->linesize[2] = 0;
            emit_picture(hw_frame, true, pts);
        };
        // Every picture the hardware decoder has ready, oldest first. Its
        // planes point into the decoder's own pool, so they are wrapped in an
        // AVFrame and scaled straight away, before the next send reuses them.
        const auto drain_hw = [&]()
        {
            HwDecoder::Picture pic;
            while (hw != nullptr && hw->receive(&pic) == 1)
            {
                if (held.used)
                {
                    emit_hw(held.pic, held.pts);
                }
                held.pic = pic;
                held.pts = pic.pts_us == INT64_MIN ? -1.0 : static_cast<double>(pic.pts_us) / 1.0e6;
                held.used = true;
            }
        };
        const auto release_hw = [&]()
        {
            if (held.used)
            {
                emit_hw(held.pic, held.pts);
                held.used = false;
            }
        };

        // The last line before the read loop. A film that opens and then dies
        // with nothing in the log died between here and the first decode, and
        // this says which decoders were live when it happened - and how much
        // of the heap was already gone before the first picture was asked for.
        {
            std::size_t live = 0, peak = 0, blocks = 0, fails = 0;
            hui_heap_stats(&live, &peak, &blocks, &fails);
            sys::log("[HUI] video running: hw=%d sw=%d audio=%d resume=%d heap=%zu/%zuMB",
                     hw != nullptr ? 1 : 0, video != nullptr ? 1 : 0, audio != nullptr ? 1 : 0,
                     resume, live >> 20, hui_heap_capacity() >> 20);
        }
        // Reading, its retries and its timing all live on the demux thread now
        // and reach the stats line through read_us_shared / read_us_peak.

        // The sound, on its own thread.
        //
        // It used to be decoded inline: a packet, a resample, a write into the
        // ring, all between two pictures. The write spins when the ring is
        // full, and while it span the picture stopped too - which is exactly
        // the "stutters every second" this fixes. Now the ring being full is
        // the audio thread's problem alone, and the video thread never learns
        // that sound exists.
        {
            const auto audio_loop = [&]()
            {
                AVFrame *frame = av_frame_alloc();
                if (frame == nullptr || audio == nullptr || resample == nullptr)
                {
                    av_frame_free(&frame);
                    audio_packets.finish(true);
                    return;
                }
                // Its own buffer and its own frame: `samples` and `decoded`
                // belong to the video thread.
                std::vector<float> pcm;
                int serial = audio_packets.serial_value();
                bool sent_eof = false;
                // Set when catch_up fires and cleared on the first successful
                // ring write after it. Guards the audio-anchor publication so a
                // sample that was mid-decode when the serial changed cannot be
                // mistaken for "the first new-content sample".
                bool anchor_pending = false;
                // Kodi/VLC: after a seek, discard decoded frames whose pts is
                // before the target. Set by catch_up from the shared atomic
                // the demux publishes before flush; cleared on the first
                // frame that reaches or passes the target.
                double skip_until_pts = -1.0;
                const auto catch_up = [&]()
                {
                    const int now_serial = audio_packets.serial_value();
                    if (now_serial == serial)
                    {
                        return;
                    }
                    // Everything decoded before the jump belongs to a moment the
                    // film no longer occupies.
                    avcodec_flush_buffers(audio);
                    serial = now_serial;
                    sent_eof = false;
                    audio_finished_.store(false, std::memory_order_release);
                    anchor_pending = true;
                    // Reset the published anchor on the audio thread's side.
                    // Doing it here (rather than in the video thread's seek
                    // handler) removes a race: the video reset could land
                    // *after* this thread had already stored the new A0,
                    // wiping it before the hold committed and forcing a
                    // fallback to `wanted`.
                    audio_anchor_pts_us_.store(INT64_MIN, std::memory_order_relaxed);
                    // Pick up the pre-target drop threshold the demux
                    // published just before flushing our queue.
                    skip_until_pts =
                        audio_skip_until_seconds_.exchange(-1.0, std::memory_order_acquire);
                };
                for (;;)
                {
                    if (!keep_running_.load())
                    {
                        break;
                    }
                    catch_up();
                    AVPacket *pkt = nullptr;
                    const int got = audio_packets.get(&pkt, 50);
                    if (got == 0)
                    {
                        continue;
                    }
                    if (got < 0)
                    {
                        if (got == -2 && !sent_eof)
                        {
                            // Drain the decoder so the last fraction of a
                            // second is not lost.
                            avcodec_send_packet(audio, nullptr);
                            sent_eof = true;
                            continue;
                        }
                        if (got == -2)
                        {
                            // The audio stream is over and its decoder flushed.
                            // Once the ring has played out the last buffered
                            // sample, tell the video thread so it can carry the
                            // trailing pictures on the wall clock instead of
                            // waiting on a sound that will never advance again.
                            if (ring_ != nullptr && ring_->available() == 0)
                            {
                                audio_finished_.store(true, std::memory_order_release);
                            }
                            wait_us(10000);
                            continue;
                        }
                        break; // stopped
                    }
                    // A seek may have landed between the check above and this
                    // packet; flushing before decoding it is what keeps a
                    // pre-seek packet out of the new ring.
                    catch_up();
                    avcodec_send_packet(audio, pkt);
                    av_packet_free(&pkt);
                    while (keep_running_.load() && avcodec_receive_frame(audio, frame) >= 0)
                    {
                        // Drop pre-target frames. This is the audio-side twin
                        // of the video thread's `drop_before` filter and the
                        // mechanism Kodi and VLC use to make both streams
                        // start at the seek target rather than at the cluster
                        // the container's keyframe index landed on.
                        if (skip_until_pts >= 0.0 && frame->pts != AV_NOPTS_VALUE &&
                            audio_index >= 0)
                        {
                            const double t =
                                seconds_of(format->streams[audio_index]->time_base, frame->pts);
                            if (t >= 0.0 && t < skip_until_pts)
                            {
                                av_frame_unref(frame);
                                continue;
                            }
                            skip_until_pts = -1.0;
                        }
                        const int capacity = swr_get_out_samples(resample, frame->nb_samples);
                        if (capacity > 0)
                        {
                            pcm.resize(static_cast<std::size_t>(capacity) * 2u);
                            std::uint8_t *planes[1] = {
                                reinterpret_cast<std::uint8_t *>(pcm.data())};
                            const int made = swr_convert(resample, planes, capacity,
                                                         frame->extended_data, frame->nb_samples);
                            const float *at = pcm.data();
                            std::size_t left = made > 0 ? static_cast<std::size_t>(made) : 0u;
                            // Loudest sample in this rescaled chunk, for the
                            // controller light's brightness pulse. Peak, not
                            // RMS: a drum hit should move the lamp now, not a
                            // chunk later. ~2k floats per 20 ms of sound.
                            if (made > 0)
                            {
                                float peak = 0.0f;
                                const std::size_t n = static_cast<std::size_t>(made) * 2u;
                                for (std::size_t i = 0; i < n; ++i)
                                {
                                    const float a = std::fabs(pcm[i]);
                                    if (a > peak)
                                        peak = a;
                                }
                                audio_level_.store(peak, std::memory_order_relaxed);
                            }
                            // Publish this frame's file time as the post-seek
                            // audio anchor BEFORE its samples enter the ring.
                            // Doing it after `ring_->write` left a window where
                            // the mixer could pull the new content and drive
                            // `played()` past the hold's `played_at_seek`, so
                            // the video thread's commit saw the sentinel and
                            // fell back to `wanted` - the delta log then read
                            // +0 ms for every seek because there was no A0 to
                            // compare against, not because A0 truly equals
                            // wanted.
                            if (anchor_pending && frame->pts != AV_NOPTS_VALUE && audio_index >= 0)
                            {
                                const double A0 =
                                    seconds_of(format->streams[audio_index]->time_base, frame->pts);
                                if (A0 >= 0.0)
                                {
                                    audio_anchor_pts_us_.store(static_cast<long long>(A0 * 1.0e6),
                                                               std::memory_order_relaxed);
                                }
                                anchor_pending = false;
                            }
                            while (left > 0 && keep_running_.load())
                            {
                                std::size_t wrote = 0;
                                {
                                    std::lock_guard<std::mutex> lock(ring_mutex_);
                                    if (ring_ != nullptr)
                                    {
                                        wrote = ring_->write(at, left);
                                    }
                                }
                                at += wrote * 2;
                                left -= wrote;
                                if (wrote == 0)
                                {
                                    // The ring is full, or the film is paused.
                                    // Either way waiting here costs nothing but
                                    // this thread.
                                    wait_us(5000);
                                }
                            }
                        }
                        av_frame_unref(frame);
                    }
                }
                av_frame_free(&frame);
                audio_packets.finish(!keep_running_.load());
            };
            audio_running = start_thread(&audio_thread, audio_loop, 1u * 1024u * 1024u);
        }
        if (!audio_running && audio != nullptr)
        {
            // Losing the sound is not a reason to lose the film: the demuxer
            // stops queueing audio, and the picture plays on silently.
            sys::log("[HUI] video audio thread refused; playing silent");
            audio_disabled.store(true);
            audio_packets.finish(true);
            avcodec_free_context(&audio);
            swr_free(&resample);
        }

        for (;;)
        {
            if (!keep_running_.load())
            {
                break;
            }

            // Settle-based seek fire: after the last FF/RW press has sat
            // undisturbed for 400 ms, queue the real seek. This runs at the
            // top of the video main loop so it is checked every iteration
            // even while paused_, which is exactly the state we entered on
            // the first press; without this, the paused_ gate below would
            // keep us from ever firing the settled seek.
            {
                const std::int64_t deadline =
                    pending_seek_deadline_us_.load(std::memory_order_relaxed);
                if (deadline > 0 && sys::monotonic_us() >= deadline)
                {
                    const int settled =
                        pending_seek_target_.exchange(-1, std::memory_order_relaxed);
                    pending_seek_deadline_us_.store(0, std::memory_order_relaxed);
                    if (settled >= 0)
                    {
                        seek_request_.store(settled, std::memory_order_relaxed);
                        sys::log("[HUI] seek settle fire target=%ds", settled);
                    }
                }
            }

            // The demux thread performs the seek, because the format context
            // is its to touch. This is the decoder catching up with it: its own
            // buffers, its own held picture, its own queue.
            const int wanted = seek_applied.exchange(-1);
            if (wanted >= 0)
            {
                {
                    if (video != nullptr)
                    {
                        avcodec_flush_buffers(video);
                    }
                    if (hw != nullptr)
                    {
                        // A flushed decoder cannot reuse its old parameter
                        // sets, so the hardware path rebuilds itself here.
                        hw->flush();
                    }
                    // The held picture belongs to the moment we just left.
                    held.used = false;
                    // Drop the sound buffered before the jump, and re-anchor
                    // the clock to the target.
                    //
                    // This used to build a new ring and delete the previous
                    // one, which is a use-after-free waiting for a fast
                    // forward: holding the skip button issues seeks back to
                    // back, and the mixer's swap lands on its command queue a
                    // few grains later - so the ring it is still reading is
                    // the one the next seek deletes. Draining the ring in place
                    // has neither problem, and it is what the reference
                    // implementation's do_seek does (`pcm_.clear()`).
                    {
                        std::lock_guard<std::mutex> ring_lock(ring_mutex_);
                        if (ring_ != nullptr)
                        {
                            ring_->drain();
                        }
                    }
                    // `played()` counts frames ever played and does not reset
                    // when the ring is drained, so the base moves instead: the
                    // clock reads `wanted` right now and advances with the
                    // sound from here.
                    const double played_s = ring_ != nullptr
                                                ? static_cast<double>(ring_->played()) /
                                                      static_cast<double>(audio::kSampleRate)
                                                : 0.0;
                    clock_base = static_cast<double>(wanted) - played_s;
                    clock_anchored = true;
                    position_.store(wanted);
                    position_ms_.store(wanted * 1000);
                    // Pictures decoded from the keyframe before the target are
                    // not shown. See `drop_before`.
                    drop_before = static_cast<double>(wanted);
                    // Pictures decoded before the jump belong to a moment the
                    // film no longer occupies; showing them after the seek is
                    // the same stale-frame trap as opening a second film.
                    std::lock_guard<std::mutex> frame_lock(frame_mutex_);
                    while (!pending_.empty())
                    {
                        recycle_locked(std::move(pending_.front().pixels));
                        pending_.pop_front();
                    }
                    // There is room now, and the worker may be waiting for it.
                    frame_cv_.notify_all();
                    // Enter the hold only when there is an audio track waiting
                    // to resume. A silent file has nothing to wait for, and a
                    // completed track already fell back to the wall clock.
                    if (audio != nullptr && !audio_finished_.load(std::memory_order_acquire))
                    {
                        seek_awaiting_audio = true;
                        played_at_seek = ring_ != nullptr ? ring_->played() : 0;
                        seek_started_us = sys::monotonic_us();
                        held_wanted = wanted;
                        // The audio thread owns the anchor-reset path (see
                        // catch_up): resetting here raced with an audio thread
                        // that had already published A0 for this serial.
                    }
                    // Settle-debounce resume: the seek that just landed came
                    // from the video loop's deadline fire (see the head of
                    // this loop), so the player was paused on the first FF
                    // press. Clear the paused gate here so the video loop can
                    // decode the post-seek picture against the still-frozen
                    // clock - but do NOT un-pause the mixer yet when we are
                    // holding for the first picture: starting sound here is
                    // what let audio run ahead of a still-freezing frame. The
                    // hold block resumes the mixer the instant that first
                    // picture is queued, so sound and picture walk from the
                    // same tick at the target the bar is pointing at.
                    if (seek_debounce_paused_.exchange(false, std::memory_order_relaxed))
                    {
                        paused_.store(false, std::memory_order_release);
                        state_.store(static_cast<int>(PlayState::playing));
                        if (!seek_awaiting_audio && mixer_ != nullptr)
                        {
                            mixer_->set_stream_paused(kSlot, false);
                        }
                        sys::log("[HUI] seek settle applied target=%ds hold=%d", wanted,
                                 seek_awaiting_audio ? 1 : 0);
                    }
                }
            }

            // Hold the resume until the first post-seek picture is actually
            // decoded and queued. The mixer is still paused, so `played()`
            // sits still and the media clock stays frozen at `wanted`: a
            // picture decoded now is scheduled against that frozen clock, and
            // only the frame at `wanted` becomes due the moment sound starts.
            // We keep decoding (fall through, no skip) rather than spin on a
            // timer - the freeze a forward seek used to show *was* this loop
            // idling while audio already played. Readiness is the event; its
            // cost is content/codec dependent, so it is measured, never timed.
            if (seek_awaiting_audio)
            {
                const std::int64_t waited_us = sys::monotonic_us() - seek_started_us;
                bool video_ready = false;
                {
                    std::lock_guard<std::mutex> frame_lock(frame_mutex_);
                    // `drop_before` already discarded the pre-target GOP, so any
                    // picture still queued here belongs at or after the target.
                    video_ready = !pending_.empty();
                }
                if (video_ready)
                {
                    // Where the audio thread's first new-content sample actually
                    // sits in the file. Anchor the media clock to A0 whenever the
                    // atomic was published; holding the mixer a little longer here
                    // usually lets the audio thread get that publish in first.
                    const long long a0_us = audio_anchor_pts_us_.load(std::memory_order_relaxed);
                    double A0 = static_cast<double>(held_wanted);
                    bool anchored = false;
                    if (a0_us != INT64_MIN)
                    {
                        const double candidate = static_cast<double>(a0_us) / 1.0e6;
                        A0 = candidate;
                        anchored = true;
                        clock_base = A0 - static_cast<double>(played_at_seek) /
                                              static_cast<double>(audio::kSampleRate);
                        clock_anchored = true;
                    }
                    sys::log("[HUI] seek commit waited=%lldms target=%ds A0=%.3fs delta=%+lldms "
                             "anchored=%d a0_raw=%lld",
                             static_cast<long long>(waited_us / 1000), held_wanted, A0,
                             static_cast<long long>((A0 - held_wanted) * 1000.0), anchored ? 1 : 0,
                             a0_us);
                    // Sound starts on this tick - the same tick the first picture
                    // is already queued - so audio never leads a freezing frame.
                    if (mixer_ != nullptr)
                    {
                        mixer_->set_stream_paused(kSlot, false);
                    }
                    seek_awaiting_audio = false;
                }
                else if (waited_us > 3'000'000)
                {
                    sys::log("[HUI] seek timeout no-video target=%ds waited=%lldms", held_wanted,
                             static_cast<long long>(waited_us / 1000));
                    if (mixer_ != nullptr)
                    {
                        mixer_->set_stream_paused(kSlot, false);
                    }
                    seek_awaiting_audio = false;
                }
                // Otherwise fall through and keep decoding toward the first
                // picture; there is nothing to wait on but the decoder itself.
            }

            if (paused_.load())
            {
                wait_us(20000);
                continue;
            }

            // The demux thread's packets, in the order they were read. A slow
            // read now costs queue depth, not picture.
            AVPacket *incoming = nullptr;
            const int got = packets.get(&incoming, 50);
            if (got == 0)
            {
                continue; // nothing ready yet
            }
            if (got < 0)
            {
                // End of the file, or the film was stopped.
                if (hw != nullptr)
                {
                    // Ask the hardware for the pictures it still holds, so the
                    // last second is not skipped.
                    hw->send(nullptr, 0, INT64_MIN);
                    drain_hw();
                    release_hw();
                }
                break;
            }
            // This thread owns the packet now: the queue handed over its only
            // reference, and `av_packet_free` below is what retires it.
            AVPacket *packet = incoming;

            if (packet->stream_index == video_index)
            {
                if (ictcp_.load(std::memory_order_relaxed))
                {
                    if (dovi == nullptr)
                    {
                        dovi.reset(new (std::nothrow) DoviRpu());
                        // Annex B puts a start code in front of every NAL;
                        // MP4 stores a length instead. Deciding once, from the
                        // first video packet, keeps the walk below simple.
                        const std::uint8_t *head = packet->data;
                        const int size = packet->size;
                        if (size >= 4 && head[0] == 0 && head[1] == 0 &&
                            (head[2] == 1 || (head[2] == 0 && head[3] == 1)))
                        {
                            dovi_annex_b = 1;
                        }
                        else if (size >= 4 && ((static_cast<int>(head[0]) << 24) |
                                               (static_cast<int>(head[1]) << 16) |
                                               (static_cast<int>(head[2]) << 8) |
                                               static_cast<int>(head[3])) <= size)
                        {
                            dovi_annex_b = 0;
                        }
                        else
                        {
                            dovi_annex_b = 1; // start codes are the safe guess
                        }
                    }
                    if (dovi != nullptr)
                    {
                        if (!dovi_reported && packet->size >= 8 && dovi->probe_packets() == 0)
                        {
                            sys::log("[HUI] dovi first pkt bytes %02x %02x %02x %02x %02x %02x "
                                     "%02x %02x annexb=%d",
                                     packet->data[0], packet->data[1], packet->data[2],
                                     packet->data[3], packet->data[4], packet->data[5],
                                     packet->data[6], packet->data[7], dovi_annex_b);
                        }
                        dovi->parse_packet(packet->data, static_cast<std::size_t>(packet->size),
                                           dovi_annex_b != 0);
                        if (!dovi_probed && dovi->probe_packets() >= 30)
                        {
                            dovi_probed = true;
                            char probe[400];
                            dovi->stats(probe, sizeof(probe));
                            sys::log("[HUI] %s", probe);
                        }
                        if (dovi->ready())
                        {
                            // The reshaping curve is per scene, so it changes a
                            // handful of times a film. Rebuild the lookup only
                            // when the numbers actually differ - and publish it
                            // for the render thread, which owns the texture.
                            const DoviFrame &curve = dovi->frame();
                            std::uint32_t signature =
                                static_cast<std::uint32_t>(curve.coef_log2_denom) * 131u +
                                static_cast<std::uint32_t>(curve.full_range ? 1 : 0);
                            // All three components, not only luma: some RPUs
                            // move only a chroma curve, and a stale table for
                            // those leaves a colour cast until the next scene.
                            for (int c = 0; c < 3; ++c)
                            {
                                signature = signature * 31u +
                                            static_cast<std::uint32_t>(curve.num_pivots[c]);
                                for (int i = 0; i < curve.num_pivots[c] && i < 9; ++i)
                                {
                                    signature = signature * 31u + curve.pivots[c][i];
                                }
                                for (int i = 0; i + 1 < curve.num_pivots[c] && i < 8; ++i)
                                {
                                    for (int k = 0; k < 3; ++k)
                                    {
                                        signature = signature * 31u +
                                                    static_cast<std::uint32_t>(curve.coef[c][i][k]);
                                    }
                                }
                            }
                            signature = signature * 31u + (curve.has_colour_step ? 1u : 0u);
                            // The colour step is part of the same publish, so it
                            // is hashed too: an RPU that only changes the
                            // matrices must still reach the GPU.
                            for (float value : curve.lms_from_ycc)
                            {
                                std::uint32_t bits = 0;
                                std::memcpy(&bits, &value, sizeof(bits));
                                signature = signature * 31u + bits;
                            }
                            for (float value : curve.rgb_from_lms)
                            {
                                std::uint32_t bits = 0;
                                std::memcpy(&bits, &value, sizeof(bits));
                                signature = signature * 31u + bits;
                            }
                            for (float value : curve.ycc_to_rgb_offset)
                            {
                                std::uint32_t bits = 0;
                                std::memcpy(&bits, &value, sizeof(bits));
                                signature = signature * 31u + bits;
                            }
                            if (signature != dovi_signature)
                            {
                                dovi_signature = signature;
                                // Presentation pts of the packet carrying this
                                // curve, so the render thread can hold it back
                                // until the picture it belongs to is shown
                                // instead of tinting the frames still queued.
                                const std::int64_t curve_pts_us =
                                    packet->pts == AV_NOPTS_VALUE
                                        ? INT64_MIN
                                        : av_rescale_q(packet->pts,
                                                       format->streams[video_index]->time_base,
                                                       AVRational{1, 1000000});
                                // Luma and both chroma components get the RPU's
                                // own inverse-reshape curves. Chroma is not a
                                // pass-through: Dolby's Ct/Cp curves have their
                                // own gain, and the colour matrix's 0.5 offset
                                // is in the reshaped domain. Where a curve is
                                // missing, identity keeps neutral sane.
                                std::array<float, 1024 * 3> table{};
                                const bool have_table =
                                    dovi_reshape_lut(curve, 0, table.data(), 1024);
                                if (!dovi_reshape_lut(curve, 1, table.data() + 1024, 1024))
                                {
                                    for (int i = 0; i < 1024; ++i)
                                        table[1024 + i] = static_cast<float>(i) / 1023.0f;
                                }
                                if (!dovi_reshape_lut(curve, 2, table.data() + 2048, 1024))
                                {
                                    for (int i = 0; i < 1024; ++i)
                                        table[2048 + i] = static_cast<float>(i) / 1023.0f;
                                }
                                std::lock_guard<std::mutex> lock(frame_mutex_);
                                dovi_lut_valid_ = have_table;
                                for (float &value : dovi_lut_)
                                    value = 0.0f;
                                if (have_table)
                                {
                                    dovi_lut_ = table;
                                }
                                dovi_colour_valid_ = curve.has_colour_step;
                                if (dovi_colour_valid_)
                                {
                                    for (int i = 0; i < 9; ++i)
                                    {
                                        dovi_from_ycc_[static_cast<std::size_t>(i)] =
                                            curve.lms_from_ycc[i];
                                        dovi_to_rgb_[static_cast<std::size_t>(i)] =
                                            curve.rgb_from_lms[i];
                                    }
                                    for (int i = 0; i < 3; ++i)
                                    {
                                        dovi_offset_[static_cast<std::size_t>(i)] =
                                            curve.ycc_to_rgb_offset[i];
                                    }
                                }
                                const std::uint32_t new_version =
                                    dovi_lut_version_.fetch_add(1, std::memory_order_relaxed) + 1;
                                CurveStamp stamp;
                                stamp.pts_us = curve_pts_us;
                                stamp.version = new_version;
                                stamp.lut = table;
                                stamp.lut_valid = have_table;
                                stamp.colour_valid = dovi_colour_valid_;
                                if (dovi_colour_valid_)
                                {
                                    stamp.from_ycc = dovi_from_ycc_;
                                    stamp.offset = dovi_offset_;
                                    stamp.to_rgb = dovi_to_rgb_;
                                }
                                dovi_curve_.push_back(std::move(stamp));
                                while (dovi_curve_.size() > kMaxCurveStamps)
                                {
                                    dovi_curve_.pop_front();
                                }
                                sys::log("[HUI] dv curve push v=%u pkt_pts=%lld table=%d",
                                         new_version, static_cast<long long>(curve_pts_us),
                                         static_cast<int>(dovi_curve_.size()));
                            }
                        }
                        if (!dovi_reported && dovi->ready())
                        {
                            dovi_reported = true;
                            char line[256];
                            dovi->describe(line, sizeof(line));
                            sys::log("[HUI] %s annexb=%d", line, dovi_annex_b);
                            const DoviFrame &f = dovi->frame();
                            for (int c = 0; c < 3; ++c)
                            {
                                sys::log("[HUI] rpu curve%d pivots=%d %d %d %d %d order=%d coef0="
                                         "%lld,%lld,%lld",
                                         c, f.pivots[c][0], f.pivots[c][1], f.pivots[c][2],
                                         f.pivots[c][3], f.pivots[c][4], f.order[c][0],
                                         static_cast<long long>(f.coef[c][0][0]),
                                         static_cast<long long>(f.coef[c][0][1]),
                                         static_cast<long long>(f.coef[c][0][2]));
                            }
                            if (f.has_ycc_to_rgb)
                            {
                                sys::log("[HUI] rpu ycc2rgb %.4f %.4f %.4f / %.4f %.4f %.4f / "
                                         "%.4f %.4f %.4f off %.4f %.4f %.4f",
                                         f.ycc_to_rgb[0], f.ycc_to_rgb[1], f.ycc_to_rgb[2],
                                         f.ycc_to_rgb[3], f.ycc_to_rgb[4], f.ycc_to_rgb[5],
                                         f.ycc_to_rgb[6], f.ycc_to_rgb[7], f.ycc_to_rgb[8],
                                         f.ycc_to_rgb_offset[0], f.ycc_to_rgb_offset[1],
                                         f.ycc_to_rgb_offset[2]);
                            }
                            if (f.has_rgb_to_lms)
                            {
                                sys::log("[HUI] rpu rgb2lms %.4f %.4f %.4f / %.4f %.4f %.4f / "
                                         "%.4f %.4f %.4f",
                                         f.rgb_to_lms[0], f.rgb_to_lms[1], f.rgb_to_lms[2],
                                         f.rgb_to_lms[3], f.rgb_to_lms[4], f.rgb_to_lms[5],
                                         f.rgb_to_lms[6], f.rgb_to_lms[7], f.rgb_to_lms[8]);
                            }
                        }
                    }
                }
                if (hw != nullptr)
                {
                    // The hardware decoder runs its own bitstream filter, so
                    // it takes the access unit as it sits in the file. It
                    // answers 1 when a picture is due first.
                    const std::int64_t pts_us =
                        packet->pts == AV_NOPTS_VALUE
                            ? INT64_MIN
                            : av_rescale_q(packet->pts, format->streams[video_index]->time_base,
                                           AVRational{1, 1000000});
                    bool fell_back = false;
                    for (;;)
                    {
                        const int sent = hw->send(packet->data, packet->size, pts_us);
                        if (sent == 1)
                        {
                            drain_hw();
                            continue;
                        }
                        if (sent < 0)
                        {
                            // The hardware gave up mid-film. Rather than end
                            // the film, reopen the software decoder where the
                            // sound is and carry on: slower, but it plays.
                            sys::log("[HUI] hwdec failed mid-film; software takes over");
                            delete hw;
                            hw = nullptr;
                            // The held picture points into the pool that just
                            // went away with the decoder.
                            held.used = false;
                            if (!open_software())
                            {
                                fail("硬件与软件解码都失败");
                                goto cleanup;
                            }
                            // The format context belongs to the demux thread,
                            // so the jump is requested rather than performed:
                            // it seeks, flushes its queue, and reports back
                            // through `seek_applied`, whose handler flushes the
                            // fresh software decoder and re-anchors the clock.
                            seek_request_.store(position());
                            fell_back = true;
                        }
                        break;
                    }
                    if (!fell_back)
                    {
                        drain_hw();
                    }
                }
                else if (video != nullptr && avcodec_send_packet(video, packet) >= 0)
                {
                    while (keep_running_.load() && avcodec_receive_frame(video, decoded) >= 0)
                    {
                        emit_picture(decoded, false,
                                     seconds_of(format->streams[video_index]->time_base,
                                                decoded->best_effort_timestamp));
                        av_frame_unref(decoded);
                    }
                }
            }
            // The packet belongs to this thread now; the queue handed over its
            // only reference.
            av_packet_free(&packet);

            const bool audio_live =
                audio != nullptr && !audio_finished_.load(std::memory_order_acquire);
            const double playhead =
                audio_live
                    ? clock_base + static_cast<double>(ring_->played()) /
                                       static_cast<double>(audio::kSampleRate)
                    : clock_base + static_cast<double>(sys::monotonic_us() - started_us) / 1.0e6;
            position_.store(static_cast<int>(playhead));
            // Subtitles need sub-second accuracy; the OSD and resume points
            // stay in whole seconds.
            position_ms_.store(static_cast<int>(playhead * 1000.0));

            // One line every few seconds: has the decoder seen frames, how
            // many the clock threw away, how many the scaler refused. This is
            // the line that separates "no pictures decoded" from "decoded but
            // dropped" when the screen shows sound and nothing else.
            {
                const std::int64_t now_us = sys::monotonic_us();
                if (now_us - last_stats_us > 3000000)
                {
                    last_stats_us = now_us;
                    std::size_t queued;
                    {
                        std::lock_guard<std::mutex> lock(frame_mutex_);
                        queued = pending_.size();
                    }
                    // The heap is on the line because a 4K picture is 18 MB of
                    // planes and the queue holds several: a crash mid-film is
                    // an exhausted heap far more often than a decoder fault,
                    // and `failures` says so outright.
                    std::size_t heap_live = 0, heap_peak = 0, heap_blocks = 0, heap_fails = 0;
                    hui_heap_stats(&heap_live, &heap_peak, &heap_blocks, &heap_fails);
                    // Windowed mean signed A/V offset at show time (ms), then
                    // reset so each line reports the last few seconds, not the
                    // whole film. Positive = picture shown after its sound.
                    const long long sync_n = sync_count_.exchange(0, std::memory_order_relaxed);
                    const long long sync_sum = sync_sum_ms_.exchange(0, std::memory_order_relaxed);
                    const long long sync_ms = sync_n > 0 ? sync_sum / sync_n : 0;
                    sys::log("[HUI] video stats seen=%lld scaled=%lld late=%lld bad=%lld "
                             "queued=%zu aq=%zu pos=%d played=%zu pack_us=%d up_us=%d "
                             "read_us=%d/%d resets=%lld sync=%lldms/%lld heap=%zu/%zuMB fails=%zu",
                             seen_frames, scaled_frames, late_drops, scale_fails, queued,
                             audio_packets.held_bytes(), position(),
                             ring_ != nullptr ? ring_->played() : 0u,
                             pack_us_.load(std::memory_order_relaxed),
                             upload_us_.load(std::memory_order_relaxed),
                             read_us_shared.load(std::memory_order_relaxed),
                             read_us_peak.load(std::memory_order_relaxed), clock_resets, sync_ms,
                             sync_n, heap_live >> 20, hui_heap_capacity() >> 20, heap_fails);
                    read_us_peak.store(0, std::memory_order_relaxed);
                }
            }
        }
    }

    if (keep_running_.load())
    {
        // The file ended on its own rather than being stopped.
        state_.store(static_cast<int>(PlayState::ended));
    }

cleanup:
    // Playback is over (natural end or stop). The reader may now be parked in
    // the tail waiting to service a seek rather than having returned at EOF, so
    // clear keep_running_ here: av_interrupt() reads it to abort any in-flight
    // av_read_frame and the loop's top check breaks, letting the join below
    // succeed instead of hanging on a reader that stays alive past EOF.
    keep_running_.store(false);
    // The reader has to be gone before the format context it reads from is
    // freed, and before the queue holding its packets dies. `finish(true)`
    // wakes it out of a `get()` on the decode side and out of the byte-budget
    // wait on its own, so the join cannot hang.
    if (demux_running)
    {
        packets.finish(true);
        audio_packets.finish(true);
        pthread_join(demux_thread, nullptr);
        demux_running = false;
    }
    // The audio thread owns `audio`, `resample` and the ring, so it goes
    // before any of them are freed.
    if (audio_running)
    {
        audio_packets.finish(true);
        pthread_join(audio_thread, nullptr);
        audio_running = false;
    }
    packets.flush();
    audio_packets.flush();
    // Give the pictures back before the next film asks for its own. Whatever
    // was still queued or parked when the film stopped is 18 MB apiece, and
    // nothing else ever clears it: the queue is emptied on a seek and on the
    // render thread's schedule, neither of which happens after the last frame.
    // Measured: 156 MB still live with no film playing, and the next film died
    // opening because of it.
    {
        std::lock_guard<std::mutex> frame_lock(frame_mutex_);
        while (!pending_.empty())
        {
            pending_.pop_front();
        }
        spare_.clear();
    }
    if (scaler != nullptr)
    {
        sws_freeContext(scaler);
    }
    if (decoded != nullptr)
    {
        av_frame_free(&decoded);
    }
    delete hw;
    hw = nullptr;
    if (hw_frame != nullptr)
    {
        av_frame_free(&hw_frame);
    }
    if (resample != nullptr)
    {
        swr_free(&resample);
    }
    if (audio != nullptr)
    {
        avcodec_free_context(&audio);
    }
    if (subtitle != nullptr)
    {
        avcodec_free_context(&subtitle);
    }
    if (video != nullptr)
    {
        avcodec_free_context(&video);
    }
    avformat_close_input(&format);
    keep_running_.store(false);
}

void Player::recycle_locked(std::vector<std::uint8_t> &&pixels)
{
    if (pixels.empty() || spare_.size() >= kMaxQueued)
    {
        return;
    }
    std::size_t held = 0;
    for (const std::vector<std::uint8_t> &spare : spare_)
    {
        held += spare.size();
    }
    // A 4K buffer is too big to keep just in case; letting it go means the next
    // frame allocates, which is cheaper than holding 200 MB of idle pixels.
    if (held + pixels.size() > spare_budget())
    {
        return;
    }
    spare_.push_back(std::move(pixels));
}

namespace
{

// Mean colour of one decoded picture, packed 0xRRGGBB and blended toward the
// colour the light already shows, so a cut slides the bar between hues instead
// of snapping to them. The planes go up to the shader as they are; this reads
// the same planes sparsely - every 8th luma column and row, every 4th of the
// half-size chroma (about 65k reads for 1080p, a rounding error next to the
// upload beside it) - and turns the mean Y'CbCr into an approximate RGB with
// the BT.709 limited-range matrix. An ambient light has no pixels to be wrong
// about, so the range and matrix of the film itself are not consulted. ICtCp
// planes (Dolby Vision Profile 5) are not Y'CbCr at all; the caller keeps
// those pictures at 0 = "no colour" and the bar holds the design's accent.
std::uint32_t picture_tint(const std::uint8_t *y, const std::uint8_t *u, const std::uint8_t *v,
                           int y_pitch, int c_pitch, int v_pitch, int width, int height,
                           bool ten_bit, bool ten_bit_low, bool planar, std::uint32_t previous)
{
    // One stored sample as an 8-bit-equivalent value: the console's decoder
    // leaves its ten bits low-aligned, FFmpeg's P010 high-aligned (see Frame).
    const auto shrink = [ten_bit, ten_bit_low](std::uint32_t value) -> double
    {
        if (!ten_bit)
            return static_cast<double>(value);
        return static_cast<double>(ten_bit_low ? (value >> 2u) : (value >> 8u));
    };
    const auto clamp8 = [](double value) -> double
    { return value < 0.0 ? 0.0 : (value > 255.0 ? 255.0 : value); };
    double sum_y = 0.0, sum_u = 0.0, sum_v = 0.0;
    double n_y = 0.0, n_c = 0.0;
    for (int row = 0; row < height; row += 8)
    {
        const std::uint8_t *plane = y + static_cast<std::size_t>(row) * y_pitch;
        if (ten_bit)
        {
            const std::uint16_t *p = reinterpret_cast<const std::uint16_t *>(plane);
            for (int x = 0; x < width; x += 8)
            {
                sum_y += shrink(p[x]);
                ++n_y;
            }
        }
        else
        {
            for (int x = 0; x < width; x += 8)
            {
                sum_y += plane[x];
                ++n_y;
            }
        }
    }
    const int cwidth = width / 2;
    const int cheight = height / 2;
    for (int row = 0; row < cheight; row += 4)
    {
        const std::uint8_t *pu = u + static_cast<std::size_t>(row) * c_pitch;
        const std::uint8_t *pv = planar ? v + static_cast<std::size_t>(row) * v_pitch : nullptr;
        for (int x = 0; x < cwidth; x += 4)
        {
            if (planar)
            {
                // FFmpeg's planar output: one U plane, one V plane.
                if (ten_bit)
                {
                    sum_u += shrink(reinterpret_cast<const std::uint16_t *>(pu)[x]);
                    sum_v += shrink(reinterpret_cast<const std::uint16_t *>(pv)[x]);
                }
                else
                {
                    sum_u += pu[x];
                    sum_v += pv[x];
                }
            }
            else
            {
                // Interleaved UV (NV12/P010): Cb Cr Cb Cr ...
                if (ten_bit)
                {
                    const std::uint16_t *p = reinterpret_cast<const std::uint16_t *>(pu);
                    sum_u += shrink(p[x * 2]);
                    sum_v += shrink(p[x * 2 + 1]);
                }
                else
                {
                    sum_u += pu[x * 2];
                    sum_v += pu[x * 2 + 1];
                }
            }
            ++n_c;
        }
    }
    if (n_y <= 0.0 || n_c <= 0.0)
    {
        return previous;
    }
    const double yy = (sum_y / n_y - 16.0) * (255.0 / 219.0);
    const double cb = sum_u / n_c - 128.0;
    const double cr = sum_v / n_c - 128.0;
    const double r = clamp8(yy + 1.7928 * cr);
    const double g = clamp8(yy - 0.2132 * cb - 0.5329 * cr);
    const double b = clamp8(yy + 2.1124 * cb);
    // A quarter of each new picture into the colour already shown; the first
    // one (previous == 0) lands whole.
    const bool fresh_session = previous == 0u;
    const double mix = fresh_session ? 1.0 : 0.25;
    const double pr = fresh_session ? 0.0 : static_cast<double>((previous >> 16) & 0xFFu);
    const double pg = fresh_session ? 0.0 : static_cast<double>((previous >> 8) & 0xFFu);
    const double pb = fresh_session ? 0.0 : static_cast<double>(previous & 0xFFu);
    const auto pack = [clamp8](double value) -> std::uint32_t
    { return static_cast<std::uint32_t>(clamp8(value)); };
    return (pack(pr + (r - pr) * mix) << 16) | (pack(pg + (g - pg) * mix) << 8) |
           pack(pb + (b - pb) * mix);
}

} // namespace

std::uint32_t Player::frame(gfx::Renderer &renderer, int *width, int *height)
{
    bool fresh = false;
    bool curve_updated = false;
    std::uint64_t session = 0;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        session = session_;
        // Everything that has come due since the last call, oldest first; the
        // newest of them is the picture, and the ones it supersedes - frames
        // the render thread was late for - go back to the decode thread as
        // buffers. A frame whose moment has not arrived is left in the queue:
        // holding it is what keeps a burst of decoding from showing all at
        // once.
        Frame due;
        bool have_due = false;
        const std::int64_t now_us = sys::monotonic_us();
        // A picture whose moment falls between two swaps belongs to the one it
        // is nearest, not the one after it.
        //
        // Waiting for the later swap rounds every picture up. A film runs at
        // 24 fps and the panel at 60 Hz, so a picture is due every two and a
        // half refreshes; rounding that up to three shows the film at 20 fps,
        // the queue backs up, and the player then has to dump pictures in a
        // burst to catch the sound again. That is not an occasional hitch, it
        // is a stutter in every single second of playback, and it looks like a
        // decoder that cannot keep up while pack_us says it is idle.
        //
        // Half a refresh of lead turns the rule into round-to-nearest, and the
        // average lands back on two and a half: two refreshes, then three,
        // then two - which is what the panel has always done with film.
        constexpr std::int64_t kShowLeadUs = 8333;
        while (!pending_.empty() && pending_.front().show_by_us <= now_us + kShowLeadUs)
        {
            if (have_due)
            {
                recycle_locked(std::move(due.pixels));
            }
            due = std::move(pending_.front());
            pending_.pop_front();
            have_due = true;
        }
        if (have_due && due.pts_us > 0)
        {
            // Sample the true A/V error the instant this picture is shown: the
            // audio playhead minus the frame's own timestamp. A healthy stream
            // sits near zero; a container whose audio and video start times
            // differ leaves a constant offset here even though late= and
            // resets= stay at zero. Read and cleared by the video thread's stats.
            const long long offset_ms = position_ms_.load(std::memory_order_relaxed) -
                                        static_cast<long long>(due.pts_us / 1000);
            sync_sum_ms_.fetch_add(offset_ms, std::memory_order_relaxed);
            sync_count_.fetch_add(1, std::memory_order_relaxed);
        }
        if (have_due)
        {
            upload_.swap(due.pixels);
            upload_width_ = due.width;
            upload_height_ = due.height;
            upload_luma_ = due.luma_bytes;
            upload_chroma_ = due.chroma_bytes;
            upload_y_pitch_ = due.y_pitch;
            upload_c_pitch_ = due.c_pitch;
            upload_v_pitch_ = due.v_pitch;
            upload_ten_bit_ = due.ten_bit;
            upload_ten_bit_low_ = due.ten_bit_low_aligned;
            upload_planar_ = due.planar_chroma;
            recycle_locked(std::move(due.pixels));
            fresh = true;

            // Bind this picture to the curve that belongs to it: the newest
            // published RPU whose pts is not after the frame's own. A scene
            // change the demuxer has already run ahead to must not tint the
            // frames still queued behind it - that was the colour flash.
            const CurveStamp *picked = nullptr;
            const CurveStamp *earliest = nullptr;
            std::int64_t best_pts = INT64_MIN;
            std::int64_t earliest_pts = INT64_MAX;
            for (const CurveStamp &stamp : dovi_curve_)
            {
                if (stamp.pts_us <= due.pts_us && stamp.pts_us >= best_pts)
                {
                    best_pts = stamp.pts_us;
                    picked = &stamp;
                }
                if (stamp.pts_us < earliest_pts)
                {
                    earliest_pts = stamp.pts_us;
                    earliest = &stamp;
                }
            }
            if (picked == nullptr)
            {
                // The frame predates every known curve (or pts is absent): use
                // the earliest published one rather than the newest.
                picked = earliest;
            }
            const std::uint32_t want_version =
                picked != nullptr ? picked->version
                                  : dovi_lut_version_.load(std::memory_order_relaxed);
            if (want_version != upload_lut_version_)
            {
                // Diagnostic split: a scene-change flash is only visible when
                // the curve this picture swaps to actually moves colour (Ct/Cp)
                // or luma (Y), and it is only wrong when that curve's pts lags
                // the picture it is painted on. Log the per-band delta and the
                // pts lag so a colour flash can be told from a brightness flash
                // by numbers, not by eye. upload_lut_ still holds the previous
                // curve here, so the delta is against what was on screen.
                if (picked != nullptr)
                {
                    float d_y = 0.0f, d_c = 0.0f;
                    for (int i = 0; i < 1024; ++i)
                    {
                        float dy = picked->lut[i] - upload_lut_[i];
                        if (dy < 0.0f)
                            dy = -dy;
                        if (dy > d_y)
                            d_y = dy;
                        float tc = picked->lut[1024 + i] - upload_lut_[1024 + i];
                        if (tc < 0.0f)
                            tc = -tc;
                        if (tc > d_c)
                            d_c = tc;
                        float pc = picked->lut[2048 + i] - upload_lut_[2048 + i];
                        if (pc < 0.0f)
                            pc = -pc;
                        if (pc > d_c)
                            d_c = pc;
                    }
                    sys::log("[HUI] dv swap ver=%u->%u lag=%lldms dY=%.4f dC=%.4f",
                             upload_lut_version_, want_version,
                             static_cast<long long>((due.pts_us - picked->pts_us) / 1000), d_y,
                             d_c);
                }
                if (picked != nullptr)
                {
                    upload_lut_ = picked->lut;
                    upload_lut_valid_ = picked->lut_valid;
                    upload_from_ycc_ = picked->from_ycc;
                    upload_offset_ = picked->offset;
                    upload_to_rgb_ = picked->to_rgb;
                    upload_colour_valid_ = picked->colour_valid;
                }
                else
                {
                    // No curve table yet: fall back to the globals, which the
                    // decode thread keeps as the newest published curve.
                    upload_lut_ = dovi_lut_;
                    upload_lut_valid_ = dovi_lut_valid_;
                    upload_from_ycc_ = dovi_from_ycc_;
                    upload_offset_ = dovi_offset_;
                    upload_to_rgb_ = dovi_to_rgb_;
                    upload_colour_valid_ = dovi_colour_valid_;
                }
                upload_lut_version_ = want_version;
                curve_updated = true;
                sys::log("[HUI] dv curve use v=%u frame_pts=%lld picked_pts=%lld table=%d",
                         want_version, static_cast<long long>(due.pts_us),
                         picked != nullptr ? static_cast<long long>(picked->pts_us) : -1LL,
                         static_cast<int>(dovi_curve_.size()));
            }

            // What the viewer sees is the gap between pictures on screen, not
            // the cost of producing them. Measured here, once per picture.
            if (last_shown_us_ != 0)
            {
                const std::int64_t gap = now_us - last_shown_us_;
                const int bucket = gap < 20000    ? 0
                                   : gap < 30000  ? 1
                                   : gap < 45000  ? 2
                                   : gap < 60000  ? 3
                                   : gap < 90000  ? 4
                                   : gap < 500000 ? 5
                                                  : 6;
                ++shown_gap_hist_[bucket];
                if (++shown_gap_n_ >= 240)
                {
                    sys::log("[HUI] video shown gap ms: <20=%lld 20-30=%lld 30-45=%lld "
                             "45-60=%lld 60-90=%lld 90-500=%lld 500+=%lld",
                             shown_gap_hist_[0], shown_gap_hist_[1], shown_gap_hist_[2],
                             shown_gap_hist_[3], shown_gap_hist_[4], shown_gap_hist_[5],
                             shown_gap_hist_[6]);
                    for (long long &value : shown_gap_hist_)
                        value = 0;
                    shown_gap_n_ = 0;
                }
            }
            last_shown_us_ = now_us;
            // The decode thread may be waiting for this room.
            frame_cv_.notify_one();
        }
    }
    // A picture handed over under this session is what the session shows from
    // here on, state or not: the worker sets `playing` as soon as the header
    // is read, long before its first frame exists.
    if (fresh)
    {
        shown_session_ = session;
    }

    // While there is no live picture for this session - still opening the next
    // film, stopped, or the source gave up - drop any texture the previous
    // film left behind. Otherwise the design paints a frozen frame of whatever
    // played before, which is exactly the "stuck on the first video" glitch
    // when starting a second one. The texture is owned here and destroyed on
    // the render thread, which is where this method runs.
    const PlayState now = state();
    if (now == PlayState::opening || now == PlayState::idle || now == PlayState::failed ||
        shown_session_ != session)
    {
        if (texture_ != 0)
        {
            renderer.batch().destroy_video_texture();
            texture_ = 0;
            texture_width_ = 0;
            texture_height_ = 0;
        }
        if (width != nullptr)
        {
            *width = 0;
        }
        if (height != nullptr)
        {
            *height = 0;
        }
        return 0;
    }

    // Only a frame the worker actually produced is uploaded. The planes go up
    // as they are: no RGBA texture is built here any more, so a 4K picture is
    // one memcpy into the driver instead of an sws pass plus an upload.
    if (fresh && upload_width_ > 0 && upload_height_ > 0 && !upload_.empty())
    {
        const std::int64_t upload_started_us = sys::monotonic_us();
        if (uploads_ < 4)
        {
            sys::log("[HUI] video upload #%d %dx%d ten=%d low=%d planar=%d pitch=%d/%d", uploads_,
                     upload_width_, upload_height_, upload_ten_bit_ ? 1 : 0,
                     upload_ten_bit_low_ ? 1 : 0, upload_planar_ ? 1 : 0, upload_y_pitch_,
                     upload_c_pitch_);
        }
        ++uploads_;
        // The decoded planes are derived once here and then uploaded.
        const std::uint8_t *y = upload_.data();
        const std::uint8_t *u = y + upload_luma_;
        const std::uint8_t *v = upload_planar_ ? u + upload_chroma_ : nullptr;
        // The controller light's colour input, off the same planes going up.
        if (ictcp_.load(std::memory_order_relaxed))
        {
            video_light_.store(0, std::memory_order_relaxed);
        }
        else
        {
            video_light_.store(picture_tint(y, u, v, upload_y_pitch_, upload_c_pitch_,
                                            upload_v_pitch_, upload_width_, upload_height_,
                                            upload_ten_bit_, upload_ten_bit_low_, upload_planar_,
                                            video_light_.load(std::memory_order_relaxed)),
                               std::memory_order_relaxed);
        }
        // A new reshaping curve is only ever published alongside a picture, so
        // this is the one place the table has to be checked. The offset is the
        // RPU's own "input offset of neutral value" (0.5, 0.5 for profile 5)
        // and is applied after the reshape; it is not re-anchored on a measured
        // median, because a colourful picture's median chroma is not neutral
        // and doing that shifts the whole image.
        // The curve was already chosen per-frame (bound to this picture's pts)
        // and copied into upload_* under frame_mutex_ above; hand it to the GPU
        // only when it actually changed.
        if (curve_updated)
        {
            renderer.batch().set_dovi_reshape(upload_lut_valid_ ? upload_lut_.data() : nullptr,
                                              1024);
            renderer.batch().set_dovi_colour(upload_from_ycc_.data(), upload_offset_.data(),
                                             upload_to_rgb_.data(), upload_colour_valid_);
        }
        renderer.batch().set_video_colorspace(
            static_cast<gfx::GlBatch::VideoTransfer>(transfer_.load(std::memory_order_relaxed)),
            matrix_.load(std::memory_order_relaxed), full_range_.load(std::memory_order_relaxed),
            static_cast<float>(peak_nits_.load(std::memory_order_relaxed)),
            wide_gamut_.load(std::memory_order_relaxed), upload_ten_bit_low_,
            ictcp_.load(std::memory_order_relaxed));
        // Diagnostic split: separate the storage (re)allocation from the
        // per-plane texsubimage so a 100 ms upload can be attributed to one of
        // them instead of guessed at. Low frequency: the first few frames and
        // then a slice every 60th picture.
        const std::int64_t pre_create_us = sys::monotonic_us();
        renderer.batch().create_video_texture(upload_width_, upload_height_, upload_ten_bit_);
        const std::int64_t post_create_us = sys::monotonic_us();
        renderer.batch().update_video_texture(y, upload_y_pitch_, u, upload_c_pitch_, v,
                                              upload_v_pitch_, upload_planar_);
        const std::int64_t post_update_us = sys::monotonic_us();
        upload_us_.store(static_cast<int>(sys::monotonic_us() - upload_started_us),
                         std::memory_order_relaxed);
        if (uploads_ <= 6 || uploads_ % 60 == 0)
        {
            sys::log("[HUI] upload split #%d create=%lld update=%lld us %dx%d ten=%d pitch=%d",
                     uploads_, static_cast<long long>(post_create_us - pre_create_us),
                     static_cast<long long>(post_update_us - post_create_us), upload_width_,
                     upload_height_, upload_ten_bit_ ? 1 : 0, upload_y_pitch_);
        }
        texture_ = renderer.batch().video_texture();
        texture_width_ = upload_width_;
        texture_height_ = upload_height_;
    }

    if (width != nullptr)
    {
        *width = texture_width_;
    }
    if (height != nullptr)
    {
        *height = texture_height_;
    }
    return texture_;
}

#else // The PC preview host has no ffmpeg: the player keeps state for the
      // screens and never builds a ring, a thread or a texture.

void Player::attach(audio::Mixer &mixer)
{
    mixer_ = &mixer;
}

bool Player::start(const std::string &, const std::vector<std::string> &, int, int, int, bool)
{
    state_.store(static_cast<int>(PlayState::failed));
    std::lock_guard<std::mutex> lock(error_mutex_);
    error_ = "预览版不播放视频";
    return false;
}

void Player::pause()
{
    if (state() == PlayState::playing)
    {
        state_.store(static_cast<int>(PlayState::paused));
    }
}

void Player::resume()
{
    if (state() == PlayState::paused)
    {
        state_.store(static_cast<int>(PlayState::playing));
    }
}

void Player::stop()
{
    state_.store(static_cast<int>(PlayState::idle));
}

void Player::stop_locked()
{
    state_.store(static_cast<int>(PlayState::idle));
}

void Player::seek(int seconds)
{
    position_.store(std::max(0, seconds));
    position_ms_.store(std::max(0, seconds) * 1000);
}

void Player::set_gain(float)
{
}

std::vector<AudioTrack> Player::audio_tracks() const
{
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    return audio_tracks_;
}

std::vector<SubtitleTrack> Player::subtitle_tracks() const
{
    std::lock_guard<std::mutex> lock(tracks_mutex_);
    return subtitle_tracks_;
}

std::string Player::subtitle_at(int) const
{
    return std::string();
}

bool Player::subtitle_bitmap_at(int, SubtitleBitmap *) const
{
    // No ffmpeg on the preview host: no PGS/DVD bitmap is ever decoded, so the
    // overlay never lights up. The render path in Service::update stays happy.
    return false;
}

bool Player::set_external_subtitle(const std::string &, const std::string &)
{
    // No ffmpeg on the preview host: the button and menu still work, but an
    // external subtitle never draws.
    return false;
}

void Player::clear_external_subtitle()
{
}

void Player::set_frame_edge(int edge)
{
    // 0 means automatic (no cap): keep it so the decode loop uploads the frame
    // at its real resolution. Positive edges are clamped to the manual tiers.
    if (edge > 0)
        edge = std::min(kMaxFrameEdge, std::max(kMinFrameEdge, edge));
    frame_edge_.store(edge, std::memory_order_relaxed);
}

std::string Player::error_text() const
{
    std::lock_guard<std::mutex> lock(error_mutex_);
    return error_;
}

void Player::fail(const char *)
{
}

void Player::retarget()
{
}

void *Player::run(void *)
{
    return nullptr;
}

void Player::decode()
{
}

std::uint32_t Player::frame(gfx::Renderer &, int *width, int *height)
{
    if (width != nullptr)
    {
        *width = 0;
    }
    if (height != nullptr)
    {
        *height = 0;
    }
    return 0;
}

#endif

} // namespace hui::fnvideo

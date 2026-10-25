// ps5-homebrew-ui - One film or episode: ranged HTTP reads through ffmpeg's
// own http protocol, H.264/HEVC decoded to RGBA for a texture, and the audio
// track resampled to 48 kHz stereo float into a mixer stream.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two threads are involved. The decode thread owns ffmpeg and the network and
// never touches GL; it leaves the newest frame in a slot that the render
// thread picks up in frame(). The audio thread reads the ring the decode
// thread writes, and its play-out counter is the clock the video is paced by -
// the sound cannot drift away from itself, so that is the only timebase worth
// following.
//
// Only the console decodes. The PC preview host has no ffmpeg, so start()
// answers "no" there and the screens keep drawing their painted player.

#pragma once

#include "audio/mixer.hpp"
#include "audio/stream_ring.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <string>
#include <vector>
namespace hui::gfx
{
class Renderer;
}

namespace hui::fnvideo
{

enum class PlayState : int
{
    idle,
    opening, // probing the container and reading the header
    playing,
    paused,
    ended,  // the file reached its own end
    failed, // see error_text()
};

// One embedded audio stream, as the container names it. `ordinal` is the
// position among audio streams, not FFmpeg's absolute stream index: a reopened
// ticket can renumber streams, an ordinal stays meaningful across the reopen
// that switching tracks needs.
struct AudioTrack
{
    int ordinal = 0;
    std::string language;
    std::string title;
    int channels = 0;
};

// One embedded subtitle stream. Bitmap subtitles (PGS/DVD) are listed but
// cannot be drawn by the text renderer; `text` says which kind it is.
// An `external` track is not in the container at all: it is a text file the
// server lists beside the video, fetched whole by `ext_guid` and decoded with
// set_external_subtitle(). Its `ordinal` lives in a namespace above the
// container tracks so the two never collide.
struct SubtitleTrack
{
    int ordinal = 0;
    std::string language;
    std::string title;
    std::string codec;
    bool text = false;
    bool external = false;
    std::string ext_guid;   // subtitle guid for GET /subtitle/dl/{guid}
    std::string ext_format; // srt / ass / vtt, picks the demuxer
};

// One decoded text cue. The decode thread fills these while it demuxes; the
// render thread asks for the one covering the current second.
struct SubtitleCue
{
    int start_ms = 0;
    int end_ms = 0;
    std::string text;
};

// One decoded bitmap subtitle frame (PGS/DVD), already converted to RGBA8
// with straight alpha. x/y/w/h place the picture inside a canvas of
// canvas_w x canvas_h -- the subtitle stream's OWN frame size (the decoder
// context's width/height, which FFmpeg validates object x/y against). This
// is what Kodi/VLC/mpv scale against: the canvas is projected into the video's
// displayed rectangle, so a 1080p-authored PGS lines up on a 4K film instead
// of being stretched to fill the screen (the Kodi #14866 failure mode).
struct SubtitleBitmap
{
    int start_ms = 0;
    int end_ms = 0;
    int serial = 0; // bumped per decoded frame; the renderer re-uploads on change
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int canvas_w = 0;
    int canvas_h = 0;
    std::vector<std::uint8_t> rgba; // w*h*4, straight alpha, top row first
};

// The manual resolution tiers the player's toolbar offers, in pixels along the
// longest edge. 0 in frame_edge_ means automatic: the picture is presented at
// the source's own size, since the shader does the scaling now. The ceiling is
// the console decoder's own (3840), so a 4K film really is kept at 4K.
inline constexpr int kMinFrameEdge = 480;
inline constexpr int kMaxFrameEdge = 3840;

class Player
{
  public:
    Player();
    ~Player();
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    // Slot 2 of the mixer, next to the music bed and the music player's
    // stream, so the two designs never fight over a slot.
    void attach(audio::Mixer &mixer);

    // Opens `url` (Api::stream_url) with `headers` and starts at
    // `resume_seconds`. `audio_track` and `subtitle_track` are ordinals from
    // audio_tracks() / subtitle_tracks(); -1 means the container's best stream.
    // Asynchronous: state() reports opening, then playing or failed.
    bool start(const std::string &url, const std::vector<std::string> &headers, int resume_seconds,
               int audio_track = -1, int subtitle_track = -1, bool subtitle_auto = false);
    void pause();
    void resume();
    void stop();
    // Absolute seconds, or `seconds` ahead when negative.
    void seek(int seconds);
    // Linear stream gain, 0..1.5. Kept across start(), so a film opened after
    // the viewer set the volume comes up at that volume rather than at 100%.
    void set_gain(float gain);

    // What the container holds. Filled by the decode thread once the header is
    // read; empty while opening or when this player is not decoding.
    std::vector<AudioTrack> audio_tracks() const;
    std::vector<SubtitleTrack> subtitle_tracks() const;
    // The ordinal actually in use (the container's best stream when no track
    // was requested), or -1 before the header is read.
    int audio_track() const
    {
        return selected_audio_.load(std::memory_order_relaxed);
    }
    int subtitle_track() const
    {
        return selected_subtitle_.load(std::memory_order_relaxed);
    }
    // Empty when no text cue covers `position_ms`, including when the selected
    // stream is bitmap-only (PGS/DVD), which this text renderer cannot draw.
    std::string subtitle_at(int position_ms) const;

    // The bitmap subtitle frame covering position_ms, copied out under the
    // lock. Returns false when none is active. Text and bitmap streams are
    // mutually exclusive per selection, so a caller draws whichever answers.
    bool subtitle_bitmap_at(int position_ms, SubtitleBitmap *out) const;

    // Load a whole external subtitle file (the raw bytes of an SRT/ASS/VTT, a
    // leading UTF-8 BOM tolerated) as cues that play alongside the running
    // container, without reopening it. The bytes are demuxed in memory once and
    // every cue gets its real timestamp, so a seek still matches with no refetch.
    // Replaces any previous external subtitle; returns false when nothing parsed.
    bool set_external_subtitle(const std::string &bytes, const std::string &format);
    // Drop the external cues - used when the viewer picks a container track or
    // turns subtitles off (which reopens the container anyway).
    void clear_external_subtitle();

    PlayState state() const
    {
        return static_cast<PlayState>(state_.load(std::memory_order_relaxed));
    }
    // One short Chinese line, empty unless state() is failed.
    std::string error_text() const;
    int position() const
    {
        return position_.load(std::memory_order_relaxed);
    }
    // The same playhead in milliseconds, for subtitle cues. Whole seconds are
    // enough for the OSD and resume points, but a cue that starts at 10.5 s
    // would otherwise appear up to a second late.
    int position_millis() const
    {
        return position_ms_.load(std::memory_order_relaxed);
    }
    int duration() const
    {
        return duration_.load(std::memory_order_relaxed);
    }
    // True once the file ended by itself, so the page can back out.
    bool finished() const
    {
        return state() == PlayState::ended;
    }

    // Render thread, once per frame: the newest decoded picture, uploaded at
    // most once per call. 0 until the first frame arrives.
    std::uint32_t frame(gfx::Renderer &renderer, int *width, int *height);

    // The longest edge a decoded frame is kept at, as one of the tiers the
    // player's toolbar offers. Read on every picture, so a change applies from
    // the next frame - and a manual tier also shrinks the upload, which is the
    // point of it.
    void set_frame_edge(int edge);
    int frame_edge() const
    {
        return frame_edge_.load(std::memory_order_relaxed);
    }
    // 0 = software (FFmpeg), 1 = hardware (PS5 Videodec2). The hardware path is
    // being brought up; until then 1 still decodes in software and falls back to
    // it. Read once when a film opens, like frame_edge_.
    void set_decoder(int decoder)
    {
        decoder_.store(decoder, std::memory_order_relaxed);
    }
    int decoder() const
    {
        return decoder_.load(std::memory_order_relaxed);
    }

  private:
    static void *run(void *user);
    void decode();
    // The body of stop(), for a caller that already holds control_mutex_.
    void stop_locked();
    void fail(const char *error);
    // Moves the writer to a fresh ring, so samples buffered before a seek are
    // never heard.
    void retarget();

    static constexpr std::size_t kSlot = 2;
    // A film's audio is 48 kHz everywhere; 1<<16 frames is 1.4 s of cushion.
    static constexpr std::size_t kRingFrames = 1u << 16;
    // The longest edge a decoded frame is kept at, in pixels. 0 = 自动: the
    // frame is uploaded at its real resolution, up to the console decoder's
    // own 3840, so a 4K film passes through untouched and the shader does the
    // scaling to the panel. A positive value is the manual tier the player's
    // toolbar offers (4K / 2K / 1080p / 720p / 480p), also clamped by
    // kMaxFrameEdge.
    std::atomic<int> frame_edge_{0}; // 0 = automatic
    // 0 = software (FFmpeg), 1 = hardware (PS5 Videodec2). The hardware path
    // is being brought up (see the plan's G/L); until it is wired, either value
    // decodes in software, so the default (1) is a preference, not a promise.
    std::atomic<int> decoder_{1}; // 1 = hardware by default

    audio::Mixer *mixer_ = nullptr;
    audio::StreamRing *ring_ = nullptr;
    audio::StreamRing *last_ring_ = nullptr;
    // The decode thread swaps `ring_` on a seek; the audio thread writes into
    // whatever `ring_` is at that moment. Without this the writer can be left
    // holding a ring that the seek after next deletes.
    std::mutex ring_mutex_;

    // start() and stop() are called from two threads: the library worker
    // resolves the ticket, the frame the viewer presses 返回 stops it. Both
    // join the decode thread, so only one of them may be inside at a time.
    std::mutex control_mutex_;

    std::atomic<bool> keep_running_{false};
    std::atomic<bool> paused_{false};
    // Set once the audio stream has fully played out (queue eof, decoder
    // flushed, ring drained). A sound track shorter than the video would else
    // freeze the audio-driven master clock and strand the trailing pictures.
    std::atomic<bool> audio_finished_{false};
    std::atomic<int> seek_request_{-1};
    std::atomic<int> state_{static_cast<int>(PlayState::idle)};
    std::atomic<int> position_{0};
    std::atomic<int> position_ms_{0};
    // The signed A/V error sampled where a picture is presented: the audio
    // playhead minus that frame's own timestamp, in milliseconds. Averaged over
    // the stats window and logged as `sync=`, it exposes a constant offset that
    // the late-drop and re-anchor counters cannot - a drift under 0.3 s looks
    // perfectly stable yet reads as bad lip sync. Positive = video behind sound.
    std::atomic<long long> sync_sum_ms_{0};
    std::atomic<long long> sync_count_{0};
    // The file-time, in microseconds, of the first sample the audio thread
    // wrote into the ring after a seek. INT64_MIN is the sentinel meaning
    // "no push since the last serial bump". The video thread reads this on
    // the hold->commit edge so the media clock can be anchored to the audio
    // content's actual starting timestamp rather than to the requested
    // target: `avformat_seek_file` lands on a cluster boundary, and a target
    // that has to wait for the next keyframe can be several seconds away from
    // where the sound really resumes. Anchoring to `wanted` in that case
    // makes the picture lead the audio by the same gap until the >0.5 s
    // re-anchor catches it - the "sound is behind after FF" complaint.
    std::atomic<long long> audio_anchor_pts_us_{INT64_MIN};
    // Settle-based seek (the user's algorithm). The first FF/RW press pauses
    // the player and stores the target here; subsequent presses just move it
    // and reset the deadline. Once monotonic_us() passes the deadline the
    // video main loop fires one real `avformat_seek_file` at the settled
    // target, the seek_applied handler resumes playback, and both streams
    // come back in step at the position the progress bar points at.
    std::atomic<int> pending_seek_target_{-1};
    std::atomic<long long> pending_seek_deadline_us_{0};
    std::atomic<bool> seek_debounce_paused_{false};
    // Kodi/VLC sync model: audio is the master clock, and both streams drop
    // content whose pts is before the target. Video already does this via
    // `drop_before`. This is the same idea on the audio side: after a seek,
    // the audio thread discards decoded frames whose pts is less than this
    // value until it publishes the anchor, so the very first sample that
    // reaches the ring is at (or a few ms past) the target. Media clock is
    // anchored at the target, both streams start there, and the progress
    // bar and the user's eyes and ears agree. -1 disables the filter.
    std::atomic<double> audio_skip_until_seconds_{-1.0};
    std::atomic<int> duration_{0};
    // Microseconds the last picture cost on each side of the handoff: the
    // plane copy on the decode thread, the texture upload on the render
    // thread. A stutter is one of the two, and which one is not visible from
    // the frame counters alone.
    std::atomic<int> pack_us_{0};
    std::atomic<int> upload_us_{0};
    // How this film's planes become colours, decided once when it opens. Set
    // on the decode thread, read on the render thread when it uploads a
    // picture. The transfer is kept as an int so this header stays free of the
    // renderer's; 0 SDR, 1 PQ (HDR10), 2 HLG. The matrix is 0 BT.601, 1
    // BT.709, 2 BT.2020.
    std::atomic<int> matrix_{1};
    std::atomic<int> transfer_{0};
    std::atomic<bool> full_range_{false};
    std::atomic<int> peak_nits_{1000};
    std::atomic<bool> wide_gamut_{false};
    // Profile 5's planes are IPTPQc2 (ICtCp), not Y'CbCr; see the shader.
    std::atomic<bool> ictcp_{false};
    // Volume is a player property, not a per-film one: start() applies the same
    // gain every time, and the mixer fades to it.
    std::atomic<float> gain_{1.0f};
    // Selected tracks. Written before the worker starts and when the worker
    // reads the header; read by the toolbar on the render thread.
    std::atomic<int> selected_audio_{-1};
    std::atomic<int> selected_subtitle_{-1};
    // True while an external subtitle file is on screen. Its cues live in
    // external_cues_ and, with this set, are the only text drawn: a container
    // subtitle still decoding underneath is ignored, so choosing an external
    // file needs no reopen and never double-stacks with an embedded track.
    std::atomic<bool> external_active_{false};
    // Fresh playback of a new title (not a track-change reopen): with no
    // explicit track, auto-pick the first Chinese subtitle stream, the way
    // the official 飞牛影视 TV app matches tracks by subtitle language code.
    std::atomic<bool> subtitle_auto_{false};

    // The failure text is a short Chinese line, written once by the worker
    // before it exits and read by the frame that draws the banner.
    mutable std::mutex error_mutex_;
    std::string error_;
    // Enumerated once per open, under tracks_mutex_. Kept outside the console
    // block so the toolbar can ask on the host too, where the list is empty.
    mutable std::mutex tracks_mutex_;
    std::vector<AudioTrack> audio_tracks_;
    std::vector<SubtitleTrack> subtitle_tracks_;
    mutable std::mutex subtitle_mutex_;
    std::deque<SubtitleCue> subtitle_cues_;
    // Cues from an external subtitle file. Kept apart from subtitle_cues_ so a
    // seek (which clears the container cues the demuxer will refill) leaves the
    // one-shot external decode intact; subtitle_at() draws from both.
    std::deque<SubtitleCue> external_cues_;
    std::deque<SubtitleBitmap> subtitle_bitmaps_;
    int subtitle_serial_ = 0;

#ifdef HUI_PS5
    // Only the console decodes, so only the console needs a thread, a frame
    // handoff and a texture.
    // ffmpeg calls this while it opens and reads the stream; returning
    // non-zero makes a blocked avformat_open_input or av_read_frame abort, so
    // stop()'s pthread_join never wedges the single Service worker behind a
    // NAS that accepted the connection but stopped answering.
    static int av_interrupt(void *opaque);
    // Open-phase wall-clock deadline in monotonic_us; 0 means no time limit.
    // It bounds reconnect loops while a film opens; playback after that is
    // cancelled by keep_running_ alone.
    std::atomic<std::int64_t> interrupt_deadline_us_{0};

    pthread_t worker_{};
    bool worker_running_ = false;

    // The frame handoff: a short queue of decoded pictures, each tagged with
    // the wall-clock moment it is due on screen. One slot meant a frame the
    // render thread had not collected yet was overwritten, which showed as
    // judder whenever a frame took a little longer than the last; three is
    // enough to absorb that without ever holding more than about a tenth of a
    // second of picture. `spare_` gives the buffers back so neither side
    // allocates once the film has been running for a moment.
    //
    // A frame is the decoder's own planes, packed one after another: luma
    // first, then the chroma (interleaved UV, or a U plane and a V plane for
    // the software decoder's planar output). No scaling and no colour
    // conversion happens here any more - the shader reads the planes and does
    // both - which is what lets a 4K film be presented at 4K.
    struct Frame
    {
        std::vector<std::uint8_t> pixels;
        std::size_t luma_bytes = 0;   // where the chroma starts inside pixels
        std::size_t chroma_bytes = 0; // size of U (and of V, when planar)
        int y_pitch = 0;
        int c_pitch = 0;
        int v_pitch = 0;
        bool ten_bit = false;
        // Where the ten bits sit in their 16-bit words. The console's decoder
        // leaves them low-aligned (value in the low ten bits); FFmpeg's
        // P010LE is high-aligned (value << 6), while its planar
        // YUV420P10LE is low-aligned again. The shader needs to know: the
        // sample scale differs by 64x.
        bool ten_bit_low_aligned = false;
        bool planar_chroma = false;
        std::int64_t show_by_us = 0;
        // Presentation pts of this picture in microseconds, so the render
        // thread can bind it to the DV curve that belongs to it rather than
        // the newest one the demuxer has already run ahead to. 0 when the
        // container gave no pts.
        std::int64_t pts_us = 0;
        int width = 0;
        int height = 0;
    };
    // Four, like the reference implementation (kMaxFrames). It was two while
    // the heap was 256 MB and a 4K picture was 19 MB; now that the heap comes
    // out of direct memory and a picture is capped at the screen size, four
    // pictures cost about 20 MB and the cadence has room to breathe.
    static constexpr std::size_t kMaxQueued = 4;
    // A count is not enough of a cap: the planes of a 4K ten-bit picture are
    // 25 MB, so three queued plus the parked spares ran the sandbox heap out
    // of memory and took the title down. Both the queue and the spare pool are
    // capped by bytes too.
    //
    // The spare pool holds one 4K picture and no more. Holding two was tried
    // and it took the title down: a 4K ten-bit picture is 18.5 MB, and
    // queued + uploaded + spare together were over 100 MB of heap alongside
    // the 22 MB Han atlas. One is enough to keep the steady state from
    // reallocating, which is the part that was actually causing stutter.
    //
    // Both budgets are scaled to the heap the app actually got, in player.cpp:
    // 40 MB of queue and 22 MB of spare is only affordable on a large heap.

    // Parks a buffer for reuse, but only while the spare pool has room for it.
    // The caller holds frame_mutex_.
    void recycle_locked(std::vector<std::uint8_t> &&pixels);

    mutable std::mutex frame_mutex_;
    std::deque<Frame> pending_;
    std::vector<std::vector<std::uint8_t>> spare_;
    // Wakes the decode thread when the render thread takes a picture. The
    // queue is short - two 4K frames on the heap this title gets - while the
    // decoder runs in bursts, so without this it fills the queue and drops the
    // rest, which is a picture missing for no reason and invisible in the
    // counters.
    std::condition_variable frame_cv_;
    // Bumped by every start(), so the render thread can tell "this film has
    // not produced a picture yet" apart from "the film I was showing". Without
    // it the playing-but-no-frame-yet window repaints the previous film's last
    // frame, which is the stuck picture when a second film is opened.
    std::uint64_t session_ = 0;

    // The Dolby Vision reshaping table: three rows of 1024, one per component
    // (I, Ct, Cp). Written by the decode thread under frame_mutex_, read by
    // the render thread under the same lock; the atomic version lets the
    // render thread skip the lock entirely while the curve is unchanged,
    // which is all but the first frame of each scene.
    // A published RPU curve, tagged with the presentation pts (µs) of the
    // packet that carried it, so each picture is reshaped by the curve that
    // belongs to it. Guarded by frame_mutex_; read by the render thread when it
    // uploads a frame. Bounded because a scene changes only a handful of times
    // a film and this need only cover the demuxer's lookahead over the queue.
    struct CurveStamp
    {
        std::int64_t pts_us = 0;
        std::uint32_t version = 0;
        std::array<float, 1024 * 3> lut{};
        bool lut_valid = false;
        std::array<float, 9> from_ycc{};
        std::array<float, 3> offset{};
        std::array<float, 9> to_rgb{};
        bool colour_valid = false;
    };
    std::deque<CurveStamp> dovi_curve_;

    std::array<float, 1024 * 3> dovi_lut_{};
    bool dovi_lut_valid_ = false;
    // The same RPU's colour step, published with the table and under the same
    // version: L'M'S' = from_ycc * ((I, Ct, Cp) - offset), RGB = to_rgb * LMS.
    // Dolby's own matrices, which differ from BT.2100's by a factor of four in
    // the chroma scaling.
    std::array<float, 9> dovi_from_ycc_{};
    std::array<float, 3> dovi_offset_{};
    std::array<float, 9> dovi_to_rgb_{};
    bool dovi_colour_valid_ = false;
    std::atomic<std::uint32_t> dovi_lut_version_{0};

    // Render thread only.
    std::uint64_t shown_session_ = 0;
    std::vector<std::uint8_t> upload_;
    std::size_t upload_luma_ = 0;
    std::size_t upload_chroma_ = 0;
    int upload_width_ = 0;
    int upload_height_ = 0;
    int upload_y_pitch_ = 0;
    int upload_c_pitch_ = 0;
    int upload_v_pitch_ = 0;
    bool upload_ten_bit_ = false;
    bool upload_ten_bit_low_ = false;
    bool upload_planar_ = false;
    std::uint32_t texture_ = 0;
    int texture_width_ = 0;
    int texture_height_ = 0;
    // The Dolby Vision reshaping table, as last handed to the GPU. The decode
    // thread rebuilds it whenever the RPU's curve changes and bumps the
    // version; the render thread copies it across only when it differs.
    std::array<float, 1024 * 3> upload_lut_{};
    bool upload_lut_valid_ = false;
    std::array<float, 9> upload_from_ycc_{};
    std::array<float, 3> upload_offset_{};
    std::array<float, 9> upload_to_rgb_{};
    bool upload_colour_valid_ = false;
    std::uint32_t upload_lut_version_ = 0;
    // How many pictures this session has handed to the GPU, for the first few
    // lines of upload diagnostics.
    int uploads_ = 0;
    // How long a picture actually stayed on screen, in milliseconds, bucketed.
    // A film at 24 fps on a 60 Hz panel should alternate two refreshes and
    // three, so a healthy histogram is split between 30-45 and 45-60. Every
    // picture landing in 45-60 means the rule is rounding up and the film is
    // being shown at 20 fps. The last bucket is a pause or a seek rather than
    // a stutter, and is kept apart for that reason. Render thread only.
    std::int64_t last_shown_us_ = 0;
    long long shown_gap_hist_[7] = {};
    long long shown_gap_n_ = 0;

    // Written before the thread starts, read only while it runs.
    std::string url_;
    std::vector<std::string> headers_;
    int resume_seconds_ = 0;
#endif
};

} // namespace hui::fnvideo

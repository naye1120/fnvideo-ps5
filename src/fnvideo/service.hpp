// ps5-homebrew-ui - The 飞牛影视 library, off the render thread.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A design is built by a factory that receives no services, so the screens
// reach this through instance(). The object owns one worker thread: it signs
// in, pulls every library and the resume row, and then answers detail and
// artwork requests. The render thread only ever copies a snapshot out of it -
// the network never runs inside draw().
//
// Artwork is the one thing here that touches the GPU, and it does so only in
// update(), which the frame loop calls: the worker downloads and decodes, and
// the pixels change hands on the render thread. See fnos/art.hpp for the cache.

#pragma once

#include "fnvideo/api.hpp"
#include "fnvideo/player.hpp"
#include "fnos/art.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// pthread rather than std::thread: the console's libc has no locale, and
// pulling in <thread> drags newlocale/strtod_l in with it, which the link then
// cannot satisfy. src/fnos/ does the same.
#include <pthread.h>

namespace hui::gfx
{
class Renderer;
}

namespace hui::fnvideo
{

// How many pictures stay on the GPU. The wall draws twelve tiles at once and
// the home page a hero plus two rows, so a page of scrolling is covered twice
// over; a 384-pixel poster is about 0.4 MB once decoded.
constexpr std::size_t kArtKeep = 64;
// The pixel width a portrait poster is fetched at. The grid tile is 229 wide,
// so this is sharp at 1:1 without paying for the original artwork.
// A poster is shown at roughly 300 virtual pixels on the wall and much larger
// on the detail hero. 384 was visibly soft on a 4K panel; 512 still fits the
// 64-entry cache in the 2 GB heap (about 96 MB of RGBA) while carrying twice
// the detail of the old size.
constexpr int kPosterWidth = 512;
// The detail page's landscape still is full-bleed 1920 wide; 1280 is where the
// server's JPEG still reads as a photograph on a television.
// The detail hero and the home backdrop are full-bleed. 1280 was upscaled on
// a 4K display; 1920 is the same source scaled once, at modest extra cost
// because only one still is visible at a time.
constexpr int kStillWidth = 1920;
// When a title has no landscape art, the detail hero falls back to its portrait
// poster. Fetching that at the 512 wall size and stretching it across 1920 was
// the soft image the page showed; ask for it once under the still key at this
// larger width instead, so the fallback is a real 1280 image, not an upscale.
constexpr int kDetailStillFallbackWidth = 1280;

class Service
{
  public:
    static Service &instance();

    // Where the NAS is and who to sign in as. Calling it again with a
    // different server or account throws the old library away and resyncs.
    void configure(const std::string &server, const std::string &user, const std::string &password);
    // Reads a "key=value" file: server, username, password. Missing keys are
    // left as they were, so a file that only names the server is valid.
    void configure_from_file(const std::string &path);
    // Re-sign-in after the address or the account was edited but the password
    // field was left empty. The settings page never sees the stored password,
    // so configure(server, user, "") would silently erase it.
    void reconfigure(const std::string &server, const std::string &user);
    // Writes the working server/account/password back to /app0/fnvideo.conf so
    // a later restart - or a redeploy that refreshes the asset folder - still
    // auto-logs in. Best-effort only: a read-only install simply skips it.
    void save_credentials(const std::string &server, const std::string &user,
                          const std::string &password);
    bool configured() const;
    // The address the settings page shows, as it was configured.
    std::string server() const;
    // The account name, for the same page.
    std::string account() const;

    // Starts the worker on first call and is a no-op afterwards, so the frame
    // loop can call it unconditionally.
    void ensure_started();

    // Re-sign-in with the server, account and password already configured
    // (from the conf file or a previous login). Used by the settings page's
    // 登录 button when the player has not typed a fresh password.
    void relogin();
    // Clear the session so the next login must be typed again. Keeps the
    // server and account for display; drops the password and the token.
    void logout();

    // What the render thread draws from. A copy: the worker keeps writing
    // while the frame is being recorded.
    struct View
    {
        // Bumped every time the worker publishes. A screen compares this with
        // the value it last drew and copies the vectors only when it changed,
        // which keeps a 241-item library off the per-frame path.
        unsigned revision = 0;
        bool configured = false;
        bool busy = false;
        bool signed_in = false;
        std::string status; // one short line, already in Chinese
        std::string server_name;
        std::vector<Item> items;        // every library, flattened
        std::vector<Item> resume;       // 继续观看, ordered by the server
        std::vector<Library> libraries; // the server's media libraries (电影/电视剧/…), in order
    };
    View view() const;
    // Cheap: reads one integer under the lock.
    unsigned revision() const;

    // One item's detail, fetched on demand. `request_detail` is cheap when the
    // guid is already loaded or in flight.
    struct Detail
    {
        bool busy = false;
        bool ready = false;
        std::string error;
        Item item;
        std::vector<FileEntry> files;
        // The rows that play: a film's own file list says nothing, a series
        // holds one per episode under its seasons, a 短剧 folder one per file.
        // Already in play order and numbered, so the strip can draw them as
        // they come.
        std::vector<Item> episodes;
        // Bumped every time the worker publishes, so a screen can tell a fresh
        // answer from the one it drew last frame.
        unsigned revision = 0;
    };
    void request_detail(const std::string &guid);
    Detail detail() const;
    // Cheap: one integer under the lock, so the frame loop can watch for a
    // detail answer without copying the episode list.
    unsigned detail_revision() const;

    // The address FFmpeg opens for this item, or empty while it is being
    // asked for. Blocking: only call it from a worker.
    bool stream_url(const std::string &item_guid, std::string *url, PlayInfo *info,
                    std::string *error);

    // Artwork bytes for a path at a pixel width. Blocking, worker only.
    bool fetch_image(const std::string &path, int width, std::string *bytes, std::string *error);

    // ---- artwork ----
    // Posters are asked for by key (the item guid) and a server path. The
    // worker fetches and decodes them; `update` hands the pixels to the cache,
    // which is the only step that needs GL. Asking twice for the same key
    // costs one lookup, so a screen may ask every frame.
    void request_art(const std::string &key, const std::string &path, int width);
    // Once per frame, from the render thread: stages what the worker decoded
    // and uploads it.
    void update(gfx::Renderer &renderer);
    // The texture for a key, 0 until it is ready. Render thread only.
    std::uint32_t art_texture(const std::string &key);
    bool art_size(const std::string &key, int *width, int *height) const; // ---- playback ----
    // The mixer the player streams its sound into. Call before the audio
    // thread starts.
    void attach_audio(audio::Mixer &mixer);
    // Asks the worker for a play ticket and opens it. Cheap: the network and
    // the demuxer stay off the render thread, and the page just watches
    // playback() change.
    void play(const std::string &item_guid, int resume_seconds);
    // Enter a film the viewer already watched to the end. `play()` erases a
    // finished item's local point, and with no local point the worker falls
    // back to the server's resume `ts` - the closing credits. This marks the
    // start explicit and local (0), which the worker honours over the server.
    void play_from_start(const std::string &item_guid);
    // Keep this item's card so a locally-watched episode can still appear in
    // 继续观看 even though the server's list call only returned the series, not
    // the episode. The page already has the Item in hand and passes it here
    // before play(); a later view() can then rebuild the row without a network
    // round trip.
    void remember(const Item &item);
    // A film the viewer watched out. The home row and the detail button both
    // consult this so 黑神话 does not keep offering 继续播放 at the credits, and
    // a locally-partially-watched item still shows in 继续观看 even when the
    // server's resume list has not caught up. `clear_finished` returns true
    // when it removed a real entry (start_detail_playback uses that to pick
    // play_from_start).
    bool is_finished(const std::string &guid) const;
    void mark_finished(const std::string &guid);
    bool clear_finished(const std::string &guid);
    // The console's own last second for a guid, or -1 when nothing has been
    // watched here. The detail page's dialog uses it to decide whether to ask.
    int local_progress(const std::string &guid) const;
    // Local favourite marks, persisted next to the progress file. The server's
    // list call does not carry is_favorite for every row, and a star pressed on
    // the detail page must survive a restart, or 最近收藏 comes up empty on the
    // next boot. view() re-applies these to each item's is_favorite.
    bool is_favorite(const std::string &guid) const;
    void set_favorite(const std::string &guid, bool favorite);
    // Restart the item already open at an explicit second, keeping the chosen
    // audio and subtitle tracks. `play()` prefers the local resume point, which
    // is right when entering a film and wrong when the decoder chip restarts
    // the one already on screen.
    void reopen_at(int seconds);
    void stop_playback();
    void toggle_pause();
    void seek_playback(int seconds);

    // Volume and picture brightness. Both persist in fnvideo.conf next to the
    // credentials, so they survive a restart. Volume is linear stream gain;
    // brightness multiplies the video's RGB and leaves the UI alone.
    void set_volume(float volume);
    float volume() const;
    void set_brightness(float brightness);
    float brightness() const;
    // Embedded tracks of the file that is open. Empty until its header has
    // been read. The toolbar lists these; selecting an audio track reopens the
    // ticket at the current second, which is the only way FFmpeg can switch
    // streams without a second decoder.
    std::vector<AudioTrack> audio_tracks() const;
    std::vector<SubtitleTrack> subtitle_tracks() const;
    int audio_track() const;
    int subtitle_track() const;
    void select_audio_track(int ordinal);
    void select_subtitle_track(int ordinal);
    // A track chosen on the detail page, before playback. `ordinal` is an index
    // into the item's audio_streams (from /stream/list); -1 leaves the
    // container's default. The subtitle choice is an index into the item's
    // subtitle_streams (the server's own list, fetched by guid when it plays);
    // -1 means auto (first Chinese), -2 means 关闭. The pair is remembered for
    // one guid and seeded into the next play() of that same item, so a choice
    // made here takes effect the moment 播放 starts. Passing a different guid
    // than the last selection starts a fresh, empty preference.
    void select_detail_audio(const std::string &guid, int ordinal);
    void select_detail_subtitle(const std::string &guid, int index);
    // The stored choice for this guid, or -1 when the page has no preference
    // for it. Lets the detail page highlight the row that is already picked.
    int detail_pref_audio(const std::string &guid) const;
    int detail_pref_subtitle(const std::string &guid) const;
    // The text cue covering the current playhead, or empty. Text subtitles
    // only; PGS/DVD bitmap streams are listed but have no drawable glyphs here.
    std::string subtitle_at() const;

    // 刷新片源: re-pull the whole library from the server on the next worker
    // pass - the same full sync a boot or relogin runs. It is client-side only:
    // mediasrv is not asked to rescan the folders. The busy line and the revision
    // bump drive the top-bar button and the poster wall redraw.
    void refresh();

    // ---- subtitle configuration (the web player's 字幕配置, minus upload) ----
    // A snapshot the worker publishes; the render thread copies it out and polls
    // `revision` to know when a search or download has answered. All the network
    // and the NAS write happen on the worker, never in draw().
    struct SubtitlePanel
    {
        bool open = false;
        bool busy = false;
        bool local = false; // hits name NAS-folder files, not online search hits
        std::string error;  // one short line, Chinese, empty when fine
        std::string status; // e.g. "正在搜索字幕…"
        std::string folder; // browse: the absolute /vol… folder currently listed
        std::vector<SubtitleHit> hits;
        unsigned revision = 0;
    };
    SubtitlePanel subtitle_panel() const;
    void open_subtitle_panel();
    void close_subtitle_panel();
    // Queue the worker to search online subtitles for the playing item. `lan`
    // is a language code ("chi", "eng", …). No-op until the item's file-level
    // media guid is known (subtitle_config_available()).
    void subtitle_search_async(const std::string &lan);
    // Queue the worker to fetch one search hit into the media folder (a NAS
    // write), then re-read the stream list so the new external file appears as
    // a selectable track under 选择字幕.
    void subtitle_download_async(const std::string &trim_id);
    // Queue the worker to list the playing item's own folder the way the web
    // picker does (POST /server/path). The subtitle files that live beside the
    // video land in the panel's hits, flagged `local`; confirming one goes
    // through subtitle_mark_async.
    void subtitle_browse_async();
    // Navigate the browse tree to another folder (a subdirectory the user
    // drilled into, or its parent to go up). `folder` is an absolute /vol…
    // path; empty means "back to the playing video's own folder".
    void subtitle_browse_open_async(const std::string &folder);
    // Parent of the folder currently listed; empty when already at the top.
    void subtitle_browse_up_async();
    // Queue the worker to register one folder file as an external subtitle of
    // the playing media (PUT /subtitle/mark, a NAS write), then re-read the
    // stream list so 选择字幕 shows the new track. `filename` is a bare name
    // from the browse listing; it is joined with the video's own folder.
    void subtitle_mark_async(const std::string &filename);
    // True once the playing item has a file-level media guid, so search and
    // download are possible.
    bool subtitle_config_available() const;
    // True when the local browse is worth offering: the playing item's volume
    // path is known (it arrives with /stream/list, like the media guid).
    bool subtitle_browse_available() const;

    // The resolution tier the player's toolbar offers, in pixels of the
    // longest edge (4K / 2K / 1080p / 720p / 480p, or 0 for automatic). Kept
    // here so the conf file can store it next to the credentials, and pushed
    // into the player, which applies it to the next decoded picture.
    void set_play_edge(int edge);
    int play_edge() const;

    // The decoder the player's toolbar offers: 0 = software (FFmpeg), 1 =
    // hardware (PS5 Videodec2). Stored here so the conf file can hold it next
    // to the credentials and the render thread can show it; pushed into the
    // player, which reads it when a film opens. The toolbar reopens the
    // current film so a change takes effect at once.
    void set_decoder(int decoder);
    int decoder() const;

    struct Playback
    {
        bool opening = false;
        bool playing = false;
        bool paused = false;
        bool failed = false;
        bool ended = false; // the file played out on its own
        int position = 0;
        int duration = 0;
        std::string error; // one short line when failed
    };
    Playback playback() const;

    // The picture the player last handed over. `update` uploads it; these only
    // read the result, so a design can draw it without seeing a renderer.
    std::uint32_t video_texture() const
    {
        return video_texture_;
    }
    void video_size(int *width, int *height) const
    {
        *width = video_width_;
        *height = video_height_;
    }

    // The live bitmap subtitle (PGS/DVD): an RGBA texture `update` uploaded,
    // plus its rectangle inside the subtitle stream's own canvas. The player
    // screen projects that canvas into the video's displayed area, so the
    // sub-picture lines up instead of stretching to the screen. False when no
    // bitmap cue covers the playhead.
    bool subtitle_overlay(std::uint32_t *texture, int *x, int *y, int *w, int *h,
                          int *canvas_w, int *canvas_h) const
    {
        if (!subtitle_live_ || subtitle_texture_ == 0)
        {
            return false;
        }
        *texture = subtitle_texture_;
        *x = subtitle_x_;
        *y = subtitle_y_;
        *w = subtitle_w_;
        *h = subtitle_h_;
        *canvas_w = subtitle_canvas_w_;
        *canvas_h = subtitle_canvas_h_;
        return true;
    }

  private:
    Service() = default;
    ~Service();
    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;

    void worker_main();
    static void *worker_entry(void *self);
    // Artwork runs on its own thread. A poster whose decoder wedges (a webp
    // that loops inside decode_image) must not be able to freeze the thread
    // that answers detail (选集) and playback; isolating it contains the hang
    // to pictures. See art_worker_main.
    void art_worker_main();
    static void *art_worker_entry(void *self);
    // One full pass: sign in, read the libraries, read the resume row.
    void sync(unsigned generation);
    void sync_detail(const std::string &guid);
    // Fill one wall row's detail-only fields (类型 / 国家 / 动态范围 / 音频) from
    // /item/{guid}. The list endpoint carries none of them, so the library's
    // filter panel would find nothing to match until this lands. Runs as the
    // lowest-priority worker job - it never delays a sync, a play or a detail.
    void enrich_one(const std::string &guid);
    // One poster: fetch, decode, and leave the pixels for the render thread.
    void sync_art(const std::string &key, const std::string &path, int width);
    // What a poster that could not be had does: counts the attempt, says what
    // went wrong in the log, and queues the job again while attempts remain.
    void art_gave_up(const std::string &key, const std::string &path, int width, const char *why);
    // Resolves the ticket and starts the player. Worker only.
    void sync_play(const std::string &guid, int resume_seconds, int audio_track, int subtitle_track,
                   bool resume_is_local, unsigned request, bool subtitle_auto);
    // Local resume points, in /app0/fnvideo-progress.conf. The server's ts is
    // only the fallback; this is the position this console actually reached.
    void load_progress();
    void save_progress();
    void note_progress(const std::string &guid, int seconds);
    // Favourite marks + the card store for locally-watched items (so their
    // 继续观看 row can be rebuilt after a restart). Separate file from the
    // credentials, loaded once alongside the progress points.
    void load_state();
    void save_state();
    // (the public overload lives near the other finished_ accessors; this
    // private one is only what note_progress()/play() consult internally.)
    // Sleeps a little while there is nothing to do. The worker polls instead
    // of waiting on a condition variable, which keeps <thread> out of the link.
    void idle();

    // What the worker is doing, and what it has finished.
    struct ArtJob
    {
        std::string key;
        std::string path;
        int width = 0;
    };
    struct ArtReady
    {
        std::string key;
        std::vector<std::uint8_t> pixels;
        int width = 0;
        int height = 0;
    };

    // One subtitle job the worker runs serially, like every other network call.
    enum class SubtitleTask
    {
        none,
        load_external, // GET /subtitle/dl/{guid}, decode into the player
        search,        // POST /subtitle/search -> panel hits
        download,      // POST /subtitle/download -> write NAS, refresh tracks
        browse,        // POST /server/path of the video's folder -> panel hits (local)
        mark,          // PUT /subtitle/mark -> write NAS, refresh tracks
    };
    struct SubtitleRequest
    {
        SubtitleTask task = SubtitleTask::none;
        bool active = false;
        std::string guid;       // subtitle guid (load_external)
        std::string media_guid; // file-level guid (search / download)
        std::string format;     // demuxer hint for load_external
        std::string trim_id;    // download target
        std::string lan;        // search language
        std::string path;       // browse: the video's volume path; mark: the
                                // absolute subtitle path to register
        int ordinal = -1;       // external track ordinal for load_external
    };

    // Runs one queued subtitle task on the worker: fetch an external subtitle
    // file for the player, or search/download an online one for the panel.
    void run_subtitle_task(const SubtitleRequest &request);
    // Re-read /stream/list for the playing item so a downloaded or marked
    // subtitle appears as a selectable external track. Worker only; never
    // call it with mutex_ held (it takes api_mutex_, then mutex_).
    void refresh_external_subs();

    mutable std::mutex mutex_;
    // Serialises the one Api instance: the worker syncs through it while the
    // player thread may be asking it for a stream address.
    std::mutex api_mutex_;
    Api api_;

    std::string server_;
    std::string user_;
    std::string password_;
    bool configured_ = false;
    bool started_ = false;
    bool stopping_ = false;
    bool needs_sync_ = true;
    // Bumped by configure(); a sync that started under an older generation
    // drops its answer instead of publishing it over the new one.
    unsigned generation_ = 0;

    pthread_t worker_{};
    // The dedicated poster thread. Left unjoined on purpose: if it is wedged in
    // a decoder loop, joining it at shutdown would hang the exit instead.
    pthread_t art_worker_{};

    View view_;
    Detail detail_;
    std::string detail_guid_;   // what the worker should fetch next
    std::string detail_loaded_; // what the detail_ block currently holds
    unsigned detail_revision_ = 0;
    // Wall rows still missing their detail-only genre / country / colour / audio
    // text. Reseeded by every sync and drained one /item/{guid} call at a time
    // whenever the worker would otherwise idle; guarded by mutex_.
    std::vector<std::string> enrich_queue_;

    // Artwork. `art_` is touched only by the render thread; the two queues and
    // the two key sets are shared, so they live under mutex_.
    std::vector<ArtJob> art_queue_;   // asked for, not yet fetched
    std::vector<ArtReady> art_ready_; // decoded, waiting for the frame
    // Keys already queued or loaded, so a screen asking every frame does not
    // queue the same poster a hundred times.
    std::unordered_set<std::string> art_known_;
    // How many times a poster has been tried and failed, keyed by poster. A
    // network hiccup or one odd file used to blacklist the key for the rest of
    // the session - "one failure, never shown again" is why a wall came up
    // empty. Attempts are capped, the job is requeued at the back of the queue
    // (so the delay is one pass over the other posters), and a successful sync
    // clears the counts.
    std::unordered_map<std::string, int> art_failed_;
    fnos::ArtCache art_{kArtKeep};

    // Playback. The player owns its own thread; the worker only resolves the
    // address it opens, because that is a signed request like every other.
    Player player_;
    // The resolution tier, mirrored from set_play_edge() for the conf file.
    // Atomic because save_credentials() also runs after update() releases the
    // lock, while the settings page can change either one on the render thread.
    std::atomic<int> play_edge_{0}; // 0 = automatic: play at the source resolution
    // The decoder choice, mirrored from set_decoder() for the conf file.
    std::atomic<int> decoder_{1}; // 0 = software, 1 = hardware (default; see set_decoder())
    std::string play_guid_;       // what the worker should open next
    int play_resume_ = 0;
    // True when play_resume_ came from the local progress file, so a stored 0
    // means "start at the beginning" and is not replaced by the server's ts.
    bool play_resume_local_ = false;
    // Ordinals passed to the next open; -1 means the container's best stream.
    int play_audio_ = -1;
    int play_subtitle_ = -1;
    // The next open may auto-pick the first Chinese subtitle track (set on a
    // fresh play, cleared by any track selection and after one consumption).
    bool play_subtitle_auto_ = false;
    bool play_pending_ = false;
    // The worker consumes play_pending_ before the network ticket resolves.
    // This stays true across that gap so the page can show 正在打开 instead of
    // falling back to its sample clock (black picture, moving progress bar).
    bool play_resolving_ = false;
    // A track change reopens the ticket. If the film was paused, pause it again
    // as soon as the new decoder reaches playing; Player::pause() only acts on
    // a playing state, so it cannot be done immediately after start().
    std::atomic<bool> play_pause_when_ready_{false};
    // Every play/reopen/stop bumps this. A ticket resolution can take seconds
    // on a NAS; without it a film the viewer already left could still open
    // after stop_playback() returned. The worker checks the value immediately
    // before and after Player::start().
    unsigned play_request_ = 0;
    // The item the player is actually decoding, not the one the worker is
    // still resolving. Progress belongs to this guid: pressing 下一集 while
    // the old episode is still on screen must not write its position onto the
    // next episode's entry.
    std::string play_active_guid_;
    // Written by the worker when the ticket could not be had, read by the page.
    std::string play_error_;
    // The external subtitle streams the server lists for the item that is
    // playing, plus its file-level media guid. Cached by guid so a track-change
    // reopen does not re-hit /stream/list; cleared whenever a fresh play or a
    // download needs the current list. subtitle_tracks() merges these over the
    // container's own tracks.
    std::vector<SubtitleStreamInfo> play_external_subs_;
    std::string play_media_guid_;
    // The playing video's path on the NAS volume (/vol2/1000/…), from
    // /stream/list's files[0].path. The local-subtitle browse lists the folder
    // behind it; empty until the stream list has been read for this item.
    std::string play_file_path_;
    // The folder currently shown in the local-subtitle browse tree. Starts at
    // the playing video's parent folder and moves as the user drills in or up;
    // `subtitle_mark_async` joins a chosen bare name with it.
    std::string browse_folder_;
    std::string play_external_subs_guid_;
    // The external track the viewer chose (its merged ordinal), or -1. Kept so
    // subtitle_track() can report an external selection and select_subtitle_track
    // knows a reopen is needed to leave one.
    int play_subtitle_ext_ = -1;
    // A detail-page track choice, kept per guid so a collection's pick survives
    // carrying onto the episodes it plays. -1 is "no choice yet" (and for the
    // subtitle, "auto"); the subtitle also uses -2 for 关闭. play() seeds the
    // open from the entry for the item it is about to play, and sync_play loads
    // the chosen external subtitle right after the film starts.
    struct DetailPref
    {
        int audio = -1;
        int subtitle = -1;
    };
    std::unordered_map<std::string, DetailPref> detail_prefs_;
    // The subtitle index the current play was seeded with, kept so sync_play
    // can re-inject the chosen external subtitle on a track-change reopen.
    int play_pref_subtitle_ = -1;
    SubtitleRequest sub_request_;
    SubtitlePanel subtitle_panel_;
    unsigned subtitle_revision_ = 0;
    // Local resume points, keyed by item guid. The server's `ts` is only a
    // fallback: it is the web player's position, not this console's.
    std::unordered_map<std::string, int> progress_;
    // The same guids newest-last, so 继续观看 can rank the row by what THIS
    // console actually watched most recently. The home row shows only the first
    // few, so without a real recency order a film just played here landed behind
    // the server's rows and never appeared. Persisted as the line order of the
    // progress file (unordered_map iteration would scramble it).
    std::vector<std::string> progress_order_;
    // Guids the viewer played out on this console. Persisted as `=0` lines in
    // the same file so a redeploy keeps the memory; 继续观看 drops them and the
    // detail page falls back to 播放 (start-from-top) without a dialog.
    std::unordered_set<std::string> finished_;
    // Cards for guids play() has been asked to open, so 继续观看 can name an
    // episode the server's list answer never carried. Capped; the progress
    // file is still the source of truth.
    std::unordered_map<std::string, Item> known_items_;
    // guid -> starred locally. Applied to items_ in view() and persisted so a
    // restart keeps 最近收藏.
    std::unordered_map<std::string, bool> favorites_;
    bool state_loaded_ = false;
    std::string last_progress_guid_;
    int last_progress_position_ = -1;
    bool progress_loaded_ = false;
    std::atomic<float> volume_{1.0f};
    std::atomic<float> brightness_{1.0f};
    // A volume slider can move twenty times while the viewer holds left; each
    // step must not rewrite fnvideo.conf on the render thread. The update()
    // loop flushes once the value has been still for a couple of seconds.
    bool settings_dirty_ = false;
    int settings_quiet_frames_ = 0;
    // Render thread only: what `update` uploaded last.
    std::uint32_t video_texture_ = 0;
    int video_width_ = 0;
    int video_height_ = 0;
    // Render thread only: the bitmap-subtitle sub-picture last uploaded, its
    // GL texture (recreated when the picture size changes), and the placement
    // snapshot the player screen reads through subtitle_overlay().
    std::uint32_t subtitle_texture_ = 0;
    int subtitle_tex_w_ = 0;
    int subtitle_tex_h_ = 0;
    int subtitle_uploaded_serial_ = -1;
    int subtitle_x_ = 0;
    int subtitle_y_ = 0;
    int subtitle_w_ = 0;
    int subtitle_h_ = 0;
    int subtitle_canvas_w_ = 0;
    int subtitle_canvas_h_ = 0;
    bool subtitle_live_ = false;
};

} // namespace hui::fnvideo

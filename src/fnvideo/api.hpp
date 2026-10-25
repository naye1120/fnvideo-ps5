// ps5-homebrew-ui - The 飞牛影视 API, as this app uses it.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Endpoints live under <server>/v/api/v1 (login is the one exception, on
// /v/api/v2) and answer {"code":0,"msg":"","data":{...}}; any other code is a
// business error and msg is the text to show. Two headers are involved on
// every call: `authx`, a per-request signature (see authx.hpp), and
// `Authorization: <token>`, a bare token with no scheme in front of it.
//
// The shape below was read off a live 0.9.8 / mediasrv 0.8.43 server; the
// notes in _research/fnvideo_routes.md record which fields are real and which
// endpoints the shipped web client calls but this build does not implement
// (notably /movie/:guid, which answers 501 - detail comes from /item/:guid).

#pragma once

#include <string>
#include <vector>

namespace hui::fnos
{
class Json;
}

namespace hui::fnvideo
{

// One media library: 电影, 电视剧, 短剧, 其他视频.
struct Library
{
    std::string guid;
    std::string title;
    std::string category; // Movie / TV / Mix / Others
    std::string poster;   // first entry of `posters`, may be empty
};

// One subtitle stream the server lists for an item, from /stream/list's
// `subtitle_streams`. `external` marks a file that lives beside the video
// rather than inside its container, and `bitmap` a PGS/DVD picture track this
// app cannot draw as text. An external, non-bitmap stream is fetched whole by
// its `guid` through GET /subtitle/dl/{guid} and decoded on the console.
struct SubtitleStreamInfo
{
    std::string guid; // the subtitle guid, the key for /subtitle/dl/{guid}
    std::string media_guid;
    std::string language;
    std::string title;
    std::string format; // srt / ass / vtt / sup
    std::string codec;  // subrip / ass / hdmv_pgs_subtitle
    bool external = false;
    bool bitmap = false;
};

// One audio stream the server lists for an item, from /stream/list's
// `audio_streams`. `index` is its position among the audio streams, which is
// the ordinal the player keys its own track selection on (a container's audio
// streams enumerate in the same order the server reports them).
struct AudioStreamInfo
{
    std::string language;
    std::string title;
    std::string codec; // codec_name, e.g. eac3 / truehd / aac
    std::string channel_layout; // "7.1", "5.1(side)", "stereo"
    int channels = 0;           // raw channel count
    int index = 0;
    bool def = false;
};

// One online subtitle hit from POST /subtitle/search. `trim_id` is sent back
// to POST /subtitle/download to have the server fetch it into the folder.
struct SubtitleHit
{
    std::string filename;
    std::string trim_id;
    std::string source; // opensubtitles / NAS / 文件夹
    std::string format; // vtt / srt / ...
    long long download = 0;
    // A folder row in the NAS browse tree: confirming it navigates in rather
    // than marking a subtitle. Set only for local browse hits.
    bool is_dir = false;
};

// A row in a library, or the subject of a detail page. Movies and series
// share this shape; `type` says which it is (Movie, TV, Directory, Video).
struct Item
{
    std::string guid;
    std::string title;
    std::string original_title;
    std::string type;
    std::string overview;
    // Artwork paths relative to /v/api/v1/sys/img - pass them to image_url().
    std::string poster;   // 2:3 portrait
    std::string backdrop; // wide still, the detail page's hero
    std::string logo;     // transparent title lockup, may be empty
    std::string ancestor_guid;
    std::string ancestor_name;
    std::string ancestor_category;
    // The row's place in the server's tree. A season points at its series, an
    // episode at its season, an episode of a 短剧 folder at the folder; the
    // top-level rows this app draws have none. `single_child_guid` is the one
    // child the list call volunteers, which is how a series names its first
    // season without a second request.
    std::string parent_guid;
    std::string parent_title;
    std::string single_child_guid;
    std::string release_date;
    std::string vote_average; // a string on the wire, "0" when unrated
    // Highest resolution the server advertises for this item, e.g. "1080p".
    // Taken from media_stream.resolutions[0]; empty when the list did not say.
    std::string resolution;
    // Detail-page metadata chips. content_ratings and production_countries come
    // from /item/{guid}; the two media_stream strings are the picture/sound
    // format the server advertises ("DolbyVision", "DolbyAtmos"). `countries`
    // is the raw ISO codes comma-joined ("GB,US") - the display mapping is a
    // view concern. The chi_ flags are set only by streams(), from /stream/list.
    std::string content_rating;
    std::string countries;
    // Real genres as a display line ("动作 · 冒险 · 喜剧"). /item/{guid} carries
    // `genres` as an array of stable ids (the list row carries none); api.cpp
    // maps each id to Chinese through a fixed table, so this needs no dictionary
    // round-trip. Empty until the detail lands.
    std::string genres_text;
    std::string color_range;
    std::string audio_type;
    bool chi_audio = false;
    bool chi_subtitle = false;
    long long runtime = 0;    // minutes, from the detail call
    long long duration = 0;   // seconds, from the list call
    long long watched_ts = 0; // resume position in seconds
    int is_favorite = 0;
    int watched = 0;
    int can_play = 0;
    int season_number = 0;
    int episode_number = 0;
    // How many episodes and seasons the row holds. A series row reports its own
    // totals here (`local_` is what is on this NAS, the other is the metadata
    // provider's count), which is what lets a card say 共 n 集 without walking
    // the episodes first.
    int number_of_episodes = 0;
    int local_number_of_episodes = 0;
    int number_of_seasons = 0;
    int local_number_of_seasons = 0;
    // The file-level media guid (first entry of the item's file list). The
    // online subtitle search / download endpoints key on this, not on the
    // item guid - an item guid answers `Permission Error` (code -5).
    std::string media_guid;
    // The video's own path on the NAS volume ("/vol2/1000/…"), first entry of
    // /stream/list's files[]. The local-subtitle browser lists the folder
    // behind it via POST /server/path; empty when the list withheld it.
    std::string file_path;
    // Every subtitle stream the server knows about for this item, embedded or
    // external, filled by streams() from /stream/list. The player lists these
    // so an external text file sitting next to the video - which is not inside
    // the original container FFmpeg opens - can be chosen and fetched.
    std::vector<SubtitleStreamInfo> subtitle_streams;
    // Every audio stream the server lists for this item, filled by streams().
    // The detail page shows these so a preferred track can be picked before
    // playback and handed to the decoder when the film opens.
    std::vector<AudioStreamInfo> audio_streams;
};

// One playable file behind an item. A movie has one; a series has one per
// episode, and `media_guid` is what the stream URL is built from.
struct FileEntry
{
    std::string media_guid;
    std::string item_guid;
    std::string filename;
    std::string file_path;
    long long duration = 0; // seconds
};

// What /play/info answers: the three stream guids plus the resume point.
struct PlayInfo
{
    std::string media_guid;
    std::string video_guid;
    std::string audio_guid;
    std::string subtitle_guid;
    long long ts = 0; // seconds already watched
};

template <typename T> struct Page
{
    std::vector<T> items;
    long long total = 0;
};

using ItemPage = Page<Item>;
using LibraryPage = Page<Library>;

// Blocking calls: run them on the network worker, never on the render thread.
class Api
{
  public:
    // "http://192.168.1.107:5666", with or without a trailing slash. A URL
    // that already carries /v/api/v1 is trimmed back to the origin.
    void set_server(const std::string &base_url);
    const std::string &server() const
    {
        return base_;
    }

    void set_token(const std::string &token)
    {
        token_ = token;
    }
    const std::string &token() const
    {
        return token_;
    }
    bool signed_in() const
    {
        return !token_.empty();
    }
    // The account the last login used, so an expired token can be renewed
    // without putting a keyboard back in the player's hands.
    const std::string &account() const
    {
        return login_user_;
    }

    // Password login. The plain text is hashed here and never kept.
    bool login(const std::string &username, const std::string &password, std::string *error);

    bool libraries(LibraryPage *out, std::string *error);
    // The cards of a library, or of the whole server when `library_guid` is
    // empty. `types` is the row kinds wanted - Movie / TV / Directory / Video -
    // and it matters: without it the endpoint answers with every season, every
    // episode and every file under the library too (1726 rows for one 电视剧
    // library here), which is not what a poster wall draws. The filter only
    // works without a library guid, so a caller that wants a whole library
    // passes an empty guid and partitions the answer by ancestor_guid itself.
    bool items(const std::string &library_guid, const std::vector<std::string> &types, int page,
               int page_size, ItemPage *out, std::string *error);
    bool item(const std::string &guid, Item *out, std::string *error);
    // The seasons of a series, in the server's order.
    bool seasons(const std::string &tv_guid, std::vector<Item> *out, std::string *error);
    // What sits under a row: a season's episodes, or a 短剧 folder's files.
    // `parent_guid` is the row's own guid. The server answers newest first, so
    // callers that want play order sort it themselves.
    bool children(const std::string &parent_guid, int page, int page_size, ItemPage *out,
                  std::string *error);
    bool files(const std::string &item_guid, std::vector<FileEntry> *out, std::string *error);
    // The audio/subtitle stream languages behind an item, used to set the
    // chi_audio / chi_subtitle flags on `item`. A failure is not fatal: the
    // detail page simply omits the Chinese-track chips.
    bool streams(const std::string &item_guid, Item *item, std::string *error);

    // ---- online subtitles (mirrors the web player's subtitle menu) ----
    // The raw bytes of one external subtitle file, GET /subtitle/dl/{guid}.
    // Token only, no authx signature - the same exception the media and image
    // endpoints make. The server may hand back a UTF-8 BOM; the caller strips
    // it before the demuxer sees the text.
    bool subtitle_content(const std::string &subtitle_guid, std::string *bytes, std::string *error);
    // POST /subtitle/search. `media_guid` must be the file-level guid; an item
    // guid comes back `Permission Error` (code -5).
    bool subtitle_search(const std::string &media_guid, const std::string &lan,
                         std::vector<SubtitleHit> *out, std::string *error);
    // POST /subtitle/download {media_guid, trim_id, sync_download}: the server
    // fetches the chosen subtitle into the media folder, where it then shows up
    // as an external stream in streams(). This writes to the NAS.
    bool subtitle_download(const std::string &media_guid, const std::string &trim_id,
                           std::string *error);
    // PUT /subtitle/mark {media_guid, filepaths}: register subtitle files that
    // already sit in the folder as streams of this media. Writes to the NAS.
    bool subtitle_mark(const std::string &media_guid, const std::vector<std::string> &filepaths,
                       std::string *error);
    // Lists one NAS folder - `folder` is an absolute /vol… directory, not a
    // file path. POST /server/path {path} answers every entry of that folder
    // (the same endpoint the web's "add NAS subtitle" tree walks); the result
    // keeps every subdirectory (is_dir, so the picker can drill in) plus the
    // subtitle files by extension. Runs on the worker like other blocking calls.
    bool folder_files(const std::string &folder, std::vector<SubtitleHit> *out,
                      std::string *error);
    // Resume list, ordered by the server.
    bool continue_watching(ItemPage *out, std::string *error);

    bool play_info(const std::string &item_guid, PlayInfo *out, std::string *error);
    // Asks for a play ticket and returns the address FFmpeg should open. The
    // ticket is a short-lived m3u8 path, but the bytes behind it are the
    // original file: the server serves /media/range/{media_guid} with Range
    // support, which is what this returns.
    bool stream_url(const std::string &item_guid, std::string *url, PlayInfo *info,
                    std::string *error);
    // The address for a ticket already in hand, so a retry costs no round trip.
    std::string range_url(const std::string &media_guid, const std::string &play_link) const;

    // Artwork: the path from Item, at a pixel width. `?w=` is the only sizing
    // parameter the server honours.
    std::string image_url(const std::string &path, int width) const;
    bool fetch_image(const std::string &path, int width, std::string *bytes, std::string *error);
    // Headers a media or artwork request must carry (token only - the stream
    // endpoint does not check `authx`).
    std::vector<std::string> media_headers() const;

  private:
    // Every endpoint goes through here. It signs, asks once, and recovers at
    // most once from an expired session.
    bool ask(const std::string &method, const std::string &path, const std::string &body_json,
             fnos::Json *data, std::string *error);
    bool sign_in(const std::string &username, const std::string &password_hash, std::string *error);

    std::string base_;
    std::string token_;
    std::string login_user_;
    std::string login_hash_; // sha256 hex, kept so re-login needs no keyboard
};

} // namespace hui::fnvideo

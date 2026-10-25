// ps5-homebrew-ui - The fnOS music API, as this app uses it.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Endpoints are under <server>/music/api/v1 and answer
// {"code":0,"msg":"","data":{...}}; "code" other than 0 is a business error
// and msg is the text to show. The session is a `music-token` cookie, so no
// Authorization header is involved. The password goes over as sha256(password)
// hex, and a device id (32 hex digits, stored with the settings) names this
// console as one of the user's devices.

#pragma once

#include <string>
#include <vector>

namespace hui::fnos
{

class Json;

struct Artist
{
    std::string guid;
    std::string name;
    std::string cover_id;
};

struct Album
{
    std::string guid;
    std::string name;
    std::string cover_id;
    std::string artist;
    int year = 0;
    int track_count = 0;
};

struct Playlist
{
    std::string guid;
    std::string name;
    std::string cover_id;
    int track_count = 0;
};

struct Track
{
    std::string guid;
    std::string title;
    std::string artist; // the artists joined with ", "
    std::string album;
    std::string cover_id;
    int duration = 0; // seconds
    bool favorite = false;
    bool has_lyric = false;
};

struct Genre
{
    std::string guid;
    std::string name;
    int track_count = 0;
};

template <typename T> struct Page
{
    using value_type = T;
    std::vector<T> items;
    long total = 0;
};

using TrackPage = Page<Track>;
using AlbumPage = Page<Album>;
using ArtistPage = Page<Artist>;
using PlaylistPage = Page<Playlist>;
using GenrePage = Page<Genre>;

// Blocking calls: run them on the network worker, never on the render thread.
class Api
{
  public:
    // `base_url` is like "http://192.168.1.20:5666" without a trailing slash
    // and without the /music/api/v1 suffix (either is accepted and trimmed).
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

    // Password login; stores the token on success.
    bool login(const std::string &username, const std::string &password,
               const std::string &device_id, std::string *error);

    bool playlists(int page, PlaylistPage *out, std::string *error);
    bool playlist_tracks(const std::string &guid, TrackPage *out, std::string *error);
    bool tracks(int page, TrackPage *out, std::string *error);
    // The whole library, a page at a time: one page is a hundred songs and a
    // real library is several hundred, so `total` decides how many to ask for.
    bool all_tracks(TrackPage *out, std::string *error);
    bool albums(int page, AlbumPage *out, std::string *error);
    bool album_tracks(const std::string &guid, TrackPage *out, std::string *error);
    bool artists(ArtistPage *out, std::string *error);
    bool artist_tracks(const std::string &guid, TrackPage *out, std::string *error);
    bool genres(GenrePage *out, std::string *error);
    bool genre_tracks(const std::string &guid, TrackPage *out, std::string *error);
    bool favorites(TrackPage *out, std::string *error);
    bool search(const std::string &text, TrackPage *out, std::string *error);
    // LRC text for one track; empty when it has none.
    bool lyric(const std::string &guid, std::string *text, std::string *error);
    bool favorite(const std::string &guid, bool on, std::string *error);

    // Where the audio and the artwork come from. Both need the cookie, so they
    // are fetched through this class rather than opened by a player directly.
    std::string stream_url(const std::string &guid) const;
    std::string cover_url(const std::string &cover_id, int size) const;
    // Headers a media or artwork request must carry.
    std::vector<std::string> media_headers() const;
    // Fetches one artwork into `bytes` (JPEG or PNG as the server sent it).
    bool fetch_cover(const std::string &cover_id, int size, std::string *bytes, std::string *error);

  private:
    std::vector<std::string> auth_headers() const;
    // One page of /track/list at a chosen size; `tracks` and `all_tracks` wrap it.
    bool tracks_page(int page, int size, TrackPage *out, std::string *error);
    // Every endpoint goes through this: it asks once, and recovers twice at
    // most - a transfer that died, and a session the NAS has forgotten.
    bool ask(const std::string &base, const std::vector<std::string> &headers,
             const std::string &path, const std::string &body_json, Json *data, std::string *error);
    // `password_hash` is already sha256 hex, which is what the endpoint wants.
    bool sign_in_with(const std::string &base, const std::string &username,
                      const std::string &password_hash, const std::string &device_id,
                      std::string *error);

    std::string base_;
    std::string token_;
    // What the last successful login used, in the shape a re-login needs: the
    // password stays hashed, so an expired token costs no keyboard again.
    std::string login_user_;
    std::string login_hash_;
    std::string login_device_;
};

} // namespace hui::fnos

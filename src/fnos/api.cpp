// ps5-homebrew-ui - the fnOS music API client.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every call goes through fnos/http and reads the
// {"code":0,"msg":"","data":{...}} envelope with the JSON reader beside this
// file. The PC snapshot host has no libcurl, so its http stub answers the same
// envelope shape from a fixed library and this file stays free of build
// branches.

#include "fnos/api.hpp"

#include "fnos/http.hpp"
#include "fnos/json.hpp"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h> // usleep: the recovery waits on the worker, never on the picture

#ifdef HUI_PS5
#include <openssl/evp.h>
#endif

namespace hui::fnos
{

namespace
{

constexpr const char *kApi = "/music/api/v1";

// A page of the whole library, and how many of them to ask for. `total` ends
// the loop on the first page that is already covered; the cap is only a
// stop-loss for a server that reports an inflated figure and keeps the worker
// busy forever. Five thousand songs is a library no home NAS holds, and one
// page of a hundred rows costs about as much as the next.
constexpr int kTrackPage = 100;
constexpr int kTrackPages = 50;

std::string number(long long value)
{
    return std::to_string(value);
}

std::string text_of(const Json &value, const char *key)
{
    return value.member(key).text();
}

long long int_of(const Json &value, const char *key)
{
    return value.member(key).number(0);
}

Track track_of(const Json &value)
{
    Track track;
    track.guid = text_of(value, "guid");
    track.title = text_of(value, "title");
    const Json artists = value.member("artists");
    if (artists.array())
    {
        for (std::size_t index = 0; index < artists.size(); ++index)
        {
            const std::string name = artists.element(index).member("name").text();
            if (name.empty())
                continue;
            if (!track.artist.empty())
                track.artist += ", ";
            track.artist += name;
        }
    }
    else
    {
        track.artist = text_of(value, "artist");
    }
    track.album = value.member("album").member("name").text();
    track.cover_id = text_of(value, "coverId");
    // The server's unit is not fixed: a five-minute song arrives as 300 from one
    // endpoint and 300000 from another. 20000 is the seam — no song is 5.5 hours
    // long, and none is a 20-second clip counted in milliseconds. The old cut sat
    // at 100000, so every piece under 100 seconds that came in milliseconds was
    // read as seconds and the remaining time showed hours.
    const long long duration = int_of(value, "duration");
    track.duration = static_cast<int>(duration > 20000 ? duration / 1000 : duration);
    track.favorite = value.member("isFavorite").flag();
    track.has_lyric = value.member("hasLyric").flag();
    if (track.title.empty())
        track.title = "未知标题";
    return track;
}

Album album_of(const Json &value)
{
    Album album;
    album.guid = text_of(value, "guid");
    album.name = text_of(value, "name");
    album.cover_id = text_of(value, "coverId");
    album.year = static_cast<int>(int_of(value, "releaseDate"));
    album.track_count = static_cast<int>(int_of(value, "trackCount"));
    if (album.name.empty())
        album.name = "未知专辑";
    return album;
}

Artist artist_of(const Json &value)
{
    Artist artist;
    artist.guid = text_of(value, "guid");
    artist.name = text_of(value, "name");
    artist.cover_id = text_of(value, "coverId");
    if (artist.name.empty())
        artist.name = "未知歌手";
    return artist;
}

Playlist playlist_of(const Json &value)
{
    Playlist playlist;
    playlist.guid = text_of(value, "guid");
    playlist.name = text_of(value, "name");
    playlist.cover_id = text_of(value, "coverId");
    playlist.track_count = static_cast<int>(int_of(value, "trackCount"));
    if (playlist.name.empty())
        playlist.name = "未知歌单";
    return playlist;
}

Genre genre_of(const Json &value)
{
    Genre genre;
    genre.guid = text_of(value, "guid");
    genre.name = text_of(value, "name");
    genre.track_count = static_cast<int>(int_of(value, "trackCount"));
    if (genre.name.empty())
        genre.name = "未知风格";
    return genre;
}

// One response that survived both the transport and the business envelope.
bool read_reply(const HttpResult &result, Json *data, std::string *error)
{
    if (!result.error.empty())
    {
        *error = "网络请求失败: " + result.error;
        return false;
    }
    if (result.status == 401 || result.status == 403)
    {
        *error = "登录已失效，请重新登录";
        return false;
    }
    Json root;
    if (!Json::parse(result.body, &root))
    {
        *error = "服务器返回了无法解析的内容";
        return false;
    }
    if (int_of(root, "code") != 0)
    {
        const std::string message = text_of(root, "msg");
        *error = message.empty() ? "请求被服务器拒绝" : message;
        return false;
    }
    *data = root.member("data");
    return true;
}

template <typename PageType, typename Parse>
bool fill_page(const Json &data, PageType *out, Parse parse)
{
    out->items.clear();
    out->total = 0;
    const Json list = data.member("list");
    for (std::size_t index = 0; index < list.size(); ++index)
    {
        typename PageType::value_type item = parse(list.element(index));
        if (!item.guid.empty())
            out->items.push_back(std::move(item));
    }
    out->total = static_cast<long>(data.member("total").number(0));
    if (out->total == 0)
        out->total = static_cast<long>(out->items.size());
    return true;
}

std::string sha256_hex(const std::string &text)
{
#ifdef HUI_PS5
    unsigned char digest[EVP_MAX_MD_SIZE] = {};
    unsigned int length = 0;
    if (EVP_Digest(text.data(), text.size(), digest, &length, EVP_sha256(), nullptr) != 1)
        return {};
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    hex.resize(length * 2);
    for (unsigned int index = 0; index < length; ++index)
    {
        hex[index * 2] = digits[digest[index] >> 4];
        hex[index * 2 + 1] = digits[digest[index] & 0x0fu];
    }
    return hex;
#else
    (void)text; // the preview host never logs in
    return {};
#endif
}

// Quotes one string into a JSON body. The guid comes from the server, but a
// quote or a backslash in it must not be able to reshape the request.
std::string json_string_field(const std::string &value)
{
    std::string out = "\"";
    for (const char c : value)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char>(c) < 0x20u)
            continue; // a control byte has no place in a guid
        out += c;
    }
    out += '"';
    return out;
}

} // namespace

void Api::set_server(const std::string &base_url)
{
    std::string value = base_url;
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    // A pasted ".../music/api/v1" is accepted too: the suffix is trimmed.
    constexpr std::size_t kSuffixLength = 13; // strlen("/music/api/v1")
    if (value.size() >= kSuffixLength &&
        value.compare(value.size() - kSuffixLength, kSuffixLength, kApi) == 0)
        value.erase(value.size() - kSuffixLength);
    base_ = value;
}

std::vector<std::string> Api::auth_headers() const
{
    std::vector<std::string> headers;
    if (!token_.empty())
        headers.push_back("Cookie: music-token=" + token_);
    return headers;
}

std::vector<std::string> Api::media_headers() const
{
    return auth_headers();
}

std::string Api::stream_url(const std::string &guid) const
{
    return base_ + kApi + "/track/stream?guid=" + http_escape(guid);
}

std::string Api::cover_url(const std::string &cover_id, int size) const
{
    if (cover_id.empty())
        return {};
    return base_ + kApi + "/static/cover?coverId=" + http_escape(cover_id) +
           "&size=" + number(size);
}

// The one POST that hands out a token. `password_hash` is sha256 hex already,
// because a re-login must not need the typed password a second time.
bool Api::sign_in_with(const std::string &base, const std::string &username,
                       const std::string &password_hash, const std::string &device_id,
                       std::string *error)
{
    const std::string body = "{\"username\":" + json_string_field(username) +
                             ",\"password\":" + json_string_field(password_hash) +
                             ",\"deviceId\":" + json_string_field(device_id) + "}";
    const HttpResult result =
        http_request("POST", base + kApi + "/user/password-login", {}, body, 1u << 20, 15);
    // A login answer arrives as a redirect when the server forces HTTPS; naming
    // that case beats reporting a plain login failure, as the web client does.
    if (result.status >= 300 && result.status < 400)
    {
        *error = "服务器要求跳转，请检查它的 HTTPS 设置";
        return false;
    }
    Json data;
    if (!read_reply(result, &data, error))
        return false;
    const std::string token = text_of(data, "userToken");
    if (token.empty())
    {
        *error = "服务器没有返回登录令牌";
        return false;
    }
    token_ = token;
    return true;
}

bool Api::login(const std::string &username, const std::string &password,
                const std::string &device_id, std::string *error)
{
    const std::string hash = sha256_hex(password);
    if (!sign_in_with(base_, username, hash, device_id, error))
        return false;
    login_user_ = username;
    login_hash_ = hash;
    login_device_ = device_id;
    return true;
}

// Every endpoint runs through here. Two things go wrong on a home network
// that are worth the player's silence: a transfer that never landed, and a
// session the NAS dropped while the console was playing - the second used to
// mean a red line of text and a keyboard, although the credentials are kept
// anyway. Both recoveries sleep on this worker thread, so no frame waits.
bool Api::ask(const std::string &base, const std::vector<std::string> &headers,
              const std::string &path, const std::string &body_json, Json *data, std::string *error)
{
    const char *method = body_json.empty() ? "GET" : "POST";
    std::vector<std::string> outgo = headers;
    bool renewed = false;
    // Renewing means "the session I sent was refused", not "I have credentials
    // somewhere": after a sign-out the kept user and hash stay in this object,
    // and a request that carried no cookie must not sign anybody back in.
    const bool had_session = !token_.empty();
    // Two transports at most, and a refused session costs one renewal plus the
    // read it was standing in for. Every way out of here names the failure.
    int transports = 0;
    for (;;)
    {
        const HttpResult result =
            http_request(method, base + kApi + path, outgo, body_json, 8u << 20, 20);
        if (!result.error.empty())
        {
            ++transports;
            if (transports < 2)
            {
                ::usleep(300 * 1000);
                continue;
            }
            *error = "网络请求失败: " + result.error;
            return false;
        }
        if (result.status == 401 || result.status == 403)
        {
            if (renewed || !had_session || login_user_.empty())
            {
                *error = "登录已失效，请重新登录";
                return false;
            }
            renewed = true;
            std::string again;
            if (!sign_in_with(base, login_user_, login_hash_, login_device_, &again))
            {
                *error = again.empty() ? "登录已失效，请重新登录" : again;
                return false;
            }
            outgo = auth_headers();
            continue;
        }
        return read_reply(result, data, error);
    }
}

bool Api::playlists(int page, PlaylistPage *out, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(), "/playlist/list?page=" + number(page) + "&size=50", {}, &data,
             error))
        return false;
    return fill_page(data, out, playlist_of);
}

bool Api::playlist_tracks(const std::string &guid, TrackPage *out, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(),
             "/track/playlist-detail/list?playlistGUID=" + http_escape(guid) + "&page=1&size=500",
             {}, &data, error))
        return false;
    return fill_page(data, out, track_of);
}

bool Api::tracks(int page, TrackPage *out, std::string *error)
{
    return tracks_page(page, kTrackPage, out, error);
}

bool Api::tracks_page(int page, int size, TrackPage *out, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(), "/track/list?page=" + number(page) + "&size=" + number(size),
             {}, &data, error))
        return false;
    return fill_page(data, out, track_of);
}

bool Api::all_tracks(TrackPage *out, std::string *error)
{
    out->items.clear();
    out->total = 0;
    for (int page = 1; page <= kTrackPages; ++page)
    {
        TrackPage slice;
        if (!tracks_page(page, kTrackPage, &slice, error))
            return false;
        for (Track &track : slice.items)
            out->items.push_back(std::move(track));
        out->total = slice.total;
        // A short page is the last one even when the server would not say how
        // many there are: `total` is 0 on some builds and fill_page backfills it
        // with the page's own size.
        if (static_cast<int>(slice.items.size()) < kTrackPage)
            break;
        if (static_cast<long>(out->items.size()) >= out->total)
            break;
    }
    return true;
}

bool Api::albums(int page, AlbumPage *out, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(), "/album/list?page=" + number(page) + "&size=50", {}, &data,
             error))
        return false;
    return fill_page(data, out, album_of);
}

bool Api::album_tracks(const std::string &guid, TrackPage *out, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(),
             "/track/album-detail/list?albumGUID=" + http_escape(guid) + "&page=1&size=500", {},
             &data, error))
        return false;
    return fill_page(data, out, track_of);
}

bool Api::artists(ArtistPage *out, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(), "/artist/list?page=1&size=200", {}, &data, error))
        return false;
    return fill_page(data, out, artist_of);
}

bool Api::artist_tracks(const std::string &guid, TrackPage *out, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(),
             "/track/artist-detail/list?artistGUID=" + http_escape(guid) + "&page=1&size=500", {},
             &data, error))
        return false;
    return fill_page(data, out, track_of);
}

bool Api::genres(GenrePage *out, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(), "/genre/list?page=1&size=100", {}, &data, error))
        return false;
    return fill_page(data, out, genre_of);
}

bool Api::genre_tracks(const std::string &guid, TrackPage *out, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(),
             "/track/genre-detail/list?genreGUID=" + http_escape(guid) + "&page=1&size=500", {},
             &data, error))
        return false;
    return fill_page(data, out, track_of);
}

bool Api::favorites(TrackPage *out, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(), "/favorite-track/list?page=1&size=-1", {}, &data, error))
        return false;
    return fill_page(data, out, track_of);
}

bool Api::search(const std::string &text, TrackPage *out, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(), "/search/track?q=" + http_escape(text) + "&page=1&size=100", {},
             &data, error))
        return false;
    return fill_page(data, out, track_of);
}

bool Api::lyric(const std::string &guid, std::string *text, std::string *error)
{
    Json data;
    if (!ask(base_, auth_headers(), "/lyric/list?trackGUID=" + http_escape(guid), {}, &data, error))
        return false;
    text->clear();
    const Json list = data.member("list");
    if (list.size() == 0)
        return true;
    // Prefer the entry named by "preferred", otherwise take the first one.
    const std::string preferred = text_of(data, "preferred");
    for (std::size_t index = 0; index < list.size(); ++index)
    {
        const Json entry = list.element(index);
        if (!preferred.empty() && text_of(entry, "guid") != preferred)
            continue;
        *text = text_of(entry, "content");
        return true;
    }
    *text = text_of(list.element(0), "content");
    return true;
}

bool Api::favorite(const std::string &guid, bool on, std::string *error)
{
    Json data;
    const std::string body = "{\"trackGUID\":" + json_string_field(guid) + "}";
    return ask(base_, auth_headers(), on ? "/favorite-track/create" : "/favorite-track/delete",
               body, &data, error);
}

bool Api::fetch_cover(const std::string &cover_id, int size, std::string *bytes, std::string *error)
{
    const std::string url = cover_url(cover_id, size);
    if (url.empty())
        return false;
    const HttpResult result = http_request("GET", url, media_headers(), {}, 16u << 20, 20);
    if (!result.error.empty())
    {
        *error = result.error;
        return false;
    }
    if (result.status < 200 || result.status >= 300)
    {
        *error = "封面返回了 HTTP " + number(result.status);
        return false;
    }
    *bytes = std::move(result.body);
    return true;
}

} // namespace hui::fnos

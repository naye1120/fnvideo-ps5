// ps5-homebrew-ui - Blocking HTTP on libcurl (console build); inert on the PC
// snapshot host, which has no libcurl and no network.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnos/http.hpp"

#include <cstdio>

#ifdef HUI_PS5

#include "fnos/console_curl.h"
#include "platform/ps5/system.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

namespace hui::fnos
{
namespace

{

struct Sink
{
    std::string *body = nullptr;
    std::size_t cap = 0;
    bool overflow = false;
};

std::size_t write_body(char *data, std::size_t size, std::size_t items, void *user)
{
    auto *sink = static_cast<Sink *>(user);
    const std::size_t bytes = size * items;
    if (sink->body->size() + bytes > sink->cap)
    {
        sink->overflow = true;
        return 0; // aborts the transfer with CURLE_WRITE_ERROR
    }
    sink->body->append(data, bytes);
    return bytes;
}

std::size_t write_append(char *data, std::size_t size, std::size_t items, void *user)
{
    auto *body = static_cast<std::string *>(user);
    body->append(data, size * items);
    return size * items;
}

// Fills a fixed buffer from the response; the caller sees the shortfall.
std::size_t write_fixed(char *data, std::size_t size, std::size_t items, void *user)
{
    auto *cursor = static_cast<std::pair<char *, std::size_t> *>(user);
    const std::size_t bytes = size * items;
    const std::size_t take = bytes < cursor->second ? bytes : cursor->second;
    std::memcpy(cursor->first, data, take);
    cursor->first += take;
    cursor->second -= take;
    return bytes;
}

// Collects response headers so the Range probe can read the total length out
// of Content-Range, which curl's easy interface does not expose.
std::size_t write_headers(char *data, std::size_t size, std::size_t items, void *user)
{
    auto *headers = static_cast<std::string *>(user);
    headers->append(data, size * items);
    return size * items;
}

// "bytes 0-0/12345" -> 12345; -1 when the header is absent.
long long total_from_range(const std::string &response_headers)
{
    const std::size_t at = response_headers.find("Content-Range:");
    if (at == std::string::npos)
        return -1;
    const std::size_t slash = response_headers.find('/', at);
    if (slash == std::string::npos)
        return -1;
    return std::strtoll(response_headers.c_str() + slash + 1, nullptr, 10);
}

// The authority of a URL, for the one decision that matters on a redirect:
// whether the session cookie may travel with the request.
std::string host_of(const std::string &url)
{
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return {};
    const std::size_t start = scheme + 3;
    const std::size_t end = url.find_first_of("/?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Turns a Location value (absolute or relative) into an absolute URL.
std::string absolute_url(const std::string &base, const std::string &location)
{
    if (location.compare(0, 7, "http://") == 0 || location.compare(0, 8, "https://") == 0)
        return location;
    const std::size_t scheme = base.find("://");
    if (scheme == std::string::npos)
        return location;
    if (!location.empty() && location[0] == '/')
    {
        const std::size_t authority_end = base.find_first_of("/?#", scheme + 3);
        return base.substr(0, authority_end == std::string::npos ? base.size() : authority_end) +
               location;
    }
    const std::size_t cut = base.find_last_of('/');
    return (cut == std::string::npos ? base : base.substr(0, cut + 1)) + location;
}

// Common options for every request made here.
//
// TLS verification is off on purpose: fnOS serves a self-signed certificate
// unless the owner installed their own, and a homebrew title has no trust
// store to check it against, so requiring verification would leave the app
// unable to sign in at all. What that costs is a machine on the same network
// being able to read the session cookie - acceptable for a personal music
// library on a LAN, and nothing privileged goes over this connection.
//
// Redirects are followed by hand rather than by curl, because the cookie must
// survive a hop to the NAS' own reverse proxy but be dropped when the hop
// leaves it (mounted cloud drives redirect to their CDN).
void apply_common(CURL *easy, int timeout_seconds)
{
    console_curl_setup(easy); // SO_NBIO on every socket, plus the resolver glue
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    if (timeout_seconds > 0)
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
    // A socket that connected and then stopped answering (a NAS that went away
    // mid-transfer, a proxy hop that hangs) must not sit on the one serialised
    // transfer gate forever: every later detail/play request queues behind it,
    // the series page never gets its episodes, and a 播放 press looks stuck on
    // 正在打开 with no play begin. CURLOPT_TIMEOUT caps the total for timed
    // requests, but a stalled-but-alive socket can outrun the wall-clock check,
    // so LOW_SPEED also aborts any transfer that receives under a byte per
    // second for that window. The long-lived stream passes timeout_seconds == 0
    // - it must not carry a *total* cap, but a 25 s gap in bytes is a dead
    // stream, not a slow one, so it still releases the gate and recovers.
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME,
                     timeout_seconds > 0 ? static_cast<long>(timeout_seconds) : 25L);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "PS5-FnMusic/1.0 (homebrew)");
}

curl_slist *append_headers(const std::vector<std::string> &headers)
{
    curl_slist *list = nullptr;
    for (const std::string &header : headers)
        list = curl_slist_append(list, header.c_str());
    return list;
}

// One transfer at a time. The console's net layer is not the POSIX one its
// headers pretend to be (see console_curl.c: the resolver, fcntl and socket
// mode all have to be shimmed), and a transfer is where a thread reaches it.
// The first moment two threads are on the wire together is a song chosen while a
// cover is still arriving. The stream ring holds about 1.4 s of audio, so a
// small request that lands in between is never heard.
std::mutex &transfer_gate()
{
    static std::mutex gate;
    return gate;
}

// Locks the transfer gate and, when a transfer holds it unusually long or
// queued long to get it, says so. This is the probe that names the stage that
// wedges the serialised network path - the same hang that starves 选集 and
// playback - so the fix targets the stalling transfer, not guesswork.
struct GateHold
{
    explicit GateHold(const char *label)
        : lock(transfer_gate(), std::defer_lock), what(label)
    {
        const std::int64_t begin = sys::monotonic_us();
        lock.lock();
        waited_us = sys::monotonic_us() - begin;
        start_us = sys::monotonic_us();
    }
    ~GateHold()
    {
        const std::int64_t held_us = sys::monotonic_us() - start_us;
        if (held_us > 5000000 || waited_us > 3000000)
        {
            sys::log("[HUI] gate %s waited=%lldms held=%lldms", what,
                     static_cast<long long>(waited_us / 1000),
                     static_cast<long long>(held_us / 1000));
        }
    }
    std::unique_lock<std::mutex> lock;
    const char *what;
    std::int64_t waited_us = 0;
    std::int64_t start_us = 0;
};

void drop_cookie(std::vector<std::string> &headers)
{
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [](const std::string &header) {
                                     return header.compare(0, 7, "Cookie:") == 0 ||
                                            header.compare(0, 7, "cookie:") == 0;
                                 }),
                  headers.end());
}

} // namespace

bool http_init()
{
    static const bool initialised = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return initialised;
}

std::string http_escape(const std::string &value)
{
    char *escaped = curl_easy_escape(nullptr, value.c_str(), static_cast<int>(value.size()));
    if (escaped == nullptr)
        return value;
    std::string result(escaped);
    curl_free(escaped);
    return result;
}

HttpResult http_request(const std::string &method, const std::string &url,
                        std::vector<std::string> headers, const std::string &body,
                        std::size_t max_bytes, int timeout_seconds)
{
    HttpResult result;
    std::string verb = method;
    std::string current = url;
    for (int hop = 0; hop <= 5; ++hop)
    {
        CURL *easy = curl_easy_init();
        if (easy == nullptr)
        {
            result.error = "curl_easy_init failed";
            return result;
        }
        curl_slist *list = append_headers(headers);
        Sink sink;
        sink.body = &result.body;
        sink.cap = max_bytes;
        apply_common(easy, timeout_seconds);
        curl_easy_setopt(easy, CURLOPT_URL, current.c_str());
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &sink);
        if (verb == "POST")
        {
            curl_easy_setopt(easy, CURLOPT_POST, 1L);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
        const GateHold hold("http");
        const CURLcode code = curl_easy_perform(easy);
        long status = 0;
        char *redirect = nullptr;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(easy, CURLINFO_REDIRECT_URL, &redirect);
        curl_easy_cleanup(easy);
        curl_slist_free_all(list);
        result.status = status;
        if (code == CURLE_WRITE_ERROR && sink.overflow)
        {
            result.error = "response larger than the limit";
            return result;
        }
        if (code != CURLE_OK)
        {
            result.error = curl_easy_strerror(code);
            return result;
        }
        if (status < 300 || status >= 400 || redirect == nullptr)
            return result;
        current = absolute_url(current, redirect);
        result.location = current;
        result.body.clear();
        if (status != 307 && status != 308)
            verb = "GET"; // 301/302/303 are answered with a GET
        if (host_of(current) != host_of(url))
            drop_cookie(headers);
    }
    result.error = "too many redirects";
    return result;
}

std::string http_resolve_redirects(const std::string &url, std::vector<std::string> headers,
                                   int timeout_seconds)
{
    std::string current = url;
    for (int hop = 0; hop < 5; ++hop)
    {
        CURL *easy = curl_easy_init();
        if (easy == nullptr)
            return current;
        curl_slist *list = append_headers(headers);
        std::string discarded;
        apply_common(easy, timeout_seconds);
        curl_easy_setopt(easy, CURLOPT_URL, current.c_str());
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list);
        curl_easy_setopt(easy, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_append);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &discarded);
        const GateHold hold("http");
        const CURLcode code = curl_easy_perform(easy);
        char *redirect = nullptr;
        curl_easy_getinfo(easy, CURLINFO_REDIRECT_URL, &redirect);
        curl_easy_cleanup(easy);
        curl_slist_free_all(list);
        if (code != CURLE_OK || redirect == nullptr)
            return current; // no hop, or the probe failed: the original URL plays
        const std::string next = absolute_url(current, redirect);
        if (host_of(next) != host_of(url))
            drop_cookie(headers);
        current = next;
    }
    return url; // a loop: better the original than nothing
}

ByteReader::~ByteReader()
{
    close();
}

void ByteReader::close()
{
    if (easy_ != nullptr)
        curl_easy_cleanup(static_cast<CURL *>(easy_));
    easy_ = nullptr;
    size_ = -1;
    url_.clear();
    headers_.clear();
    error_.clear();
}

bool ByteReader::open(const std::string &url, std::vector<std::string> headers, std::string *error)
{
    close();
    CURL *easy = curl_easy_init();
    if (easy == nullptr)
    {
        if (error != nullptr)
            *error = "curl_easy_init failed";
        return false;
    }
    curl_slist *list = append_headers(headers);
    std::string discarded;
    response_headers_.clear();
    apply_common(easy, 0); // a long-lived stream must not carry a total timeout
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(easy, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_append);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &discarded);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_headers);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &response_headers_);
    // Two markers around the stream's first wire call: the ladder on the next
    // launch says whether the second network thread got as far as curl at all.
    sys::log("[HUI] stream open start");
    CURLcode code;
    {
        const GateHold hold("http");
        code = curl_easy_perform(easy);
    }
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(list);
    sys::log("[HUI] stream open status=%d", static_cast<int>(status));
    if (code != CURLE_OK)
    {
        curl_easy_cleanup(easy);
        if (error != nullptr)
            *error = curl_easy_strerror(code);
        return false;
    }
    if (status != 200 && status != 206)
    {
        curl_easy_cleanup(easy);
        if (error != nullptr)
            *error = "stream refused: HTTP " + std::to_string(status);
        return false;
    }
    long long total = total_from_range(response_headers_);
    if (total < 0)
    {
        curl_off_t length = 0;
        curl_easy_getinfo(easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
        // A 206 without Content-Range, or a 200 that does not say its length,
        // leaves the size unknown; the reader then asks for open-ended ranges
        // and stops when the server closes.
        total = (status == 200 && length > 0) ? static_cast<long long>(length) : -1;
    }
    easy_ = easy;
    url_ = url;
    headers_ = std::move(headers);
    size_ = total;
    return true;
}

std::size_t ByteReader::read(std::uint64_t offset, void *out, std::size_t count)
{
    if (easy_ == nullptr || count == 0)
        return 0;
    // Bytes past the end are the file running out, not a transfer that failed.
    // Asking for them built the range "12345-12344", the server answered 416,
    // and the decoder thread reported that as a broken network the moment a
    // song reached its last second: the pill said 网络错误 and the queue never
    // moved on to the next track.
    if (size_ > 0 && offset >= static_cast<std::uint64_t>(size_))
        return 0;
    auto *easy = static_cast<CURL *>(easy_);
    char range[64];
    // Always a bounded range, even when the total is unknown: this reader
    // stops filling the moment its request is satisfied, which aborts the
    // transfer. Aborting an open-ended response leaves the kept-alive
    // connection in a state the next request on the same handle has to guess
    // about, and the console's socket layer is not the POSIX one libcurl was
    // built for. Asking for exactly `count` bytes ends the response by itself.
    const unsigned long long last = size_ > 0 && offset + count > static_cast<std::uint64_t>(size_)
                                        ? static_cast<unsigned long long>(size_) - 1
                                        : static_cast<unsigned long long>(offset + count - 1);
    std::snprintf(range, sizeof(range), "%llu-%llu", static_cast<unsigned long long>(offset), last);
    curl_slist *list = append_headers(headers_);
    std::pair<char *, std::size_t> cursor{static_cast<char *>(out), count};
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(easy, CURLOPT_RANGE, range);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_fixed);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &cursor);
    // Re-arm both sinks on every transfer. The header one is not decoration:
    // open() pointed it at a stack string that died when open() returned, so
    // this second transfer wrote each response header line over a dead stack
    // object — which is how the decoder thread died inside curl_easy_perform.
    response_headers_.clear();
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_headers);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &response_headers_);
    const bool brief = log_left_ > 0;
    if (brief)
    {
        --log_left_;
        sys::log("[HUI] stream get %s of %lld", range, size_);
    }
    CURLcode code;
    {
        const GateHold hold("http");
        code = curl_easy_perform(easy);
    }
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(list);
    if (brief)
        sys::log("[HUI] stream got %d status=%d code=%d", static_cast<int>(count - cursor.second),
                 static_cast<int>(status), static_cast<int>(code));
    if (code != CURLE_OK)
    {
        error_ = curl_easy_strerror(code);
        return 0;
    }
    if (status >= 400)
    {
        error_ = "HTTP " + std::to_string(status);
        return 0;
    }
    return count - cursor.second;
}

} // namespace hui::fnos

#else // the PC snapshot host: no libcurl, no sockets.

namespace
{

// The preview host answers the music API out of this library, in the same
// envelope the console reads, so the API client has one code path and the
// snapshot pictures carry real Chinese titles.

struct DemoTrack
{
    const char *guid;
    const char *title;
    const char *artist;
    const char *album;
    const char *genre;
    int duration; // seconds
    int favorite; // 1 or 0
};

const DemoTrack kTracks[] = {
    {"demo-track-1", "月光下的凤尾竹", "中央民族乐团", "云南印象", "民乐", 245, 0},
    {"demo-track-2", "山鬼", "沪上琴社", "楚辞", "古琴", 312, 1},
    {"demo-track-3", "沧海一声笑", "黄霑", "笑傲江湖", "影视原声", 198, 0},
    {"demo-track-4", "茉莉花", "江苏民歌团", "江南民歌", "民乐", 264, 0},
    {"demo-track-5", "二泉映月", "华彦钧", "二泉", "二胡", 421, 1},
    {"demo-track-6", "十面埋伏", "琵琶谱", "琵琶独奏", "琵琶", 512, 0},
    {"demo-track-7", "平湖秋月", "广东音乐社", "西湖十景", "民乐", 226, 0},
    {"demo-track-8", "梅花三弄", "古琴研究会", "琴谱集成", "古琴", 388, 0},
};

bool pick(const DemoTrack &track, const std::string &text)
{
    if (text.empty() || text == "全部")
        return true;
    return std::string(track.title).find(text) != std::string::npos ||
           std::string(track.artist).find(text) != std::string::npos ||
           std::string(track.album).find(text) != std::string::npos;
}

std::string tracks_json(const std::string &text, bool favorites_only)
{
    std::string out = R"({"code":0,"msg":"","data":{"total":0,"list":[)";
    std::size_t shown = 0;
    for (const DemoTrack &track : kTracks)
    {
        if (favorites_only && track.favorite == 0)
            continue;
        if (!pick(track, text))
            continue;
        char row[512];
        const int written = std::snprintf(
            row, sizeof(row),
            R"(%s{"guid":"%s","title":"%s","artists":[{"name":"%s"}],"album":{"name":"%s",)"
            R"("trackCount":8},"duration":%d,"isFavorite":%s,"hasLyric":%s,"accessStatus":0})",
            shown == 0 ? "" : ",", track.guid, track.title, track.artist, track.album,
            track.duration, track.favorite != 0 ? "true" : "false",
            track.duration % 3 == 0 ? "true" : "false");
        if (written <= 0)
            break;
        out += row;
        ++shown;
    }
    out += R"(]}})";
    return out;
}

// {"guid":..,"name":..,"<key>":8} rows, which is the shape of the album,
// artist, genre and playlist sections.
std::string group_json(const char *key, const char *suffix, const std::string *names,
                       std::size_t count)
{
    std::string out = R"({"code":0,"msg":"","data":{"total":0,"list":[)";
    for (std::size_t index = 0; index < count; ++index)
    {
        char row[256];
        const int written =
            std::snprintf(row, sizeof(row), R"(%s{"guid":"demo-%s-%zu","name":"%s","%s":8})",
                          index == 0 ? "" : ",", suffix, index, names[index].c_str(), key);
        if (written <= 0)
            break;
        out += row;
    }
    out += R"(]}})";
    return out;
}

bool holds(const std::string &url, const char *needle)
{
    return url.find(needle) != std::string::npos;
}

std::string query_value(const std::string &url, const char *key)
{
    const std::string name = std::string(key) + "=";
    const std::size_t start = url.find(name);
    if (start == std::string::npos)
        return {};
    const std::size_t from = start + name.size();
    const std::size_t stop = url.find('&', from);
    return url.substr(from, stop == std::string::npos ? std::string::npos : stop - from);
}

std::string demo_answer(const std::string &url)
{
    if (holds(url, "/user/password-login"))
        return R"({"code":0,"msg":"","data":{"userToken":"preview-token","user":{"name":"预览"}}})";
    if (holds(url, "/lyric/list"))
        return R"({"code":0,"msg":"","data":{"preferred":"demo-lyric-1","list":[{"guid":"demo-lyric-1",)"
               R"("isLRC":true,"offset":0,"content":"[00:00.00]预览用歌词\n[00:08.00]月出皎兮\n)"
               R"([00:16.00]佼人僚兮\n"}]}})";
    if (holds(url, "/track/playlist-detail/list") || holds(url, "/track/list") ||
        holds(url, "/track/album-detail/list") || holds(url, "/track/artist-detail/list") ||
        holds(url, "/track/genre-detail/list"))
        return tracks_json({}, false);
    if (holds(url, "/favorite-track/list"))
        return tracks_json({}, true);
    if (holds(url, "/search/track"))
        return tracks_json(query_value(url, "q"), false);
    if (holds(url, "/playlist/list"))
    {
        const std::string names[] = {"我的收藏", "睡前古琴", "民族管弦", "工作时的白噪音"};
        return group_json("trackCount", "playlist", names, 4);
    }
    if (holds(url, "/album/list"))
    {
        const std::string names[] = {"云南印象", "楚辞", "笑傲江湖",
                                     "江南民歌", "二泉", "琵琶独奏"};
        return group_json("trackCount", "album", names, 6);
    }
    if (holds(url, "/artist/list"))
    {
        const std::string names[] = {"中央民族乐团", "沪上琴社", "黄霑", "华彦钧"};
        return group_json("trackCount", "artist", names, 4);
    }
    if (holds(url, "/genre/list"))
    {
        const std::string names[] = {"民乐", "古琴", "二胡", "琵琶", "影视原声"};
        return group_json("trackCount", "genre", names, 5);
    }
    return R"({"code":0,"msg":"","data":{"total":0,"list":[]}})";
}

} // namespace

namespace hui::fnos
{

bool http_init()
{
    return true;
}

std::string http_escape(const std::string &value)
{
    return value;
}

HttpResult http_request(const std::string &, const std::string &url,
                        std::vector<std::string> headers, const std::string &, std::size_t, int)
{
    HttpResult result;
    // Preview seam: one token value is refused so the re-login a real NAS does
    // on an expired session has something to answer here. Nothing on a console
    // ever sees this branch.
    for (const std::string &line : headers)
    {
        if (line.find("music-token=preview-stale") != std::string::npos)
        {
            result.status = 401;
            result.body = R"({"code":401,"msg":"token invalid","data":{}})";
            return result;
        }
    }
    if (holds(url, "/static/cover"))
    {
        // No artwork is served to the preview: the UI draws its placeholder.
        result.status = 404;
        result.error = "preview host has no artwork";
        return result;
    }
    result.status = 200;
    result.body = demo_answer(url);
    return result;
}

std::string http_resolve_redirects(const std::string &url, std::vector<std::string>, int)
{
    // Preview seam: a marked stream address answers as if it had been redirected
    // to another host, so the prefetch that caches a resolved address has an
    // observable result here. Nothing on a console ever sees this branch.
    if (url.find("preview-redirect") != std::string::npos)
        return "http://preview.resolved/stream";
    return url;
}

ByteReader::~ByteReader() = default;

void ByteReader::close()
{
}

bool ByteReader::open(const std::string &, std::vector<std::string>, std::string *error)
{
    if (error != nullptr)
        *error = "this build has no network (PC preview)";
    return false;
}

std::size_t ByteReader::read(std::uint64_t, void *, std::size_t)
{
    return 0;
}

} // namespace hui::fnos

#endif

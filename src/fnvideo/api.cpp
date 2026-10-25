// ps5-homebrew-ui - The 飞牛影视 API. See api.hpp for the shape of the data.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnvideo/api.hpp"

#include "fnos/http.hpp"
#include "fnos/json.hpp"
#include "fnvideo/authx.hpp"

#include <cctype>
#include <cstdio>
#include <utility>

namespace hui::fnvideo
{
namespace
{

using fnos::HttpResult;
using fnos::Json;

// A library answer is a few hundred kilobytes of JSON; an artwork is up to a
// megabyte. Both caps are generous enough to never truncate a real answer and
// tight enough that a runaway response cannot exhaust the console's memory.
constexpr std::size_t kMaxJsonBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxImageBytes = 8u * 1024u * 1024u;
constexpr int kTimeoutSeconds = 20;

// The server speaks of one kind of business failure we can act on: an expired
// session. Everything else is a message to show.
constexpr long long kAuthFailed = -2;

std::string json_quote(const std::string &value)
{
    std::string out("\"");
    for (const char ch : value)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                char escape[8];
                std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(ch));
                out += escape;
            }
            else
            {
                out.push_back(ch);
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

// "a=1&b=2" with the values already escaped, which is what goes on the wire.
std::string json_field(const char *key, const std::string &value)
{
    std::string out(json_quote(key));
    out.push_back(':');
    out += json_quote(value);
    return out;
}

std::string json_number_field(const char *key, long long value)
{
    std::string out(json_quote(key));
    out.push_back(':');
    out += std::to_string(value);
    return out;
}

// The first non-empty artwork path out of a node, whichever field the server
// happened to fill: detail calls use `posters`/`backdrops`/`logos` (strings),
// list calls use `poster_list` (an array) or `poster`.
std::string artwork(const Json &node, const char *single, const char *list)
{
    const Json &one = node.member(single);
    if (!one.null())
    {
        const std::string path = one.text();
        if (!path.empty())
        {
            return path;
        }
    }
    const Json &many = node.member(list);
    if (many.array() && many.size() > 0)
    {
        return many.element(0).text();
    }
    return std::string();
}

// /item/{guid} reports genres as an array of stable ids (the server's own
// /tag/genres enum: 1 Action, 4 Comedy, 9 Sci-Fi & Fantasy, …). Those ids never
// change, so the Chinese display names live in this fixed table rather than
// behind a dictionary round-trip; an id we do not recognise is skipped, not
// guessed at. The list row carries no genres at all, so this only fills on the
// detail call.
static const char *genre_zh(int id)
{
    switch (id)
    {
        case 1: return "动作";
        case 2: return "冒险";
        case 3: return "动画";
        case 4: return "喜剧";
        case 5: return "犯罪";
        case 6: return "纪录片";
        case 7: return "剧情";
        case 8: return "家庭";
        case 9: return "科幻奇幻";
        case 10: return "历史";
        case 11: return "恐怖";
        case 12: return "音乐";
        case 13: return "悬疑";
        case 14: return "爱情";
        case 16: return "电视电影";
        case 17: return "惊悚";
        case 18: return "战争政治";
        case 19: return "西部";
        case 30: return "动作冒险";
        case 31: return "儿童";
        case 32: return "新闻";
        case 33: return "真人秀";
        case 35: return "肥皂剧";
        case 36: return "脱口秀";
        case 101: return "运动";
        case 102: return "财经";
        case 103: return "教育";
        case 104: return "生活方式";
        case 105: return "本地";
        case 106: return "其他";
        case 109: return "剧集";
        case 110: return "综艺";
        case 111: return "儿童";
        default: return nullptr;
    }
}

Item parse_item(const Json &node)
{
    Item item;
    item.guid = node.member("guid").text();
    item.title = node.member("title").text();
    item.original_title = node.member("original_title").text();
    item.type = node.member("type").text();
    item.overview = node.member("overview").text();
    item.poster = artwork(node, "posters", "poster_list");
    // A film or a series row carries a single `poster` string (with the pixel
    // size beside it) instead of the arrays the detail call uses; without this
    // fallback every 电影 and 电视剧 card came up with no artwork at all.
    if (item.poster.empty())
    {
        item.poster = node.member("poster").text();
    }
    item.backdrop = artwork(node, "backdrops", "poster_list");
    if (item.backdrop.empty())
    {
        item.backdrop = node.member("backdrop").text();
    }
    item.logo = artwork(node, "logos", "poster_list");
    item.ancestor_guid = node.member("ancestor_guid").text();
    item.ancestor_name = node.member("ancestor_name").text();
    item.ancestor_category = node.member("ancestor_category").text();
    item.parent_guid = node.member("parent_guid").text();
    item.parent_title = node.member("parent_title").text();
    item.single_child_guid = node.member("single_child_guid").text();
    item.release_date = node.member("release_date").text();
    item.vote_average = node.member("vote_average").text();

    // media_stream.resolutions is an array, highest first; the list endpoint
    // fills it, the detail endpoint does too.
    const Json &stream = node.member("media_stream");
    const Json &resolutions = stream.member("resolutions");
    if (resolutions.array() && resolutions.size() > 0)
    {
        item.resolution = resolutions.element(0).text();
    }
    const Json &color_range = stream.member("color_range_type");
    if (color_range.array() && color_range.size() > 0)
    {
        item.color_range = color_range.element(0).text();
    }
    const Json &audio_type = stream.member("audio_type");
    if (audio_type.array() && audio_type.size() > 0)
    {
        item.audio_type = audio_type.element(0).text();
    }
    // content_ratings and production_countries are only on the detail answer;
    // the list row leaves them absent, which is fine - the chips are skipped.
    item.content_rating = node.member("content_ratings").text();
    const Json &countries = node.member("production_countries");
    if (countries.array())
    {
        for (std::size_t i = 0; i < countries.size(); ++i)
        {
            if (i != 0)
            {
                item.countries.push_back(',');
            }
            item.countries += countries.element(i).text();
        }
    }

    // genres is a detail-only array of stable ids; resolve them to Chinese here
    // so the detail page can show "动作 · 冒险 · 喜剧". A list row has none, so
    // genres_text stays empty and the poster wall falls back to the year.
    const Json &genres = node.member("genres");
    if (genres.array())
    {
        for (std::size_t i = 0; i < genres.size(); ++i)
        {
            const int id = static_cast<int>(genres.element(i).number(-1));
            const char *zh = genre_zh(id);
            if (zh == nullptr)
                continue;
            if (!item.genres_text.empty())
                item.genres_text += " \u00b7 ";
            item.genres_text += zh;
        }
    }

    item.runtime = node.member("runtime").number(0);
    item.duration = node.member("duration").number(0);
    item.watched_ts = node.member("watched_ts").number(0);
    item.is_favorite = static_cast<int>(node.member("is_favorite").number(0));
    item.watched = static_cast<int>(node.member("watched").number(0));
    item.can_play = static_cast<int>(node.member("can_play").number(0));
    item.season_number = static_cast<int>(node.member("season_number").number(0));
    item.episode_number = static_cast<int>(node.member("episode_number").number(0));
    item.number_of_episodes = static_cast<int>(node.member("number_of_episodes").number(0));
    item.local_number_of_episodes =
        static_cast<int>(node.member("local_number_of_episodes").number(0));
    item.number_of_seasons = static_cast<int>(node.member("number_of_seasons").number(0));
    item.local_number_of_seasons =
        static_cast<int>(node.member("local_number_of_seasons").number(0));

    // The list endpoint leaves `title` empty for some directory rows and puts
    // the readable name in `tv_title`/`parent_title`; the web client does the
    // same fallback.
    if (item.title.empty())
    {
        item.title = node.member("tv_title").text();
    }
    return item;
}

// The `data` member of a successful answer, or null when the field is absent.
bool data_of(const Json &root, Json *data)
{
    *data = root.member("data");
    return !data->null();
}

} // namespace

void Api::set_server(const std::string &base_url)
{
    std::string base(base_url);
    while (!base.empty() && base.back() == '/')
    {
        base.pop_back();
    }
    // Tolerate a pasted API URL: keep the origin only.
    for (const char *suffix : {"/v/api/v1", "/music/api/v1", "/v"})
    {
        const std::string tail(suffix);
        if (base.size() >= tail.size() &&
            base.compare(base.size() - tail.size(), tail.size(), tail) == 0)
        {
            base.erase(base.size() - tail.size());
            break;
        }
    }
    base_ = base;
}

bool Api::login(const std::string &username, const std::string &password, std::string *error)
{
    login_user_ = username;
    login_hash_ = password_digest(password);
    return sign_in(login_user_, login_hash_, error);
}

bool Api::sign_in(const std::string &username, const std::string &password_hash, std::string *error)
{
    if (base_.empty())
    {
        if (error != nullptr)
        {
            *error = "no server configured";
        }
        return false;
    }

    std::string body("{");
    body += json_field("username", username);
    body += ",";
    body += json_field("password", password_hash);
    body += ",";
    body += json_field("app_name", "trimemedia-web");
    body += "}";

    // Login is the one call on /v/api/v2. It still wants a signature.
    const std::string path("/v/api/v2/user/loginByPassword");
    SignRequest request;
    request.method = "POST";
    request.path = path;
    request.body = body;

    const std::vector<std::string> headers = {"authx: " + authx_header(request),
                                              "Content-Type: application/json",
                                              "Accept: application/json"};
    const HttpResult result =
        fnos::http_request("POST", base_ + path, headers, body, kMaxJsonBytes, kTimeoutSeconds);
    if (!result.error.empty())
    {
        if (error != nullptr)
        {
            *error = result.error;
        }
        return false;
    }

    Json root;
    if (!Json::parse(result.body, &root))
    {
        if (error != nullptr)
        {
            *error = "unreadable login answer";
        }
        return false;
    }
    if (root.member("code").number(0) != 0)
    {
        if (error != nullptr)
        {
            const std::string message = root.member("msg").text();
            *error = message.empty() ? "login refused" : message;
        }
        return false;
    }

    Json data;
    if (!data_of(root, &data))
    {
        if (error != nullptr)
        {
            *error = "login answer carried no token";
        }
        return false;
    }
    token_ = data.member("token").text();
    if (token_.empty())
    {
        if (error != nullptr)
        {
            *error = "login answer carried no token";
        }
        return false;
    }
    return true;
}

bool Api::ask(const std::string &method, const std::string &path, const std::string &body_json,
              Json *data, std::string *error)
{
    if (base_.empty())
    {
        if (error != nullptr)
        {
            *error = "no server configured";
        }
        return false;
    }

    // Two attempts at most: a session the server has forgotten is signed in
    // again and the same call repeated. Anything else is reported as it came.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const std::string full_path = "/v/api/v1" + path;
        SignRequest request;
        request.method = method;
        request.path = full_path;
        request.body = body_json;

        std::vector<std::string> headers = {"authx: " + authx_header(request),
                                            "Accept: application/json"};
        if (!token_.empty())
        {
            headers.push_back("Authorization: " + token_);
        }
        if (!body_json.empty())
        {
            headers.push_back("Content-Type: application/json");
        }

        const HttpResult result = fnos::http_request(method, base_ + full_path, headers, body_json,
                                                     kMaxJsonBytes, kTimeoutSeconds);
        if (!result.error.empty())
        {
            if (error != nullptr)
            {
                *error = result.error;
            }
            return false;
        }

        Json root;
        if (!Json::parse(result.body, &root))
        {
            if (error != nullptr)
            {
                *error = "unreadable answer";
            }
            return false;
        }

        const long long code = root.member("code").number(0);
        if (code == 0)
        {
            if (data != nullptr)
            {
                *data = root.member("data");
            }
            return true;
        }

        if (code == kAuthFailed && attempt == 0 && !login_hash_.empty() &&
            sign_in(login_user_, login_hash_, error))
        {
            continue; // the renewed token is used by the second attempt
        }

        if (error != nullptr)
        {
            const std::string message = root.member("msg").text();
            *error = message.empty() ? "request refused" : message;
        }
        return false;
    }

    if (error != nullptr)
    {
        *error = "request refused";
    }
    return false;
}

bool Api::libraries(LibraryPage *out, std::string *error)
{
    Json data;
    if (!ask("GET", "/mediadb/list", std::string(), &data, error))
    {
        return false;
    }
    if (!data.array())
    {
        if (error != nullptr)
        {
            *error = "library list was not an array";
        }
        return false;
    }

    out->items.clear();
    out->items.reserve(data.size());
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        const Json &node = data.element(i);
        Library library;
        library.guid = node.member("guid").text();
        library.title = node.member("title").text();
        library.category = node.member("category").text();
        library.poster = artwork(node, "poster", "posters");
        out->items.push_back(std::move(library));
    }
    out->total = static_cast<long long>(out->items.size());
    return true;
}

bool Api::items(const std::string &library_guid, const std::vector<std::string> &types, int page,
                int page_size, ItemPage *out, std::string *error)
{
    std::string body("{");
    if (!types.empty())
    {
        // `tags.type` is what keeps a library's answer to its top-level rows.
        // Asking by `ancestor_guid` alone returns every season, episode and
        // file underneath as well - 1726 rows for the 电视剧 library here,
        // 7158 for 短剧 - and since they all share one ancestor_guid the wall
        // collapsed the entire library into a single card.
        body += json_quote("tags");
        body += ":{\"type\":[";
        for (std::size_t i = 0; i < types.size(); ++i)
        {
            if (i != 0)
            {
                body.push_back(',');
            }
            body += json_quote(types[i]);
        }
        body += "]},";
    }
    if (!library_guid.empty())
    {
        body += json_field("ancestor_guid", library_guid);
        body.push_back(',');
    }
    body += json_number_field("page", page);
    body.push_back(',');
    body += json_number_field("page_size", page_size);
    body.push_back('}');

    Json data;
    if (!ask("POST", "/item/list", body, &data, error))
    {
        return false;
    }

    out->items.clear();
    out->total = data.member("total").number(0);
    const Json &list = data.member("list");
    if (!list.array())
    {
        return true; // an empty library answers with no list at all
    }
    out->items.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        out->items.push_back(parse_item(list.element(i)));
    }
    return true;
}

bool Api::children(const std::string &parent_guid, int page, int page_size, ItemPage *out,
                   std::string *error)
{
    std::string body("{");
    body += json_field("parent_guid", parent_guid);
    body.push_back(',');
    body += json_number_field("page", page);
    body.push_back(',');
    body += json_number_field("page_size", page_size);
    body.push_back('}');

    Json data;
    if (!ask("POST", "/item/list", body, &data, error))
    {
        return false;
    }
    out->items.clear();
    out->total = data.member("total").number(0);
    const Json &list = data.member("list");
    if (!list.array())
    {
        return true;
    }
    out->items.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        out->items.push_back(parse_item(list.element(i)));
    }
    return true;
}

bool Api::seasons(const std::string &tv_guid, std::vector<Item> *out, std::string *error)
{
    out->clear();
    Json data;
    if (!ask("GET", "/season/list/" + tv_guid, std::string(), &data, error))
    {
        return false;
    }
    if (!data.array())
    {
        return true; // a film or an unparsed folder answers with null
    }
    out->reserve(data.size());
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        out->push_back(parse_item(data.element(i)));
    }
    return true;
}

bool Api::item(const std::string &guid, Item *out, std::string *error)
{
    Json data;
    if (!ask("GET", "/item/" + guid, std::string(), &data, error))
    {
        return false;
    }
    *out = parse_item(data);
    return true;
}

bool Api::files(const std::string &item_guid, std::vector<FileEntry> *out, std::string *error)
{
    Json data;
    if (!ask("GET", "/media/itemfile/" + item_guid, std::string(), &data, error))
    {
        return false;
    }
    out->clear();
    if (!data.array())
    {
        return true;
    }
    out->reserve(data.size());
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        const Json &node = data.element(i);
        FileEntry entry;
        entry.media_guid = node.member("media_guid").text();
        entry.item_guid = node.member("item_guid").text();
        entry.filename = node.member("filename").text();
        entry.file_path = node.member("file_path").text();
        entry.duration = node.member("duration").number(0);
        out->push_back(std::move(entry));
    }
    return true;
}

bool Api::streams(const std::string &item_guid, Item *item, std::string *error)
{
    Json data;
    if (!ask("GET", "/stream/list/" + item_guid, std::string(), &data, error))
    {
        return false;
    }
    // Any audio / subtitle stream whose language is Chinese flips the matching
    // chip. The server uses ISO 639-2 "chi" here, but "zho" and "zh-*" show up
    // in some rips, so all three prefixes count.
    const auto chinese = [](const std::string &language)
    {
        return language == "chi" || language == "zho" || language.rfind("chi", 0) == 0 ||
               language.rfind("zh", 0) == 0;
    };
    auto scan = [&data, &chinese](const char *key)
    {
        const Json &list = data.member(key);
        if (!list.array())
        {
            return false;
        }
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            if (chinese(list.element(i).member("language").text()))
            {
                return true;
            }
        }
        return false;
    };
    item->chi_audio = scan("audio_streams");
    item->chi_subtitle = scan("subtitle_streams");

    // The file-level media guid is the first file's; online subtitle search and
    // download key on it, so the detail page has to carry it forward. mediasrv
    // 0.8.43 names that field plain `guid` inside files[] (no media_guid member
    // there - the streams below do carry one), so both spellings are read.
    const Json &files = data.member("files");
    if (files.array() && files.size() > 0)
    {
        item->media_guid = files.element(0).member("media_guid").text();
        if (item->media_guid.empty())
        {
            item->media_guid = files.element(0).member("guid").text();
        }
        // The volume path behind that file: the local-subtitle browser lists
        // the folder it lives in.
        item->file_path = files.element(0).member("path").text();
    }

    // Every subtitle stream, embedded or external. The player surfaces the
    // external, non-bitmap ones so a text file that lives beside the video -
    // never inside the container FFmpeg opens - can still be chosen and drawn.
    const Json &subs = data.member("subtitle_streams");
    item->subtitle_streams.clear();
    if (subs.array())
    {
        for (std::size_t i = 0; i < subs.size(); ++i)
        {
            const Json &node = subs.element(i);
            SubtitleStreamInfo info;
            info.guid = node.member("guid").text();
            info.media_guid = node.member("media_guid").text();
            info.language = node.member("language").text();
            info.title = node.member("title").text();
            info.format = node.member("format").text();
            info.codec = node.member("codec_name").text();
            info.external = node.member("is_external").number(0) != 0;
            info.bitmap = node.member("is_bitmap").number(0) != 0;
            if (!info.guid.empty())
            {
                item->subtitle_streams.push_back(std::move(info));
            }
        }
    }
    // Every audio stream. Same shape as the subtitle list; `index` is the
    // position among audio streams, which lines up with the player's own audio
    // ordinal so a track chosen on the detail page can be applied on open.
    const Json &auds = data.member("audio_streams");
    item->audio_streams.clear();
    if (auds.array())
    {
        for (std::size_t i = 0; i < auds.size(); ++i)
        {
            const Json &node = auds.element(i);
            AudioStreamInfo info;
            info.language = node.member("language").text();
            info.title = node.member("title").text();
            info.codec = node.member("codec_name").text();
            info.channel_layout = node.member("channel_layout").text();
            info.channels = static_cast<int>(node.member("channels").number(0));
            info.index = static_cast<int>(item->audio_streams.size());
            info.def = node.member("is_default").number(0) != 0;
            item->audio_streams.push_back(std::move(info));
        }
    }
    return true;
}

bool Api::subtitle_content(const std::string &subtitle_guid, std::string *bytes,
                           std::string *error)
{
    if (subtitle_guid.empty())
    {
        if (error != nullptr)
        {
            *error = "no subtitle guid";
        }
        return false;
    }
    // Token only, no signature - the endpoint hands back the raw file bytes
    // (application/octet-stream, sometimes a UTF-8 BOM), not a JSON envelope.
    const std::string url = base_ + "/v/api/v1/subtitle/dl/" + subtitle_guid;
    const HttpResult result =
        fnos::http_request("GET", url, media_headers(), std::string(), kMaxImageBytes, kTimeoutSeconds);
    if (!result.error.empty())
    {
        if (error != nullptr)
        {
            *error = result.error;
        }
        return false;
    }
    if (result.status < 200 || result.status >= 300 || result.body.empty())
    {
        if (error != nullptr)
        {
            *error = "subtitle file unavailable";
        }
        return false;
    }
    *bytes = std::move(result.body);
    return true;
}

bool Api::subtitle_search(const std::string &media_guid, const std::string &lan,
                          std::vector<SubtitleHit> *out, std::string *error)
{
    std::string body("{");
    body += json_field("lan", lan);
    body += ",";
    body += json_field("media_guid", media_guid);
    body += "}";

    Json data;
    if (!ask("POST", "/subtitle/search", body, &data, error))
    {
        return false;
    }
    out->clear();
    const Json &list = data.member("subtitles");
    if (!list.array())
    {
        return true;
    }
    for (std::size_t i = 0; i < list.size(); ++i)
    {
        const Json &node = list.element(i);
        SubtitleHit hit;
        hit.filename = node.member("filename").text();
        hit.trim_id = node.member("trim_id").text();
        hit.source = node.member("source").text();
        hit.format = node.member("format").text();
        hit.download = node.member("download").number(0);
        if (!hit.trim_id.empty())
        {
            out->push_back(std::move(hit));
        }
    }
    return true;
}

bool Api::subtitle_download(const std::string &media_guid, const std::string &trim_id,
                           std::string *error)
{
    std::string body("{");
    body += json_field("media_guid", media_guid);
    body += ",";
    body += json_field("trim_id", trim_id);
    body += ",";
    body += "\"sync_download\":1";
    body += "}";
    return ask("POST", "/subtitle/download", body, nullptr, error);
}

bool Api::subtitle_mark(const std::string &media_guid, const std::vector<std::string> &filepaths,
                        std::string *error)
{
    std::string body("{");
    body += json_field("media_guid", media_guid);
    body += ",\"filepaths\":[";
    for (std::size_t i = 0; i < filepaths.size(); ++i)
    {
        if (i != 0)
        {
            body += ",";
        }
        body += json_quote(filepaths[i]);
    }
    body += "]}";
    return ask("PUT", "/subtitle/mark", body, nullptr, error);
}

namespace
{

bool subtitle_extension(const std::string &name)
{
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size())
        return false;
    std::string ext = name.substr(dot + 1);
    for (char &ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext == "srt" || ext == "ass" || ext == "ssa" || ext == "vtt" || ext == "sub" ||
           ext == "idx";
}

} // namespace

bool Api::folder_files(const std::string &folder, std::vector<SubtitleHit> *out,
                       std::string *error)
{
    out->clear();
    if (folder.empty())
    {
        if (error != nullptr)
            *error = "还没有可浏览的文件夹";
        return false;
    }
    // List one folder exactly the way the web's "add NAS subtitle" tree does:
    // POST /server/path {path} answers every entry (filename, is_dir,
    // file_size) under an absolute /vol<N>/<uid> directory, signed with the
    // same authx + token as every other call. Keep every subdirectory so the
    // picker can drill in, and only subtitle-extension files so the list stays
    // readable. Dirs first, then files.
    const std::string body = "{" + json_field("path", folder) + "}";
    Json data;
    if (!ask("POST", "/server/path", body, &data, error))
        return false;
    if (!data.array())
        return true;
    std::vector<SubtitleHit> dirs;
    std::vector<SubtitleHit> subs;
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        const Json &node = data.element(i);
        const std::string name = node.member("filename").text();
        if (name.empty())
            continue;
        if (node.member("is_dir").flag())
        {
            SubtitleHit hit;
            hit.filename = name;
            hit.is_dir = true;
            hit.source = "文件夹";
            dirs.push_back(std::move(hit));
            continue;
        }
        const std::size_t dot = name.find_last_of('.');
        if (!subtitle_extension(name))
            continue;
        SubtitleHit hit;
        hit.filename = name;
        hit.source = "NAS";
        hit.format = dot == std::string::npos ? std::string() : name.substr(dot + 1);
        for (char &ch : hit.format)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        subs.push_back(std::move(hit));
    }
    for (SubtitleHit &hit : dirs)
        out->push_back(std::move(hit));
    for (SubtitleHit &hit : subs)
        out->push_back(std::move(hit));
    return true;
}

bool Api::continue_watching(ItemPage *out, std::string *error)
{
    Json data;
    if (!ask("GET", "/play/list", std::string(), &data, error))
    {
        return false;
    }
    out->items.clear();
    if (!data.array())
    {
        out->total = 0;
        return true;
    }
    out->items.reserve(data.size());
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        out->items.push_back(parse_item(data.element(i)));
    }
    out->total = static_cast<long long>(out->items.size());
    return true;
}

bool Api::play_info(const std::string &item_guid, PlayInfo *out, std::string *error)
{
    std::string body("{");
    body += json_field("item_guid", item_guid);
    body += "}";

    Json data;
    if (!ask("POST", "/play/info", body, &data, error))
    {
        return false;
    }
    out->media_guid = data.member("media_guid").text();
    out->video_guid = data.member("video_guid").text();
    out->audio_guid = data.member("audio_guid").text();
    out->subtitle_guid = data.member("subtitle_guid").text();
    out->ts = data.member("ts").number(0);
    return true;
}

std::string Api::range_url(const std::string &media_guid, const std::string &play_link) const
{
    std::string url(base_);
    url += "/v/api/v1/media/range/";
    url += media_guid;
    url += "?playlink=";
    url += fnos::http_escape(play_link);
    return url;
}

bool Api::stream_url(const std::string &item_guid, std::string *url, PlayInfo *info,
                     std::string *error)
{
    PlayInfo resolved;
    if (!play_info(item_guid, &resolved, error))
    {
        return false;
    }
    if (resolved.media_guid.empty())
    {
        if (error != nullptr)
        {
            *error = "this item has no playable file";
        }
        return false;
    }

    std::string body("{");
    body += json_field("media_guid", resolved.media_guid);
    body += ",";
    body += json_field("video_guid", resolved.video_guid);
    body += ",";
    body += json_field("audio_guid", resolved.audio_guid);
    body += ",";
    // Tells the server not to spin up a transcode session: the bytes behind
    // the ticket are the original file, which is what this app plays.
    body += "\"without_stream\":true";
    body += "}";

    Json data;
    if (!ask("POST", "/play/play", body, &data, error))
    {
        return false;
    }
    const std::string link = data.member("play_link").text();
    if (link.empty())
    {
        if (error != nullptr)
        {
            *error = "the server issued no play link";
        }
        return false;
    }

    *url = range_url(resolved.media_guid, link);
    if (info != nullptr)
    {
        *info = resolved;
    }
    return true;
}

std::string Api::image_url(const std::string &path, int width) const
{
    if (path.empty())
    {
        return std::string();
    }
    std::string url(base_);
    url += "/v/api/v1/sys/img";
    url += path;
    url += "?w=";
    url += std::to_string(width);
    return url;
}

std::vector<std::string> Api::media_headers() const
{
    if (token_.empty())
    {
        return {};
    }
    return {"Authorization: " + token_};
}

bool Api::fetch_image(const std::string &path, int width, std::string *bytes, std::string *error)
{
    const std::string url = image_url(path, width);
    if (url.empty())
    {
        if (error != nullptr)
        {
            *error = "no artwork path";
        }
        return false;
    }

    const HttpResult result = fnos::http_request("GET", url, media_headers(), std::string(),
                                                 kMaxImageBytes, kTimeoutSeconds);
    if (!result.error.empty())
    {
        if (error != nullptr)
        {
            *error = result.error;
        }
        return false;
    }
    *bytes = result.body;
    return true;
}

} // namespace hui::fnvideo

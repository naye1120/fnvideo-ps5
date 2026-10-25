// ps5-homebrew-ui - The 飞牛影视 library worker. See service.hpp for why.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fnvideo/service.hpp"

#include "core/save_file.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/system.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <utility>

#ifdef HUI_PS5
extern "C" int sceKernelUsleep(unsigned int microseconds);
#else
#include <unistd.h>
#endif

namespace hui::fnvideo
{
namespace
{

// The worker sleeps between jobs. Twenty milliseconds is far below what a
// person notices and far above the cost of taking the lock to look.
constexpr unsigned kIdleMilliseconds = 20;

// Local resume points. Separate from fnvideo.conf on purpose: that file holds
// the owner's NAS credentials and is preserved across deployments, while this
// one is disposable state.
constexpr const char *kProgressPath = "/app0/fnvideo-progress.conf";
// Favourite marks and the card store for locally-watched items. Separate from
// the progress points so a corrupt card line never takes the resume map down
// with it; deleted on a fresh install just the same.
constexpr const char *kStatePath = "/app0/fnvideo-state.conf";
// Writing the whole map is cheap, but not something to do every second while
// a film plays: ten seconds is finer than the OSD's own second and far below
// the cost of a flash write.
constexpr int kProgressStepSeconds = 10;

// The server caps a page at a few hundred rows; asking for this many means a
// library of 241 items is one request and a larger one is a handful.
constexpr int kPageSize = 200;
// A library that never reports a total still has to stop somewhere.
constexpr int kMaxPages = 40;

// The list call is made once for the whole server, so a row that arrives
// without its library's name has to look it up by the ancestor_guid it does
// carry. Both fall back to an empty string, which the caller leaves alone.
std::string library_title(const std::vector<Library> &libraries, const std::string &guid)
{
    for (const Library &library : libraries)
    {
        if (library.guid == guid)
        {
            return library.title;
        }
    }
    return std::string();
}

std::string library_category(const std::vector<Library> &libraries, const std::string &guid)
{
    for (const Library &library : libraries)
    {
        if (library.guid == guid)
        {
            return library.category;
        }
    }
    return std::string();
}

// A folder of short episodes holds a few dozen files; one page covers it and a
// season that somehow holds more is cut rather than paged, because the strip
// has no way to ask for the rest.
constexpr int kEpisodePageSize = 500;

// The container's own subtitle streams are numbered 0..N-1 by the player. The
// external files the server lists beside the video live in a separate ordinal
// namespace above that, so a menu row can name an external subtitle without
// colliding with an embedded stream. subtitle_tracks() mints them and
// select_subtitle_track() recognises the range; nowhere else reads the base.
constexpr int kExternalSubtitleBase = 1000;

// "S01E55.mp4" -> season 1, episode 55. The children of a season are named by
// the server, not numbered, so this is where the strip's 第 n 集 comes from.
// Returns false when the name holds no SxxEyy at all.
bool episode_number_from_name(const std::string &name, int *season, int *episode)
{
    const auto digits_after = [&name](char marker, std::size_t from, int *out)
    {
        for (std::size_t at = from; at + 1 < name.size(); ++at)
        {
            if (std::toupper(static_cast<unsigned char>(name[at])) != marker)
            {
                continue;
            }
            std::size_t end = at + 1;
            int value = 0;
            while (end < name.size() && std::isdigit(static_cast<unsigned char>(name[end])))
            {
                value = value * 10 + (name[end] - '0');
                ++end;
            }
            if (end > at + 1)
            {
                *out = value;
                return true;
            }
        }
        return false;
    };

    int read_season = 0;
    int read_episode = 0;
    if (!digits_after('E', 0, &read_episode))
    {
        return false;
    }
    digits_after('S', 0, &read_season);
    if (season != nullptr)
    {
        *season = read_season;
    }
    if (episode != nullptr)
    {
        *episode = read_episode;
    }
    return true;
}

// How often one poster may be tried before it is left alone. Three is enough
// for a NAS that was restarting or a connection that dropped mid-page, and
// few enough that a file the server cannot serve does not spin the worker.
// Generous enough that a NAS having one slow second while the wall also
// fetches borrowed wrapper posters and backdrops does not lock a tile grey for
// the whole session, but bounded so a path the server truly cannot serve still
// stops spinning the worker.
constexpr int kArtTries = 6;

// The first bytes of a fetched picture, as text: "RIFF....WEBP" for a webp,
// "FFD8FF" written as hex for a jpeg. The log needs it to separate "the server
// sent nothing" from "the server sent a webp this build cannot decode".
std::string magic_of(const std::string &bytes)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    const std::size_t take = std::min<std::size_t>(bytes.size(), 8u);
    for (std::size_t i = 0; i < take; ++i)
    {
        const auto value = static_cast<unsigned char>(bytes[i]);
        if (value >= 0x20 && value < 0x7f)
        {
            out.push_back(static_cast<char>(value));
        }
        else
        {
            out.push_back(kHex[value >> 4]);
            out.push_back(kHex[value & 15]);
        }
    }
    return out;
}

std::string trimmed(const std::string &text)
{
    std::size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'))
    {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

} // namespace

Service &Service::instance()
{
    static Service service;
    return service;
}

Service::~Service()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    if (started_)
    {
        pthread_join(worker_, nullptr);
        started_ = false;
    }
}

void Service::configure(const std::string &server, const std::string &user,
                        const std::string &password)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (configured_ && server == server_ && user == user_ && password == password_)
        {
            return; // nothing changed, keep the library that is already loaded
        }
        server_ = server;
        user_ = user;
        password_ = password;
        configured_ = !server_.empty();
        ++generation_;
        needs_sync_ = true;
        view_ = View();
        view_.configured = configured_;
        view_.status = configured_ ? "正在连接…" : "未配置服务器";
        ++view_.revision;
        detail_ = Detail();
        detail_guid_.clear();
        detail_loaded_.clear();
        // A different server means different posters; the keys are guids and
        // would otherwise look like a hit. The same goes for remembered cards:
        // an old NAS's episode must not be drawn into the new server's 继续观看.
        known_items_.clear();
        art_queue_.clear();
        art_ready_.clear();
        art_known_.clear();
        art_failed_.clear();
        // Stop is outside the lock, but the request epoch and the active-item
        // marker must move now so a ticket the worker already resolved cannot
        // open after the old session was torn down.
        ++play_request_;
        play_pending_ = false;
        play_guid_.clear();
        play_active_guid_.clear();
        play_error_.clear();
    }
    // Outside the lock: stopping joins the player's thread and clears GL
    // textures, and configure() runs on the render thread (from enter()).
    player_.stop();
    art_.clear();
}

void Service::configure_from_file(const std::string &path)
{
    // save::read_file rather than <fstream>: the console has no locale, and
    // iostream drags it in. Same reason the worker does not use <thread>.
    std::string text;
    if (!save::read_file(path, &text, 4096))
    {
        return;
    }

    std::string server;
    std::string user;
    std::string password;
    int edge = 0;
    int decoder = 1; // default hardware; overridden only when the file names it
    bool have_decoder = false;
    float volume = 1.0f;
    float brightness = 1.0f;
    bool have_volume = false;
    bool have_brightness = false;
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t line_end = text.find('\n', at);
        if (line_end == std::string::npos)
        {
            line_end = text.size();
        }
        std::string line = text.substr(at, line_end - at);
        at = line_end + 1;

        // Only a line whose first non-blank character is '#' is a comment. A
        // password may legitimately contain one; stripping from the first '#'
        // anywhere silently truncated it.
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#')
        {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }
        const std::string key = trimmed(line.substr(0, equals));
        const std::string value = trimmed(line.substr(equals + 1));
        if (value.empty())
        {
            continue;
        }
        if (key == "server")
        {
            server = value;
        }
        else if (key == "username")
        {
            user = value;
        }
        else if (key == "password")
        {
            password = value;
        }
        else if (key == "edge")
        {
            edge = std::atoi(value.c_str());
        }
        else if (key == "decoder")
        {
            decoder = std::atoi(value.c_str());
            have_decoder = true;
        }
        else if (key == "volume")
        {
            volume = static_cast<float>(std::atof(value.c_str()));
            have_volume = true;
        }
        else if (key == "brightness")
        {
            brightness = static_cast<float>(std::atof(value.c_str()));
            have_brightness = true;
        }
    }

    // Local progress is loaded before the first play() can ask for it. The
    // file is separate from fnvideo.conf, so a redeploy that preserves the
    // NAS settings still starts from this console's last second.
    load_progress();
    if (have_volume)
    {
        set_volume(volume);
    }
    if (have_brightness)
    {
        set_brightness(brightness);
    }

    if (edge > 0)
    {
        // Before configure(): the tier is a player setting, not part of the
        // session, so it must survive a server that never answers.
        set_play_edge(edge);
    }
    if (have_decoder)
    {
        // The decoder is likewise a player setting that must survive a server
        // that never answers, so it is applied before configure() too.
        set_decoder(decoder);
    }
    if (!server.empty())
    {
        configure(server, user, password);
    }
}

void Service::reconfigure(const std::string &server, const std::string &user)
{
    std::string password;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        password = password_;
    }
    if (!server.empty())
    {
        configure(server, user, password);
    }
}

void Service::load_progress()
{
    if (progress_loaded_)
    {
        return;
    }
    progress_loaded_ = true;
    // The favourite + card store lives in its own file and must load even when
    // the progress file has not been created yet (a first boot where the viewer
    // only starred things).
    load_state();
    std::string text;
    if (!save::read_file(kProgressPath, &text, 256u * 1024u))
    {
        return;
    }
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t line_end = text.find('\n', at);
        if (line_end == std::string::npos)
        {
            line_end = text.size();
        }
        const std::string line = trimmed(text.substr(at, line_end - at));
        at = line_end + 1;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }
        const std::string guid = trimmed(line.substr(0, equals));
        const std::string value = trimmed(line.substr(equals + 1));
        if (guid.empty() || value.empty())
        {
            continue;
        }
        const int seconds = std::atoi(value.c_str());
        if (seconds > 0)
        {
            progress_[guid] = seconds;
            // File line order == recency (oldest first, newest last), because
            // save_progress writes in progress_order_ order.
            if (std::find(progress_order_.begin(), progress_order_.end(), guid) ==
                progress_order_.end())
            {
                progress_order_.push_back(guid);
            }
        }
        else if (seconds == 0)
        {
            // A `=0` line means the viewer played this out on the console.
            // 继续观看 drops it and the detail page starts from the top rather
            // than resuming into the closing credits.
            finished_.insert(guid);
        }
    }
    sys::log("[HUI] local progress entries=%d finished=%d", static_cast<int>(progress_.size()),
             static_cast<int>(finished_.size()));
}

void Service::save_progress()
{
    std::string text = "# 本机播放进度（按条目 guid），可随时删除\n";
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Write newest-last, following progress_order_. Any progress_ entry the
        // order missed (a first save before a play tick) is appended so nothing
        // is lost, but the file's line order is the resume ranking.
        std::unordered_set<std::string> written;
        for (const std::string &guid : progress_order_)
        {
            const auto entry = progress_.find(guid);
            if (entry == progress_.end() || entry->second <= 0)
            {
                continue;
            }
            written.insert(guid);
            text += entry->first;
            text += "=";
            text += std::to_string(entry->second);
            text += "\n";
        }
        for (const auto &entry : progress_)
        {
            if (entry.second <= 0 || written.count(entry.first) != 0)
            {
                continue;
            }
            text += entry.first;
            text += "=";
            text += std::to_string(entry.second);
            text += "\n";
        }
        for (const auto &guid : finished_)
        {
            // A finished film is stored as `guid=0` so a redeploy that keeps
            // this file still remembers not to offer 继续播放 at the credits.
            text += guid;
            text += "=0\n";
        }
    }
    save::write_atomic(kProgressPath, text);
    save_state();
}

void Service::load_state()
{
    if (state_loaded_)
    {
        return;
    }
    state_loaded_ = true;
    std::string text;
    if (!save::read_file(kStatePath, &text, 512u * 1024u))
    {
        sys::log("[HUI] state file absent at %s - favorites start empty", kStatePath);
        return;
    }
    std::size_t at = 0;
    while (at < text.size())
    {
        std::size_t line_end = text.find('\n', at);
        if (line_end == std::string::npos)
        {
            line_end = text.size();
        }
        const std::string line = text.substr(at, line_end - at);
        at = line_end + 1;
        // Fields are tab-separated. `C` is a card record, `F` a favourite.
        const std::size_t tag_end = line.find('\t');
        const std::string tag = tag_end == std::string::npos ? line : line.substr(0, tag_end);
        if (tag != "C" && tag != "F" && tag != "T")
        {
            continue;
        }
        std::vector<std::string> f;
        std::size_t p = tag_end == std::string::npos ? std::string::npos : tag_end + 1;
        while (p != std::string::npos)
        {
            const std::size_t nxt = line.find('\t', p);
            f.push_back(nxt == std::string::npos ? line.substr(p) : line.substr(p, nxt - p));
            p = nxt == std::string::npos ? std::string::npos : nxt + 1;
        }
        if (f.empty() || f[0].empty())
        {
            continue;
        }
        if (tag == "F")
        {
            favorites_[f[0]] = (f.size() > 1 && f[1] != "0");
            continue;
        }
        // A `T` line is a per-item track pick made on the detail page (or
        // mirrored back from the player): guid, audio ordinal, subtitle index.
        // Restoring it here is what keeps the choice after a cold restart
        // instead of silently falling back to the container default.
        if (tag == "T")
        {
            if (f.size() >= 3)
            {
                DetailPref pref;
                pref.audio = std::atoi(f[1].c_str());
                pref.subtitle = std::atoi(f[2].c_str());
                detail_prefs_[f[0]] = pref;
            }
            continue;
        }
        // A card needs its guid + the fields service_item() fills. Anything
        // shorter is a half-written line from an interrupted save: ignore it.
        if (f.size() < 18)
        {
            continue;
        }
        Item item;
        item.guid = f[0];
        item.title = f[1];
        item.ancestor_guid = f[2];
        item.ancestor_name = f[3];
        item.ancestor_category = f[4];
        item.poster = f[5];
        item.backdrop = f[6];
        item.parent_title = f[7];
        item.release_date = f[8];
        item.resolution = f[9];
        item.runtime = std::atoll(f[10].c_str());
        item.duration = std::atoll(f[11].c_str());
        item.season_number = std::atoi(f[12].c_str());
        item.episode_number = std::atoi(f[13].c_str());
        item.local_number_of_episodes = std::atoi(f[14].c_str());
        item.is_favorite = std::atoi(f[15].c_str());
        item.type = f[16];
        item.watched_ts = std::atoll(f[17].c_str());
        known_items_[item.guid] = std::move(item);
    }
    sys::log("[HUI] state favorites=%d cards=%d", static_cast<int>(favorites_.size()),
             static_cast<int>(known_items_.size()));
}

void Service::save_state()
{
    // Sanitize a field: tabs and newlines would split one record across lines.
    const auto clean = [](std::string s)
    {
        for (char &c : s)
        {
            if (c == '\t' || c == '\n' || c == '\r')
            {
                c = ' ';
            }
        }
        return s;
    };
    std::string text = "# 本机收藏与继续观看卡片（guid 制表分隔），可随时删除\n";
    std::string cards;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &entry : favorites_)
        {
            text += "F\t";
            text += clean(entry.first);
            text += "\t";
            text += entry.second ? "1" : "0";
            text += "\n";
        }
        // The viewer's audio / subtitle picks per item. Only guids they actually
        // touched are in the map, so the file stays small; a cold restart reads
        // them straight back into detail_prefs_ and play() seeds from them.
        for (const auto &entry : detail_prefs_)
        {
            text += "T\t";
            text += clean(entry.first);
            text += "\t";
            text += std::to_string(entry.second.audio);
            text += "\t";
            text += std::to_string(entry.second.subtitle);
            text += "\n";
        }
        // Only persist cards the console actually reached a position in, so the
        // file stays small and matches the resume rows we can rebuild.
        for (const auto &pend : progress_)
        {
            const auto known = known_items_.find(pend.first);
            if (known == known_items_.end())
            {
                continue;
            }
            const Item &item = known->second;
            cards += "C\t";
            cards += clean(item.guid);
            cards += "\t";
            cards += clean(item.title);
            cards += "\t";
            cards += clean(item.ancestor_guid);
            cards += "\t";
            cards += clean(item.ancestor_name);
            cards += "\t";
            cards += clean(item.ancestor_category);
            cards += "\t";
            cards += clean(item.poster);
            cards += "\t";
            cards += clean(item.backdrop);
            cards += "\t";
            cards += clean(item.parent_title);
            cards += "\t";
            cards += clean(item.release_date);
            cards += "\t";
            cards += clean(item.resolution);
            cards += "\t";
            cards += std::to_string(item.runtime);
            cards += "\t";
            cards += std::to_string(item.duration);
            cards += "\t";
            cards += std::to_string(item.season_number);
            cards += "\t";
            cards += std::to_string(item.episode_number);
            cards += "\t";
            cards += std::to_string(item.local_number_of_episodes);
            cards += "\t";
            cards += std::to_string(item.is_favorite);
            cards += "\t";
            cards += clean(item.type);
            cards += "\t";
            cards += std::to_string(pend.second); // watched_ts = the local position
            cards += "\n";
        }
    }
    text += cards;
    save::write_atomic(kStatePath, text);
}

bool Service::is_favorite(const std::string &guid) const
{
    if (guid.empty())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = favorites_.find(guid);
    return found != favorites_.end() && found->second;
}

void Service::set_favorite(const std::string &guid, bool favorite)
{
    if (guid.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        favorites_[guid] = favorite;
        ++view_.revision; // the home row and the detail button read the view
    }
    save_state();
}

void Service::note_progress(const std::string &guid, int seconds)
{
    if (guid.empty() || seconds <= 0)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (guid == last_progress_guid_ &&
            std::abs(seconds - last_progress_position_) < kProgressStepSeconds)
        {
            return;
        }
        progress_[guid] = seconds;
        // Newest-last recency for 继续观看 ranking: pull the guid to the back so
        // the film being watched here now always leads the home row.
        const auto ord = std::find(progress_order_.begin(), progress_order_.end(), guid);
        if (ord != progress_order_.end())
        {
            progress_order_.erase(ord);
        }
        progress_order_.push_back(guid);
        last_progress_guid_ = guid;
        last_progress_position_ = seconds;
        // Restarting a film the console previously watched out un-finishes it
        // so 继续观看 offers it again.
        finished_.erase(guid);
        // The home page caches the view by revision; a new resume point must
        // make it copy 继续观看 again when the viewer comes back.
        ++view_.revision;
    }
    save_progress();
}

int Service::local_progress(const std::string &guid) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = progress_.find(guid);
    return found == progress_.end() ? -1 : found->second;
}

bool Service::is_finished(const std::string &guid) const
{
    if (guid.empty())
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_.count(guid) > 0;
}

void Service::mark_finished(const std::string &guid)
{
    if (guid.empty())
    {
        return;
    }
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        changed = finished_.insert(guid).second;
        if (progress_.erase(guid) > 0) changed = true;
        if (last_progress_guid_ == guid)
        {
            last_progress_guid_.clear();
            last_progress_position_ = -1;
        }
        if (changed) ++view_.revision;
    }
    if (changed) save_progress();
}

bool Service::clear_finished(const std::string &guid)
{
    if (guid.empty())
    {
        return false;
    }
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        removed = finished_.erase(guid) > 0;
        if (removed) ++view_.revision;
    }
    if (removed) save_progress();
    return removed;
}

bool Service::configured() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return configured_;
}

void Service::save_credentials(const std::string &server, const std::string &user,
                               const std::string &password)
{
    if (server.empty())
    {
        return;
    }
    // The console's /app0 lists nothing, but a plain write still lands in the
    // app's own folder. Atomic so a crash mid-write never strands a half-file.
    std::string text = "# 飞牛影视 fnOS 服务（本机部署用，不入库）\n";
    text += "server=";
    text += server;
    text += "\n";
    text += "username=";
    text += user;
    text += "\n";
    text += "password=";
    text += password;
    text += "\n";
    text += "edge=";
    text += std::to_string(play_edge_.load(std::memory_order_relaxed));
    text += "\n";
    text += "decoder=";
    text += std::to_string(decoder_.load(std::memory_order_relaxed));
    text += "\n";
    char number[32];
    std::snprintf(number, sizeof(number), "%.3f", volume_.load(std::memory_order_relaxed));
    text += "volume=";
    text += number;
    text += "\n";
    std::snprintf(number, sizeof(number), "%.3f", brightness_.load(std::memory_order_relaxed));
    text += "brightness=";
    text += number;
    text += "\n";
    save::write_atomic("/app0/fnvideo.conf", text);
}

std::string Service::server() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return server_;
}

std::string Service::account() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return user_;
}

void Service::ensure_started()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_)
    {
        return;
    }
    // This is also the one place every launch passes through, whether or not
    // a conf file exists. Loading here means a missing/renamed config cannot
    // silently discard this console's local resume points.
    load_progress();
    if (pthread_create(&worker_, nullptr, &Service::worker_entry, this) == 0)
    {
        started_ = true;
    }
    // A second, dedicated thread for artwork. It is intentionally not tracked by
    // started_ and not joined at shutdown: if a poster decoder ever wedges, the
    // exit must still proceed. When it is healthy it sees stopping_ and returns.
    (void)pthread_create(&art_worker_, nullptr, &Service::art_worker_entry, this);
}

void Service::relogin()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_)
    {
        return;
    }
    needs_sync_ = true;
    view_.busy = true;
    view_.signed_in = false;
    view_.status = "正在连接…";
    ++view_.revision;
}

void Service::logout()
{
    std::string server;
    std::string user;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        password_.clear();
        // A sync that was already in flight must not publish the old session
        // or write its password back after this point.
        ++generation_;
        needs_sync_ = false;
        ++play_request_;
        play_pending_ = false;
        play_guid_.clear();
        play_active_guid_.clear();
        play_error_.clear();
        server = server_;
        user = user_;
        view_ = View();
        view_.configured = configured_;
        view_.signed_in = false;
        view_.status = "已退出登录";
        ++view_.revision;
    }
    player_.stop();
    // Persist the signed-out state. The next launch must not silently log in
    // with the credentials the previous session left in the file.
    save_credentials(server, user, std::string());
}

void *Service::worker_entry(void *self)
{
    static_cast<Service *>(self)->worker_main();
    return nullptr;
}

void *Service::art_worker_entry(void *self)
{
    static_cast<Service *>(self)->art_worker_main();
    return nullptr;
}

void Service::idle()
{
#ifdef HUI_PS5
    sceKernelUsleep(kIdleMilliseconds * 1000u);
#else
    ::usleep(kIdleMilliseconds * 1000u);
#endif
}

Service::View Service::view() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    View answer = view_;
    // Local favourite marks. The server's list row either has no is_favorite
    // field or the wrong one (the web app stars things on a different account),
    // so the persisted favorites_ are what the console actually means. Applied
    // before the resume filter and the progress overlay so home_recent_ and the
    // detail page's star agree on the same frame.
    const auto apply_favorite = [this](Item &item)
    {
        const auto f = favorites_.find(item.guid);
        if (f != favorites_.end())
        {
            item.is_favorite = f->second ? 1 : 0;
        }
    };
    for (Item &item : answer.items)
    {
        apply_favorite(item);
    }
    for (Item &item : answer.resume)
    {
        apply_favorite(item);
    }
    // 继续观看 must agree with where the film actually resumes. The server's
    // ts is the web player's position; this console's own map is newer after
    // the first local playback. Overlay it on the copies the page draws, and
    // add a row for a film this console has watched but the server has not.
    const auto overlay_progress = [this](Item &item)
    {
        const auto found = progress_.find(item.guid);
        if (found != progress_.end())
        {
            item.watched_ts = found->second;
        }
    };
    for (Item &item : answer.resume)
    {
        overlay_progress(item);
    }
    // The wall and the detail page read the same local position, so a film
    // this console watched 20 minutes ago says 继续播放 there too.
    for (Item &item : answer.items)
    {
        overlay_progress(item);
    }
    // 继续观看 filter. The server's own resume list (`/play/list`) is trusted:
    // an entry the server lists is something to continue, even if this console
    // has no local progress for it (watched on the web) and even if the list
    // row carries no `watched_ts` field. Only two local signals are strong
    // enough to hide a listed row: the viewer played it out HERE (finished_),
    // or this console's own position is already in the closing credits
    // (ratio >= 0.95). v29 over-filtered with a `watched<=0` and a `<2%` rule
    // and wiped out half the in-progress rows, so both are gone.
    const auto keep_resume = [this](const Item &item)
    {
        if (item.guid.empty())
        {
            return false;
        }
        if (finished_.count(item.guid) > 0)
        {
            return false;
        }
        const auto local = progress_.find(item.guid);
        if (local != progress_.end())
        {
            const int dur = item.duration > 0 ? item.duration : item.runtime * 60;
            if (dur > 0 && local->second > 0)
            {
                const float ratio = static_cast<float>(local->second) / static_cast<float>(dur);
                if (ratio >= 0.95f)
                {
                    return false;
                }
            }
        }
        return true;
    };
    answer.resume.erase(std::remove_if(answer.resume.begin(), answer.resume.end(), keep_resume),
                        answer.resume.end());
    for (const auto &entry : progress_)
    {
        bool listed = false;
        for (const Item &item : answer.resume)
        {
            if (item.guid == entry.first)
            {
                listed = true;
                break;
            }
        }
        if (listed)
        {
            continue;
        }
        if (finished_.count(entry.first) > 0)
        {
            continue;
        }
        const Item *src = nullptr;
        int dur = 0;
        for (const Item &item : answer.items)
        {
            if (item.guid == entry.first)
            {
                src = &item;
                dur = item.duration > 0 ? item.duration : item.runtime * 60;
                break;
            }
        }
        if (!src)
        {
            const auto known = known_items_.find(entry.first);
            if (known != known_items_.end())
            {
                src = &known->second;
                dur = known->second.duration > 0 ? known->second.duration
                                                 : known->second.runtime * 60;
            }
        }
        if (!src)
        {
            continue;
        }
        // A local position in the credits is not worth resuming; anything else
        // the console actually reached is a real in-progress row.
        if (dur > 0 && entry.second > 0)
        {
            const float r = static_cast<float>(entry.second) / static_cast<float>(dur);
            if (r >= 0.95f)
            {
                continue;
            }
        }
        Item copy = *src;
        copy.watched_ts = entry.second;
        answer.resume.push_back(std::move(copy));
    }
    // Rank 继续观看 by what THIS console watched most recently, newest first. The
    // home row only draws the first few entries, and a film played here is often
    // missing from (or buried in the middle of) the server's own resume list, so
    // without this a just-watched item never surfaced after a restart. Entries
    // with no local progress (watched on the web) keep the server order at the
    // tail. progress_order_ is oldest-first, so a higher index is more recent.
    {
        std::unordered_map<std::string, int> rank;
        for (std::size_t i = 0; i < progress_order_.size(); ++i)
        {
            rank[progress_order_[i]] = static_cast<int>(i);
        }
        const auto recency = [&rank](const Item &item) -> int
        {
            const auto found = rank.find(item.guid);
            return found == rank.end() ? -1 : found->second;
        };
        std::stable_sort(answer.resume.begin(), answer.resume.end(),
                         [&recency](const Item &a, const Item &b)
                         { return recency(a) > recency(b); });
    }
    return answer;
}

unsigned Service::revision() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return view_.revision;
}

Service::Detail Service::detail() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Detail answer = detail_;
    // The detail cache is the server's copy. The play label and the episode
    // bars are drawn from it, so the local position has to be overlaid here
    // just as it is for the home row.
    const auto overlay = [this](Item &item)
    {
        const auto found = progress_.find(item.guid);
        if (found != progress_.end())
        {
            item.watched_ts = found->second;
        }
    };
    overlay(answer.item);
    for (Item &episode : answer.episodes)
    {
        overlay(episode);
    }
    return answer;
}

unsigned Service::detail_revision() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return detail_.revision;
}

void Service::request_detail(const std::string &guid)
{
    if (guid.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // detail_loaded_ survives a later request that replaced the cache, so
        // it cannot mean "detail_ holds this guid". Checking the cache itself
        // lets a failed or superseded fetch be retried.
        if ((detail_.ready && detail_.item.guid == guid) || (detail_.busy && detail_guid_ == guid))
        {
            return; // already here, or already on its way
        }
        detail_ = Detail();
        detail_.busy = true;
        detail_guid_ = guid;
    }
}

bool Service::stream_url(const std::string &item_guid, std::string *url, PlayInfo *info,
                         std::string *error)
{
    std::lock_guard<std::mutex> lock(api_mutex_);
    return api_.stream_url(item_guid, url, info, error);
}

bool Service::fetch_image(const std::string &path, int width, std::string *bytes,
                          std::string *error)
{
    std::lock_guard<std::mutex> lock(api_mutex_);
    return api_.fetch_image(path, width, bytes, error);
}

void Service::request_art(const std::string &key, const std::string &path, int width)
{
    if (key.empty() || path.empty() || width <= 0)
    {
        return;
    }
    // The GPU cache is the real "do I already have this" answer. It holds the
    // most recent kArtKeep pictures and drops older ones as the viewer
    // scrolls. The old code gated only on art_known_, which kept every fetched
    // key for the whole session - so a poster that scrolled past the 64-slot
    // window was evicted from the GPU but never asked for again, and came back
    // a permanent placeholder even though the server had the art. That is the
    // "部分海报没有" the viewer sees after browsing a big 短剧/电视剧 library
    // and then opening 电影. `art_` is render-thread only and this runs on the
    // render thread, so reading it needs no lock.
    if (!art_.wants(key))
    {
        return; // resident, or already staged for the next upload
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (art_known_.count(key) != 0 || art_failed_.count(key) != 0)
    {
        return; // already queued or in flight on the art worker
    }
    // A screen asks for what it is about to draw, so the queue is normally a
    // dozen entries. The cap is there so that a future page that asks for the
    // whole library cannot turn into an unbounded download.
    if (art_queue_.size() >= 96)
    {
        return;
    }
    art_known_.insert(key);
    art_queue_.push_back(ArtJob{key, path, width});
}

void Service::update(gfx::Renderer &renderer)
{
    std::vector<ArtReady> ready;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ready.swap(art_ready_);
        // Each of these is about to enter `art_`. Drop it from art_known_ so
        // that, once the cache evicts it to make room, request_art will queue
        // it again instead of treating the stale known-key as "already have
        // it" forever (the permanent-placeholder bug).
        for (const ArtReady &art : ready)
        {
            art_known_.erase(art.key);
        }
    }
    // Staging and uploading both happen here, on the render thread, because
    // that is the only thread that may talk to GL.
    for (ArtReady &art : ready)
    {
        art_.stage(art.key, std::move(art.pixels), art.width, art.height);
    }
    art_.upload(renderer);

    // The player's newest frame, uploaded the same way. The design reads the
    // handle back with video_texture() when it draws.
    video_texture_ = player_.frame(renderer, &video_width_, &video_height_);
    renderer.batch().set_brightness(brightness_.load(std::memory_order_relaxed));

    // Bitmap subtitle (PGS/DVD). The player decoded it to RGBA on the demux
    // thread; copy it out and upload it here, the only thread allowed to touch
    // GL. The texture is (re)made only when the picture size changes, and its
    // pixels refreshed only when a new frame arrives, so a stable caption
    // costs nothing on the frames between cues.
    {
        SubtitleBitmap bm;
        const bool hit = player_.subtitle_bitmap_at(player_.position_millis(), &bm);
        if (hit && bm.w > 0 && bm.h > 0 && !bm.rgba.empty())
        {
            if (bm.w != subtitle_tex_w_ || bm.h != subtitle_tex_h_)
            {
                if (subtitle_texture_ != 0)
                {
                    renderer.batch().destroy_texture(subtitle_texture_);
                    subtitle_texture_ = 0;
                }
                subtitle_texture_ = renderer.batch().create_texture(bm.w, bm.h, bm.rgba.data());
                subtitle_tex_w_ = bm.w;
                subtitle_tex_h_ = bm.h;
                subtitle_uploaded_serial_ = bm.serial; // the create already carried the pixels
            }
            else if (bm.serial != subtitle_uploaded_serial_ && subtitle_texture_ != 0)
            {
                renderer.batch().update_texture(subtitle_texture_, bm.w, bm.h, bm.rgba.data());
                subtitle_uploaded_serial_ = bm.serial;
            }
            if (subtitle_texture_ != 0)
            {
                subtitle_x_ = bm.x;
                subtitle_y_ = bm.y;
                subtitle_w_ = bm.w;
                subtitle_h_ = bm.h;
                subtitle_canvas_w_ = bm.canvas_w;
                subtitle_canvas_h_ = bm.canvas_h;
                subtitle_live_ = true;
            }
            else
            {
                subtitle_live_ = false;
            }
        }
        else
        {
            subtitle_live_ = false;
        }
        // One line per live/not-live transition (throttled by the state flip),
        // so a "字幕没出来" report can be split into "the playhead never lands
        // in a cue window" (hit=false every frame) versus "a cue is active but
        // the overlay still does not paint" (hit=true, tex id shown).
        static bool logged_live = false;
        static int logged_flips = 0;
        const bool now_live = subtitle_live_;
        if (logged_flips < 12 && now_live != logged_live)
        {
            logged_live = now_live;
            ++logged_flips;
            sys::log("[HUI] pgs overlay live=%d hit=%d pos=%d tex=%u %dx%d+%d+%d canvas=%dx%d",
                     now_live ? 1 : 0, hit ? 1 : 0, player_.position_millis(),
                     subtitle_texture_, subtitle_w_, subtitle_h_, subtitle_x_, subtitle_y_,
                     subtitle_canvas_w_, subtitle_canvas_h_);
        }
    }

    // A track switch on a paused film reopens the ticket at the same second.
    // The new decoder always starts playing, so pause it the first frame it
    // reaches playing; pausing while it is still opening would be ignored.
    if (play_pause_when_ready_.load(std::memory_order_relaxed) &&
        player_.state() == PlayState::playing)
    {
        player_.pause();
        play_pause_when_ready_.store(false, std::memory_order_relaxed);
    }

    // Persist a slider once it has been still for two seconds. Writing on
    // every 5% step would put a flash write between two film frames.
    bool flush_settings = false;
    std::string flush_server;
    std::string flush_user;
    std::string flush_password;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (settings_dirty_ && ++settings_quiet_frames_ >= 120)
        {
            settings_dirty_ = false;
            settings_quiet_frames_ = 0;
            if (configured_ && !server_.empty() && !password_.empty())
            {
                flush_server = server_;
                flush_user = user_;
                flush_password = password_;
                flush_settings = true;
            }
        }
    }
    if (flush_settings)
    {
        save_credentials(flush_server, flush_user, flush_password);
    }

    // Persist this console's position while it plays. The server's `ts` is a
    // fallback only; without this map every entry jumped back to whatever the
    // web player last reported.
    const Playback play = playback();
    std::string guid;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        guid = play_active_guid_;
    }
    if (!guid.empty() && (play.playing || play.paused) && play.position > 0)
    {
        note_progress(guid, play.position);
    }
    else if (!guid.empty() && play.ended)
    {
        // Played out: a later 继续观看 should start from the beginning, not
        // from the closing credits.
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (progress_.erase(guid) > 0) changed = true;
            if (finished_.insert(guid).second) changed = true;
            if (last_progress_guid_ == guid)
            {
                last_progress_guid_.clear();
                last_progress_position_ = -1;
            }
            if (changed)
            {
                ++view_.revision;
            }
        }
        if (changed)
        {
            save_progress();
        }
    }
}

// ---- playback ----

void Service::attach_audio(audio::Mixer &mixer)
{
    player_.attach(mixer);
}

void Service::play(const std::string &item_guid, int resume_seconds)
{
    if (item_guid.empty())
    {
        return;
    }
    // This console's own position outranks the server's `ts`: the NAS only
    // knows where the web player stopped, and that is why entering a film used
    // to jump to a place nobody had reached here.
    const int local = local_progress(item_guid);
    std::lock_guard<std::mutex> lock(mutex_);
    play_guid_ = item_guid;
    play_resume_local_ = local >= 0;
    play_resume_ = local >= 0 ? local : std::max(0, resume_seconds);
    // A new item starts on the container's own best tracks - unless the detail
    // page recorded a choice for THIS item, then the open is seeded from it. An
    // explicit subtitle 关闭 (-2) or a chosen track (>=0, loaded in sync_play)
    // both end the auto-pick; -1 (unset) leaves the first-Chinese-sub default.
    const auto pref = detail_prefs_.find(item_guid);
    if (pref != detail_prefs_.end())
    {
        play_audio_ = pref->second.audio;
        play_subtitle_ = -1;
        play_subtitle_auto_ = (pref->second.subtitle == -1);
        play_pref_subtitle_ = pref->second.subtitle;
    }
    else
    {
        play_audio_ = -1;
        play_subtitle_ = -1;
        play_subtitle_auto_ = true; // fresh open: default to the first Chinese sub
        play_pref_subtitle_ = -1;
    }
    ++play_request_;
    play_pending_ = true;
    play_resolving_ = true;
    play_pause_when_ready_.store(false, std::memory_order_relaxed);
    play_error_.clear();
}

void Service::play_from_start(const std::string &item_guid)
{
    if (item_guid.empty())
    {
        return;
    }
    // Force an explicit, local start at 0 so the worker does not fall back to
    // the server's resume `ts` (see the header note). Mirrors play() with the
    // local-point branch pinned on.
    std::lock_guard<std::mutex> lock(mutex_);
    play_guid_ = item_guid;
    play_resume_ = 0;
    play_resume_local_ = true;
    // A re-watch from the top un-finishes the film: next time the viewer backs
    // out half-way through, 继续观看 should offer the new resume point rather
    // than treat the earlier 播完 mark as still valid.
    finished_.erase(item_guid);
    const auto pref = detail_prefs_.find(item_guid);
    if (pref != detail_prefs_.end())
    {
        play_audio_ = pref->second.audio;
        play_subtitle_ = -1;
        play_subtitle_auto_ = (pref->second.subtitle == -1);
        play_pref_subtitle_ = pref->second.subtitle;
    }
    else
    {
        play_audio_ = -1;
        play_subtitle_ = -1;
        play_subtitle_auto_ = true; // fresh open: default to the first Chinese sub
        play_pref_subtitle_ = -1;
    }
    ++play_request_;
    play_pending_ = true;
    play_resolving_ = true;
    play_pause_when_ready_.store(false, std::memory_order_relaxed);
    play_error_.clear();
}

void Service::remember(const Item &item)
{
    if (item.guid.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (known_items_.size() >= 128 && known_items_.count(item.guid) == 0)
    {
        // The map is only a drawing aid; dropping one card costs nothing and
        // the progress entry it belonged to still lives in the file.
        known_items_.erase(known_items_.begin());
    }
    known_items_[item.guid] = item;
}

void Service::reopen_at(int seconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (play_guid_.empty())
    {
        return;
    }
    play_resume_ = std::max(0, seconds);
    play_resume_local_ = true;
    ++play_request_;
    play_pending_ = true;
    play_resolving_ = true;
    if (player_.state() == PlayState::paused)
    {
        play_pause_when_ready_.store(true, std::memory_order_relaxed);
    }
    play_error_.clear();
}

void Service::stop_playback()
{
    // Keep the last second before the player's position is cleared. A viewer
    // who backs out at 40:00 expects 继续观看 to say 40:00 on this console even
    // if the NAS still says 12:00.
    const int position = player_.position();
    const bool ended = player_.state() == PlayState::ended;
    std::string guid;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Use the item the decoder actually owns, not a request the worker may
        // still be resolving: otherwise leaving during 下一集 can write the old
        // episode's position onto the new episode's progress entry.
        guid = play_active_guid_;
        ++play_request_;
        play_pending_ = false;
        play_resolving_ = false;
        play_pause_when_ready_.store(false, std::memory_order_relaxed);
        play_error_.clear();
        play_guid_.clear();
    }
    if (ended)
    {
        // Service::update() erases a finished film's progress so 继续观看 does
        // not offer the closing credits. The page then calls stop_playback in
        // the same frame; recording the end position here would put the entry
        // straight back. Marking it finished also hides it from 继续观看 when
        // the server's own resume list still carries the old watched_ts.
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (progress_.erase(guid) > 0) changed = true;
            if (!guid.empty() && finished_.insert(guid).second) changed = true;
            if (last_progress_guid_ == guid)
            {
                last_progress_guid_.clear();
                last_progress_position_ = -1;
            }
            if (changed)
            {
                ++view_.revision;
            }
        }
        if (changed)
        {
            save_progress();
        }
    }
    else if (!guid.empty() && position > 0)
    {
        note_progress(guid, position);
    }
    player_.stop();
    std::lock_guard<std::mutex> lock(mutex_);
    play_active_guid_.clear();
    play_error_.clear();
}

void Service::toggle_pause()
{
    if (player_.state() == PlayState::paused)
    {
        player_.resume();
    }
    else
    {
        player_.pause();
    }
}

void Service::seek_playback(int seconds)
{
    player_.seek(seconds);
}

void Service::set_volume(float volume)
{
    volume = std::clamp(volume, 0.0f, 1.0f);
    volume_.store(volume, std::memory_order_relaxed);
    player_.set_gain(volume);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_dirty_ = true;
        settings_quiet_frames_ = 0;
    }
}

float Service::volume() const
{
    return volume_.load(std::memory_order_relaxed);
}

void Service::set_brightness(float brightness)
{
    brightness = std::clamp(brightness, 0.5f, 1.5f);
    brightness_.store(brightness, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_dirty_ = true;
        settings_quiet_frames_ = 0;
    }
}

float Service::brightness() const
{
    return brightness_.load(std::memory_order_relaxed);
}

std::vector<AudioTrack> Service::audio_tracks() const
{
    return player_.audio_tracks();
}

std::vector<SubtitleTrack> Service::subtitle_tracks() const
{
    // The container's embedded tracks first (their ordinals are what the player
    // chose them by), then the external files the server listed for this item.
    // A bitmap external (an outside .sup/PGS) is skipped: this renderer draws
    // text only, and there is no picture path for a file that is not in the
    // container.
    std::vector<SubtitleTrack> tracks = player_.subtitle_tracks();
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t j = 0; j < play_external_subs_.size(); ++j)
    {
        const SubtitleStreamInfo &stream = play_external_subs_[j];
        if (stream.bitmap || stream.guid.empty())
        {
            continue;
        }
        SubtitleTrack track;
        track.ordinal = kExternalSubtitleBase + static_cast<int>(j);
        track.language = stream.language;
        track.title = stream.title;
        track.codec = stream.codec.empty() ? stream.format : stream.codec;
        track.text = true;
        track.external = true;
        track.ext_guid = stream.guid;
        track.ext_format = stream.format;
        tracks.push_back(std::move(track));
    }
    return tracks;
}

int Service::audio_track() const
{
    return player_.audio_track();
}

int Service::subtitle_track() const
{
    // An external selection outranks the container's ordinal: while one is on
    // screen the player is not decoding an embedded track for display.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (play_subtitle_ext_ >= kExternalSubtitleBase)
        {
            return play_subtitle_ext_;
        }
    }
    return player_.subtitle_track();
}

void Service::select_audio_track(int ordinal)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (play_guid_.empty())
    {
        return;
    }
    // Mirror the choice back to the detail page's stored preference so leaving
    // the player and returning shows what the viewer just picked, not the
    // earlier detail-page choice (the reported desync). A film's play guid is
    // its detail guid, and the audio ordinal is the same 0-based server stream
    // index the detail chooser stores, so it writes straight through.
    detail_prefs_[play_guid_].audio = ordinal;
    // -1 means "the container's default" on the first open. Compare with the
    // decoder's actual choice too, so confirming the track that is already
    // playing does not reopen the film at the same second for no reason.
    if (ordinal == player_.audio_track() && (play_audio_ == ordinal || play_audio_ < 0))
    {
        return;
    }
    play_audio_ = ordinal;
    play_subtitle_auto_ = false; // an explicit choice, even on audio, ends auto
    play_resume_ = player_.position();
    play_resume_local_ = true;
    ++play_request_;
    play_pending_ = true;
    play_resolving_ = true;
    play_pause_when_ready_.store(player_.state() == PlayState::paused, std::memory_order_relaxed);
    play_error_.clear();
}

void Service::select_subtitle_track(int ordinal)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (play_guid_.empty())
    {
        return;
    }
    // Keep the detail page in sync with a player-side subtitle change. The
    // player's ordinal lives in its own namespace (container stream order for
    // embedded, kExternalSubtitleBase + server index for externals); translate
    // it back to the server subtitle-list index the detail chooser stores.
    {
        int pref = -2; // -2 == 关闭; -1 would mean "auto" which the player has no notion of
        if (ordinal >= kExternalSubtitleBase)
        {
            const int s = ordinal - kExternalSubtitleBase;
            if (s >= 0 && s < static_cast<int>(play_external_subs_.size()))
                pref = s;
        }
        else if (ordinal >= 0)
        {
            int seen = 0;
            for (int i = 0; i < static_cast<int>(play_external_subs_.size()); ++i)
            {
                if (!play_external_subs_[static_cast<std::size_t>(i)].external)
                {
                    if (seen == ordinal)
                    {
                        pref = i;
                        break;
                    }
                    ++seen;
                }
            }
        }
        detail_prefs_[play_guid_].subtitle = pref;
    }
    if (ordinal >= kExternalSubtitleBase)
    {
        const int j = ordinal - kExternalSubtitleBase;
        if (j < 0 || j >= static_cast<int>(play_external_subs_.size()))
        {
            return;
        }
        const SubtitleStreamInfo ext = play_external_subs_[static_cast<std::size_t>(j)];
        if (ext.guid.empty())
        {
            return;
        }
        if (play_subtitle_ext_ == ordinal)
        {
            return; // already showing this external file
        }
        // No reopen: the container keeps playing, and while an external file is
        // active the player draws only its cues (an embedded track decoding
        // underneath is ignored). Queue the fetch - /subtitle/dl is a network
        // call and cannot run on the render thread - for the worker to inject.
        play_subtitle_ext_ = ordinal;
        sub_request_ = SubtitleRequest{};
        sub_request_.task = SubtitleTask::load_external;
        sub_request_.guid = ext.guid;
        sub_request_.format = ext.format;
        sub_request_.ordinal = ordinal;
        sub_request_.active = true;
        return;
    }
    // A container track, or -1 for off. Leaving an external file needs a reopen
    // to reset the player's external state even when the container ordinal
    // matches what the player last chose, so skip the already-selected early
    // return in that case.
    const bool was_external = play_subtitle_ext_ >= kExternalSubtitleBase;
    play_subtitle_ext_ = -1;
    // -1 is "off". If the decoder has not selected anything yet, selecting
    // off is already true; if it selected a default track, selecting off must
    // still reopen so that the overlay actually disappears.
    if (!was_external && ordinal == player_.subtitle_track() &&
        (play_subtitle_ == ordinal || play_subtitle_ < 0))
    {
        return;
    }
    play_subtitle_ = ordinal;
    play_subtitle_auto_ = false; // the viewer chose; do not second-guess on reopen
    // The subtitle decoder is chosen when the container opens, so a track
    // change reopens the ticket at the same second just like audio does.
    play_resume_ = player_.position();
    play_resume_local_ = true;
    ++play_request_;
    play_pending_ = true;
    play_resolving_ = true;
    play_pause_when_ready_.store(player_.state() == PlayState::paused, std::memory_order_relaxed);
    play_error_.clear();
}

std::string Service::subtitle_at() const
{
    return player_.subtitle_at(player_.position_millis());
}

void Service::select_detail_audio(const std::string &guid, int ordinal)
{
    if (guid.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        detail_prefs_[guid].audio = ordinal;
    }
    // Persist outside the lock (save_state re-locks; std::mutex is not
    // recursive), the same way a favourite toggle writes through.
    save_state();
}

void Service::select_detail_subtitle(const std::string &guid, int index)
{
    if (guid.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        detail_prefs_[guid].subtitle = index;
    }
    save_state();
}

int Service::detail_pref_audio(const std::string &guid) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = detail_prefs_.find(guid);
    return found != detail_prefs_.end() ? found->second.audio : -1;
}

int Service::detail_pref_subtitle(const std::string &guid) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = detail_prefs_.find(guid);
    return found != detail_prefs_.end() ? found->second.subtitle : -1;
}

void Service::refresh()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_)
    {
        return;
    }
    // Queue the same full pass a boot or relogin runs. The worker re-signs in,
    // re-reads the libraries and the resume row, then clears busy and bumps the
    // revision the poster wall redraws on. generation_ is not bumped, so the
    // running session and any open film survive a refresh.
    needs_sync_ = true;
    view_.busy = true;
    view_.status = "正在刷新片源…";
    ++view_.revision;
}

Service::SubtitlePanel Service::subtitle_panel() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return subtitle_panel_;
}

void Service::open_subtitle_panel()
{
    std::lock_guard<std::mutex> lock(mutex_);
    subtitle_panel_ = SubtitlePanel();
    subtitle_panel_.open = true;
    subtitle_panel_.revision = ++subtitle_revision_;
}

void Service::close_subtitle_panel()
{
    std::lock_guard<std::mutex> lock(mutex_);
    subtitle_panel_.open = false;
    subtitle_panel_.revision = ++subtitle_revision_;
}

bool Service::subtitle_config_available() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !play_media_guid_.empty();
}

bool Service::subtitle_browse_available() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !play_file_path_.empty();
}

namespace
{

// Parent directory of an absolute /vol… path. "a/b/c" -> "a/b"; "a" or "/"
// have no parent above the volume root, so "/" maps to "" (the top) and a
// path with no separator maps to "".
std::string folder_parent(const std::string &path)
{
    const std::size_t cut = path.find_last_of('/');
    if (cut == std::string::npos)
        return std::string();
    if (cut == 0)
        return std::string(); // "/volN" or "/" - the volume root is the top
    return path.substr(0, cut);
}

// "folder/name", doubling up only when the folder is not already the root.
std::string folder_child(const std::string &folder, const std::string &name)
{
    if (folder.empty() || folder == "/")
        return "/" + name;
    return folder + "/" + name;
}

} // namespace

void Service::subtitle_search_async(const std::string &lan)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (play_media_guid_.empty())
    {
        subtitle_panel_.busy = false;
        subtitle_panel_.error = "暂时无法搜索：未取到文件标识";
        subtitle_panel_.revision = ++subtitle_revision_;
        return;
    }
    sub_request_ = SubtitleRequest{};
    sub_request_.task = SubtitleTask::search;
    sub_request_.media_guid = play_media_guid_;
    sub_request_.lan = lan.empty() ? "chi" : lan;
    sub_request_.active = true;
    subtitle_panel_.busy = true;
    subtitle_panel_.error.clear();
    subtitle_panel_.hits.clear();
    subtitle_panel_.status = "正在搜索字幕…";
    subtitle_panel_.revision = ++subtitle_revision_;
}

void Service::subtitle_download_async(const std::string &trim_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (play_media_guid_.empty() || trim_id.empty())
    {
        subtitle_panel_.busy = false;
        subtitle_panel_.error = "无法下载：缺少字幕标识";
        subtitle_panel_.revision = ++subtitle_revision_;
        return;
    }
    sub_request_ = SubtitleRequest{};
    sub_request_.task = SubtitleTask::download;
    sub_request_.media_guid = play_media_guid_;
    sub_request_.trim_id = trim_id;
    sub_request_.active = true;
    subtitle_panel_.busy = true;
    subtitle_panel_.error.clear();
    subtitle_panel_.status = "正在下载并写入媒体目录…";
    subtitle_panel_.revision = ++subtitle_revision_;
}

void Service::subtitle_browse_async()
{
    std::lock_guard<std::mutex> lock(mutex_);
    subtitle_panel_.local = true;
    if (play_file_path_.empty())
    {
        subtitle_panel_.busy = false;
        subtitle_panel_.error = "暂时无法浏览：未取到影片路径";
        subtitle_panel_.revision = ++subtitle_revision_;
        return;
    }
    // The tree opens on the video's own folder; the viewer then drills into
    // the per-movie subfolders where the subtitle actually lives.
    browse_folder_ = folder_parent(play_file_path_);
    sub_request_ = SubtitleRequest{};
    sub_request_.task = SubtitleTask::browse;
    sub_request_.media_guid = play_media_guid_;
    sub_request_.path = browse_folder_;
    sub_request_.active = true;
    subtitle_panel_.busy = true;
    subtitle_panel_.error.clear();
    subtitle_panel_.hits.clear();
    subtitle_panel_.folder = browse_folder_;
    subtitle_panel_.status = "正在浏览 NAS 文件夹…";
    subtitle_panel_.revision = ++subtitle_revision_;
}

void Service::subtitle_browse_open_async(const std::string &folder)
{
    std::lock_guard<std::mutex> lock(mutex_);
    subtitle_panel_.local = true;
    // An empty target means "go home" - the playing video's own folder.
    std::string target = folder;
    if (target.empty())
    {
        if (play_file_path_.empty())
        {
            subtitle_panel_.busy = false;
            subtitle_panel_.error = "暂时无法浏览：未取到影片路径";
            subtitle_panel_.revision = ++subtitle_revision_;
            return;
        }
        target = folder_parent(play_file_path_);
    }
    browse_folder_ = target;
    sub_request_ = SubtitleRequest{};
    sub_request_.task = SubtitleTask::browse;
    sub_request_.media_guid = play_media_guid_;
    sub_request_.path = target;
    sub_request_.active = true;
    subtitle_panel_.busy = true;
    subtitle_panel_.error.clear();
    subtitle_panel_.hits.clear();
    subtitle_panel_.folder = target;
    subtitle_panel_.status = "正在浏览 NAS 文件夹…";
    subtitle_panel_.revision = ++subtitle_revision_;
}

void Service::subtitle_browse_up_async()
{
    // Compute the parent off the lock, then let the shared opener enqueue.
    std::string parent;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string &here =
            subtitle_panel_.folder.empty() ? browse_folder_ : subtitle_panel_.folder;
        parent = folder_parent(here);
    }
    if (parent.empty())
        return; // already at the volume root: nowhere up to go
    subtitle_browse_open_async(parent);
}

void Service::subtitle_mark_async(const std::string &filename)
{
    std::lock_guard<std::mutex> lock(mutex_);
    subtitle_panel_.local = true;
    // The chosen name joins with the folder currently listed, not the video's
    // own - the viewer may have drilled into a subfolder before picking.
    if (play_media_guid_.empty() || filename.empty() || browse_folder_.empty())
    {
        subtitle_panel_.busy = false;
        subtitle_panel_.error = "无法标记：缺少影片信息";
        subtitle_panel_.revision = ++subtitle_revision_;
        return;
    }
    sub_request_ = SubtitleRequest{};
    sub_request_.task = SubtitleTask::mark;
    sub_request_.media_guid = play_media_guid_;
    sub_request_.path = folder_child(browse_folder_, filename);
    sub_request_.active = true;
    subtitle_panel_.busy = true;
    subtitle_panel_.error.clear();
    subtitle_panel_.status = "正在标记字幕文件…";
    subtitle_panel_.revision = ++subtitle_revision_;
}

// Re-read /stream/list for the item that is playing, so a subtitle the
// server just wrote or marked shows up as a selectable external track. The
// caller must NOT hold mutex_ (the Api lock order is api_mutex_ -> mutex_).
void Service::refresh_external_subs()
{
    std::string item_guid;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        item_guid = play_guid_;
    }
    if (item_guid.empty())
    {
        return;
    }
    Item probe;
    std::string ignored;
    bool refreshed = false;
    {
        std::lock_guard<std::mutex> api_lock(api_mutex_);
        refreshed = api_.streams(item_guid, &probe, &ignored);
    }
    if (refreshed)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        play_external_subs_ = std::move(probe.subtitle_streams);
        if (!probe.media_guid.empty())
            play_media_guid_ = probe.media_guid;
        if (!probe.file_path.empty())
            play_file_path_ = probe.file_path;
        play_external_subs_guid_ = item_guid;
    }
}

void Service::run_subtitle_task(const SubtitleRequest &request)
{
    switch (request.task)
    {
    case SubtitleTask::load_external:
    {
        std::string bytes;
        std::string error;
        bool fetched = false;
        {
            std::lock_guard<std::mutex> api_lock(api_mutex_);
            fetched = api_.subtitle_content(request.guid, &bytes, &error);
        }
        if (!fetched)
        {
            sys::log("[HUI] subtitle dl fail guid=%s err=%s", request.guid.c_str(),
                     error.c_str());
            // Drop the stale selection so the menu highlights nothing rather
            // than a file that is not on screen.
            std::lock_guard<std::mutex> lock(mutex_);
            if (play_subtitle_ext_ == request.ordinal)
            {
                play_subtitle_ext_ = -1;
            }
            break;
        }
        // The player decodes the whole file at once; a later seek matches with
        // no refetch because every cue carries its own timestamp.
        player_.set_external_subtitle(bytes, request.format);
        break;
    }
    case SubtitleTask::search:
    {
        std::vector<SubtitleHit> hits;
        std::string error;
        bool ok = false;
        {
            std::lock_guard<std::mutex> api_lock(api_mutex_);
            ok = api_.subtitle_search(request.media_guid, request.lan, &hits, &error);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        subtitle_panel_.busy = false;
        if (ok)
        {
            subtitle_panel_.hits = std::move(hits);
            subtitle_panel_.error.clear();
            subtitle_panel_.status = subtitle_panel_.hits.empty() ? "没有搜索结果" : std::string();
        }
        else
        {
            subtitle_panel_.hits.clear();
            subtitle_panel_.error = error.empty() ? "搜索失败" : error;
        }
        subtitle_panel_.revision = ++subtitle_revision_;
        break;
    }
    case SubtitleTask::download:
    {
        std::string error;
        bool ok = false;
        {
            std::lock_guard<std::mutex> api_lock(api_mutex_);
            ok = api_.subtitle_download(request.media_guid, request.trim_id, &error);
        }
        if (ok)
        {
            // Re-read the stream list so the file the server just wrote shows
            // up as a selectable external track. If mediasrv has not scraped
            // it this instant, a later reopen picks it up.
            refresh_external_subs();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        subtitle_panel_.busy = false;
        subtitle_panel_.status = ok ? "下载成功，已加入字幕轨道" : std::string();
        subtitle_panel_.error = ok ? std::string() : (error.empty() ? "下载失败" : error);
        subtitle_panel_.revision = ++subtitle_revision_;
        break;
    }
    case SubtitleTask::browse:
    {
        // List one folder of the tree the way the web picker does - POST
        // /server/path over the same signed channel as every other call, so
        // nothing extra has to be enabled on the NAS. api_mutex_ keeps it off
        // the wire while a stream reads. The result carries subdirectories (so
        // the viewer can drill in) plus the subtitle files in this folder.
        std::vector<SubtitleHit> entries;
        std::string error;
        bool ok = false;
        {
            std::lock_guard<std::mutex> api_lock(api_mutex_);
            ok = api_.folder_files(request.path, &entries, &error);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        subtitle_panel_.busy = false;
        subtitle_panel_.local = true;
        subtitle_panel_.folder = request.path;
        subtitle_panel_.hits.clear();
        if (ok)
        {
            // A leading "go up" row whenever this folder has a parent, so the
            // tree is navigable both ways without a separate control.
            if (!folder_parent(request.path).empty())
            {
                SubtitleHit up;
                up.filename = "..";
                up.is_dir = true;
                up.source = "返回上级目录";
                subtitle_panel_.hits.push_back(std::move(up));
            }
            int subs = 0;
            for (SubtitleHit &hit : entries)
            {
                if (!hit.is_dir)
                    ++subs;
                subtitle_panel_.hits.push_back(std::move(hit));
            }
            subtitle_panel_.error.clear();
            subtitle_panel_.status =
                subs > 0
                    ? "找到 " + std::to_string(subs) + " 个字幕 · 可进入子文件夹"
                    : "本层没有字幕文件 · 可进入子文件夹或返回上级";
        }
        else
        {
            subtitle_panel_.error = error.empty() ? "读取 NAS 文件夹失败" : error;
        }
        subtitle_panel_.revision = ++subtitle_revision_;
        break;
    }
    case SubtitleTask::mark:
    {
        std::string error;
        bool ok = false;
        {
            std::lock_guard<std::mutex> api_lock(api_mutex_);
            std::vector<std::string> paths;
            paths.push_back(request.path);
            ok = api_.subtitle_mark(request.media_guid, paths, &error);
        }
        if (ok)
        {
            refresh_external_subs();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        subtitle_panel_.busy = false;
        subtitle_panel_.local = true;
        bool picked = false;
        if (ok)
        {
            for (const SubtitleStreamInfo &stream : play_external_subs_)
            {
                if (stream.external && !stream.guid.empty() &&
                    request.path.find(stream.title) != std::string::npos && !stream.title.empty())
                {
                    picked = true;
                    break;
                }
            }
        }
        subtitle_panel_.status =
            ok ? (picked ? "标记成功，已加入字幕轨道" : "已标记，返回选择字幕查看") : std::string();
        subtitle_panel_.error = ok ? std::string() : (error.empty() ? "标记失败" : error);
        subtitle_panel_.revision = ++subtitle_revision_;
        break;
    }
    case SubtitleTask::none:
    default:
        break;
    }
}

Service::Playback Service::playback() const
{
    Playback answer;
    switch (player_.state())
    {
    case PlayState::opening:
        answer.opening = true;
        break;
    case PlayState::playing:
        answer.playing = true;
        break;
    case PlayState::paused:
        answer.paused = true;
        break;
    case PlayState::failed:
        answer.failed = true;
        break;
    case PlayState::ended:
        answer.ended = true;
        break;
    default:
        break;
    }
    answer.position = player_.position();
    answer.duration = player_.duration();
    bool resolving = false;
    std::string resolve_error;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resolving = play_pending_ || play_resolving_;
        resolve_error = play_error_;
    }
    // The worker clears play_pending_ when it picks the job up, long before
    // the signed ticket comes back. Treat that whole window as opening, or the
    // page falls through to its sample clock and looks hung.
    answer.opening = answer.opening || resolving;
    if (answer.failed)
    {
        answer.error = player_.error_text();
        if (answer.error.empty())
        {
            answer.error = resolve_error;
        }
    }
    if (!answer.opening && !answer.playing && !answer.paused && !answer.ended &&
        !resolve_error.empty())
    {
        answer.failed = true;
        answer.error = resolve_error;
    }
    return answer;
}

void Service::sync_play(const std::string &guid, int resume_seconds, int audio_track,
                        int subtitle_track, bool resume_is_local, unsigned request,
                        bool subtitle_auto)
{
    // Stop whatever is playing first. Otherwise, when the next film's address
    // fails to resolve (or it simply takes a moment to open), the previous
    // film's audio keeps running and its last frame stays on screen - the
    // "stuck on the first video, music won't stop" glitch.
    // The stop() that tears the previous film down runs *before* "play begin".
    // A decoder thread stuck in av_read_frame that ignores the interrupt makes
    // this join hang, and the single worker stalls on it - which is exactly the
    // "stuck on 正在打开 and no play begin ever prints" signature. Bracket it so
    // the next repro says whether we even reached the new stream's resolve.
    const std::int64_t stop_t0 = sys::monotonic_us();
    sys::log("[HUI] sync_play enter req=%u guid=%s stop-begin", request, guid.c_str());
    player_.stop();
    sys::log("[HUI] sync_play stop-done req=%u took=%lldms", request,
             static_cast<long long>((sys::monotonic_us() - stop_t0) / 1000));
    sys::log("[HUI] play begin req=%u resume=%d guid=%s", request, resume_seconds,
             guid.c_str());

    std::string url;
    PlayInfo info;
    std::string error;
    bool resolved = false;
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(api_mutex_);
        resolved = api_.stream_url(guid, &url, &info, &error);
        if (resolved)
        {
            // A local position of 0 is still authoritative: it means "this
            // console finished here" or "started and left", not "ask the web
            // player where it was".
            const int start = (resume_seconds > 0 || resume_is_local) ? resume_seconds
                                                                      : static_cast<int>(info.ts);
            bool need_ext = false;
            {
                // Resolving can take seconds. If the viewer backed out (or
                // picked another episode) meanwhile, opening now would leave
                // audio playing over a page that has already gone home.
                std::lock_guard<std::mutex> state(mutex_);
                cancelled = request != play_request_ || play_guid_ != guid;
                need_ext = play_external_subs_guid_ != guid;
                // Any open here is a fresh container decode; no external file
                // carries over a reopen (Player::start clears its cues too).
                play_subtitle_ext_ = -1;
                // The ticket's own file-level guid: search/download key on it,
                // and it is authoritative even when /stream/list withholds it
                // (its files[] carry no media_guid member on mediasrv 0.8.43).
                play_media_guid_ = info.media_guid;
            }
            if (!cancelled && need_ext)
            {
                // The subtitle menu lists the external files the server keeps
                // beside this video. /stream/list is cached by guid, so a
                // track-change reopen does not re-hit it; a fresh item does.
                // api_mutex_ is already held for the ticket.
                Item probe;
                std::string sub_error;
                if (api_.streams(guid, &probe, &sub_error))
                {
                    std::lock_guard<std::mutex> state(mutex_);
                    play_external_subs_ = std::move(probe.subtitle_streams);
                    if (!probe.media_guid.empty())
                        play_media_guid_ = probe.media_guid;
                    // The volume path the local-subtitle browser lists.
                    play_file_path_ = probe.file_path;
                    play_external_subs_guid_ = guid;
                }
                else
                {
                    // The track merge is silently empty when this fails; say
                    // so, because "search finds nothing" is otherwise a dead
                    // end for the next repro.
                    sys::log("[HUI] stream list fail guid=%s err=%s", guid.c_str(),
                             sub_error.c_str());
                }
            }
            if (!cancelled && subtitle_track < 0)
            {
                // A subtitle picked on the detail page has to select a CONTAINER
                // stream, not only a sidecar file. The old path just loaded an
                // external .srt by guid, so an embedded pick - including a PGS
                // picture track - was dropped and the film came up on the
                // decoder's default ("选字幕没生效"). Map the stored server-list
                // index onto its position among the container's embedded subtitle
                // streams and hand that ordinal to start(), which chooses it when
                // the header is read. Externals keep their own guid path below.
                std::lock_guard<std::mutex> state(mutex_);
                if (play_pref_subtitle_ >= 0 &&
                    play_pref_subtitle_ < static_cast<int>(play_external_subs_.size()) &&
                    !play_external_subs_[static_cast<std::size_t>(play_pref_subtitle_)].external)
                {
                    int ord = 0;
                    for (int i = 0; i < play_pref_subtitle_; ++i)
                    {
                        if (!play_external_subs_[static_cast<std::size_t>(i)].external)
                            ++ord;
                    }
                    subtitle_track = ord;
                    subtitle_auto = false;
                }
            }
            if (!cancelled)
            {
                // The player reads the bytes itself; only the token header
                // rides along, which is all the range endpoint checks.
                const bool started =
                    player_.start(url, api_.media_headers(), start, audio_track, subtitle_track,
                                  subtitle_auto);
                sys::log("[HUI] play start rc=%d", started ? 1 : 0);
                bool current = false;
                {
                    std::lock_guard<std::mutex> state(mutex_);
                    current = request == play_request_ && play_guid_ == guid;
                    if (started && current)
                    {
                        play_active_guid_ = guid;
                        play_resolving_ = false;
                        play_error_.clear();
                        // A subtitle chosen on the detail page for this item:
                        // fetch it by guid now that the film is up, mirroring
                        // the player menu's external-subtitle path. Re-queued on
                        // every (re)open so an audio-track reopen keeps it.
                        if (play_pref_subtitle_ >= 0 &&
                            play_pref_subtitle_ < static_cast<int>(play_external_subs_.size()))
                        {
                            const SubtitleStreamInfo ext = play_external_subs_[static_cast<std::size_t>(
                                play_pref_subtitle_)];
                            if (!ext.guid.empty() && !ext.bitmap && ext.external)
                            {
                                play_subtitle_ext_ = kExternalSubtitleBase + play_pref_subtitle_;
                                sub_request_ = SubtitleRequest{};
                                sub_request_.task = SubtitleTask::load_external;
                                sub_request_.guid = ext.guid;
                                sub_request_.format = ext.format;
                                sub_request_.ordinal = play_subtitle_ext_;
                                sub_request_.active = true;
                            }
                        }
                    }
                }
                if (started)
                {
                    if (current)
                    {
                        return;
                    }
                    // The start won the race with a cancellation just after
                    // the check above. Do not let it play for a page that has
                    // already left; a newer request will be started by the
                    // worker after this function returns.
                    player_.stop();
                    return;
                }
                error = "播放器起不来";
            }
        }
    }
    sys::log("[HUI] play resolve %s err=%s", resolved ? "ok" : "fail", error.c_str());
    if (cancelled)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (request == play_request_)
    {
        play_resolving_ = false;
        play_pause_when_ready_.store(false, std::memory_order_relaxed);
        play_error_ = error.empty() ? "取不到播放地址" : error;
        sys::log("[HUI] play failed req=%u err=%s", request, play_error_.c_str());
    }
}

std::uint32_t Service::art_texture(const std::string &key)
{
    return art_.texture(key);
}

bool Service::art_size(const std::string &key, int *width, int *height) const
{
    return art_.size(key, width, height);
}

void Service::set_play_edge(int edge)
{
    // 0 means automatic: play at the source resolution, no cap. Negatives are
    // invalid and ignored; positives are clamped to the manual tiers' range.
    if (edge < 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (edge > kMaxFrameEdge)
    {
        edge = kMaxFrameEdge;
    }
    play_edge_ = edge;
    // The player reads it on the next decoded picture, so a tier picked while
    // a film runs applies at once (and is what caps the upload).
    player_.set_frame_edge(edge);
    // Persist next to the credentials. Without a password there is nothing to
    // write back safely - logout() drops it - and the next successful sync
    // saves the file anyway.
    if (configured_ && !server_.empty() && !password_.empty())
    {
        save_credentials(server_, user_, password_);
    }
}

int Service::play_edge() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return play_edge_.load(std::memory_order_relaxed);
}

void Service::set_decoder(int decoder)
{
    // Clamp to the two known values; anything else is treated as software.
    if (decoder != 0)
    {
        decoder = 1; // hardware (default)
    }
    std::lock_guard<std::mutex> lock(mutex_);
    decoder_ = decoder;
    // The player reads it when the next film opens, so a choice made mid-film
    // does not tear down the running decode (the hardware path is pending G/L).
    player_.set_decoder(decoder);
    if (configured_ && !server_.empty() && !password_.empty())
    {
        save_credentials(server_, user_, password_);
    }
}

int Service::decoder() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return decoder_.load(std::memory_order_relaxed);
}

void Service::sync_art(const std::string &key, const std::string &path, int width)
{
    std::string bytes;
    std::string error;
    bool fetched = false;
    {
        std::lock_guard<std::mutex> lock(api_mutex_);
        fetched = api_.signed_in() && api_.fetch_image(path, width, &bytes, &error);
    }
    if (!fetched)
    {
        art_gave_up(key, path, width, error.empty() ? "fetch failed" : error.c_str());
        return;
    }
    // Marks the fetch/decode boundary. If a poster wedges, this line tells the
    // two apart: "art fetched" with no following "poster ok"/"decode failed"
    // means decode_image looped (contained to the art thread now); no "art
    // fetched" at all means the fetch itself never returned (transfer gate).
    sys::log("[HUI] art fetched key=%s bytes=%d", key.c_str(), static_cast<int>(bytes.size()));

    ArtReady ready;
    ready.key = key;
    // Decoded at the same width it was asked for, so a portrait poster keeps
    // its own aspect: the tile crops with a uv rectangle, not the decoder.
    if (!fnos::decode_image(bytes, width, &ready.pixels, &ready.width, &ready.height))
    {
        // The size and the magic say which of the two it is: a server that
        // answered with an error page, or a picture this FFmpeg build has no
        // decoder for (a webp would show up here as "RIFF....WEBP").
        sys::log("[HUI] poster decode failed key=%s bytes=%d magic=%s", key.c_str(),
                 static_cast<int>(bytes.size()), magic_of(bytes).c_str());
        art_gave_up(key, path, width, "decode failed");
        return;
    }

    // Three lines are enough to confirm on the console that the webp path works
    // (posters arrive as RIFF/WEBP and are decoded by libwebp), without turning
    // a poster wall into a log flood. Only the art worker calls this.
    static int logged = 0;
    if (logged < 400)
    {
        ++logged;
        sys::log("[HUI] poster ok key=%s %dx%d bytes=%d", key.c_str(), ready.width, ready.height,
                 static_cast<int>(bytes.size()));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    art_failed_.erase(key);
    art_ready_.push_back(std::move(ready));
}

void Service::art_gave_up(const std::string &key, const std::string &path, int width,
                          const char *why)
{
    std::lock_guard<std::mutex> lock(mutex_);
    art_known_.erase(key);
    const int tries = ++art_failed_[key];
    if (tries < kArtTries)
    {
        // Back at the end of the queue: the posters ahead of it are the delay,
        // so a NAS that is merely having a moment costs no extra requests and
        // a screen asking again does not double the job.
        art_queue_.push_back(ArtJob{key, path, width});
        art_known_.insert(key);
    }
    sys::log("[HUI] poster give up try=%d/%d key=%s why=%s", tries, kArtTries, key.c_str(),
             why != nullptr ? why : "");
}

void Service::worker_main()
{
    for (;;)
    {
        std::string job;
        std::string play_guid;
        int play_resume = 0;
        int play_audio = -1;
        int play_subtitle = -1;
        bool play_subtitle_auto = false;
        bool play_resume_local = false;
        unsigned generation = 0;
        unsigned play_request = 0;
        bool run_sync = false;
        bool run_play = false;
        bool run_sub = false;
        bool run_enrich = false;
        SubtitleRequest sub_request;
        bool have_work = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_)
            {
                return;
            }
            if (needs_sync_)
            {
                needs_sync_ = false;
                run_sync = true;
                have_work = true;
                generation = generation_;
            }
            else if (play_pending_)
            {
                // A press on 播放 outranks everything else the worker has.
                play_pending_ = false;
                run_play = true;
                have_work = true;
                play_guid = play_guid_;
                play_resume = play_resume_;
                play_audio = play_audio_;
                play_subtitle = play_subtitle_;
                play_subtitle_auto = play_subtitle_auto_;
                play_subtitle_auto_ = false; // consumed by this open only
                play_resume_local = play_resume_local_;
                play_request = play_request_;
            }
            else if (!detail_guid_.empty())
            {
                job = detail_guid_;
                detail_guid_.clear();
                have_work = true;
            }
            else if (sub_request_.active)
            {
                // A subtitle job (external load / search / download) queues
                // behind navigation but runs on this same serial worker, so it
                // never races the shelf or playback on the one Api instance.
                sub_request = sub_request_;
                sub_request_.active = false;
                run_sub = true;
                have_work = true;
            }
            else if (!enrich_queue_.empty())
            {
                // The lowest priority there is: fill one wall row's detail-only
                // genre / country / colour / audio text. It only runs when the
                // worker would otherwise idle, so a 播放 or a 详情页 fetch always
                // outranks it, and the filter panel fills in progressively.
                job = enrich_queue_.front();
                enrich_queue_.erase(enrich_queue_.begin());
                run_enrich = true;
                have_work = true;
            }
            // Artwork is deliberately not taken here. A poster whose decoder
            // wedges (a webp that loops inside decode_image) used to freeze
            // this one worker, so no detail and no play job ever ran again -
            // the series page showed no 选集 and 播放 stuck on 正在打开 forever
            // (v14/v15 both wedged on the same two posters). Posters now run on
            // their own thread (art_worker_main), where a hang is contained to
            // artwork and cannot starve the shelf, the detail fetch, or playback.
        }

        // Polling rather than a condition variable. The cost is one lock every
        // twenty milliseconds while the console sits idle on a menu; the gain
        // is that <thread> and its locale symbols stay out of the link.
        if (!have_work)
        {
            idle();
            continue;
        }

        // One job runs at a time; if any blocks (an HTTP call without a
        // timeout, an avformat probe, or a player_.stop() whose decoder thread
        // will not abort) every later play/detail queues behind it - the
        // "stuck on 正在打开 with no play begin" case. Log the job as it starts
        // and, only when it runs long, as it ends, so a hang names the stage
        // that wedged the worker instead of guessing.
        const char *job_kind =
            run_sync ? "sync" : run_play ? "play" : run_sub ? "subtitle" : run_enrich ? "enrich" : "detail";
        const char *job_guid =
            run_sync ? "" : run_play ? play_guid.c_str() : run_sub ? sub_request.guid.c_str() : job.c_str();
        sys::log("[HUI] worker job=%s guid=%s", job_kind, job_guid);
        const std::int64_t job_t0 = sys::monotonic_us();
        if (run_sync)
        {
            sync(generation);
        }
        else if (run_play)
        {
            sync_play(play_guid, play_resume, play_audio, play_subtitle, play_resume_local,
                      play_request, play_subtitle_auto);
        }
        else if (run_sub)
        {
            run_subtitle_task(sub_request);
        }
        else if (run_enrich)
        {
            enrich_one(job);
        }
        else
        {
            sync_detail(job);
        }
        const std::int64_t job_us = sys::monotonic_us() - job_t0;
        if (job_us > 3000000)
        {
            sys::log("[HUI] worker job=%s done slow=%lldms", job_kind,
                     static_cast<long long>(job_us / 1000));
        }
    }
}

void Service::art_worker_main()
{
    // The poster thread. It only ever drains art_queue_; the shelf, detail and
    // playback live on the main worker. The fetch serialises on the same
    // transfer gate as everything else (one socket at a time), but the decode
    // runs here with no lock held, so a decoder that loops on one bad webp
    // stalls only artwork - the 选集 fetch and 播放 keep working.
    for (;;)
    {
        std::string art_key;
        std::string art_path;
        int art_width = 0;
        bool have_art = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_)
            {
                return;
            }
            if (!art_queue_.empty())
            {
                art_key = art_queue_.front().key;
                art_path = art_queue_.front().path;
                art_width = art_queue_.front().width;
                art_queue_.erase(art_queue_.begin());
                have_art = true;
            }
        }
        if (!have_art)
        {
            idle();
            continue;
        }
        const std::int64_t job_t0 = sys::monotonic_us();
        sync_art(art_key, art_path, art_width);
        const std::int64_t job_us = sys::monotonic_us() - job_t0;
        if (job_us > 3000000)
        {
            sys::log("[HUI] art job done slow=%lldms key=%s", static_cast<long long>(job_us / 1000),
                     art_key.c_str());
        }
    }
}

void Service::sync(unsigned generation)
{
    std::string server;
    std::string user;
    std::string password;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        server = server_;
        user = user_;
        password = password_;
    }

    if (server.empty())
    {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.busy = false;
        view_.signed_in = false;
        view_.status = "未配置服务器";
        view_.libraries.clear();
        ++view_.revision;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        view_.busy = true;
        view_.status = "正在连接…";
        ++view_.revision;
    }

    std::string error;
    bool signed_in = false;
    std::string server_name;
    std::vector<Item> items;
    std::vector<Item> resume;
    std::vector<Library> libraries; // the server's media libraries, in order
    bool catalog_ok = false;

    {
        std::lock_guard<std::mutex> api_lock(api_mutex_);
        api_.set_server(server);
        signed_in = api_.login(user, password, &error);
        if (signed_in)
        {
            LibraryPage library_page;
            if (api_.libraries(&library_page, &error))
            {
                // One pass over the whole server, filtered to the row kinds a
                // wall draws. Two things about this are load-bearing:
                //
                //   * the `tags.type` filter is what stops the answer growing
                //     to every season, episode and file (this server: 241 cards
                //     versus 8954 rows), and
                //   * it is only honoured when no `ancestor_guid` is sent, so
                //     the request cannot be made per library. Each row carries
                //     the ancestor_guid it belongs to, and the rail partitions
                //     on that.
                static const std::vector<std::string> kTopTypes{"Movie", "TV", "Directory",
                                                                "Video"};
                long long collected = 0;
                bool items_ok = true;
                for (int page = 1; page <= kMaxPages; ++page)
                {
                    ItemPage chunk;
                    if (!api_.items(std::string(), kTopTypes, page, kPageSize, &chunk, &error))
                    {
                        // A partial first page is not a library. Keep the old
                        // view instead of publishing a half-empty wall; a
                        // single range timeout used to make the NAS look empty.
                        items_ok = false;
                        break;
                    }
                    for (auto item : chunk.items)
                    {
                        if (item.ancestor_name.empty())
                        {
                            item.ancestor_name =
                                library_title(library_page.items, item.ancestor_guid);
                        }
                        if (item.ancestor_category.empty())
                        {
                            item.ancestor_category =
                                library_category(library_page.items, item.ancestor_guid);
                        }
                        items.push_back(std::move(item));
                    }
                    collected += static_cast<long long>(chunk.items.size());
                    if (chunk.items.empty() || collected >= chunk.total)
                    {
                        break;
                    }
                }

                if (items_ok)
                {
                    // A handful of top-level rows are server-synthesised show
                    // wrappers (type TV / Directory) that come back with no
                    // `poster` of their own. The official client paints them
                    // from the show's own artwork - the first picture under the
                    // first season - and the server does return it there, so
                    // reach the same bytes rather than leave a grey tile for a
                    // title the library has artwork for. Only the empty rows
                    // cost a request (about sixteen per catalog pass), kept
                    // small, and a failure simply leaves the poster empty.
                    auto first_poster_among = [](const std::vector<Item> &rows) -> std::string
                    {
                        for (const Item &row : rows)
                        {
                            if (!row.poster.empty())
                            {
                                return row.poster;
                            }
                        }
                        return std::string();
                    };
                    std::string art_ignore;
                    for (Item &it : items)
                    {
                        if (!it.poster.empty() || it.guid.empty())
                        {
                            continue;
                        }
                        std::vector<Item> seasons;
                        api_.seasons(it.guid, &seasons, &art_ignore);
                        if (!seasons.empty())
                        {
                            ItemPage eps;
                            if (api_.children(seasons.front().guid, 1, 8, &eps, &art_ignore))
                            {
                                it.poster = first_poster_among(eps.items);
                            }
                        }
                        if (it.poster.empty())
                        {
                            // Directory-style wrappers list their content (or a
                            // season) directly under themselves instead of via
                            // /season/list; try one level, then one more.
                            ItemPage kids;
                            if (api_.children(it.guid, 1, 8, &kids, &art_ignore))
                            {
                                it.poster = first_poster_among(kids.items);
                                if (it.poster.empty() && !kids.items.empty())
                                {
                                    ItemPage grand;
                                    if (api_.children(kids.items.front().guid, 1, 8, &grand,
                                                      &art_ignore))
                                    {
                                        it.poster = first_poster_among(grand.items);
                                    }
                                }
                            }
                        }
                    }

                    ItemPage page;
                    // A missing resume list is not fatal: the library is still
                    // usable, and the local progress file remains authoritative.
                    if (api_.continue_watching(&page, &error))
                    {
                        resume = page.items;
                    }
                    libraries = library_page.items;
                    error.clear();
                    catalog_ok = true;
                }
            }
        }
    }

    // A reconfigure while this pass was running means the answer is stale.
    bool favorites_grew = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_)
        {
            return;
        }
        // One line the console log can be read against: how many cards came
        // back and how many libraries they belong to. Without it a wrong query
        // looks exactly like an empty library.
        sys::log("[HUI] library cards=%d libraries=%d resume=%d", static_cast<int>(items.size()),
                 static_cast<int>(libraries.size()), static_cast<int>(resume.size()));
        // Artwork diagnostic: how many rows came back with neither a poster
        // nor a backdrop from the server, so 海报 missing is attributed to the
        // API answer and not to a fetch/decode failure downstream. The list
        // endpoint sometimes fills `poster_list` only on the first page, or
        // leaves both empty for a freshly-scanned title.
        {
            int no_poster = 0;
            int no_backdrop = 0;
            for (const Item &it : items)
            {
                if (it.poster.empty())
                {
                    ++no_poster;
                }
                if (it.backdrop.empty())
                {
                    ++no_backdrop;
                }
            }
            sys::log("[HUI] art gaps no_poster=%d/%d no_backdrop=%d/%d", no_poster,
                     static_cast<int>(items.size()), no_backdrop, static_cast<int>(items.size()));
            // Enumerate the cards that will actually draw an empty poster - the
            // ones the viewer sees as a grey tile. Backdrop gaps are noise here
            // (157 of them, and the wall does not use a backdrop), so filter on
            // poster only, and tag each with its library ancestor so a device
            // grab can be cross-checked against a screenshot of a specific row.
            for (const Item &it : items)
            {
                if (!it.poster.empty())
                {
                    continue;
                }
                sys::log("[HUI] art blank guid=%s ancestor=%s title=%s", it.guid.c_str(),
                         it.ancestor_guid.c_str(), it.title.c_str());
            }
        }
        view_.busy = false;
        view_.signed_in = signed_in;
        if (!signed_in)
        {
            view_.status = error.empty() ? "登录失败" : ("登录失败：" + error);
            view_.libraries.clear();
            ++view_.revision;
            return;
        }
        if (!catalog_ok)
        {
            // Keep whatever was already on screen. Replacing a working wall
            // with an empty one on a transient HTTP failure is worse than
            // showing a stale library and an honest status line.
            view_.status = error.empty() ? "媒体库读取失败" : ("媒体库读取失败：" + error);
            ++view_.revision;
            return;
        }
        // Persist the working credentials so a restart or a redeploy that
        // refreshes the asset folder still auto-logs in without retyping.
        save_credentials(server, user, password);
        view_.server_name = server_name;
        // Seed the local favourite map from the server's own marks on the very
        // first sync after a deploy that had no state file yet. Prior versions
        // (v29/v30) kept stars in a memory-only override map that never
        // reached disk, so a fan who had starred things there would open 最近
        // 收藏 to an empty row until they re-starred on v31. Copying the
        // server's `is_favorite` (fnOS web app writes that field) into
        // favorites_ here means those entries survive and are the exact
        // opposite of a later un-star toggle, which stores false. Once a
        // guid has been touched locally it is left alone - the console wins
        // over the server.
        //
        // The write itself is deferred past the lock: save_state() takes the
        // same mutex_ again, and std::mutex is not recursive.
        {
            int seeded = 0;
            for (const Item &it : items)
            {
                if (it.guid.empty())
                {
                    continue;
                }
                if (favorites_.count(it.guid) != 0)
                {
                    continue;
                }
                if (it.is_favorite != 0)
                {
                    favorites_[it.guid] = true;
                    ++seeded;
                }
            }
            if (seeded > 0)
            {
                sys::log("[HUI] favorites seeded from server=%d", seeded);
                favorites_grew = true;
            }
        }
        view_.items = std::move(items);
        view_.resume = std::move(resume);
        view_.libraries = std::move(libraries);
        view_.status = view_.items.empty() ? "媒体库是空的" : std::string();
        // A poster that failed while the server was unreachable is worth
        // another try now that the library answered.
        art_failed_.clear();
        // The list rows carry no genre / country / colour / audio - only
        // /item/{guid} does - so requeue every card for the background enrich
        // pass. It drains one detail at a time in the idle gaps; the filter
        // panel reads whatever has landed so far and fills in as it goes.
        enrich_queue_.clear();
        for (const Item &it : view_.items)
        {
            if (!it.guid.empty())
            {
                enrich_queue_.push_back(it.guid);
            }
        }
        ++view_.revision;
    }
    // mutex_ is released by the closing brace above; the write down here does
    // not re-enter the lock and deadlock.
    if (favorites_grew)
    {
        save_state();
    }
}

void Service::sync_detail(const std::string &guid)
{
    Item item;
    std::vector<FileEntry> files;
    std::vector<Item> episodes;
    std::string error;

    {
        std::lock_guard<std::mutex> api_lock(api_mutex_);
        if (!api_.signed_in())
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!detail_guid_.empty() && detail_guid_ != guid)
            {
                return;
            }
            detail_ = Detail();
            detail_.error = "未登录";
            detail_.revision = ++detail_revision_;
            detail_loaded_.clear();
            return;
        }
        if (!api_.item(guid, &item, &error))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!detail_guid_.empty() && detail_guid_ != guid)
            {
                return;
            }
            detail_ = Detail();
            detail_.error = error.empty() ? "取详情失败" : error;
            detail_.revision = ++detail_revision_;
            detail_loaded_.clear();
            return;
        }
        api_.files(guid, &files, &error); // a missing file list is not fatal
        // The Chinese audio / subtitle chips read /stream/list; failing it just
        // drops those two chips, so it is not treated as fatal either.
        api_.streams(guid, &item, &error);

        // What actually plays sits one level down: a series keeps its episodes
        // under a season, a 短剧 folder keeps its files directly under itself,
        // and /item/{guid} describes none of them. Both are asked for here, on
        // the worker - the strip never waits inside draw().
        if (item.type == "TV")
        {
            std::vector<Item> seasons;
            if (api_.seasons(guid, &seasons, &error))
            {
                for (const Item &season : seasons)
                {
                    ItemPage chunk;
                    if (!api_.children(season.guid, 1, kEpisodePageSize, &chunk, &error))
                    {
                        continue;
                    }
                    for (auto episode : chunk.items)
                    {
                        if (episode.season_number <= 0)
                        {
                            episode.season_number = season.season_number;
                        }
                        episodes.push_back(std::move(episode));
                    }
                }
            }
        }
        else if (item.type == "Directory")
        {
            ItemPage chunk;
            if (api_.children(guid, 1, kEpisodePageSize, &chunk, &error))
            {
                episodes = std::move(chunk.items);
            }
        }
    }

    // The server hands the children back newest-first and numbers them only in
    // the file name ("S01E55.mp4"), so the strip would otherwise read backwards
    // and call everything 第 1 集.
    for (std::size_t i = 0; i < episodes.size(); ++i)
    {
        Item &episode = episodes[i];
        int season = 0;
        int number = 0;
        if (episode_number_from_name(episode.title, &season, &number))
        {
            if (episode.season_number <= 0 && season > 0)
            {
                episode.season_number = season;
            }
            if (episode.episode_number <= 0)
            {
                episode.episode_number = number;
            }
        }
        if (episode.episode_number <= 0)
        {
            // Nothing in the name to read: keep the server's order, which for a
            // 短剧 folder is 55, 54, 53 … so the count runs the other way.
            episode.episode_number = static_cast<int>(episodes.size() - i);
        }
        if (episode.title.empty())
        {
            episode.title = "第 " + std::to_string(episode.episode_number) + " 集";
        }
    }
    std::stable_sort(episodes.begin(), episodes.end(),
                     [](const Item &left, const Item &right)
                     {
                         if (left.season_number != right.season_number)
                         {
                             return left.season_number < right.season_number;
                         }
                         return left.episode_number < right.episode_number;
                     });

    // A series carries no artwork of its own: /item/{guid} for a TV row has
    // no poster, posters or backdrops field at all - every still belongs to an
    // episode. The web client shows the first episode's still as the show's
    // picture, so the detail page does the same rather than a blank panel.
    if (item.poster.empty() && !episodes.empty())
    {
        for (const Item &episode : episodes)
        {
            if (!episode.poster.empty())
            {
                item.poster = episode.poster;
                break;
            }
        }
    }
    if (item.backdrop.empty() && !episodes.empty())
    {
        for (const Item &episode : episodes)
        {
            // A film-style backdrop only; an episode's own poster is square-ish
            // and would be stretched across the 1920x720 hero.
            if (!episode.backdrop.empty())
            {
                item.backdrop = episode.backdrop;
                break;
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!detail_guid_.empty() && detail_guid_ != guid)
    {
        return;
    }
    detail_ = Detail();
    detail_.ready = true;
    // What the detail resolved, so a series with no episodes in the strip can
    // be told apart from one whose episodes the server did not list.
    const bool has_poster = !item.poster.empty();
    const bool has_backdrop = !item.backdrop.empty();
    sys::log("[HUI] detail %s type=%s episodes=%d files=%d poster=%d backdrop=%d", guid.c_str(),
             item.type.c_str(), static_cast<int>(episodes.size()), static_cast<int>(files.size()),
             has_poster ? 1 : 0, has_backdrop ? 1 : 0);
    detail_.item = std::move(item);
    detail_.files = std::move(files);
    detail_.episodes = std::move(episodes);
    detail_.revision = ++detail_revision_;
    detail_loaded_ = guid;
}

void Service::enrich_one(const std::string &guid)
{
    // The network runs with only api_mutex_ held, exactly like sync_detail: a
    // detail call takes a moment and must not freeze the shelf's lock. Then a
    // short lock merges whatever the answer added into the published row.
    Item detail;
    std::string error;
    {
        std::lock_guard<std::mutex> api_lock(api_mutex_);
        if (!api_.signed_in())
        {
            return;
        }
        if (!api_.item(guid, &detail, &error))
        {
            return; // transient miss: the next 刷新片源 reseeds and retries
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (Item &row : view_.items)
    {
        if (row.guid != guid)
        {
            continue;
        }
        bool changed = false;
        // Only fill a field the list left empty; a value already on the row (or
        // set by the user) is never overwritten by the background pass.
        const auto merge = [&changed](std::string &dst, const std::string &src) {
            if (dst.empty() && !src.empty())
            {
                dst = src;
                changed = true;
            }
        };
        merge(row.genres_text, detail.genres_text);
        merge(row.countries, detail.countries);
        merge(row.color_range, detail.color_range);
        merge(row.audio_type, detail.audio_type);
        merge(row.resolution, detail.resolution);
        merge(row.content_rating, detail.content_rating);
        if (changed)
        {
            ++view_.revision;
        }
        break;
    }
}

} // namespace hui::fnvideo

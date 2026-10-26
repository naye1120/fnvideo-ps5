// ps5-homebrew-ui - Design "fnvideo": the fnOS (飞牛) film and TV player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The screen a NAS library of films and series wants:
//
//   - a narrow rail on the left (首页/电影/剧集/其他/收藏/搜索/设置) and a
//     cinematic content zone on the right: a hero card that fills the width,
//     a 继续观看 row of 16:9 cards with their progress, and a 最近收藏 row of
//     portrait posters;
//   - the library page is a poster grid with filter chips, the detail page a
//     backdrop, a poster, the synopsis and the episode strip, and the player a
//     full-bleed frame with an OSD that never leaves the pad more than a press
//     away;
//   - one gliding focus ring, drawn by this design, carries the eye between
//     cards: no component owns the focus here, the page does.
//
// The data on screen is the NAS library once it answers, and - on the PC
// preview host, which has no NAS - an invented sample catalogue behind
// HUI_FNVIDEO_SAMPLE=1. See preview_sample().

#include "concepts/concepts.hpp"

#include "core/input.hpp"
#include "fnvideo/pinyin.hpp"
#include "fnvideo/service.hpp"
#include "platform/ps5/system.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hui::concepts
{
namespace
{

using gfx::Align;
using gfx::Color;
using gfx::Rect;

constexpr float kWidth = gfx::kVirtualWidth;
constexpr float kHeight = gfx::kVirtualHeight;
constexpr float kMargin = 96.0f;
constexpr float kContentX = 340.0f;
// The detail page's 剧情简介 folds to this many lines (the web page keeps a
// couple and hands the rest to 展开); it wraps to a narrower measure than the
// unfolded block, so the fold's width and line count live together and stay in
// step between the draw and the "is there something to expand" check.
constexpr float kSynopsisCollapsedWidth = 1240.0f;
constexpr int kSynopsisCollapsedLines = 4;
constexpr float kContentW = kWidth - kContentX - kMargin;
constexpr float kRailX = 64.0f;

// The palette: coal, cool ink, one blue for everything the player can press.
const Color kInk = Color::rgb(0xf6f8fb);
const Color kMuted = Color::rgb(0xccd5e1);
const Color kFaint = Color::rgb(0x9aa5b5);
const Color kAccent = Color::rgb(0x5aa2f0);
const Color kOnAccent = Color::rgb(0x0a1a2c);
// Endpoints of the glossy gradient on the primary 播放 button: a lit top edge
// fading to a deeper base, so the main action reads as a raised, pressable
// control (the flat solid fill took a shadow the dark backdrop swallowed).
const Color kAccentHi = Color::rgb(0x74b4f6);
const Color kAccentLo = Color::rgb(0x3f86d8);
const Color kCoal = Color::rgb(0x1c283f);
const Color kCoalLow = Color::rgb(0x27364f);
const Color kPanel = Color::rgb(0x35475f);
const Color kWhite = Color::rgb(0xffffff);
const Color kClear = Color::rgb(0x000000, 0.0f);

// The button surface recipe borrowed from the upstream UI kit: a soft raised
// body - a drop shadow the fill floats on, with a hairline. fnvideo keeps its
// own palette and its own focus cursor (draw_cursor), so only the surface's
// depth colours are read from here; the shapes are passed per call.
const ui::Theme &ui_theme()
{
    static const ui::Theme theme = []
    {
        ui::Theme t{};
        t.id = "fnvideo";
        t.name = "fnvideo";
        t.family = "";
        t.summary = "";
        t.page = kCoal;
        t.surface = kCoalLow;
        t.surface_high = kWhite.with_alpha(0.08f);
        t.text = kInk;
        t.text_muted = kMuted;
        t.primary = kAccent;
        t.on_primary = kOnAccent;
        t.secondary = kWhite.with_alpha(0.05f);
        t.on_secondary = kMuted;
        t.accent = kAccent;
        t.outline = kWhite.with_alpha(0.16f);
        t.focus = kAccent;
        t.shadow = Color::rgb(0x000000, 0.5f);
        t.light = kWhite.with_alpha(0.25f);
        t.style = ui::SurfaceStyle::soft;
        t.corner = ui::Corner::round;
        t.radius = 24.0f;
        t.border = 0.0f;
        t.shadow_offset = 6.0f;
        t.shadow_blur = 18.0f;
        t.dark = true;
        return t;
    }();
    return theme;
}

constexpr float kRailTop = 236.0f;
constexpr float kRailPitch = 66.0f;
constexpr float hero_index_Y = 116.0f;
constexpr float hero_index_H = 356.0f;
constexpr float kRowLabel1 = 548.0f;
constexpr float kRow1Y = 574.0f;
constexpr float kRow1H = 168.0f;
constexpr float kRowLabel2 = 782.0f;
constexpr float kRow2Y = 806.0f;
constexpr float kRow2H = 196.0f;
constexpr float kHintY = 1034.0f;
// The player's on-screen chrome leaves the picture alone once a film has been
// running for a moment. Three and a half seconds is long enough to read the
// title and reach the bar, short enough that it never sits over a scene.
constexpr float kPlayerOsdTimeout = 3.5f;

enum class Zone : int
{
    boot,
    home,
    library,
    detail,
    player,
    settings,
};

enum class Kind : int
{
    movie,
    show,
    other,
};

// What a card on screen needs, whichever side it came from: the sample
// catalogue below, or the NAS through fnvideo::Service. `poster` and
// `backdrop` hold the server's artwork path, empty for the sample rows, which
// paint their posters from `tint` instead.
struct Item
{
    std::string guid;
    // The collection this row belongs to. A card is one row, so this is the
    // row's own guid; the sample library uses it to fold eight episodes into
    // one show, and the wall groups by it.
    std::string series;
    // The series' name for an episode row, empty for a film. The wall and the
    // detail page name the collection, not whichever episode came first.
    std::string series_title;
    // The library the row was filed under (the server's ancestor_guid). The
    // rail partitions on this rather than on the category, because a 短剧
    // library holds rows the server categorised as TV.
    std::string library;
    std::string title;
    std::string meta;
    std::string genre;
    std::string synopsis;
    std::string badge;
    // The official detail page's metadata chips under the title. Everything here
    // is the server's own value - a chip is drawn only when the field is filled,
    // never guessed. `country` is already mapped to a Chinese display name.
    std::string content_rating;
    std::string country;
    std::string color_range_type;
    std::string audio_type;
    bool chi_audio = false;
    bool chi_subtitle = false;
    // The server's own values for the detail page's file card: the resolution
    // it advertises and the runtime in minutes. Nothing about them is guessed.
    std::string resolution;
    int runtime = 0;
    // The list call's seconds. Kept for the progress bar of a locally watched
    // episode whose server row never carried a duration.
    int duration = 0;
    std::string poster;
    std::string backdrop;
    std::string year; // "2026", empty when the server sent no date
    Kind kind = Kind::movie;
    std::string type;     // the server's raw type: Movie / TV / Video / Directory
    std::string category; // the server's ancestor_category: Movie / TV / Others / Mix
    int is_favorite = 0;  // the server's is_favorite flag, for the 收藏 shelf
    int watched = 0;      // the server's watched flag (0/1), for the 已观看 filter
    float rating = 0.0f;
    float progress = 0.0f;
    int episodes = 0; // how many episodes the collection holds, 0 for a film
    int season = 0;
    int episode = 0;
    // Precomputed search haystacks for the Latin keyboard (see search_matches):
    // the title's full pinyin, its syllable initials, and the server's Latin
    // original title lowercased. Filled in view_item() so the per-frame filter
    // is only a substring test.
    std::string search_pinyin;
    std::string search_initials;
    std::string search_latin;
    std::uint32_t tint = 0x2f5d63;
};

// One season of the detail page's strip: where its episodes sit in the flat
// list the page walks.
struct SeasonRun
{
    int season = 0;
    int begin = 0; // first episode in the detail's episode list
    int end = 0;   // one past the last
};

// Chips of one strip kept on screen at a time. A long row is walked by
// sliding this window instead of scrolling the page.
constexpr int kSeasonSlots = 10;
constexpr int kEpisodeSlots = 12;
// The detail page's four real actions: 播放, 收藏, and the two track selectors
// (音轨, 字幕). The seasons and episodes continue from this base, so the two
// new stops slide everything below them along without touching their maths.
constexpr int kDetailActions = 4;

// The settings form: three rows that open the keyboard, then the actions.
constexpr int kSettingsRows = 7;
constexpr float kSettingsTop = 250.0f;
constexpr float kSettingsRowPitch = 84.0f;
constexpr float kSettingsRowTop = 268.0f;
// Seven rows: the three text fields, 登录/退出登录 and 缓存/关于. Resolution and
// decoder live in the player's own toolbar, next to the picture they change.
constexpr float kSettingsHeight = 608.0f;

// The first chip of a strip that keeps `focus` inside the window, so a row
// with more entries than slots can still be walked end to end.
int strip_first(int focus, int slots, int total)
{
    if (total <= slots)
    {
        return 0;
    }
    return std::clamp(focus - slots + 1, 0, total - slots);
}

// One line of text kept inside max_width: it drops a size first - a smaller
// title still reads as the title - and only then takes an ellipsis. Font::fit
// cuts at codepoints, so a Han string is never split down the middle.
float fit_text(gfx::DrawList &list, const ui::FontRef &font, std::string_view value, float x,
               float baseline, float size, Color color, float max_width,
               gfx::Align align = gfx::Align::left)
{
    if (max_width <= 0.0f)
    {
        return 0.0f;
    }
    float draw_size = size;
    const float full = font.measure(value, size);
    if (full > max_width)
    {
        // Measure scales linearly with the size, so the width lands in one
        // step; the floor keeps the shrink moderate and lets the ellipsis
        // carry the rest.
        const float floor_size = std::max(size * 0.75f, 13.0f);
        draw_size = std::clamp(size * max_width / full, std::min(floor_size, size), size);
    }
    // Half a pixel of slack: a rounding wobble must not cost a glyph.
    if (font.measure(value, draw_size) <= max_width + 0.5f)
    {
        return ui::text(list, font, value, x, baseline, draw_size, color, align);
    }
    return ui::text(list, font, font.font->fit(value, draw_size, max_width), x, baseline, draw_size,
                    color, align);
}

const char *const kTechniques[] = {
    "a cinematic hero, a continue-watching row and a poster wall over one sample library",
    "one gliding focus ring drawn by the page instead of by a component",
    "posters painted from the palette: no artwork, no texture uploads",
    "the player's OSD: progress, episode strip and resolution / decoder chips",
};

// The first character of a UTF-8 string, for the watermark on a poster.
std::string first_glyph(std::string_view text)
{
    std::string out;
    if (text.empty())
        return out;
    const auto lead = static_cast<unsigned char>(text[0]);
    int length = 1;
    if ((lead & 0x80u) == 0)
        length = 1;
    else if ((lead & 0xe0u) == 0xc0u)
        length = 2;
    else if ((lead & 0xf0u) == 0xe0u)
        length = 3;
    else
        length = 4;
    out.assign(text.data(), static_cast<std::size_t>(length));
    return out;
}

// ISO 3166-1 alpha-2 codes -> Chinese display name, for the detail chip. The
// server sends production_countries as raw codes ("GB,US"); a code missing from
// the table is shown as-is so a rare country is never left blank.
std::string country_text(std::string_view codes)
{
    struct Name
    {
        const char *code;
        const char *zh;
    };
    static const Name kNames[] = {
        {"CN", "中国"},
        {"HK", "中国香港"},
        {"TW", "中国台湾"},
        {"MO", "中国澳门"},
        {"US", "美国"},
        {"GB", "英国"},
        {"JP", "日本"},
        {"KR", "韩国"},
        {"FR", "法国"},
        {"DE", "德国"},
        {"CA", "加拿大"},
        {"AU", "澳大利亚"},
        {"IN", "印度"},
        {"IT", "意大利"},
        {"ES", "西班牙"},
        {"RU", "俄罗斯"},
        {"TH", "泰国"},
        {"SE", "瑞典"},
        {"NO", "挪威"},
        {"DK", "丹麦"},
        {"NL", "荷兰"},
        {"BE", "比利时"},
        {"IE", "爱尔兰"},
        {"CH", "瑞士"},
        {"AT", "奥地利"},
        {"PL", "波兰"},
        {"CZ", "捷克"},
        {"BR", "巴西"},
        {"MX", "墨西哥"},
        {"NZ", "新西兰"},
        {"SG", "新加坡"},
        {"ID", "印度尼西亚"},
        {"PH", "菲律宾"},
        {"VN", "越南"},
        {"MY", "马来西亚"},
        {"PT", "葡萄牙"},
        // Codes the library actually uses but the first table missed - a real
        // /item/{guid} sweep showed TR / SK / ZA / SA / LU leaking through as raw
        // two-letter codes while every other country read in Chinese, which is
        // the inconsistency on the detail chips. Extended with the neighbouring
        // common ISO 3166-1 alpha-2 codes so rarely-seen origins stay coherent.
        {"TR", "土耳其"},
        {"SK", "斯洛伐克"},
        {"ZA", "南非"},
        {"SA", "沙特阿拉伯"},
        {"LU", "卢森堡"},
        {"IL", "以色列"},
        {"AE", "阿联酋"},
        {"GR", "希腊"},
        {"FI", "芬兰"},
        {"IS", "冰岛"},
        {"HU", "匈牙利"},
        {"RO", "罗马尼亚"},
        {"BG", "保加利亚"},
        {"HR", "克罗地亚"},
        {"RS", "塞尔维亚"},
        {"UA", "乌克兰"},
        {"AR", "阿根廷"},
        {"CL", "智利"},
        {"CO", "哥伦比亚"},
        {"PE", "秘鲁"},
        {"EG", "埃及"},
        {"NG", "尼日利亚"},
        {"KE", "肯尼亚"},
        {"MA", "摩洛哥"},
        {"IR", "伊朗"},
        {"IQ", "伊拉克"},
        {"PK", "巴基斯坦"},
        {"KZ", "哈萨克斯坦"},
    };
    std::string out;
    std::size_t begin = 0;
    while (begin <= codes.size())
    {
        const std::size_t comma = codes.find(',', begin);
        const std::string_view code = codes.substr(
            begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
        if (!code.empty())
        {
            if (!out.empty())
            {
                out += " / ";
            }
            const char *name = nullptr;
            for (const Name &entry : kNames)
            {
                if (code == entry.code)
                {
                    name = entry.zh;
                    break;
                }
            }
            out.append(name != nullptr ? name : std::string(code));
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        begin = comma + 1;
    }
    return out;
}

std::string clock_text(float seconds)
{
    const int total = static_cast<int>(seconds);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", total / 3600, (total / 60) % 60,
                  total % 60);
    return buffer;
}

// The PC preview host has no NAS to talk to, so every content-bearing page
// would be a blank photograph. HUI_FNVIDEO_SAMPLE=1 (only the host sets it)
// puts an invented library in its place; a console never sets it, so a real
// library - and a real sign-in failure - can never be papered over.
bool preview_sample()
{
    const char *on = std::getenv("HUI_FNVIDEO_SAMPLE");
    return on != nullptr && on[0] == '1';
}

// "1 小时 46 分" from a minute count.
std::string runtime_text(long long minutes)
{
    if (minutes <= 0)
    {
        return std::string();
    }
    const long long hours = minutes / 60;
    const long long rest = minutes % 60;
    std::string out;
    if (hours > 0)
    {
        out += std::to_string(hours);
        out += " 小时 ";
    }
    if (rest > 0 || hours == 0)
    {
        out += std::to_string(rest);
        out += " 分";
    }
    while (!out.empty() && out.back() == ' ')
    {
        out.pop_back();
    }
    return out;
}

// A stable colour per title, so a poster keeps its tint between launches
// while the real cover is still being fetched.
std::uint32_t tint_for(const std::string &text)
{
    std::uint32_t hash = 2166136261u;
    for (const char ch : text)
    {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 16777619u;
    }
    const float hue = static_cast<float>(hash % 360u) / 60.0f;
    constexpr float saturation = 0.42f;
    constexpr float value = 0.44f;
    const int sector = static_cast<int>(hue) % 6;
    const float fraction = hue - static_cast<float>(static_cast<int>(hue));
    const float p = value * (1.0f - saturation);
    const float q = value * (1.0f - saturation * fraction);
    const float t = value * (1.0f - saturation * (1.0f - fraction));
    float r = value;
    float g = p;
    float b = q;
    switch (sector)
    {
    case 0:
        r = value;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = value;
        b = p;
        break;
    case 2:
        r = p;
        g = value;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = value;
        break;
    case 4:
        r = t;
        g = p;
        b = value;
        break;
    default:
        r = value;
        g = p;
        b = q;
        break;
    }
    const auto channel = [](float unit)
    { return static_cast<std::uint32_t>(unit * 255.0f + 0.5f) & 0xffu; };
    return (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

// "挣脱枷锁的她（55集）吴鑫&贡兴等 - SKIT" -> 55. A 短剧 folder is a Directory
// row: the server counts no episodes for it, but the folder name does, and the
// card's 集数 line reads better than nothing.
int episode_count_from_title(const std::string &title)
{
    for (std::size_t at = title.find("集"); at != std::string::npos; at = title.find("集", at + 3))
    {
        std::size_t begin = at;
        while (begin > 0 && std::isdigit(static_cast<unsigned char>(title[begin - 1])))
        {
            --begin;
        }
        if (begin < at)
        {
            return std::atoi(title.substr(begin, at - begin).c_str());
        }
    }
    return 0;
}

// Whether a row is named after its file ("S01E55.mp4") rather than its title.
// A 短剧 folder's files and a series' episodes both are, and the card should
// name the collection instead.
bool named_after_file(const std::string &title)
{
    static const char *const kSuffixes[] = {".mp4",  ".mkv", ".ts",  ".avi",  ".mov",
                                            ".m2ts", ".wmv", ".flv", ".rmvb", ".webm"};
    for (const char *suffix : kSuffixes)
    {
        const std::string tail(suffix);
        if (title.size() > tail.size() &&
            title.compare(title.size() - tail.size(), tail.size(), tail) == 0)
        {
            return true;
        }
    }
    return false;
}

// Which shelf an API item belongs on. The server calls them ancestor
// categories; 短剧 is a Mix library of episodes and reads as a series.
Kind kind_of(const fnvideo::Item &source)
{
    if (source.ancestor_category == "Movie" || source.type == "Movie")
    {
        return Kind::movie;
    }
    if (source.ancestor_category == "Others" || source.type == "Directory")
    {
        return source.ancestor_category == "Others" ? Kind::other : Kind::show;
    }
    return Kind::show;
}

// The badge is the resolution the server advertises, upper-cased to match the
// sample catalogue's "4K"/"1080P".
std::string badge_text(const fnvideo::Item &source)
{
    std::string out = source.resolution;
    for (char &ch : out)
    {
        if (ch >= 'a' && ch <= 'z')
        {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    return out;
}

// "2026 · 电影 · 1 小时 46 分", the line under a card's title.
std::string meta_text(const fnvideo::Item &source, Kind kind)
{
    std::string out;
    if (source.release_date.size() >= 4)
    {
        out += source.release_date.substr(0, 4);
        out += " · ";
    }
    out += kind == Kind::movie ? "电影" : (kind == Kind::show ? "剧集" : "视频");
    const std::string length = runtime_text(source.runtime);
    if (!length.empty())
    {
        out += " · ";
        out += length;
    }
    return out;
}

// The on-screen keyboard is the kit's ui::Keyboard: it paints to this page's
// DrawList, carries shift, symbols and a repeatable backspace, and is the same
// control the other concepts use, so the form reads as part of the system.

class Fnvideo final : public app::Concept
{
  public:
    explicit Fnvideo(app::Context &context) : version_(context.version)
    {
        glyphs_ = context.fonts;
        if (glyphs_.cjk.font != nullptr)
        {
            glyphs_.regular = glyphs_.cjk;
            glyphs_.semibold = glyphs_.cjk;
            glyphs_.display = glyphs_.cjk;
        }
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "fnvideo",
            "飞牛影视",
            "飞牛 fnOS 影视服务：电影、剧集、继续观看与全屏播放器",
            "src/concepts/fnvideo.cpp",
            audio::SoundSet::glass,
            Color::rgb(0x5aa2f0),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        zone_ = Zone::boot;
        boot_age_ = 0.0f;
        enter_ = 0.0f;
        rail_ = 0;
        rail_focus_ = false;
        row_ = 0;
        col_ = 0;
        continue_scroll_ = 0;
        grid_ = 0;
        detail_ = 0;
        detail_focus_ = 0;
        episode_ = 0;
        paused_ = false;
        position_ = 0.0f;
        player_bar_ = false;
        player_chip_ = 0;
        player_menu_ = PlayerMenu::none;
        player_menu_sel_ = 0;
        player_osd_ = true;
        player_idle_ = 0.0f;
        stick_hud_timer_ = 0.0f;
        settings_row_ = 0;
        cursor_ready_ = false;
        // A new launch picks a new 随机推荐 instead of always opening the same
        // film. The choice is made once per launch, when the library arrives,
        // not on every revision bump.
        hero_picked_ = false;
        hero_seed_ = static_cast<std::uint64_t>(sys::monotonic_us());
        hero_slot_n_ = 0;
        hero_slide_ = 0;
        hero_rotate_ = 0.0f;
        hero_swap_ = 1.0f;

        // The library lives behind a worker thread. Point it at the file the
        // app ships, start it once, and let the first frames draw the sample
        // catalogue until the NAS answers - a blank screen would be worse
        // than an invented one.
        auto &service = fnvideo::Service::instance();
        // The writable file wins. The shipped file is only a first-run
        // fallback; loading it unconditionally would put the shipped account
        // and password back over anything the viewer changed in settings on
        // every launch.
        service.configure_from_file("/app0/fnvideo.conf");
        if (!service.configured())
        {
            service.configure_from_file("/app0/assets/fnvideo.conf");
        }
        service.ensure_started();
        sync_library();
        // The library may have been rebuilt while another design was in
        // front; the episode strip belongs to detail_, so it is remade here.
        rebuild_detail_episodes();

        // Seed the form from whatever the conf file already supplied, so the
        // 登录 button can re-authenticate with a stored password without the
        // user re-typing the address. The password itself is not exposed by
        // the service, so that draft stays empty until the user types one.
        server_text_ = fnvideo::Service::instance().server();
        account_text_ = fnvideo::Service::instance().account();
        password_text_.clear();
        settings_edit_ = -1;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        clock_ += dt;
        enter_ = std::min(1.0f, enter_ + dt * 2.6f);
        // The Options shortcut's "已重置筛选" confirmation, fading over the wall.
        if (filter_toast_ > 0.0f)
        {
            filter_toast_ = std::max(0.0f, filter_toast_ - dt);
        }
        // The 随机推荐 banner: ease a freshly-rotated card in over its tinted
        // floor (the kit's hero swap), and while on 首页 dwell 3s before moving
        // to the next card. A manual L1/R1 flip (update_home) resets the dwell.
        if (hero_swap_ < 1.0f)
        {
            hero_swap_ = std::min(1.0f, hero_swap_ + dt / 0.38f);
        }
        if (zone_ == Zone::home && hero_slot_n_ > 1)
        {
            hero_rotate_ += dt;
            if (hero_rotate_ >= 3.0f)
            {
                set_hero_slide(hero_slide_ + 1);
            }
        }
        sync_library();
        // A detail answer lands a moment after the page asks for it, so the
        // episode strip is rebuilt when the revision moves, not only when the
        // page opens.
        sync_detail_answer();
        // A fresh publish, a rail switch, a filter or a sort changed the shelf
        // projection; rebuild it once here instead of in the const draw path.
        if (shelf_dirty_)
        {
            rebuild_shelf();
            shelf_dirty_ = false;
        }
        // Posters for the page that is about to be drawn. Asking again for one
        // that is here or on its way costs a hash lookup, so this is safe to
        // run on every frame.
        request_art();

        if (zone_ == Zone::boot)
        {
            boot_age_ += dt;
            // A press lifts the splash, but not the shell's hand-off press and
            // not a stray direction from a controller resting on a stick. The
            // old 0.4 s / any-nav rule skipped the animation almost every
            // launch, which read as "the startup animation is broken".
            const bool skipped = boot_age_ > 1.1f && (input.is_pressed(Action::confirm) ||
                                                      input.is_pressed(Action::back));
            if (skipped || boot_age_ >= 2.8f)
            {
                feedback.play(audio::Cue::back);
                zone_ = Zone::home;
            }
            return;
        }

        if (zone_ == Zone::player)
            update_player(input, dt, feedback);
        else
            update_pages(input, feedback);

        // Advance the keyboard's springs only while a field is open.
        if ((zone_ == Zone::settings && settings_edit_ >= 0) ||
            (zone_ == Zone::library && search_edit_))
            keyboard_.update(dt);

        glide_focus(dt);
    }

    void draw(app::Frame &frame) const override
    {
        // While the decoder is painting, its picture covers every pixel the
        // procedural backdrop would show, so the shader pass is pure
        // overdraw. Reading playback costs one lock; the pass costs a full
        // screen of fragments.
        const fnvideo::Service::Playback play = fnvideo::Service::instance().playback();
        const bool painting = zone_ == Zone::player &&
                              fnvideo::Service::instance().video_texture() != 0 &&
                              (play.playing || play.paused);
        // Blue "homebrew lab" aurora: slow domain-warped colour clouds behind the
        // whole UI, driven by clock_. The decoder picture covers every pixel while
        // playing, so the shader pass is skipped then.
        frame.backdrop.mode = painting ? gfx::BackdropMode::none : gfx::BackdropMode::aurora;
        frame.backdrop.colors[0] = Color::rgb(0x0a1a30); // deep navy (top)
        frame.backdrop.colors[1] = Color::rgb(0x163058); // blue (bottom)
        frame.backdrop.colors[2] = Color::rgb(0x2f6fb5); // aurora cloud, mid blue
        frame.backdrop.colors[3] = kAccent;              // aurora cloud, bright blue
        frame.backdrop.time = clock_;

        gfx::DrawList &list = frame.scene;
        if (zone_ == Zone::boot)
        {
            draw_boot(list);
            return;
        }

        if (zone_ == Zone::player)
        {
            draw_player(list);
            return;
        }

        // The detail page is full-bleed, as the web page is: no rail, no
        // top bar, its own breadcrumb.
        if (zone_ != Zone::detail)
        {
            draw_rail(list);
            draw_top(list);
        }
        switch (zone_)
        {
        case Zone::home:
            draw_home(list);
            break;
        case Zone::library:
            draw_library(list);
            break;
        case Zone::detail:
            draw_detail(list);
            break;
        case Zone::settings:
            draw_settings(list);
            break;
        default:
            break;
        }
        draw_cursor(list);
        draw_hints(list);
        // The resume dialog paints on top of the page that opened it, and
        // swallows the pad via update_pages() above.
        if (resume_ask_)
        {
            draw_resume_ask(list);
        }
        // The track chooser overlays the detail page the same way, on top of
        // the cursor and hints so nothing of the page behind stays interactive.
        if (zone_ == Zone::detail && detail_track_menu_ != 0)
        {
            draw_detail_track_menu(list);
        }
        // The library filter panel overlays its page the same way, on top of the
        // cursor and hints so nothing of the wall behind it stays interactive.
        if (zone_ == Zone::library && panel_open_)
        {
            draw_filter_panel(list);
        }
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    // A pull-down over the picture's lower edge. Audio and subtitle menus list
    // the container's own tracks; volume and brightness are sliders so a
    // left/right press changes them without leaving the film.
    enum class PlayerMenu
    {
        none,
        audio,
        subtitle,
        // The web player's 字幕配置, minus a local upload: an online subtitle
        // search whose rows download straight into the media folder.
        subtitle_search,
        // 本地字幕：mediasrv 0.8.43 withholds every folder endpoint (HTTP
        // 501), so the picker lists the film's own folder over the NAS' FTP
        // and confirming a name registers it through /subtitle/mark.
        subtitle_browse,
        volume,
        brightness,
    };

    // ---- input ----

    void update_pages(const InputFrame &input, app::Feedback &feedback)
    {
        // The resume dialog takes the pad before anything else: a stray L2/R2
        // or a Cross on the page behind it must not start a second film.
        if (resume_ask_)
        {
            update_resume_ask(input, feedback);
            return;
        }
        // The detail page's audio / subtitle chooser owns the pad while it is
        // up, for the same reason: a stray shoulder or Cross must not move the
        // page focus behind the list.
        if (zone_ == Zone::detail && detail_track_menu_ != 0)
        {
            update_detail_track_menu(input, feedback);
            return;
        }
        // If the viewer left the series page before its episode list arrived,
        // the pending banner play must die with the page; otherwise returning
        // later would start a film they no longer asked for.
        if (hero_play_pending_ && zone_ != Zone::detail)
        {
            hero_play_pending_ = false;
        }
        // While a settings field is open the keyboard owns the pad: back must
        // cancel the edit, not drop out of the settings page. Handle that here
        // so update_settings() sees the press before the home-exit below does.
        if (zone_ == Zone::settings && settings_edit_ >= 0)
        {
            update_settings(input, feedback);
            return;
        }
        // The search keyboard owns the pad while it is open, for the same
        // reason: Circle closes the keyboard, it does not leave the shelf.
        if (zone_ == Zone::library && search_edit_)
        {
            update_library(input, feedback);
            return;
        }
        // The filter panel owns the pad while it is up, so a stray shoulder or
        // Cross cannot move the wall behind it.
        if (zone_ == Zone::library && panel_open_)
        {
            update_filter_panel(input, feedback);
            return;
        }
        // Options is the 筛选 shortcut. Handled here, BEFORE the rail and quick-
        // strip blocks, so it reaches the panel from every library state - the
        // wall, a focused rail (the rail commits on move, so a board is already
        // behind it), or the chip strip. A clean board opens the panel (快速开始);
        // a filtered board snaps every dimension back to 全部 (快速重置). Not while
        // the search keyboard is up, where Options means 完成.
        if (zone_ == Zone::library && !panel_open_ && !search_edit_ &&
            input.is_pressed(Action::menu))
        {
            if (active_filter_count() > 0)
            {
                reset_filters();
                filter_toast_ = 1.8f;
            }
            else
            {
                panel_open_ = true;
                panel_row_ = 0;
                panel_col_ = row_selection(0);
            }
            feedback.play(audio::Cue::select);
            return;
        }
        // L2 and R2 walk the shelf and switch on move, in every zone and even
        // while the rail holds the cursor - so a shoulder press always changes
        // the section without first leaving the rail. L1 and R1 belong to the
        // shell, which uses them to switch designs.
        if (input.is_pressed(Action::jump_next))
        {
            rail_ = (rail_ + 1) % rail_count();
            open_rail(feedback);
            return;
        }
        if (input.is_pressed(Action::jump_prev))
        {
            rail_ = (rail_ + rail_count() - 1) % rail_count();
            open_rail(feedback);
            return;
        }

        // The rail takes the pad while it has the focus: up and down walk it,
        // confirm opens a section, right or back returns to the page.
        if (rail_focus_)
        {
            if (input.nav == Direction::down && rail_ + 1 < rail_count())
            {
                ++rail_;
                // The rail commits on move: the section under the cursor opens
                // without a second press. rail_focus_ stays up so the viewer
                // keeps walking; right/back leaves them on the last section.
                open_rail(feedback);
                feedback.play(audio::Cue::focus);
            }
            else if (input.nav == Direction::up && rail_ > 0)
            {
                --rail_;
                open_rail(feedback);
                feedback.play(audio::Cue::focus);
            }
            else if (input.is_pressed(Action::confirm))
            {
                rail_focus_ = false;
                open_rail(feedback);
            }
            else if (input.nav == Direction::right || input.is_pressed(Action::back))
            {
                rail_focus_ = false;
                feedback.play(audio::Cue::back);
            }
            return;
        }

        // L2 and R2 already handled above, before the rail's own focus block.
        if (input.is_pressed(Action::back))
        {
            if (zone_ == Zone::detail)
            {
                // A detail page remembers where it was opened from. Home used
                // to return to the library tab, which could even be 首页 (an
                // empty grid) when the hero was opened before any tab.
                hero_play_pending_ = false;
                zone_ = detail_back_;
                feedback.play(audio::Cue::back);
                return;
            }
            if (zone_ == Zone::library || zone_ == Zone::settings)
            {
                rail_ = 0;
                zone_ = Zone::home;
                feedback.play(audio::Cue::back);
                return;
            }
        }

        if (zone_ == Zone::home)
            update_home(input, feedback);
        else if (zone_ == Zone::library)
            update_library(input, feedback);
        else if (zone_ == Zone::detail)
            update_detail(input, feedback);
        else if (zone_ == Zone::settings)
            update_settings(input, feedback);
    }

    void open_rail(app::Feedback &feedback)
    {
        if (rail_is_home(rail_))
            zone_ = Zone::home;
        else if (rail_is_settings(rail_))
            zone_ = Zone::settings;
        else
        {
            // 收藏, 搜索 and each library tab all draw the poster grid; the
            // shelf a tab filters by lives in shelf_match(), not the zone.
            zone_ = Zone::library;
            grid_ = 0;
            grid_scroll_ = 0;
            // A rail switch closes the filter panel and the quick strip, and
            // remakes the shelf projection for the new tab's own shelf_match.
            // 筛选只对当前板块生效: moving to another 板块 drops the filters, so
            // each board starts clean instead of inheriting the last one's set.
            panel_open_ = false;
            chip_focus_ = false;
            reset_filters();
            shelf_dirty_ = true;
            // Leaving a tab closes its search keyboard, if it was open. A
            // freshly chosen 搜索 tab opens it: that tab's only job is a query.
            search_edit_ = false;
            // Only the confirm press opens the search keyboard; walking the rail
            // onto 搜索 previews the shelf without grabbing the pad.
            if (rail_is_search(rail_) && !rail_focus_)
            {
                begin_search(feedback);
            }
        }
        row_ = 0;
        col_ = 0;
        continue_scroll_ = 0;
        refresh_focus_ = false;
        feedback.play(audio::Cue::tab);
    }

    // The rail lists the NAS's own libraries plus four fixed entries: 首页,
    // then one tab per fnvideo::Library (its title, filtered by its category),
    // then 收藏 / 搜索 / 设置. Index 0 is always 首页; the settings tab is
    // always last.
    int rail_library_count() const
    {
        return static_cast<int>(libraries_.size());
    }
    int rail_count() const
    {
        return 1 + rail_library_count() + 3;
    }
    int rail_settings_index() const
    {
        return rail_count() - 1;
    }
    int rail_search_index() const
    {
        return rail_count() - 2;
    }
    int rail_favorites_index() const
    {
        return rail_count() - 3;
    }
    bool rail_is_home(int i) const
    {
        return i == 0;
    }
    bool rail_is_library(int i) const
    {
        return i >= 1 && i <= rail_library_count();
    }
    bool rail_is_favorites(int i) const
    {
        return i == rail_favorites_index();
    }
    bool rail_is_search(int i) const
    {
        return i == rail_search_index();
    }
    bool rail_is_settings(int i) const
    {
        return i == rail_settings_index();
    }
    const fnvideo::Library &rail_library(int i) const
    {
        return libraries_[static_cast<std::size_t>(i - 1)];
    }
    // The text a rail row shows. Library tabs carry the server's own name; a
    // library with no title falls back to its category code.
    const char *rail_label(int i) const
    {
        if (i <= 0)
            return "首页";
        if (rail_is_library(i))
        {
            const fnvideo::Library &lib = rail_library(i);
            return lib.title.empty() ? lib.category.c_str() : lib.title.c_str();
        }
        if (rail_is_favorites(i))
            return "收藏";
        if (rail_is_search(i))
            return "搜索";
        return "设置";
    }
    // How many cards a rail tab actually holds, counted the way the grid does
    // (over the deduped wall_, one row per collection). 首页 / 搜索 / 设置 have
    // no shelf of their own, so they carry no number and return 0.
    int rail_item_count(int i) const
    {
        if (rail_is_favorites(i))
        {
            int n = 0;
            for (const int head : wall_)
                if (items_[static_cast<std::size_t>(head)].is_favorite != 0)
                    ++n;
            return n;
        }
        if (rail_is_library(i))
        {
            const fnvideo::Library &library = rail_library(i);
            int n = 0;
            for (const int head : wall_)
            {
                const Item &item = items_[static_cast<std::size_t>(head)];
                if (!item.library.empty() ? item.library == library.guid
                                          : item.category == library.category)
                    ++n;
            }
            return n;
        }
        return 0;
    }
    // Case-insensitive substring, so "战" or "war" both find "战争片". Titles
    // are Han more often than not, and server categories are ASCII.
    static bool title_contains(const std::string &title, const std::string &query)
    {
        if (query.empty())
            return true;
        const auto lower = [](char ch)
        { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); };
        std::string hay;
        std::string needle;
        hay.reserve(title.size());
        needle.reserve(query.size());
        for (const char ch : title)
            hay.push_back(lower(ch));
        for (const char ch : query)
            needle.push_back(lower(ch));
        return hay.find(needle) != std::string::npos;
    }

    // The 搜索 query is typed on the Latin keyboard, so a Han title is reached
    // through pinyin rather than the characters themselves. The query is matched
    // as a substring of the title's full syllables ("jidao"), its initials
    // ("jdcs"), or the Latin original title; a plain substring of the title is
    // kept too, so a pure-ASCII row still matches when typed directly.
    static bool search_matches(const Item &item, const std::string &query)
    {
        if (query.empty())
            return true;
        std::string needle;
        needle.reserve(query.size());
        for (const char ch : query)
        {
            if (std::isalnum(static_cast<unsigned char>(ch)))
            {
                needle.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
        }
        if (needle.empty())
            return true; // only spaces / punctuation typed: do not empty the shelf
        return title_contains(item.title, query) ||
               item.search_pinyin.find(needle) != std::string::npos ||
               item.search_initials.find(needle) != std::string::npos ||
               item.search_latin.find(needle) != std::string::npos;
    }

    // The top-bar 刷新片源 button is a real target only on the shelves that hold
    // a wall: home and any library / 收藏 tab. The 搜索 tab is a query with no
    // shelf to refresh and settings has no wall, so neither gets the button.
    bool refresh_available() const
    {
        if (zone_ == Zone::home)
        {
            return true;
        }
        return zone_ == Zone::library && !rail_is_search(rail_);
    }

    // While the refresh button holds the pad it owns every press: Cross fires
    // the re-pull, Down or Circle hands the focus back to the wall. It returns
    // true when it consumed the frame so the page's own navigation is skipped.
    bool handle_refresh_focus(const InputFrame &input, app::Feedback &feedback)
    {
        if (!refresh_focus_)
        {
            return false;
        }
        if (input.is_pressed(Action::confirm))
        {
            fnvideo::Service::instance().refresh();
            feedback.play(audio::Cue::select);
            return true; // stay put while the button reads 刷新中…
        }
        if (input.nav == Direction::down || input.is_pressed(Action::back))
        {
            refresh_focus_ = false;
            feedback.play(audio::Cue::back);
            return true;
        }
        return true; // left/right/up do nothing while the button is focused
    }

    void update_home(const InputFrame &input, app::Feedback &feedback)
    {
        if (handle_refresh_focus(input, feedback))
        {
            return;
        }
        // L1 / R1 flip the 随机推荐 banner to the previous / next card. The D-pad
        // left/right already drives the 播放 / 详情 buttons, so the shoulders steer
        // the rotation (single-concept build: the shell leaves them for us).
        if (hero_slot_n_ > 1 &&
            (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev)))
        {
            set_hero_slide(hero_slide_ + (input.is_pressed(Action::page_next) ? 1 : -1));
            feedback.play(audio::Cue::focus);
            return;
        }
        const int rows = 3; // hero, continue, recent
        if (input.nav == Direction::down)
        {
            // Skip rows the NAS has not filled yet, so the focus never lands on
            // a phantom card. A home row is empty until the worker publishes.
            int next = row_ + 1;
            while (next < rows && row_count(next) == 0)
            {
                ++next;
            }
            if (next < rows)
            {
                // Carry the column over instead of snapping back to the first
                // card. Walking a grid row by row should keep the horizontal
                // position, clamped when the next row is shorter.
                const int keep = row_ == 0 ? 0 : col_;
                row_ = next;
                col_ = std::min(keep, std::max(0, row_count(next) - 1));
                feedback.play(audio::Cue::focus);
            }
            else
            {
                feedback.play(audio::Cue::error);
            }
        }
        else if (input.nav == Direction::up)
        {
            int prev = row_ - 1;
            while (prev >= 0 && row_count(prev) == 0)
            {
                --prev;
            }
            if (prev >= 0)
            {
                const int keep = row_ == 0 ? 0 : col_;
                row_ = prev;
                col_ = std::min(keep, std::max(0, row_count(prev) - 1));
                feedback.play(audio::Cue::focus);
            }
            else if (row_ == 0 && refresh_available())
            {
                // Above the hero there is no row: the top-bar 刷新片源 button
                // lives one notch higher, so Up lands the focus on it.
                refresh_focus_ = true;
                feedback.play(audio::Cue::focus);
            }
            else
            {
                feedback.play(audio::Cue::error);
            }
        }
        else if (input.nav == Direction::right)
        {
            const int count = row_count(row_);
            if (col_ + 1 < count)
            {
                ++col_;
                feedback.play(audio::Cue::focus);
            }
            else
            {
                feedback.play(audio::Cue::error);
            }
        }
        else if (input.nav == Direction::left)
        {
            if (col_ > 0)
            {
                --col_;
                feedback.play(audio::Cue::focus);
            }
            else
            {
                // The leftmost card (or the hero's 播放 button) hands the focus
                // to the rail, which is how a player reaches 设置 without a
                // second stick.
                rail_focus_ = true;
                feedback.play(audio::Cue::focus);
            }
        }

        if (row_ == 1)
        {
            clamp_continue_scroll();
        }

        if (input.is_pressed(Action::confirm))
        {
            if (items_.empty() || row_count(row_) == 0)
            {
                feedback.play(audio::Cue::error);
                return;
            }
            if (row_ == 0)
            {
                // The hero's two painted buttons are real targets. 播放 means
                // the same thing for a film and a series: start watching. A
                // series has no single file, so play_hero() waits for its
                // episode list and starts the next episode; 详情 opens the
                // page without starting anything.
                if (col_ == 0)
                {
                    play_hero(feedback);
                }
                else if (open_detail(hero_index_, Zone::home))
                {
                    zone_ = Zone::detail;
                    feedback.play(audio::Cue::select);
                }
                return;
            }
            if (open_detail(home_item(row_, col_), Zone::home))
            {
                zone_ = Zone::detail;
                feedback.play(audio::Cue::select);
            }
        }
    }

    // Rotate the banner to another 随机推荐 slot (auto dwell or L1/R1): point
    // hero_index_ at the new card, ease it in, and restart the dwell timer so a
    // hand flip isn't instantly overwritten by the clock.
    void set_hero_slide(int slide)
    {
        if (hero_slot_n_ <= 0)
        {
            return;
        }
        hero_slide_ = ((slide % hero_slot_n_) + hero_slot_n_) % hero_slot_n_;
        hero_index_ = hero_slots_[static_cast<std::size_t>(hero_slide_)];
        hero_swap_ = 0.0f;
        hero_rotate_ = 0.0f;
    }

    // The banner's playback button now works the same way for movies and
    // series: open the detail page, where the viewer can press 播放 to start.
    // This matches the behaviour of the 飞牛影视 web page and TV app, where
    // clicking a hero card always enters the detail page first.
    void play_hero(app::Feedback &feedback)
    {
        const Item hero = item_at(hero_index_);
        if (hero.guid.empty())
        {
            feedback.play(audio::Cue::error);
            return;
        }
        if (!open_detail(hero_index_, Zone::home))
        {
            feedback.play(audio::Cue::error);
            return;
        }
        zone_ = Zone::detail;
        feedback.play(audio::Cue::select);
    }

    void update_library(const InputFrame &input, app::Feedback &feedback)
    {
        if (handle_refresh_focus(input, feedback))
        {
            return;
        }
        // The quick strip under the heading owns the pad while focused: left /
        // right pick a preset, Up hands off to the top-bar 刷新 button, Down /
        // Circle drop back to the wall, and Cross applies (筛选 opens the panel).
        if (chip_focus_)
        {
            if (input.nav == Direction::left && chip_ > 0)
            {
                --chip_;
                feedback.play(audio::Cue::focus);
            }
            else if (input.nav == Direction::right && chip_ < 3)
            {
                ++chip_;
                feedback.play(audio::Cue::focus);
            }
            else if (input.nav == Direction::up)
            {
                chip_focus_ = false;
                refresh_focus_ = true;
                feedback.play(audio::Cue::focus);
            }
            else if (input.nav == Direction::down || input.is_pressed(Action::back))
            {
                chip_focus_ = false;
                feedback.play(audio::Cue::back);
            }
            if (input.is_pressed(Action::confirm))
            {
                open_rail_preset(chip_, feedback);
                chip_focus_ = false;
            }
            return;
        }
        // The Options filter shortcut is handled up in update_pages(), before the
        // rail and quick-strip blocks, so it reaches the panel from any library
        // state. Only the search keyboard's own Options (完成) is handled below.
        if (search_edit_)
        {
            shelf_dirty_ = true; // live-filter the wall as the query is typed
            // Options is a one-press 完成: it closes the keyboard and keeps the
            // query, so the viewer never has to hunt the on-screen button.
            if (input.is_pressed(Action::menu))
            {
                search_edit_ = false;
                keyboard_.set_active(false);
                feedback.play(audio::Cue::select);
                return;
            }
            const ui::Event event = keyboard_.handle(input, feedback);
            if (event == ui::Event::activated || event == ui::Event::cancelled)
            {
                search_edit_ = false;
                keyboard_.set_active(false);
                feedback.play(event == ui::Event::activated ? audio::Cue::select
                                                            : audio::Cue::back);
            }
            return;
        }
        const int count = library_count();
        if (count == 0)
        {
            // A query with no hits leaves an empty wall, not a dead tab: Cross
            // opens the keyboard again so the viewer can correct it.
            if (rail_is_search(rail_) && input.is_pressed(Action::confirm))
            {
                begin_search(feedback);
            }
            return;
        }
        constexpr int kColumns = kGridColumns;
        if (input.nav == Direction::right && grid_ + 1 < count)
        {
            ++grid_;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::left && grid_ > 0)
        {
            --grid_;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::left)
        {
            rail_focus_ = true;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::down && grid_ + kColumns < count)
        {
            grid_ += kColumns;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::up && grid_ >= kColumns)
        {
            grid_ -= kColumns;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::up && refresh_available())
        {
            // On the first grid row there is nowhere further up in the wall; the
            // quick strip (筛选 / 最近添加 / 高分 / 未看) is the next stop, and Up
            // from that strip reaches the top-bar 刷新片源 button.
            chip_focus_ = true;
            chip_ = 0;
            feedback.play(audio::Cue::focus);
        }
        // The ring must never leave the two rows on screen; this is what makes
        // a library longer than one screen walkable.
        clamp_grid_scroll();

        if (input.is_pressed(Action::confirm))
        {
            if (open_detail(library_item(grid_)))
            {
                zone_ = Zone::detail;
                feedback.play(audio::Cue::select);
            }
        }
    }

    void update_detail(const InputFrame &input, app::Feedback &feedback)
    {
        // 播放 on a series' banner opens this page only to fetch the episode
        // list; the press already asked for playback, so start as soon as the
        // answer is here instead of making the viewer press 播放 again.
        if (hero_play_pending_)
        {
            if (!detail_episodes_.empty())
            {
                start_detail_playback(hero_resume_episode(), feedback);
                return;
            }
            if (!detail_error_.empty())
            {
                hero_play_pending_ = false;
                feedback.play(audio::Cue::error);
                return;
            }
            // A ready answer with no episodes cannot be played (the server
            // sent a collection without its children); stop waiting and show
            // the page rather than hanging on a request that already landed.
            const fnvideo::Service::Detail answer = fnvideo::Service::instance().detail();
            if (answer.ready && answer.item.guid == detail_guid_)
            {
                hero_play_pending_ = false;
                feedback.play(audio::Cue::error);
                return;
            }
        }

        // □ folds / unfolds the 剧情简介, the web page's 展开/收起. Collapsed it
        // shows kSynopsisCollapsedLines lines; there is only something to expand
        // when the text wraps past that, so an unfold on a short synopsis is a
        // no-op.
        if (input.is_pressed(Action::west))
        {
            if (detail_synopsis_open_)
            {
                detail_synopsis_open_ = false;
                feedback.play(audio::Cue::back);
                return;
            }
            const std::string &plot = detail_item().synopsis;
            const std::size_t plot_lines =
                plot.empty()
                    ? 0u
                    : glyphs_.regular.font->wrap(plot, 21.0f, kSynopsisCollapsedWidth).size();
            if (plot_lines > static_cast<std::size_t>(kSynopsisCollapsedLines))
            {
                detail_synopsis_open_ = true;
                feedback.play(audio::Cue::focus);
            }
            return;
        }

        const int seasons = season_focus_count();
        const int last = detail_last_focus();
        // 0 播放, 1 收藏, 2 音轨, 3 字幕, one chip per season, then that
        // season's episodes - the layout draw_detail paints under the same
        // indices (kDetailActions covers the first four).
        if (input.nav == Direction::right && detail_focus_ < last)
        {
            const int before = detail_focus_;
            ++detail_focus_;
            // A series page has no track pickers, so its two focus slots are
            // skipped: walking right past 收藏 lands straight on the season /
            // episode strip instead of an empty capsule.
            if (!detail_pickers_enabled() && (detail_focus_ == 2 || detail_focus_ == 3))
                detail_focus_ = kDetailActions;
            // The chip row and the episode row share one index line. Entering
            // the episodes commits the season the user walked onto, otherwise
            // the strip snaps back to the previously chosen season.
            if (detail_focus_ == episode_base() && season_focus_count() > 0)
            {
                season_sel_ = std::clamp(before - kDetailActions, 0, season_count() - 1);
            }
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::left && detail_focus_ == episode_base() &&
                 season_focus_count() > 0)
        {
            // Left from episode one returns to the chip for the season being
            // shown, not to the last season in the strip.
            detail_focus_ = kDetailActions + std::clamp(current_season(), 0, season_count() - 1);
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::left && detail_focus_ > 0)
        {
            --detail_focus_;
            // Mirror of the right-nav skip: leaving the strip leftward steps
            // back over the hidden pickers and stops on 收藏.
            if (!detail_pickers_enabled() && (detail_focus_ == 2 || detail_focus_ == 3))
                detail_focus_ = 1;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::down && detail_focus_ < kDetailActions &&
                 !detail_episodes_.empty())
        {
            detail_focus_ = kDetailActions;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::down && detail_focus_ >= kDetailActions &&
                 detail_focus_ < episode_base())
        {
            // A season chip leads into the episodes it just selected.
            season_sel_ = std::clamp(detail_focus_ - kDetailActions, 0, season_count() - 1);
            detail_focus_ = episode_base();
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::up && detail_focus_ >= episode_base() && seasons > 0)
        {
            const int shown = std::clamp(current_season(), 0, season_count() - 1);
            detail_focus_ = kDetailActions + shown;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::up && detail_focus_ >= kDetailActions)
        {
            detail_focus_ = 0;
            feedback.play(audio::Cue::focus);
        }

        if (input.is_pressed(Action::confirm))
        {
            if (detail_focus_ == 0)
            {
                // A collection's own guid is not a playable file. Pressing
                // 播放 before its episode list lands used to send that guid to
                // stream_url and fail (with no visible error); the helper
                // refuses instead of starting a collection.
                start_detail_playback(detail_episodes_.empty() ? -1 : episode_, feedback);
            }
            else if (detail_focus_ == 1)
            {
                // The star belongs to this row, not to the page. It used to be
                // one bool shared by every film, so marking one marked them all.
                if (detail_ >= 0 && detail_ < static_cast<int>(items_.size()))
                {
                    Item &shown = items_[static_cast<std::size_t>(detail_)];
                    const int next = shown.is_favorite == 0 ? 1 : 0;
                    shown.is_favorite = next;
                    // Persist through the service so a restart keeps 最近收藏.
                    // The in-memory favorite_overrides_ used to be the only
                    // store and evaporated on exit.
                    if (!shown.guid.empty())
                    {
                        fnvideo::Service::instance().set_favorite(shown.guid, next != 0);
                        favorite_overrides_[shown.guid] = next != 0;
                    }
                }
                feedback.play(audio::Cue::toggle);
            }
            else if (detail_focus_ == 2)
            {
                open_detail_track_menu(1, feedback);
            }
            else if (detail_focus_ == 3)
            {
                open_detail_track_menu(2, feedback);
            }
            else if (detail_focus_ < episode_base())
            {
                // Choosing a season takes the focus into its episodes; the
                // strip has been showing them since the chip was reached.
                season_sel_ = std::clamp(detail_focus_ - kDetailActions, 0, season_count() - 1);
                detail_focus_ = episode_base();
                feedback.play(audio::Cue::focus);
            }
            else
            {
                start_detail_playback(focused_episode(), feedback);
            }
        }
    }

    // The player's toolbar: one chip per thing that can be changed without
    // leaving the picture. A series adds the two episode steps, so every
    // other chip sits two places later there than on a film.
    bool player_series() const
    {
        return !detail_episodes_.empty();
    }
    void show_player_osd()
    {
        player_osd_ = true;
        player_idle_ = 0.0f;
    }
    int player_chip_count() const
    {
        return player_series() ? 9 : 7;
    }
    std::string player_audio_label() const
    {
        const auto tracks = fnvideo::Service::instance().audio_tracks();
        if (tracks.empty())
        {
            return "音轨";
        }
        const int current = fnvideo::Service::instance().audio_track();
        int index = 0;
        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            if (tracks[i].ordinal == current)
            {
                index = static_cast<int>(i);
                break;
            }
        }
        const fnvideo::AudioTrack &track = tracks[static_cast<std::size_t>(index)];
        return "音轨 " + detail_stream_name(track.language, track.title, index);
    }
    std::string player_subtitle_label() const
    {
        const auto tracks = fnvideo::Service::instance().subtitle_tracks();
        if (tracks.empty())
        {
            return "字幕";
        }
        const int current = fnvideo::Service::instance().subtitle_track();
        if (current < 0)
        {
            return "字幕 关";
        }
        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            const fnvideo::SubtitleTrack &track = tracks[i];
            if (track.ordinal == current)
            {
                return "字幕 " + subtitle_display_name(track.language, track.title, !track.text,
                                                       track.external, static_cast<int>(i));
            }
        }
        return "字幕 开";
    }
    std::string player_chip_label(int chip) const
    {
        const bool series = player_series();
        const int pause_chip = series ? 1 : 0;
        const int edge_chip = series ? 3 : 1;
        const int decoder_chip = series ? 4 : 2;
        const int audio_chip = series ? 5 : 3;
        const int subtitle_chip = series ? 6 : 4;
        const int volume_chip = series ? 7 : 5;
        const int brightness_chip = series ? 8 : 6;
        if (series && chip == 0)
            return "上一集";
        if (chip == pause_chip)
            return paused_ ? "播放" : "暂停";
        if (series && chip == 2)
            return "下一集";
        if (chip == edge_chip)
            return std::string("分辨率 ") + play_edge_label();
        if (chip == decoder_chip)
            return std::string("解码 ") + play_decoder_label();
        if (chip == audio_chip)
            return player_audio_label();
        if (chip == subtitle_chip)
            return player_subtitle_label();
        if (chip == volume_chip)
        {
            return "音量 " +
                   std::to_string(
                       static_cast<int>(fnvideo::Service::instance().volume() * 100.0f + 0.5f)) +
                   "%";
        }
        if (chip == brightness_chip)
        {
            return "亮度 " +
                   std::to_string(static_cast<int>(
                       fnvideo::Service::instance().brightness() * 100.0f + 0.5f)) +
                   "%";
        }
        return std::string();
    }

    void open_player_menu(PlayerMenu menu)
    {
        player_menu_ = menu;
        player_menu_sel_ = 0;
        auto &service = fnvideo::Service::instance();
        if (menu == PlayerMenu::audio)
        {
            const auto tracks = service.audio_tracks();
            const int current = service.audio_track();
            for (std::size_t i = 0; i < tracks.size(); ++i)
            {
                if (tracks[i].ordinal == current)
                {
                    player_menu_sel_ = static_cast<int>(i);
                    break;
                }
            }
        }
        else if (menu == PlayerMenu::subtitle)
        {
            const auto tracks = service.subtitle_tracks();
            const int current = service.subtitle_track();
            if (current >= 0)
            {
                for (std::size_t i = 0; i < tracks.size(); ++i)
                {
                    if (tracks[i].ordinal == current)
                    {
                        player_menu_sel_ = static_cast<int>(i) + 1;
                        break;
                    }
                }
            }
        }
    }

    // Cross on a focused chip. The pause and episode chips act at once;
    // resolution is read on the next picture, so it acts at once too. The
    // decoder is chosen when a film opens, so switching it mid-film reopens
    // the same film where the sound is - otherwise the chip would look broken
    // until the next episode.
    void activate_player_chip(int chip, const fnvideo::Service::Playback &play,
                              app::Feedback &feedback)
    {
        auto &service = fnvideo::Service::instance();
        const bool series = player_series();
        const int pause_chip = series ? 1 : 0;
        const int edge_chip = series ? 3 : 1;
        const int decoder_chip = series ? 4 : 2;
        const int audio_chip = series ? 5 : 3;
        const int subtitle_chip = series ? 6 : 4;
        const int volume_chip = series ? 7 : 5;
        const int brightness_chip = series ? 8 : 6;
        if (series && chip == 0)
        {
            step_episode(-1, feedback);
            return;
        }
        if (series && chip == 2)
        {
            step_episode(1, feedback);
            return;
        }
        if (chip == pause_chip)
        {
            service.toggle_pause();
            paused_ = !paused_;
            show_player_osd();
            feedback.play(audio::Cue::select);
            return;
        }
        if (chip == edge_chip)
        {
            cycle_play_edge(1, feedback);
            return;
        }
        if (chip == decoder_chip)
        {
            const int before = play_decoder_value();
            cycle_play_decoder(1, feedback);
            if (play_decoder_value() != before && (play.playing || play.paused))
            {
                service.reopen_at(play.position);
            }
            return;
        }
        if (chip == audio_chip)
        {
            // The track list can still be empty for a moment after the file
            // opens. Showing the pull-down with an explanation beats a button
            // that appears to do nothing.
            open_player_menu(PlayerMenu::audio);
            feedback.play(audio::Cue::select);
            return;
        }
        if (chip == subtitle_chip)
        {
            open_player_menu(PlayerMenu::subtitle);
            feedback.play(audio::Cue::select);
            return;
        }
        if (chip == volume_chip)
        {
            open_player_menu(PlayerMenu::volume);
            feedback.play(audio::Cue::select);
            return;
        }
        if (chip == brightness_chip)
        {
            open_player_menu(PlayerMenu::brightness);
            feedback.play(audio::Cue::select);
            return;
        }
        feedback.play(audio::Cue::toggle);
    }

    void update_player_menu(const InputFrame &input, app::Feedback &feedback)
    {
        auto &service = fnvideo::Service::instance();
        // The subtitle panels own the pad while they are open: both are
        // reached from rows of the subtitle menu and back out to that menu,
        // so the film's track list is one press away after a download or a
        // mark. One handler, because the online search and the NAS browse
        // differ only in what a confirmed row does.
        if (player_menu_ == PlayerMenu::subtitle_search ||
            player_menu_ == PlayerMenu::subtitle_browse)
        {
            const bool browse = player_menu_ == PlayerMenu::subtitle_browse;
            const fnvideo::Service::SubtitlePanel panel = service.subtitle_panel();
            if (input.is_pressed(Action::back))
            {
                service.close_subtitle_panel();
                player_menu_ = PlayerMenu::subtitle;
                player_menu_sel_ = 0;
                feedback.play(audio::Cue::back);
                return;
            }
            const int hits = static_cast<int>(panel.hits.size());
            if (!panel.busy && hits > 0)
            {
                if (input.nav == Direction::up || input.nav == Direction::down)
                {
                    const int delta = input.nav == Direction::down ? 1 : -1;
                    player_menu_sel_ = std::clamp(player_menu_sel_ + delta, 0, hits - 1);
                    feedback.play(audio::Cue::focus);
                    return;
                }
                if (input.is_pressed(Action::confirm))
                {
                    const std::size_t i = static_cast<std::size_t>(player_menu_sel_);
                    if (i < panel.hits.size())
                    {
                        const fnvideo::SubtitleHit &hit = panel.hits[i];
                        // Online hits download into the folder (a NAS write);
                        // a NAS folder row either navigates the tree (a
                        // directory) or registers the subtitle (a file).
                        if (browse && hit.is_dir)
                        {
                            if (hit.filename == "..")
                            {
                                service.subtitle_browse_up_async();
                            }
                            else
                            {
                                std::string child = panel.folder;
                                if (!child.empty() && child != "/")
                                    child += "/";
                                child += hit.filename;
                                service.subtitle_browse_open_async(child);
                            }
                            player_menu_sel_ = 0;
                            feedback.play(audio::Cue::focus);
                        }
                        else if (browse)
                        {
                            service.subtitle_mark_async(hit.filename);
                            feedback.play(audio::Cue::select);
                        }
                        else
                        {
                            service.subtitle_download_async(hit.trim_id);
                            feedback.play(audio::Cue::select);
                        }
                    }
                }
            }
            return; // busy, or nothing found yet: the pad stays put
        }
        if (input.is_pressed(Action::back))
        {
            player_menu_ = PlayerMenu::none;
            feedback.play(audio::Cue::back);
            return;
        }
        if (player_menu_ == PlayerMenu::volume || player_menu_ == PlayerMenu::brightness)
        {
            const bool up = input.nav == Direction::right;
            const bool down = input.nav == Direction::left;
            if (up || down)
            {
                const float step = up ? 0.05f : -0.05f;
                if (player_menu_ == PlayerMenu::volume)
                {
                    service.set_volume(service.volume() + step);
                }
                else
                {
                    service.set_brightness(service.brightness() + step);
                }
                feedback.play(audio::Cue::slider);
                return;
            }
            if (input.is_pressed(Action::confirm))
            {
                player_menu_ = PlayerMenu::none;
                feedback.play(audio::Cue::select);
            }
            return;
        }

        const bool subtitle = player_menu_ == PlayerMenu::subtitle;
        // One row for 关闭, one per track, then the two actions: the online
        // subtitle search and the NAS-folder browse (the web's 导入本地字幕,
        // minus an upload - the files are already on the shelf).
        const int count = subtitle ? static_cast<int>(service.subtitle_tracks().size()) + 3
                                   : static_cast<int>(service.audio_tracks().size());
        if (count <= 0)
        {
            // Nothing to choose. Keep the explanation on screen until the
            // viewer dismisses it instead of flashing shut on the first frame.
            if (input.is_pressed(Action::confirm))
            {
                player_menu_ = PlayerMenu::none;
                feedback.play(audio::Cue::toggle);
            }
            return;
        }
        // The worker can replace the track list while the menu is open (a
        // reopen after an earlier switch). Keep the row inside the list that
        // is about to be indexed.
        player_menu_sel_ = std::clamp(player_menu_sel_, 0, count - 1);
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int delta = input.nav == Direction::down ? 1 : -1;
            player_menu_sel_ = std::clamp(player_menu_sel_ + delta, 0, count - 1);
            feedback.play(audio::Cue::focus);
            return;
        }
        if (!input.is_pressed(Action::confirm))
        {
            return;
        }
        if (subtitle)
        {
            const auto tracks = service.subtitle_tracks();
            const int search_row = static_cast<int>(tracks.size()) + 1;
            const int browse_row = static_cast<int>(tracks.size()) + 2;
            if (player_menu_sel_ == search_row)
            {
                // Open the search panel and fire the query at once, mirroring
                // the web page where choosing 字幕配置 immediately lists hits.
                service.open_subtitle_panel();
                service.subtitle_search_async("chi");
                player_menu_ = PlayerMenu::subtitle_search;
                player_menu_sel_ = 0;
                feedback.play(audio::Cue::select);
                return;
            }
            if (player_menu_sel_ == browse_row)
            {
                // Same shape: open the panel and list the folder at once.
                service.open_subtitle_panel();
                service.subtitle_browse_async();
                player_menu_ = PlayerMenu::subtitle_browse;
                player_menu_sel_ = 0;
                feedback.play(audio::Cue::select);
                return;
            }
            if (player_menu_sel_ == 0)
            {
                service.select_subtitle_track(-1);
            }
            else
            {
                const std::size_t index = static_cast<std::size_t>(player_menu_sel_ - 1);
                if (index < tracks.size())
                {
                    service.select_subtitle_track(tracks[index].ordinal);
                }
            }
        }
        else
        {
            const auto tracks = service.audio_tracks();
            const std::size_t index = static_cast<std::size_t>(player_menu_sel_);
            if (index < tracks.size())
            {
                // Confirming the track that is already playing must not make
                // a paused film look as if it resumed.
                if (tracks[index].ordinal != service.audio_track())
                {
                    // The service remembers a paused state across the reopen;
                    // clearing this here made the film start playing again.
                    service.select_audio_track(tracks[index].ordinal);
                }
            }
        }
        player_menu_ = PlayerMenu::none;
        feedback.play(audio::Cue::select);
    }

    void update_player(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        auto &service = fnvideo::Service::instance();
        const fnvideo::Service::Playback play = service.playback();
        // Live when the NAS answered; the sample clock below only drives the
        // pages the PC preview and the tour draw, which have no stream. A
        // failed open is live too: it must show its error, not a fake clock.
        // The preview sample has no server by design, so its open failure is
        // not a failure the page should report.
        const bool live =
            play.playing || play.paused || play.opening || (play.failed && !preview_sample());

        // The stick readout counts down every frame; an adjustment this frame
        // resets it (see the analog block below), so it only fades once the
        // thumb has rested.
        if (stick_hud_timer_ > 0.0f)
        {
            stick_hud_timer_ = std::max(0.0f, stick_hud_timer_ - dt);
        }

        // B2 diagnosis: a stream wedged in the opening state (部分视频卡在正在
        // 打开). The worker holds opening while it resolves the play ticket and
        // probes the container. This only names the guid and how long we have
        // waited, so the next step targets the fetch rather than guessing.
        if (play.opening)
        {
            open_wait_ += dt;
            const int bucket = static_cast<int>(open_wait_ / 5.0f);
            if (bucket != open_log_bucket_)
            {
                open_log_bucket_ = bucket;
                if (bucket > 0)
                    sys::log("[HUI] still opening guid=%s wait=%.0fs", playable_guid().c_str(),
                             open_wait_);
            }
        }
        else
        {
            open_wait_ = 0.0f;
            open_log_bucket_ = 0;
        }

        if (play.ended)
        {
            // The file played out: back to the page it was started from, which
            // is what every streaming app does at the end of a film.
            service.stop_playback();
            // Service::stop_playback() already marks the guid finished on this
            // console, which hides it from 继续观看 and flips the detail page's
            // button back to 播放 (start from the top). No need to duplicate
            // the insert here.
            player_bar_ = false;
            player_chip_ = 0;
            player_menu_ = PlayerMenu::none;
            player_osd_ = true;
            player_idle_ = 0.0f;
            zone_ = Zone::detail;
            feedback.play(audio::Cue::back);
            return;
        }

        // Any press brings the chrome back, including the one that also acts on
        // it. The idle countdown only runs while the picture is actually
        // playing and nothing is open.
        if (input.nav != Direction::none || input.pressed != 0)
        {
            show_player_osd();
        }

        if (player_menu_ != PlayerMenu::none)
        {
            update_player_menu(input, feedback);
            return;
        }

        if (live)
        {
            // Opening is not paused. Treating it that way drew the pause icon
            // over a film that had not started yet and made the toolbar claim
            // the wrong action.
            paused_ = play.paused;
            position_ = play.duration > 0 ? std::clamp(static_cast<float>(play.position) /
                                                           static_cast<float>(play.duration),
                                                       0.0f, 1.0f)
                                          : 0.0f;
        }
        else if (!paused_ && (!play.failed || preview_sample()))
        {
            // A sample clock: the frame is not decoded yet, so the bar moves
            // with wall time and the OSD reads like a real playback.
            position_ = std::min(1.0f, position_ + dt / 420.0f);
            if (position_ >= 1.0f)
                position_ = 0.0f;
        }

        // ---- analog stick sliders (watching state only) ----
        // Left stick up/down rides the volume, right stick up/down the picture
        // brightness. These read the raw analog axis. A horizontal left-stick
        // push is NOT handled here: it feeds the normal fast-forward path below
        // (the tracker turns it into nav left/right), just with a bigger step.
        // The toolbar and the pull-down menus own the pad entirely, so this only
        // runs while watching.
        if (!player_bar_ && player_menu_ == PlayerMenu::none && live)
        {
            constexpr float kFine = 0.12f;       // dead zone before a slider moves
            constexpr float kVolumeSpeed = 0.7f; // full tilt crosses the range in ~1.4 s
            constexpr float kBrightnessSpeed = 0.9f;
            const float sy = std::fabs(input.stick_y) > kFine ? input.stick_y : 0.0f;
            const float s2y = std::fabs(input.stick2_y) > kFine ? input.stick2_y : 0.0f;
            if (sy != 0.0f)
            {
                // Squared, sign kept: a slight tilt creeps, a full one runs. Up
                // (negative y) raises the volume.
                const float shaped = sy * std::fabs(sy);
                const float v =
                    std::clamp(service.volume() - shaped * kVolumeSpeed * dt, 0.0f, 1.0f);
                service.set_volume(v);
                stick_hud_text_ =
                    "音量 " + std::to_string(static_cast<int>(v * 100.0f + 0.5f)) + "%";
                stick_hud_fraction_ = v;
                stick_hud_timer_ = 1.3f;
                show_player_osd();
            }
            if (s2y != 0.0f)
            {
                const float shaped = s2y * std::fabs(s2y);
                const float b =
                    std::clamp(service.brightness() - shaped * kBrightnessSpeed * dt, 0.5f, 1.5f);
                service.set_brightness(b);
                stick_hud_text_ =
                    "亮度 " + std::to_string(static_cast<int>(b * 100.0f + 0.5f)) + "%";
                stick_hud_fraction_ = (b - 0.5f) / 1.0f;
                stick_hud_timer_ = 1.3f;
                show_player_osd();
            }
        }

        // L1/R1 快进快退的长按连发节奏：与导航（左摇杆 / 方向键）同出一辙
        // ——按下当帧先跳一次，之后按住越过 0.35s 起始延迟，每 0.11s 补跳
        // 一次；松手或换向则取消长按。算出本帧该不该跳，交给下面的分支消费。
        bool shoulder_prev_step = false;
        bool shoulder_next_step = false;
        {
            constexpr float kShoulderRepeatDelay = 0.35f;
            constexpr float kShoulderRepeatInterval = 0.11f;
            const bool prev_edge = input.is_pressed(Action::page_prev);
            const bool next_edge = input.is_pressed(Action::page_next);
            const bool prev_hold = input.is_held(Action::page_prev);
            const bool next_hold = input.is_held(Action::page_next);
            if (shoulder_key_ != Action::count)
                shoulder_hold_ = std::max(0.0f, shoulder_hold_ - dt);
            if (prev_edge)
            {
                shoulder_prev_step = true;
                shoulder_key_ = Action::page_prev;
                shoulder_hold_ = kShoulderRepeatDelay;
            }
            else if (next_edge)
            {
                shoulder_next_step = true;
                shoulder_key_ = Action::page_next;
                shoulder_hold_ = kShoulderRepeatDelay;
            }
            else if (shoulder_key_ == Action::page_prev && prev_hold && shoulder_hold_ <= 0.0f)
            {
                shoulder_prev_step = true;
                shoulder_hold_ = kShoulderRepeatInterval;
            }
            else if (shoulder_key_ == Action::page_next && next_hold && shoulder_hold_ <= 0.0f)
            {
                shoulder_next_step = true;
                shoulder_hold_ = kShoulderRepeatInterval;
            }
            else if (!prev_hold && !next_hold)
            {
                shoulder_key_ = Action::count;
                shoulder_hold_ = 0.0f;
            }
        }

        const int chip_count = player_chip_count();
        if (player_chip_ >= chip_count)
        {
            player_chip_ = 0;
        }

        if (player_bar_)
        {
            // The toolbar owns left and right while it is open. Up and down
            // close it, so the pad returns to seeking without a mode to
            // remember.
            if (input.nav == Direction::left)
            {
                player_chip_ = (player_chip_ + chip_count - 1) % chip_count;
                feedback.play(audio::Cue::focus);
                return;
            }
            if (input.nav == Direction::right)
            {
                player_chip_ = (player_chip_ + 1) % chip_count;
                feedback.play(audio::Cue::focus);
                return;
            }
            if (input.nav == Direction::up || input.nav == Direction::down)
            {
                player_bar_ = false;
                feedback.play(audio::Cue::back);
                return;
            }
            if (input.is_pressed(Action::back))
            {
                // The toolbar is a layer of its own: the first Circle puts it
                // away, and only a second one leaves the film. Leaving on the
                // first press threw away the viewer's place by accident.
                player_bar_ = false;
                player_chip_ = 0;
                feedback.play(audio::Cue::back);
                return;
            }
            if (input.is_pressed(Action::confirm))
            {
                activate_player_chip(player_chip_, play, feedback);
                return;
            }
        }
        else if (input.nav == Direction::up && !input.nav_from_stick)
        {
            // Up opens the toolbar on its usual chip: pause on a film, pause
            // (not 上一集) on a series. Down still has no meaning here, so
            // nothing is lost by leaving it. Gated to the D-pad: the left stick
            // rides the volume instead (see the analog block above).
            player_bar_ = true;
            player_chip_ = player_series() ? 1 : 0;
            feedback.play(audio::Cue::focus);
            return;
        }
        else if (input.is_pressed(Action::confirm))
        {
            // While the ticket is opening there is nothing to pause yet; a
            // flip here used to leave the OSD showing pause for a stream that
            // was still several seconds from its first frame.
            if (play.playing || play.paused)
            {
                service.toggle_pause();
                paused_ = !play.paused;
                show_player_osd();
                feedback.play(audio::Cue::select);
            }
        }
        else if (input.nav == Direction::left)
        {
            if (live)
            {
                // Normal fast-forward, only the step is bigger: a held
                // left-stick push (the tracker reports it as nav with
                // nav_from_stick) steps a full minute, the D-pad keeps its
                // 10-second nudge. Same Player::seek settle path either way.
                service.seek_playback(play.position - (input.nav_from_stick ? 60 : 10));
            }
            position_ = std::max(0.0f, position_ - 0.02f);
            feedback.play(audio::Cue::slider);
        }
        else if (input.nav == Direction::right)
        {
            if (live)
            {
                service.seek_playback(play.position + (input.nav_from_stick ? 60 : 10));
            }
            position_ = std::min(1.0f, position_ + 0.02f);
            feedback.play(audio::Cue::slider);
        }
        else if (shoulder_prev_step)
        {
            // L1 快退：步长 ±30s（比左摇杆的 ±60s 温和一档），并支持上面的
            // 长按连发。走同一 Player::seek 沉降路径，快退时 play.position
            // 尚未提交，重复下发同一绝对目标不会跑飞。
            if (live)
                service.seek_playback(play.position - 30);
            position_ = std::max(0.0f, position_ - 0.02f);
            feedback.play(audio::Cue::slider);
        }
        else if (shoulder_next_step)
        {
            // R1 快进：同 L1，步长 ±30s 并支持长按连发。
            if (live)
                service.seek_playback(play.position + 30);
            position_ = std::min(1.0f, position_ + 0.02f);
            feedback.play(audio::Cue::slider);
        }
        else if (input.is_pressed(Action::jump_next))
        {
            step_episode(1, feedback);
        }
        else if (input.is_pressed(Action::jump_prev))
        {
            step_episode(-1, feedback);
        }
        if (input.is_pressed(Action::back))
        {
            // Leaving the page stops the decoder: there is nowhere else for
            // the sound to go.
            service.stop_playback();
            player_bar_ = false;
            player_chip_ = 0;
            player_menu_ = PlayerMenu::none;
            player_osd_ = true;
            player_idle_ = 0.0f;
            zone_ = Zone::detail;
            feedback.play(audio::Cue::back);
            return;
        }

        // A paused or opening player keeps its controls: there is nothing to
        // watch behind them yet, and the pause mark is the only feedback that
        // the press worked. A running picture gets out of the way.
        if (player_osd_ && !player_bar_ && !paused_ && !play.opening && !play.failed &&
            player_menu_ == PlayerMenu::none && play.playing)
        {
            player_idle_ += dt;
            if (player_idle_ >= kPlayerOsdTimeout)
            {
                player_osd_ = false;
            }
        }
    }

    void update_settings(const InputFrame &input, app::Feedback &feedback)
    {
        if (settings_edit_ >= 0)
        {
            // The kit keyboard owns the pad while a field is open: Cross types
            // or confirms (Done), Circle cancels. The callbacks feed the draft.
            const ui::Event event = keyboard_.handle(input, feedback);
            if (event == ui::Event::activated)
                commit_edit(feedback);
            else if (event == ui::Event::cancelled)
                cancel_edit(feedback);
            return;
        }

        if (input.nav == Direction::down && settings_row_ + 1 < kSettingsRows)
        {
            ++settings_row_;
            feedback.play(audio::Cue::focus);
            return;
        }
        if (input.nav == Direction::up && settings_row_ > 0)
        {
            --settings_row_;
            feedback.play(audio::Cue::focus);
            return;
        }
        // 播放分辨率 and 解码方式 live on the player's toolbar now, next to
        // the picture they change, so this page is only the connection and
        // the local information.
        if (input.is_pressed(Action::confirm))
        {
            // The three text rows open the on-screen keyboard; the rest act.
            if (settings_row_ == 0 || settings_row_ == 1 || settings_row_ == 2)
            {
                begin_edit(settings_row_, feedback);
            }
            else if (settings_row_ == 3)
            {
                do_login(feedback);
            }
            else if (settings_row_ == 4)
            {
                do_logout(feedback);
            }
            // 缓存 and 关于 are informational rows. They used to play a toggle
            // cue on Cross, which promised an action that never happened.
        }
    }

    // The resolution cap the player decodes and uploads at: 0 = automatic
    // (the source's real resolution, up to 4K), or a manual tier. A manual
    // tier is a real saving now - the picture is scaled before upload - so on
    // a 4K film it is the difference between 18 MB and 2 MB per frame.
    int play_edge_value() const
    {
        return fnvideo::Service::instance().play_edge();
    }

    // What the chip reads: 自动 (play at the real resolution) or the manual
    // tier's name. Read live so the chip tracks a value changed elsewhere.
    const char *play_edge_label() const
    {
        const int edge = fnvideo::Service::instance().play_edge();
        if (edge <= 0)
            return "自动";
        if (edge >= 3600)
            return "4K";
        if (edge >= 2400)
            return "2K";
        if (edge >= 1700)
            return "1080p";
        if (edge >= 1100)
            return "720p";
        return "480p";
    }

    void cycle_play_edge(int step, app::Feedback &feedback)
    {
        // 0 = automatic (source resolution), then the same tiers a TV's
        // resolution menu offers. 854 rather than 960 is 480p's 16:9 width.
        constexpr std::array<int, 6> kTiers{0, 3840, 2560, 1920, 1280, 854};
        const int cur = fnvideo::Service::instance().play_edge();
        int slot = 0;
        for (int i = 0; i < static_cast<int>(kTiers.size()); ++i)
        {
            if (kTiers[static_cast<std::size_t>(i)] == cur)
            {
                slot = i;
                break;
            }
        }
        slot = (slot + step + static_cast<int>(kTiers.size())) % static_cast<int>(kTiers.size());
        // The service stores it, writes it to the conf file and hands it to
        // the player, which reads it on the next decoded picture.
        fnvideo::Service::instance().set_play_edge(kTiers[static_cast<std::size_t>(slot)]);
        feedback.play(audio::Cue::slider);
    }

    // The decoder the player uses: 0 = software (FFmpeg), 1 = hardware (PS5
    // Videodec2). The hardware path decodes 4K HEVC on the console's video
    // block; software is the fallback and the comparison. Read when a film
    // opens, so a change from the toolbar restarts the film where it is.
    int play_decoder_value() const
    {
        return fnvideo::Service::instance().decoder();
    }

    // What the chip reads: 软解 (software) or 硬解 (hardware). Read live.
    const char *play_decoder_label() const
    {
        return fnvideo::Service::instance().decoder() == 0 ? "软解" : "硬解";
    }

    void cycle_play_decoder(int step, app::Feedback &feedback)
    {
        // 1 = hardware first (the default), then 0 = software.
        constexpr std::array<int, 2> kModes{1, 0};
        const int cur = fnvideo::Service::instance().decoder();
        int slot = 0;
        for (int i = 0; i < static_cast<int>(kModes.size()); ++i)
        {
            if (kModes[static_cast<std::size_t>(i)] == cur)
            {
                slot = i;
                break;
            }
        }
        slot = (slot + step + static_cast<int>(kModes.size())) % static_cast<int>(kModes.size());
        const int mode = kModes[static_cast<std::size_t>(slot)];
        // The service stores it, writes it to the conf file and hands it to
        // the player. The player reads the decoder when a film opens, so the
        // caller reopens the current one (see the toolbar) when it is live.
        if (mode != cur)
        {
            fnvideo::Service::instance().set_decoder(mode);
        }
        feedback.play(audio::Cue::slider);
    }

    // ---- settings: keyboard input ----

    void begin_edit(int field, app::Feedback &feedback)
    {
        settings_edit_ = field;
        edit_buffer_ = field == 0 ? server_text_ : field == 1 ? account_text_ : password_text_;
        // The kit keyboard: letters + symbols, with shift (L2), layout (R2),
        // space (Triangle) and backspace (Square) bound, so it feels like the
        // console's own text entry rather than a bespoke grid.
        keyboard_.set_layouts({ui::KeyboardLayout::letters(), ui::KeyboardLayout::symbols()});
        keyboard_.style.bindings = ui::KeyboardBindings::standard();
        keyboard_.style.key_width = 0.0f; // keys share the bounds' width
        keyboard_.style.surfaces = true;
        keyboard_.style.wide_labels = true;
        keyboard_.style.binding_glyphs = true;
        keyboard_.set_bounds({kContentX + 24.0f, 424.0f, kContentW - 48.0f, 396.0f});
        keyboard_.style.max_length = field == 2 ? 64 : 256;
        keyboard_.set_length(static_cast<int>(edit_buffer_.size()));
        keyboard_.set_active(true);
        keyboard_.enter();
        keyboard_.on_text = [this](std::string_view utf8) { edit_buffer_.append(utf8); };
        keyboard_.on_backspace = [this]()
        {
            if (!edit_buffer_.empty())
                edit_buffer_.pop_back();
        };
        feedback.play(audio::Cue::select);
    }

    // The 搜索 shelf's keyboard. It reuses the kit control and the same
    // bindings as the settings fields; every character filters the wall
    // immediately, so a query never needs a separate "apply" press.
    void begin_search(app::Feedback &feedback)
    {
        search_edit_ = true;
        keyboard_.set_layouts({ui::KeyboardLayout::letters(), ui::KeyboardLayout::symbols()});
        keyboard_.style.bindings = ui::KeyboardBindings::standard();
        keyboard_.style.key_width = 0.0f;
        keyboard_.style.surfaces = true;
        keyboard_.style.wide_labels = true;
        keyboard_.style.binding_glyphs = true;
        keyboard_.set_bounds({kContentX + 40.0f, 400.0f, kContentW - 80.0f, 420.0f});
        keyboard_.style.max_length = 64;
        keyboard_.set_length(static_cast<int>(search_input_.size()));
        keyboard_.set_active(true);
        keyboard_.enter();
        keyboard_.on_text = [this](std::string_view utf8)
        {
            search_input_.append(utf8);
            grid_ = 0;
            grid_scroll_ = 0;
        };
        keyboard_.on_backspace = [this]()
        {
            if (!search_input_.empty())
            {
                search_input_.pop_back();
            }
            grid_ = 0;
            grid_scroll_ = 0;
        };
        feedback.play(audio::Cue::select);
    }

    void commit_edit(app::Feedback &feedback)
    {
        if (settings_edit_ == 0)
        {
            server_text_ = edit_buffer_;
        }
        else if (settings_edit_ == 1)
        {
            account_text_ = edit_buffer_;
        }
        else
        {
            password_text_ = edit_buffer_;
        }
        settings_edit_ = -1;
        keyboard_.set_active(false);
        feedback.play(audio::Cue::select);
    }

    void cancel_edit(app::Feedback &feedback)
    {
        settings_edit_ = -1;
        keyboard_.set_active(false);
        feedback.play(audio::Cue::back);
    }

    void do_login(app::Feedback &feedback)
    {
        auto &service = fnvideo::Service::instance();
        const std::string current = service.server();
        const std::string current_user = service.account();
        // The password field starts empty because the settings page never
        // reads the stored one back. A changed address/account therefore must
        // reuse it: calling configure(..., "") here erased the password and
        // made the next sign-in fail. A freshly typed password still wins.
        const bool address_changed = server_text_ != current || account_text_ != current_user;
        if (!password_text_.empty())
        {
            service.configure(server_text_, account_text_, password_text_);
        }
        else if (address_changed)
        {
            service.reconfigure(server_text_, account_text_);
        }
        else
        {
            service.relogin();
        }
        feedback.play(audio::Cue::select);
    }

    void do_logout(app::Feedback &feedback)
    {
        fnvideo::Service::instance().logout();
        server_text_ = fnvideo::Service::instance().server();
        account_text_ = fnvideo::Service::instance().account();
        password_text_.clear();
        settings_edit_ = -1;
        feedback.play(audio::Cue::back);
    }

    // ---- focus ----

    int row_count(int row) const
    {
        if (row == 0)
            return items_.empty() ? 0 : 2; // 播放 / 详情
        if (row == 1)
            return static_cast<int>(home_continue_.size());
        return static_cast<int>(home_recent_.size());
    }

    // The worker can shrink a home row between two frames. Keep the ring on a
    // card that is still drawn; without this the cached row/column could point
    // past the end of a refilled 继续观看 row.
    void clamp_home_focus()
    {
        if (items_.empty())
        {
            row_ = 0;
            col_ = 0;
            return;
        }
        row_ = std::clamp(row_, 0, 2);
        if (row_count(row_) == 0)
        {
            row_ = 0;
        }
        col_ = std::clamp(col_, 0, std::max(0, row_count(row_) - 1));
    }

    int home_item(int row, int col) const
    {
        if (row == 0)
            return hero_index_;
        const std::vector<int> &vec = (row == 1) ? home_continue_ : home_recent_;
        // The home rows can be empty when the NAS has not answered yet; rather
        // than index off the end, fall back to the hero so a confirm never
        // reads from a null vector.
        if (vec.empty())
            return hero_index_;
        col = std::clamp(col, 0, static_cast<int>(vec.size()) - 1);
        return vec[static_cast<std::size_t>(col)];
    }

    // The wall only shows collections: the card a filter counts is the card
    // the grid draws, so the focus ring and the remote land on the same thing.
    // What the rail tab filters by is its whole job: 首页/设置 have no grid, a
    // library tab keeps the items the server filed under that library's
    // category, 收藏 keeps what the player hearted, and 搜索 keeps the titles
    // that contain the query (everything while it is empty).
    bool shelf_match(const Item &item) const
    {
        if (rail_is_home(rail_) || rail_is_settings(rail_))
            return false;
        if (rail_is_favorites(rail_))
            return item.is_favorite != 0;
        if (rail_is_search(rail_))
            return search_matches(item, search_input_);
        if (rail_is_library(rail_))
        {
            const fnvideo::Library &library = rail_library(rail_);
            // The row's own library, not its category: the 短剧 library holds
            // rows the server categorised as TV, and filtering on the category
            // would file them under 电视剧 instead.
            if (!item.library.empty())
            {
                return item.library == library.guid;
            }
            return item.category == library.category;
        }
        return false;
    }

    int library_count() const
    {
        if (rail_is_home(rail_) || rail_is_settings(rail_))
            return 0;
        return static_cast<int>(shelf_.size());
    }

    int library_item(int index) const
    {
        if (index >= 0 && index < static_cast<int>(shelf_.size()))
            return shelf_[static_cast<std::size_t>(index)];
        return hero_index_;
    }

    // ---- 库页筛选 / 排序（客户端，参考飞牛网页的弹出式筛选面板） ----
    // The NAS does no filtering on /item/list - every filter key we probed left
    // `total` untouched - so the web itself filters the loaded rows in the
    // browser. We do the same: the shelf is the wall projected through the
    // active filters and the current sort, rebuilt only when something changes
    // (a fresh publish, a rail switch, a filter or sort tap, a new query). Most
    // dimensions read fields the list row already carries (分类/分辨率/动态范围/
    // 音频/年份/观看); 类型 and 国家 read the detail-only genre / country text,
    // which the worker fills in on the background enrich pass.
    static constexpr int kFilterDims = 8; // 分类,类型,分辨率,动态范围,音频,国家,年份,观看
    static constexpr int kPanelRows = kFilterDims + 1; // +1 for the 排序 row

    static const char *filter_dim_label(int row)
    {
        static const char *const k[] = {"影视分类", "类型",       "分辨率",
                                        "动态范围", "音频规格",   "国家/地区",
                                        "发行年份", "是否已观看", "排序"};
        return k[row];
    }
    // One row's option labels. 排序 (row kFilterDims) has its own set; the eight
    // filter dimensions index the same `f_` array.
    static const char *const *filter_opts(int row, int &n)
    {
        static const char *const cat[] = {"全部", "电影", "电视剧", "其他"};
        static const char *const genre[] = {"全部", "剧情", "喜剧",     "动作", "冒险",
                                            "动画", "悬疑", "科幻奇幻", "恐怖", "爱情"};
        static const char *const res[] = {"全部", "4K", "1080P", "720P", "其他"};
        static const char *const color[] = {"全部", "杜比视界", "HDR10", "SDR", "其他"};
        static const char *const audio[] = {"全部", "杜比全景声", "DTS", "立体声", "其他"};
        static const char *const region[] = {"全部", "中国", "美国", "英国",
                                             "日本", "韩国", "法国", "印度"};
        static const char *const year[] = {"全部",     "今年",     "近三年", "2020年代",
                                           "2010年代", "2000年代", "更早"};
        static const char *const watch[] = {"全部", "已观看", "未观看"};
        static const char *const sort[] = {"添加日期", "评分", "标题", "年份"};
        switch (row)
        {
        case 0:
            n = 4;
            return cat;
        case 1:
            n = 10;
            return genre;
        case 2:
            n = 5;
            return res;
        case 3:
            n = 5;
            return color;
        case 4:
            n = 5;
            return audio;
        case 5:
            n = 8;
            return region;
        case 6:
            n = 7;
            return year;
        case 7:
            n = 3;
            return watch;
        default:
            n = 4;
            return sort;
        }
    }
    int row_selection(int row) const
    {
        return row < kFilterDims ? f_[row] : sort_field_;
    }
    void set_row_selection(int row, int col)
    {
        if (row < kFilterDims)
            f_[row] = col;
        else
            sort_field_ = col;
        shelf_dirty_ = true;
    }
    // How many filter dimensions currently exclude their "全部" (0) default.
    int active_filter_count() const
    {
        int n = 0;
        for (int i = 0; i < kFilterDims; ++i)
            if (f_[i] != 0)
                ++n;
        return n;
    }
    void reset_filters()
    {
        for (int i = 0; i < kFilterDims; ++i)
            f_[i] = 0;
        shelf_dirty_ = true;
    }

    static bool contains_ci(const std::string &hay, const std::string &needle)
    {
        if (needle.empty())
            return true;
        std::string h, s;
        h.reserve(hay.size());
        for (char ch : hay)
            h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        for (char ch : needle)
            s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        return h.find(s) != std::string::npos;
    }
    // Does a row fall inside every active filter? Empty fields (a genre the
    // server has not described yet) are excluded once that dimension is set,
    // which is the honest result: the library genuinely has no rows tagged so.
    bool item_passes(const Item &it) const
    {
        if (f_[0] != 0)
        {
            const bool movie = it.kind == Kind::movie;
            const bool show = it.kind == Kind::show;
            if (f_[0] == 1 && !movie)
                return false;
            if (f_[0] == 2 && !show)
                return false;
            if (f_[0] == 3 && (movie || show))
                return false;
        }
        int n = 0;
        if (f_[1] > 0)
        {
            const char *const *g = filter_opts(1, n);
            if (it.genre.find(g[f_[1]]) == std::string::npos)
                return false;
        }
        if (f_[5] > 0)
        {
            const char *const *r = filter_opts(5, n);
            if (it.country.find(r[f_[5]]) == std::string::npos)
                return false;
        }
        if (f_[2] > 0)
        {
            const std::string rl = it.resolution;
            const bool k4 = contains_ci(rl, "4k") || contains_ci(rl, "2160");
            const bool k1080 = contains_ci(rl, "1080");
            const bool k720 = contains_ci(rl, "720");
            const bool ok = f_[2] == 1   ? k4
                            : f_[2] == 2 ? k1080
                            : f_[2] == 3 ? k720
                                         : !(k4 || k1080 || k720);
            if (!ok)
                return false;
        }
        if (f_[3] > 0)
        {
            const std::string &cl = it.color_range_type;
            const bool dv = contains_ci(cl, "dolby") || contains_ci(cl, "vision");
            const bool hdr = contains_ci(cl, "hdr");
            const bool sdr = contains_ci(cl, "sdr");
            const bool ok = f_[3] == 1   ? dv
                            : f_[3] == 2 ? hdr
                            : f_[3] == 3 ? (sdr || cl.empty())
                                         : !(dv || hdr);
            if (!ok)
                return false;
        }
        if (f_[4] > 0)
        {
            const std::string &al = it.audio_type;
            const bool atmos = contains_ci(al, "atmos");
            const bool dts = contains_ci(al, "dts");
            const bool stereo = contains_ci(al, "stereo") || contains_ci(al, "aac");
            const bool ok = f_[4] == 1   ? atmos
                            : f_[4] == 2 ? dts
                            : f_[4] == 3 ? stereo
                                         : !(atmos || dts);
            if (!ok)
                return false;
        }
        if (f_[6] > 0)
        {
            const int y = std::atoi(it.year.c_str());
            const int now = newest_year_ > 0 ? newest_year_ : 2026;
            bool ok = false;
            switch (f_[6])
            {
            case 1:
                ok = y == now;
                break;
            case 2:
                ok = y > now - 3 && y <= now;
                break;
            case 3:
                ok = y >= 2020 && y < 2030;
                break;
            case 4:
                ok = y >= 2010 && y < 2020;
                break;
            case 5:
                ok = y >= 2000 && y < 2010;
                break;
            case 6:
                ok = y > 0 && y < 2000;
                break;
            default:
                ok = true;
                break;
            }
            if (!ok)
                return false;
        }
        if (f_[7] == 1 && it.watched == 0)
            return false;
        if (f_[7] == 2 && it.watched != 0)
            return false;
        return true;
    }

    // Project wall_ through shelf_match (the rail's own filter) + item_passes,
    // then order by the current sort. Runs on a change, not every frame.
    void rebuild_shelf()
    {
        shelf_.clear();
        newest_year_ = 0;
        for (int i = 0; i < library_items_ && i < static_cast<int>(items_.size()); ++i)
        {
            const int y = std::atoi(items_[static_cast<std::size_t>(i)].year.c_str());
            if (y > newest_year_)
                newest_year_ = y;
        }
        if (rail_is_home(rail_) || rail_is_settings(rail_))
            return;
        for (const int head : wall_)
        {
            const Item &item = items_[static_cast<std::size_t>(head)];
            if (shelf_match(item) && item_passes(item))
                shelf_.push_back(head);
        }
        auto cmp = [&](int a, int b)
        {
            const Item &x = items_[static_cast<std::size_t>(a)];
            const Item &y = items_[static_cast<std::size_t>(b)];
            switch (sort_field_)
            {
            case 1:
                return x.rating > y.rating;
            case 2:
                return x.title < y.title;
            case 3:
                return std::atoi(x.year.c_str()) > std::atoi(y.year.c_str());
            default:
                return false; // 0 添加日期: keep the server / wall order
            }
        };
        if (sort_field_ != 0)
            std::stable_sort(shelf_.begin(), shelf_.end(), cmp);
        grid_ = std::clamp(grid_, 0, std::max(0, static_cast<int>(shelf_.size()) - 1));
        grid_scroll_ = std::clamp(grid_scroll_, 0, std::max(0, grid_rows_total() - kGridRows));
    }

    void open_rail_preset(int chip, app::Feedback &feedback)
    {
        switch (chip)
        {
        case 0: // 筛选: the full popup panel
            panel_open_ = true;
            panel_row_ = 0;
            panel_col_ = row_selection(0);
            break;
        case 1: // 最近添加
            sort_field_ = 0;
            shelf_dirty_ = true;
            break;
        case 2: // 高分
            sort_field_ = 1;
            shelf_dirty_ = true;
            break;
        case 3: // 未看: quick toggle of the 已观看 dimension
            f_[7] = f_[7] == 2 ? 0 : 2;
            shelf_dirty_ = true;
            break;
        }
        feedback.play(audio::Cue::select);
    }

    // The popup owns the pad while it is up, exactly as the detail track menu
    // does. Up/down walk dimensions, left/right walk that row's options, Cross
    // applies, and Circle / Options close it.
    void update_filter_panel(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.is_pressed(Action::back) || input.is_pressed(Action::menu))
        {
            panel_open_ = false;
            feedback.play(audio::Cue::back);
            return;
        }
        int n = 0;
        filter_opts(panel_row_, n);
        if (input.nav == Direction::up)
        {
            panel_row_ = (panel_row_ + kPanelRows - 1) % kPanelRows;
            panel_col_ = row_selection(panel_row_);
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::down)
        {
            panel_row_ = (panel_row_ + 1) % kPanelRows;
            panel_col_ = row_selection(panel_row_);
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::left && panel_col_ > 0)
        {
            --panel_col_;
            feedback.play(audio::Cue::focus);
        }
        else if (input.nav == Direction::right && panel_col_ + 1 < n)
        {
            ++panel_col_;
            feedback.play(audio::Cue::focus);
        }
        if (input.is_pressed(Action::confirm))
        {
            set_row_selection(panel_row_, panel_col_);
            feedback.play(audio::Cue::select);
        }
    }

    // A centered panel over a dimmed wall. Each dimension is a row: its label on
    // the left, then that row's option chips. The cursor highlights the focused
    // option; the already-applied choice in every row stays lit.
    void draw_filter_panel(gfx::DrawList &list) const
    {
        list.rounded_rect({0.0f, 0.0f, kWidth, kHeight}, 0.0f, kCoal.with_alpha(0.78f));
        const float px = kContentX + 60.0f;
        const float pw = kContentW - 120.0f;
        const float py = 150.0f;
        const float ph = 800.0f;
        list.rounded_rect({px, py, pw, ph}, 26.0f, kCoalLow.with_alpha(0.99f));
        list.bordered_rect({px, py, pw, ph}, 26.0f, kClear, 1.5f, kWhite.with_alpha(0.14f));
        ui::text(list, glyphs_.semibold, "筛选", px + 34.0f, py + 46.0f, 30.0f, kAccent);
        ui::text(list, glyphs_.regular, "方向键选择 · ✕ 应用 · ○ / Options 返回", px + pw - 34.0f,
                 py + 46.0f, 18.0f, kMuted.with_alpha(0.7f), Align::right);

        const float row_top = py + 96.0f;
        const float row_pitch = 78.0f;
        const float label_x = px + 34.0f;
        const float opt_x0 = px + 190.0f;
        for (int row = 0; row < kPanelRows; ++row)
        {
            const float ry = row_top + static_cast<float>(row) * row_pitch;
            const bool focused_row = panel_row_ == row;
            ui::text(list, glyphs_.regular, filter_dim_label(row), label_x, ry + 20.0f, 18.0f,
                     focused_row ? kInk : kMuted.with_alpha(0.75f));
            int n = 0;
            const char *const *opts = filter_opts(row, n);
            float ox = opt_x0;
            for (int i = 0; i < n; ++i)
            {
                const float cw = std::max(64.0f, glyphs_.regular.measure(opts[i], 16.0f) + 30.0f);
                if (ox + cw > px + pw - 30.0f) // wrap a long row onto a second line
                {
                    ox = opt_x0;
                }
                const bool selected = row_selection(row) == i;
                const bool cursor = focused_row && panel_col_ == i;
                const Color fill =
                    cursor ? kAccent
                           : (selected ? kWhite.with_alpha(0.18f) : kWhite.with_alpha(0.05f));
                list.rounded_rect({ox, ry, cw, 40.0f}, 18.0f, fill);
                ui::text(list, glyphs_.regular, opts[i], ox + cw * 0.5f, ry + 26.0f, 16.0f,
                         cursor ? kOnAccent : (selected ? kInk : kMuted.with_alpha(0.75f)),
                         Align::center);
                ox += cw + 10.0f;
            }
        }
    }

    Rect focus_rect() const
    {
        if (zone_ == Zone::home)
        {
            if (row_ == 0)
                return {kContentX, hero_index_Y, kContentW, hero_index_H};
            if (row_ == 1)
                return wide_card(col_);
            return recent_card(col_);
        }
        if (zone_ == Zone::library)
            return grid_card(grid_);
        if (zone_ == Zone::detail)
        {
            if (detail_focus_ == 0)
                return {96.0f, 566.0f, 160.0f, 48.0f};
            if (detail_focus_ == 1)
                return {268.0f, 566.0f, 58.0f, 48.0f};
            if (detail_focus_ == 2)
                return {338.0f, 566.0f, 150.0f, 48.0f};
            if (detail_focus_ == 3)
                return {500.0f, 566.0f, 150.0f, 48.0f};
            if (detail_focus_ >= episode_base())
            {
                const SeasonRun &run = current_run();
                const int local = std::clamp(focused_episode() - run.begin, 0,
                                             std::max(0, run.end - run.begin - 1));
                return episode_chip(local);
            }
            return season_chip(detail_focus_ - kDetailActions);
        }
        if (zone_ == Zone::settings)
            return {kContentX + 40.0f,
                    kSettingsRowTop + static_cast<float>(settings_row_) * kSettingsRowPitch,
                    kContentW - 80.0f, 78.0f};
        return {0.0f, 0.0f, 0.0f, 0.0f};
    }

    void glide_focus(float dt)
    {
        const Rect target = focus_rect();
        if (!cursor_ready_)
        {
            cursor_ = target;
            cursor_ready_ = true;
            return;
        }
        const float k = std::min(1.0f, dt * 16.0f);
        cursor_.x += (target.x - cursor_.x) * k;
        cursor_.y += (target.y - cursor_.y) * k;
        cursor_.w += (target.w - cursor_.w) * k;
        cursor_.h += (target.h - cursor_.h) * k;
    }

    Rect wide_card(int index) const
    {
        constexpr float gap = 24.0f;
        constexpr float w = 313.0f;
        const int slot = index - continue_scroll_;
        return {kContentX + static_cast<float>(slot) * (w + gap), kRow1Y, w, kRow1H};
    }

    // How many wide cards fit across the content column at once; the rest are
    // reached by sliding the window as the focus walks right (clamp_continue_scroll).
    static constexpr int kContinueVisible = static_cast<int>(kContentW / (313.0f + 24.0f));

    // Keeps the focused 继续观看 card inside the on-screen window without
    // scrolling past either end - clamp_grid_scroll for a single row.
    void clamp_continue_scroll()
    {
        const int n = static_cast<int>(home_continue_.size());
        const int vis = kContinueVisible > 0 ? kContinueVisible : 1;
        if (n <= vis)
        {
            continue_scroll_ = 0;
            return;
        }
        if (col_ < continue_scroll_)
            continue_scroll_ = col_;
        if (col_ > continue_scroll_ + vis - 1)
            continue_scroll_ = col_ - vis + 1;
        continue_scroll_ = std::clamp(continue_scroll_, 0, n - vis);
    }

    Rect recent_card(int index) const
    {
        constexpr float gap = 22.0f;
        constexpr float w = 150.0f;
        return {kContentX + static_cast<float>(index) * (w + gap), kRow2Y, w, kRow2H};
    }

    // The grid's geometry, in one place: six across, and two rows fit between
    // the page heading and the hint line on a 1080 screen. A library of 128
    // cards is 22 rows, so the window slides instead of the page.
    static constexpr int kGridColumns = 6;
    static constexpr int kGridRows = 2;
    static constexpr float kGridTop = 286.0f;
    static constexpr float kGridCardW = 229.0f;
    static constexpr float kGridCardH = 342.0f;
    static constexpr float kGridGap = 22.0f;

    int grid_rows_total() const
    {
        const int count = library_count();
        return (count + kGridColumns - 1) / kGridColumns;
    }

    // The window the page actually draws. Clamped again here, not only in
    // clamp_grid_scroll(), because a background sync can shrink the library
    // between two frames and the drawing side must never trust a stale row.
    int grid_scroll_now() const
    {
        return std::clamp(grid_scroll_, 0, std::max(0, grid_rows_total() - kGridRows));
    }

    // Keeps the focused card inside the two rows that are on screen. The
    // window is row-aligned, so a card never sits half off the top, and it
    // never scrolls past the last row.
    void clamp_grid_scroll()
    {
        const int rows = grid_rows_total();
        const int focus_row = grid_ / kGridColumns;
        if (focus_row < grid_scroll_)
        {
            grid_scroll_ = focus_row;
        }
        if (focus_row > grid_scroll_ + kGridRows - 1)
        {
            grid_scroll_ = focus_row - kGridRows + 1;
        }
        grid_scroll_ = std::clamp(grid_scroll_, 0, std::max(0, rows - kGridRows));
    }

    Rect grid_card(int index) const
    {
        const int column = index % kGridColumns;
        const int line = index / kGridColumns - grid_scroll_now();
        return {kContentX + static_cast<float>(column) * (kGridCardW + kGridGap),
                kGridTop + static_cast<float>(line) * (kGridCardH + kGridGap), kGridCardW,
                kGridCardH};
    }

    // Where the episode strip's chips sit for this frame. The window slides
    // with the focus, so every chip of a long season stays reachable without
    // scrolling the page, and the ring can only land on a chip that is drawn.
    int episode_window() const
    {
        const SeasonRun &run = current_run();
        const int count = std::max(0, run.end - run.begin);
        if (count == 0)
        {
            return 0;
        }
        const int shown = std::min(count, kEpisodeSlots);
        int focus = std::clamp(episode_ - run.begin, 0, shown - 1);
        if (detail_focus_ >= episode_base())
        {
            focus = std::clamp(focused_episode() - run.begin, 0, shown - 1);
        }
        return strip_first(focus, kEpisodeSlots, count);
    }

    // `index` is the episode's place inside the season the strip shows.
    Rect episode_chip(int index) const
    {
        constexpr float gap = 12.0f;
        constexpr float w = 80.0f;
        const int column = index - episode_window();
        // One season: the chips sit where they always did. More than one: a
        // row of season chips takes the line above them.
        const float y = season_focus_count() > 0 ? 866.0f : 826.0f;
        return {96.0f + static_cast<float>(column) * (w + gap), y, w, 62.0f};
    }

    int season_window() const
    {
        const int count = season_focus_count();
        if (count == 0)
        {
            return 0;
        }
        int focus = std::clamp(season_sel_, 0, count - 1);
        if (detail_focus_ >= kDetailActions && detail_focus_ < episode_base())
        {
            focus = std::clamp(detail_focus_ - kDetailActions, 0, count - 1);
        }
        return strip_first(focus, kSeasonSlots, count);
    }

    // `index` is the absolute season, 0 being the first one the list had.
    Rect season_chip(int index) const
    {
        constexpr float gap = 10.0f;
        constexpr float w = 88.0f;
        const int column = index - season_window();
        return {96.0f + static_cast<float>(column) * (w + gap), 812.0f, w, 40.0f};
    }

    // ---- drawing ----

    void draw_rail(gfx::DrawList &list) const
    {
        // The brand: a play mark in the accent and the product name, sized up
        // so it reads at a glance rather than hiding in the top corner.
        const Rect mark{kRailX, 64.0f, 44.0f, 44.0f};
        list.rounded_rect(mark, 12.0f, kAccent);
        list.bordered_rect(mark, 12.0f, kClear, 1.5f, kWhite.with_alpha(0.28f));
        list.glow(mark, 12.0f, 12.0f, kAccent.with_alpha(0.26f));
        list.triangle({mark.x + 15.0f, mark.y + 12.0f, 18.0f, 20.0f}, kOnAccent, 0.0f, 1.5708f);
        ui::text(list, glyphs_.semibold, "飞牛影视", kRailX + 56.0f, 98.0f, 34.0f, kInk);

        for (int i = 0; i < rail_count(); ++i)
        {
            const float y = kRailTop + static_cast<float>(i) * kRailPitch;
            const bool active = rail_ == i;
            const bool hot = active || (rail_focus_ && rail_ == i);
            if (hot)
            {
                list.rounded_rect({kRailX - 18.0f, y - 30.0f, 4.0f, 40.0f}, 2.0f, kAccent);
                list.glow({kRailX - 20.0f, y - 32.0f, 8.0f, 44.0f}, 4.0f, 18.0f,
                          kAccent.with_alpha(0.35f));
                if (rail_focus_)
                    list.rounded_rect({kRailX - 30.0f, y - 38.0f, 220.0f, 56.0f}, 12.0f,
                                      kWhite.with_alpha(0.05f));
            }
            fit_text(list, glyphs_.semibold, rail_label(i), kRailX, y, active ? 25.0f : 23.0f,
                     active ? kInk : kMuted.with_alpha(0.62f), 132.0f);
            const int cnt = rail_item_count(i);
            if (cnt > 0)
                ui::text(list, glyphs_.regular, std::to_string(cnt), kRailX + 182.0f, y,
                         active ? 21.0f : 19.0f, active ? kMuted.with_alpha(0.85f) : kFaint,
                         Align::right);
        }

        // A hairline between the rail and the page, so the two never blend.
        list.rounded_rect({kRailX + 196.0f, 200.0f, 1.0f, 640.0f}, 0.5f, kWhite.with_alpha(0.05f));
    }

    void draw_top(gfx::DrawList &list) const
    {
        const std::string section = rail_label(rail_);
        // A breadcrumb, not a second title: the page names itself below.
        const std::string crumb =
            zone_ == Zone::home ? std::string("首页") : std::string("首页 · ") + section;
        ui::text(list, glyphs_.regular, crumb, kContentX, 84.0f, 18.0f, kFaint);
        // The connection line tells the truth: which NAS, and whether the
        // worker got in. A refused login shows the reason here rather than
        // hiding behind a green dot.
        const std::string state =
            status_.empty() ? (server_label() + (signed_in_ ? " · 已连接" : " · 未连接")) : status_;
        ui::text(list, glyphs_.regular, state, kWidth - kMargin, 84.0f, 18.0f, kFaint,
                 Align::right);
        const Color dot = signed_in_ ? Color::rgb(0x3ecf8e) : Color::rgb(0xb6afa5);
        // Park the lamp just left of the state text's own edge. A fixed offset
        // collided with the label once the address + 已连接 ran wider than it,
        // because the text is right-aligned and grows leftward into the dot.
        const float state_w = glyphs_.regular.measure(state, 18.0f);
        const float dot_x = kWidth - kMargin - state_w - 16.0f;
        list.circle(dot_x, 78.0f, 5.0f, dot);

        // 刷新片源: a pure reload icon on the same line as the connection lamp,
        // parked just left of it. A shape (open arc + arrowhead), not a glyph,
        // since the font carries no icon. Its x is derived from the lamp's own
        // edge, so it never collides with the right-aligned state text even on a
        // long address. Only the shelves that own a wall show it.
        if (refresh_available())
        {
            const bool busy = fnvideo::Service::instance().view().busy;
            const bool hot = refresh_focus_;
            constexpr float icon = 34.0f;
            const float ix = dot_x - 16.0f - icon; // 16px gap from the lamp edge
            const float iy = 78.0f - icon * 0.5f;  // centred on the lamp line
            const Color glyph = hot ? kOnAccent : kFaint;
            if (hot)
                list.rounded_rect({ix, iy, icon, icon}, 10.0f, kAccent);
            else
                list.rounded_rect({ix, iy, icon, icon}, 10.0f, kCoal.with_alpha(0.75f));
            list.bordered_rect({ix, iy, icon, icon}, 10.0f, kClear, 1.5f,
                               kWhite.with_alpha(hot ? 0.55f : 0.20f));
            // The reload mark: a near-full circle left open at the top with a
            // small arrowhead at the gap. While refreshing it spins.
            const float cx = ix + icon * 0.5f;
            const float cy = iy + icon * 0.5f;
            const float r = icon * 0.30f;
            const float spin = busy ? clock_ * 2.4f : 0.0f;
            list.arc(cx, cy, r, 3.0f, 0.6f + spin, 5.1f, glyph);
            list.triangle({cx + r * 0.30f, cy - r - 6.0f, 10.0f, 10.0f}, glyph, 0.0f, 2.3f);
        }
    }

    // The address without its scheme, which is all that fits on one line.
    std::string server_label() const
    {
        std::string server = fnvideo::Service::instance().server();
        const std::size_t scheme = server.find("://");
        if (scheme != std::string::npos)
        {
            server.erase(0, scheme + 3);
        }
        while (!server.empty() && server.back() == '/')
        {
            server.pop_back();
        }
        return server.empty() ? std::string("未配置") : server;
    }

    void draw_home(gfx::DrawList &list) const
    {
        const float rise = (1.0f - enter_) * 24.0f;
        const Item &hero = item_at(hero_index_);
        // The banner's floor is always the tinted plate; when the 随机推荐
        // rotates, the artwork and words fade in over it (the kit's hero swap),
        // so a new card eases in instead of popping into place.
        const float fade = hero_swap_;

        // ---- the hero ----
        const Rect box{kContentX, hero_index_Y + rise, kContentW, hero_index_H};
        list.gradient_rect(box, 26.0f, Color::rgb(hero.tint, 0.92f),
                           gfx::mix(Color::rgb(hero.tint), kCoal, 0.78f));
        list.push_opacity(fade);
        // The wide still when the NAS has one, cropped to the banner's shape.
        const Plate art = still_plate(hero, box.w, box.h);
        if (art.texture != 0)
        {
            list.image(art.texture, box, art.uv, kWhite, 26.0f);
        }
        // A dark scrim on the left half carries the words. It is heavier over
        // a photograph than over a flat plate, which needs no help reading.
        list.gradient_rect_h({box.x, box.y, box.w * 0.62f, box.h}, 26.0f,
                             kCoal.with_alpha(art.texture != 0 ? 0.90f : 0.72f), kClear);
        if (art.texture != 0)
        {
            // A second scrim along the bottom, so the two buttons sit on
            // something rather than on whatever the still happens to show.
            list.gradient_rect({box.x, box.y + box.h * 0.58f, box.w, box.h * 0.42f}, 26.0f, kClear,
                               kCoal.with_alpha(0.62f));
        }
        else
        {
            // A soft ring stands in for the still: it reads as artwork, not as
            // a missing picture.
            list.ring(box.x + box.w * 0.80f, box.y + box.h * 0.46f, 128.0f, 2.0f,
                      kWhite.with_alpha(0.14f));
            list.ring(box.x + box.w * 0.80f, box.y + box.h * 0.46f, 176.0f, 1.0f,
                      kWhite.with_alpha(0.07f));
            list.circle(box.x + box.w * 0.80f, box.y + box.h * 0.46f, 96.0f,
                        kWhite.with_alpha(0.06f));
        }
        // The hero has no ring of its own, so its frame says when it holds the
        // focus. While the rail owns the pad its frame must go away too, or
        // two things look focused at once.
        const bool hero_hot = row_ == 0 && !rail_focus_;
        if (hero_hot)
            list.bordered_rect({box.x - 3.0f, box.y - 3.0f, box.w + 6.0f, box.h + 6.0f}, 29.0f,
                               kClear, 2.0f, kWhite.with_alpha(0.34f));

        const char *const tag = "随机推荐";
        const float tag_w = glyphs_.semibold.measure(tag, 17.0f) + 24.0f;
        list.rounded_rect({box.x + 48.0f, box.y + 46.0f, tag_w, 30.0f}, 8.0f,
                          kAccent.with_alpha(0.22f));
        ui::text(list, glyphs_.semibold, tag, box.x + 60.0f, box.y + 68.0f, 17.0f, kAccent);
        fit_text(list, glyphs_.semibold, hero.title, box.x + 48.0f, box.y + 156.0f, 60.0f, kInk,
                 box.w - 96.0f);
        fit_text(list, glyphs_.regular, hero.meta, box.x + 48.0f, box.y + 196.0f, 21.0f, kMuted,
                 box.w - 96.0f);
        fit_text(list, glyphs_.regular, hero.synopsis, box.x + 48.0f, box.y + 246.0f, 20.0f,
                 kMuted.with_alpha(0.86f), box.w - 96.0f);
        // The two hero buttons are its real targets. Left/right picks one and
        // Cross acts; both borrow the kit's glossed-surface recipe (the same one
        // the detail page's 播放 key uses) - a drop shadow, a lit-to-deep
        // gradient, a shine over the top half and a rim - so they read as raised,
        // pressable controls, not flat chips. The active one is glossed; the
        // other rests as a panel, and both share a size and radius.
        const bool play_sel = hero_hot && col_ == 0;
        const bool detail_sel = hero_hot && col_ == 1;
        const Rect pb{box.x + 48.0f, box.y + 286.0f, 168.0f, 52.0f};
        const Rect db{box.x + 232.0f, box.y + 286.0f, 168.0f, 52.0f};
        const auto hero_button = [&](const Rect &r, bool hot, bool play_icon)
        {
            const Color ink = hot ? kOnAccent : kInk;
            if (hot)
            {
                list.shadow({r.x, r.y + 4.0f, r.w, r.h}, 26.0f, 14.0f, Color::rgb(0x000000, 0.5f));
                list.gradient_rect(r, 26.0f, kAccentHi, kAccentLo);
                list.gradient_rect({r.x + 3.0f, r.y + 3.0f, r.w - 6.0f, r.h * 0.46f}, 23.0f,
                                   kWhite.with_alpha(0.22f), kWhite.with_alpha(0.0f));
                list.bordered_rect(r, 26.0f, kClear, 1.2f, Color::rgb(0x2f6fb5));
            }
            else
            {
                list.rounded_rect(r, 26.0f, kWhite.with_alpha(0.06f));
                list.bordered_rect(r, 26.0f, kClear, 1.5f, kWhite.with_alpha(0.22f));
            }
            if (play_icon)
            {
                // Centre the icon+label group in 播放.
                constexpr float icon_w = 13.0f;
                constexpr float lbl_gap = 12.0f;
                const float lw = glyphs_.semibold.measure("播放", 21.0f);
                const float gx = r.x + (r.w - (icon_w + lbl_gap + lw)) * 0.5f;
                list.triangle({gx, r.y + 18.0f, icon_w, 15.0f}, ink, 0.0f, 1.5708f);
                ui::text(list, glyphs_.semibold, "播放", gx + icon_w + lbl_gap, r.y + 34.0f, 21.0f,
                         ink);
            }
            else
            {
                ui::text(list, glyphs_.semibold, "详情", r.cx(), r.y + 34.0f, 21.0f, ink,
                         Align::center);
            }
        };
        // 播放 is the default primary, so it stays glossed unless the hero is hot
        // and 详情 holds the pick; 详情 only glosses when picked.
        hero_button(pb, play_sel || !hero_hot, true);
        hero_button(db, detail_sel, false);

        // ---- the 随机推荐 pager ----
        // One dot per banner card, the current one lit and a touch larger, so the
        // rotation reads as a small set rather than a single poster that changes.
        for (int i = 0; i < hero_slot_n_; ++i)
        {
            const bool on = (i == hero_slide_);
            const float cx = box.x + box.w - 40.0f - (hero_slot_n_ - 1 - i) * 20.0f;
            const float cy = box.y + 312.0f;
            list.circle(cx, cy, on ? 6.0f : 4.0f, on ? kAccent : kWhite.with_alpha(0.30f));
        }
        list.pop_opacity();

        // ---- 继续观看 ----
        ui::text(list, glyphs_.semibold, "继续观看", kContentX, kRowLabel1, 22.0f, kInk);
        ui::text(list, glyphs_.regular, "从上次停下的地方接着看", kContentX + 108.0f, kRowLabel1,
                 17.0f, kFaint);
        const int cont_n = static_cast<int>(home_continue_.size());
        const int vis = kContinueVisible > 0 ? kContinueVisible : 1;
        const int first = std::clamp(continue_scroll_, 0, std::max(0, cont_n - vis));
        const int last = std::min(cont_n, first + vis);
        for (int i = first; i < last; ++i)
        {
            const Item &item = items_[home_continue_[static_cast<std::size_t>(i)]];
            draw_wide_card(list, wide_card(i), item);
        }
        // A thin rail under the row says more cards wait off-screen.
        if (cont_n > vis)
        {
            const float bar_y = kRow1Y + kRow1H + 8.0f;
            const float thumb_w = kContentW * static_cast<float>(vis) / static_cast<float>(cont_n);
            const float span = kContentW - thumb_w;
            const float thumb_x =
                kContentX + span * static_cast<float>(first) / static_cast<float>(cont_n - vis);
            list.rounded_rect({kContentX, bar_y, kContentW, 4.0f}, 2.0f, kWhite.with_alpha(0.08f));
            list.rounded_rect({thumb_x, bar_y, thumb_w, 4.0f}, 2.0f, kWhite.with_alpha(0.24f));
        }

        // ---- 最近收藏 ----
        ui::text(list, glyphs_.semibold, "最近收藏", kContentX, kRowLabel2, 22.0f, kInk);
        if (home_recent_.empty())
        {
            ui::text(list, glyphs_.regular, "在详情页按收藏，这里会出现你的片单",
                     kContentX + 108.0f, kRowLabel2, 17.0f, kFaint);
        }
        else
        {
            ui::text(list, glyphs_.regular, "你标记收藏的条目", kContentX + 108.0f, kRowLabel2,
                     17.0f, kFaint);
            for (int i = 0; i < static_cast<int>(home_recent_.size()); ++i)
            {
                const Item &item = items_[home_recent_[static_cast<std::size_t>(i)]];
                draw_small_poster(list, recent_card(i), item);
            }
        }
    }

    void draw_wide_card(gfx::DrawList &list, const Rect &box, const Item &item) const
    {
        const Plate art = still_plate(item, box.w, box.h);
        if (art.texture != 0)
        {
            list.image(art.texture, box, art.uv, kWhite, 18.0f);
        }
        else
        {
            list.gradient_rect(box, 18.0f, Color::rgb(item.tint, 0.95f),
                               gfx::mix(Color::rgb(item.tint), kCoal, 0.80f));
            list.ring(box.x + box.w * 0.76f, box.y + box.h * 0.36f, 42.0f, 1.5f,
                      kWhite.with_alpha(0.13f));
        }
        // The foot that carries the words, on artwork as on a plate.
        list.gradient_rect({box.x, box.y + box.h * 0.42f, box.w, box.h * 0.58f}, 18.0f, kClear,
                           kCoal.with_alpha(0.86f));
        fit_text(list, glyphs_.semibold, item.title, box.x + 20.0f, box.y + box.h - 42.0f, 22.0f,
                 kInk, box.w - 40.0f);
        fit_text(list, glyphs_.regular, item.meta, box.x + 20.0f, box.y + box.h - 18.0f, 15.0f,
                 kMuted.with_alpha(0.8f), box.w - 40.0f);
        // The progress line sits on the bottom edge, as on every streaming app.
        const float bar = box.w - 40.0f;
        list.rounded_rect({box.x + 20.0f, box.y + box.h - 8.0f, bar, 3.0f}, 1.5f,
                          kWhite.with_alpha(0.18f));
        list.rounded_rect({box.x + 20.0f, box.y + box.h - 8.0f, bar * item.progress, 3.0f}, 1.5f,
                          kAccent);
    }

    void draw_small_poster(gfx::DrawList &list, const Rect &box, const Item &item) const
    {
        const Plate art = poster_plate(item, box.w, box.h);
        if (art.texture != 0)
        {
            list.image(art.texture, box, art.uv, kWhite, 14.0f);
            // The caption sits on the poster itself, so it gets a scrim.
            list.gradient_rect({box.x, box.y + box.h * 0.52f, box.w, box.h * 0.48f}, 14.0f, kClear,
                               kCoal.with_alpha(0.92f));
        }
        else
        {
            list.gradient_rect(box, 14.0f, Color::rgb(item.tint, 1.0f),
                               gfx::mix(Color::rgb(item.tint), kCoal, 0.84f));
            const std::string glyph = first_glyph(item.title);
            if (!glyph.empty())
                ui::text(list, glyphs_.semibold, glyph, box.cx(), box.y + box.h * 0.44f,
                         box.w * 0.52f, kWhite.with_alpha(0.12f), Align::center);
        }
        fit_text(list, glyphs_.semibold, item.title, box.x + 14.0f, box.y + box.h - 40.0f, 18.0f,
                 kInk, box.w - 28.0f);
        fit_text(list, glyphs_.regular, item.badge, box.x + 14.0f, box.y + box.h - 18.0f, 14.0f,
                 kMuted.with_alpha(0.75f), box.w - 28.0f);
    }

    void draw_library(gfx::DrawList &list) const
    {
        // The heading is the rail row's own name, so it says 电影 / 电视剧 /
        // 短剧 / 其他视频 exactly as the server filed them rather than a guess
        // made from a rail index.
        const char *title = rail_is_library(rail_)     ? rail_label(rail_)
                            : rail_is_favorites(rail_) ? "收藏"
                            : rail_is_search(rail_)    ? "搜索"
                                                       : "首页";
        const char *sub = rail_is_favorites(rail_) ? "你在 NAS 上标了心的条目"
                          : rail_is_search(rail_)  ? "在影视库里按名字找"
                                                   : "按添加时间排列，最新的在前";
        ui::text(list, glyphs_.semibold, title, kContentX, 178.0f, 46.0f, kInk);
        char count[64];
        std::snprintf(count, sizeof(count), "%d 个条目", library_count());
        ui::text(list, glyphs_.regular, count, kContentX + 150.0f, 176.0f, 20.0f, kFaint);
        ui::text(list, glyphs_.regular, sub, kContentX, 210.0f, 18.0f, kMuted.with_alpha(0.7f));

        // The quick strip: 筛选 opens the popup panel, the other three are one
        // press presets. They light up when their state is the active one, and
        // the focused chip (reached by Up from the first row) takes the accent.
        const char *const filters[] = {"筛选", "最近添加", "高分", "未看"};
        const bool chip_active[4] = {
            active_filter_count() > 0,
            sort_field_ == 0,
            sort_field_ == 1,
            f_[7] == 2,
        };
        float x = kContentX + 260.0f;
        for (int i = 0; i < 4; ++i)
        {
            std::string label = filters[i];
            if (i == 0 && active_filter_count() > 0)
                label += "·" + std::to_string(active_filter_count());
            const bool hot = chip_focus_ && chip_ == i;
            const float w = std::max(96.0f, glyphs_.regular.measure(label, 18.0f) + 44.0f);
            const bool on = chip_active[i] || hot;
            list.rounded_rect({x, 156.0f, w, 40.0f}, 20.0f,
                              hot ? kAccent
                                  : (on ? kWhite.with_alpha(0.14f) : kWhite.with_alpha(0.04f)));
            ui::text(list, glyphs_.regular, label, x + w * 0.5f, 182.0f, 18.0f,
                     hot ? kOnAccent : (on ? kInk : kMuted.with_alpha(0.7f)), Align::center);
            x += w + 12.0f;
        }

        // Transient confirmation that the Options shortcut just cleared the
        // filters, so a quick reset reads as an action, not a silent refresh.
        if (filter_toast_ > 0.0f)
        {
            const float a = std::min(1.0f, filter_toast_ / 0.5f);
            ui::text(list, glyphs_.semibold, "已重置筛选，显示全部", kContentX, 246.0f, 22.0f,
                     kAccent.with_alpha(a));
        }

        // Only the two rows inside the window are drawn; the rest of the
        // library would land off the bottom of a 1080 screen anyway.
        const int count_items = library_count();
        const int first_row = grid_scroll_now();
        const int last_row = std::min(grid_rows_total(), first_row + kGridRows);
        for (int i = first_row * kGridColumns; i < std::min(count_items, last_row * kGridColumns);
             ++i)
        {
            const Item &item = items_[library_item(i)];
            draw_grid_poster(list, grid_card(i), item);
        }

        draw_grid_scrollbar(list);

        if (search_edit_)
        {
            // A solid panel under the keys: the poster wall behind it would
            // otherwise make the labels unreadable. It stops above the hint
            // line so the buttons still say what they do.
            list.rounded_rect({kContentX, 320.0f, kContentW, 540.0f}, 26.0f,
                              kCoal.with_alpha(0.97f));
            ui::text(list, glyphs_.semibold, "搜索片名", kContentX + 40.0f, 352.0f, 24.0f, kAccent);
            const std::string shown = search_input_.empty() ? "输入片名…" : search_input_;
            fit_text(list, glyphs_.regular, shown, kContentX + 40.0f, 390.0f, 28.0f,
                     search_input_.empty() ? kMuted : kInk, kContentW - 80.0f);
            ui::Canvas canvas{list, glyphs_, 0u, clock_};
            keyboard_.draw(canvas);
        }
    }

    // A thin bar at the right edge of the content column, so a 22-row library
    // still says how far down the page the player has walked.
    void draw_grid_scrollbar(gfx::DrawList &list) const
    {
        const int rows = grid_rows_total();
        if (rows <= kGridRows)
            return;
        constexpr float kTrackX = kContentX + 6.0f * (kGridCardW + kGridGap) - 14.0f;
        constexpr float kTrackY = kGridTop;
        constexpr float kTrackH = kGridRows * kGridCardH + (kGridRows - 1) * kGridGap; // 706.0f
        const float thumb_h = kTrackH * static_cast<float>(kGridRows) / static_cast<float>(rows);
        const float span = kTrackH - thumb_h;
        const float thumb_y = kTrackY + span * static_cast<float>(grid_scroll_now()) /
                                            static_cast<float>(rows - kGridRows);
        list.rounded_rect({kTrackX, kTrackY, 6.0f, kTrackH}, 3.0f, kWhite.with_alpha(0.06f));
        list.rounded_rect({kTrackX, thumb_y, 6.0f, thumb_h}, 3.0f, kWhite.with_alpha(0.22f));
    }

    // Turn the server's genre / ancestor field into the web's " · " tag line:
    // whatever separator the value uses - half- or full-width comma, the CJK
    // ideographic comma, a slash or a pipe - becomes " · ", and empty runs
    // (leading, trailing or doubled separators) are dropped so no blank tag
    // survives. A value already separated by "·" passes through untouched.
    static std::string normalize_genres(const std::string &raw)
    {
        std::vector<std::string> parts;
        std::string cur;
        auto flush = [&]()
        {
            const std::size_t a = cur.find_first_not_of(' ');
            const std::size_t b = cur.find_last_not_of(' ');
            if (a != std::string::npos)
                parts.push_back(cur.substr(a, b - a + 1));
            cur.clear();
        };
        const std::string seps[] = {"\uFF0C", "\u3001", "\uFF0F", "|", "/", ","};
        for (std::size_t i = 0; i < raw.size();)
        {
            bool matched = false;
            for (const std::string &s : seps)
            {
                if (raw.compare(i, s.size(), s) == 0)
                {
                    flush();
                    i += s.size();
                    matched = true;
                    break;
                }
            }
            if (matched)
                continue;
            cur.push_back(raw[i++]);
        }
        flush();
        std::string out;
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            if (i)
                out += " \u00b7 ";
            out += parts[i];
        }
        return out;
    }

    void draw_grid_poster(gfx::DrawList &list, const Rect &box, const Item &item) const
    {
        const Plate art = poster_plate(item, box.w, box.h);
        if (art.texture != 0)
        {
            list.image(art.texture, box, art.uv, kWhite, 16.0f);
            // The foot the caption sits on. Over a poster it has to be opaque
            // enough that a bright still does not eat the words.
            list.gradient_rect({box.x, box.y + box.h * 0.62f, box.w, box.h * 0.38f}, 16.0f, kClear,
                               kCoal.with_alpha(0.94f));
        }
        else
        {
            list.gradient_rect(box, 16.0f, Color::rgb(item.tint, 1.0f),
                               gfx::mix(Color::rgb(item.tint), kCoal, 0.86f));
            const std::string glyph = first_glyph(item.title);
            if (!glyph.empty())
                ui::text(list, glyphs_.semibold, glyph, box.cx(), box.y + box.h * 0.40f,
                         box.w * 0.46f, kWhite.with_alpha(0.10f), Align::center);
            list.ring(box.cx(), box.y + box.h * 0.40f, box.w * 0.28f, 1.5f,
                      kWhite.with_alpha(0.08f));
        }

        // The caption reads as a genre line - "动作 · 冒险 · 科幻奇幻". The
        // server's ancestor_name is normalised so whichever separator it uses
        // becomes " · "; a collection keeps its episode count on the tail.
        fit_text(list, glyphs_.semibold, heading_of(item), box.x + 18.0f, box.y + box.h - 62.0f,
                 22.0f, kInk, box.w - 36.0f);
        const std::string genre_str = normalize_genres(item.genre);
        std::string line;
        if (!genre_str.empty())
        {
            line = genre_str;
            if (item.episodes > 0)
            {
                line += " · ";
                line += std::to_string(item.episodes);
                line += " 集";
            }
        }
        else if (item.episodes > 0)
        {
            if (!item.year.empty())
            {
                line = item.year;
                line += " · ";
            }
            line += std::to_string(item.episodes);
            line += " 集";
        }
        else
        {
            line = !item.country.empty() ? item.country : item.year;
        }
        fit_text(list, glyphs_.regular, line, box.x + 18.0f, box.y + box.h - 34.0f, 15.0f,
                 kMuted.with_alpha(0.78f), box.w - 36.0f);

        // Resolution, the way the web client reads it: a small, quiet dark pill
        // in the poster's top-right corner - not the bright accent block that
        // used to fight the artwork. The value is the highest tier the server
        // advertises (media_stream.resolutions[0]), upper-cased by badge_text().
        if (!item.badge.empty())
        {
            const float rfs = 12.0f;
            const float rtw = glyphs_.semibold.measure(item.badge, rfs);
            const Rect rr{box.x + box.w - 12.0f - (rtw + 14.0f), box.y + 12.0f, rtw + 14.0f, 20.0f};
            list.rounded_rect(rr, 5.0f, kCoal.with_alpha(0.62f));
            list.bordered_rect(rr, 5.0f, kClear, 1.0f, kWhite.with_alpha(0.22f));
            ui::text(list, glyphs_.semibold, item.badge, rr.cx(), rr.cy() + rfs * 0.36f, rfs,
                     kWhite.with_alpha(0.86f), Align::center);
        }

        // Score: a gold star badge at the top-left. Only drawn when the server
        // actually rated the row. Trimmed down this pass so it reads as a small
        // mark, not a second poster.
        if (item.rating > 0.0f)
        {
            char rt[16];
            std::snprintf(rt, sizeof(rt), "%.1f", static_cast<double>(item.rating));
            const std::string rs(rt);
            const Color gold = Color::rgb(0xf5c542);
            const float fs = 14.0f;
            const float tw = glyphs_.semibold.measure(rs, fs);
            const float star = 13.0f;
            const float pad = 9.0f;
            const Rect rb{box.x + 12.0f, box.y + 12.0f, pad + star + 5.0f + tw + pad, 24.0f};
            list.rounded_rect(rb, 7.0f, kCoal.with_alpha(0.62f));
            list.bordered_rect(rb, 7.0f, kClear, 1.0f, gold.with_alpha(0.5f));
            list.star(rb.x + pad + star * 0.5f, rb.cy(), 6.0f, gold);
            ui::text(list, glyphs_.semibold, rs, rb.x + pad + star + 5.0f, rb.cy() + fs * 0.36f, fs,
                     gold);
        }
    }

    // ---- detail page audio / subtitle pickers ----
    // Both read the item's server stream list (from /stream/list) and a stored
    // preference keyed on this page's guid; play() seeds the open from it, so a
    // choice made here lands on the decoder when 播放 starts. -1 is the default
    // (best container audio / auto first-Chinese subtitle) and subtitles add
    // -2 = 关闭. A press cycles to the next stream rather than opening a list
    // panel, which keeps the detail focus machine a single line of extra logic.
    static const char *lang_name(const std::string &code)
    {
        // A handful of ISO tags the server carries on a stream, shown in
        // Chinese; an unknown code is rendered raw by detail_stream_name so a
        // tag we have not mapped is never guessed at.
        if (code == "chi" || code == "zho" || code == "zh" || code == "chs" || code == "sc")
            return "中文";
        if (code == "yue" || code == "cht" || code == "tra" || code == "tc")
            return "繁体";
        if (code == "eng" || code == "en")
            return "英文";
        if (code == "jpn" || code == "ja")
            return "日语";
        if (code == "kor" || code == "ko")
            return "韩语";
        return nullptr;
    }

    static std::string detail_stream_name(const std::string &language, const std::string &title,
                                          int number)
    {
        // The server's own track title ("Mandarin"/"Cantonese"/"CHS"/"CHS&ENG")
        // is the most specific label and what the web page leads with, so it wins
        // over the language code: two Chinese audio tracks that share the "chi"
        // code but carry different titles used to collapse into one indistinct
        // "中文" the viewer could not tell apart. An unmapped / unknown code is
        // never shown raw.
        if (!title.empty())
            return title;
        if (const char *cn = lang_name(language))
            return cn;
        if (!language.empty() && language != "und" && language != "zxx" &&
            language != "zz-unknow" && language.rfind("zz", 0) != 0)
            return language;
        return "第 " + std::to_string(number + 1) + " 轨";
    }

    // "5.1(side)" -> "5.1"; "stereo" stays; an empty layout falls back to the
    // raw channel count. The web shows the layout, so the console leads with it.
    static std::string audio_channel_text(const fnvideo::AudioStreamInfo &a)
    {
        std::string ch = a.channel_layout;
        const std::size_t paren = ch.find('(');
        if (paren != std::string::npos)
            ch = ch.substr(0, paren);
        while (!ch.empty() && (ch.back() == ' ' || ch.back() == '.'))
            ch.pop_back();
        if (ch.empty() && a.channels > 0)
            ch = std::to_string(a.channels) + "ch";
        return ch;
    }

    // The one subtitle label, shared by the detail picker, the detail chooser
    // and the player's chip + pull-down so the same track reads the same in
    // both rooms. Earlier the player showed the raw tag ("chi") while the
    // detail page showed the Chinese name - the drift the viewer noticed.
    // `bitmap` is a PGS/graphics track; `external` a sidecar file beside it.
    static std::string subtitle_display_name(const std::string &language, const std::string &title,
                                             bool bitmap, bool external, int number)
    {
        std::string label = detail_stream_name(language, title, number);
        if (bitmap)
            label += " 图像";
        else if (external)
            label += " 外挂";
        return label;
    }

    std::string detail_audio_label(const std::vector<fnvideo::AudioStreamInfo> &auds) const
    {
        const int pref = fnvideo::Service::instance().detail_pref_audio(detail_guid_);
        if (auds.empty() || pref < 0)
            return "音轨 默认";
        const int idx = std::clamp(pref, 0, static_cast<int>(auds.size()) - 1);
        const fnvideo::AudioStreamInfo &a = auds[static_cast<std::size_t>(idx)];
        std::string label = "音轨 " + detail_stream_name(a.language, a.title, idx);
        const std::string ch = audio_channel_text(a);
        if (!ch.empty())
            label += " " + ch;
        if (!a.codec.empty())
            label += " " + a.codec;
        return label;
    }

    std::string detail_subtitle_label(const std::vector<fnvideo::SubtitleStreamInfo> &subs) const
    {
        const int pref = fnvideo::Service::instance().detail_pref_subtitle(detail_guid_);
        if (subs.empty())
            return "字幕";
        if (pref == -2)
            return "字幕 关";
        if (pref < 0)
            return "字幕";
        const int idx = std::clamp(pref, 0, static_cast<int>(subs.size()) - 1);
        const fnvideo::SubtitleStreamInfo &s = subs[static_cast<std::size_t>(idx)];
        return "字幕 " + subtitle_display_name(s.language, s.title, s.bitmap, s.external, idx);
    }

    // Rows: audio = 默认 + one per stream. subtitle = one per stream + 关闭
    // (no 自动 row). The row index and the stored preference value are two
    // different things: audio row 0 is the default, the last subtitle row is
    // 关闭, the middle rows map straight onto the server's stream ordinal.
    static int detail_track_pref_to_row(int which, int pref, int n)
    {
        // Audio: row 0 is 默认 (pref < 0), rows 1..n are the streams.
        if (which == 1)
            return (pref < 0 || n <= 0) ? 0 : std::clamp(pref, 0, n - 1) + 1;
        // Subtitle: rows 0..n-1 are the streams and row n is 关闭 (pref -2).
        // The 自动 row is gone, so an unset choice (pref < 0) parks the cursor
        // on the first row but marks nothing as chosen (the panel matches each
        // row back to the stored pref to decide the dot).
        if (pref == -2)
            return n; // 关闭 is the last row
        if (pref < 0 || n <= 0)
            return 0;
        return std::clamp(pref, 0, n - 1);
    }

    static int detail_track_row_to_pref(int which, int row, int n)
    {
        if (which == 1)
            return row == 0 ? -1 : row - 1; // 默认 / that stream
        // Subtitle: the last row (n) is 关闭, every other row is that stream.
        return row >= n ? -2 : row;
    }

    // The list text for each row, built from one already-read Detail so the
    // panel does not copy the whole detail once per row every frame.
    std::vector<std::string> detail_track_labels(int which,
                                                 const fnvideo::Service::Detail &det) const
    {
        std::vector<std::string> out;
        if (which == 1)
        {
            out.push_back("默认（容器最佳）");
            const auto &auds = det.item.audio_streams;
            for (std::size_t i = 0; i < auds.size(); ++i)
            {
                const fnvideo::AudioStreamInfo &a = auds[i];
                std::string label = detail_stream_name(a.language, a.title, static_cast<int>(i));
                const std::string ch = audio_channel_text(a);
                if (!ch.empty())
                    label += " \u00b7 " + ch;
                if (!a.codec.empty())
                    label += " \u00b7 " + a.codec;
                out.push_back(std::move(label));
            }
        }
        else
        {
            // No 自动 row: the panel lists the real subtitle streams and a
            // trailing 关闭字幕. An unset preference simply marks nothing here.
            const auto &subs = det.item.subtitle_streams;
            for (std::size_t i = 0; i < subs.size(); ++i)
            {
                const fnvideo::SubtitleStreamInfo &s = subs[i];
                out.push_back(subtitle_display_name(s.language, s.title, s.bitmap, s.external,
                                                    static_cast<int>(i)));
            }
            out.push_back("关闭字幕");
        }
        return out;
    }

    // The two track pickers only apply to a single playable file. A series
    // collection has no streams of its own (/stream/list is per-episode), so on
    // those pages the pickers are hidden and their focus slots skipped.
    bool detail_pickers_enabled() const
    {
        return detail_item().episodes <= 0;
    }

    void open_detail_track_menu(int which, app::Feedback &feedback)
    {
        // A series has nothing to pick here; the capsule is already hidden and
        // its focus slot skipped, so this is just a defensive no-op.
        if (!detail_pickers_enabled())
            return;
        // The chooser always opens, even when the server listed no stream for
        // this item - a chip that refuses to open reads as broken (this is what
        // "点不开" was). An empty audio list shows just 默认; an empty subtitle
        // list shows just 关闭字幕 (the online-search / NAS-browse entries are a
        // later step).
        const auto det = fnvideo::Service::instance().detail();
        const int n = which == 1 ? static_cast<int>(det.item.audio_streams.size())
                                 : static_cast<int>(det.item.subtitle_streams.size());
        const int pref = which == 1
                             ? fnvideo::Service::instance().detail_pref_audio(detail_guid_)
                             : fnvideo::Service::instance().detail_pref_subtitle(detail_guid_);
        detail_track_menu_ = which;
        detail_track_sel_ = detail_track_pref_to_row(which, pref, n);
        feedback.play(audio::Cue::tab);
    }

    // The pad while the chooser is up (update_pages short-circuits here): up and
    // down walk the list, Cross stores the pick on this page's guid, Circle or
    // Back closes without changing it.
    void update_detail_track_menu(const InputFrame &input, app::Feedback &feedback)
    {
        const auto det = fnvideo::Service::instance().detail();
        const int n = detail_track_menu_ == 1 ? static_cast<int>(det.item.audio_streams.size())
                                              : static_cast<int>(det.item.subtitle_streams.size());
        const int rows = 1 + n; // audio: 默认 + n; subtitle: n streams + 关闭
        if (input.nav == Direction::down)
        {
            if (detail_track_sel_ + 1 < rows)
            {
                ++detail_track_sel_;
                feedback.play(audio::Cue::focus);
            }
        }
        else if (input.nav == Direction::up)
        {
            if (detail_track_sel_ > 0)
            {
                --detail_track_sel_;
                feedback.play(audio::Cue::focus);
            }
        }
        else if (input.is_pressed(Action::back))
        {
            detail_track_menu_ = 0;
            feedback.play(audio::Cue::back);
        }
        else if (input.is_pressed(Action::confirm))
        {
            const int val = detail_track_row_to_pref(detail_track_menu_, detail_track_sel_, n);
            if (detail_track_menu_ == 1)
                fnvideo::Service::instance().select_detail_audio(detail_guid_, val);
            else
                fnvideo::Service::instance().select_detail_subtitle(detail_guid_, val);
            detail_track_menu_ = 0;
            feedback.play(audio::Cue::toggle);
        }
    }

    void draw_detail_track_menu(gfx::DrawList &list) const
    {
        // Styled to match the player's own track menu (draw_player_menu): the
        // same opaque coal card, the same focused-row accent band, the same
        // sizes and the same 8-row window with a counter - so choosing a track
        // here and choosing one mid-film read as the very same control.
        const auto det = fnvideo::Service::instance().detail();
        const std::vector<std::string> labels = detail_track_labels(detail_track_menu_, det);
        const int rows = static_cast<int>(labels.size());
        if (rows == 0)
            return;
        const int n = detail_track_menu_ == 1 ? static_cast<int>(det.item.audio_streams.size())
                                              : static_cast<int>(det.item.subtitle_streams.size());
        const int current_pref =
            detail_track_menu_ == 1
                ? fnvideo::Service::instance().detail_pref_audio(detail_guid_)
                : fnvideo::Service::instance().detail_pref_subtitle(detail_guid_);

        const int selected = std::clamp(detail_track_sel_, 0, rows - 1);
        constexpr int kMaxRows = 8;
        const int visible = std::min(rows, kMaxRows);
        const int first = std::clamp(selected - visible + 1, 0, std::max(0, rows - visible));

        constexpr float kRowPitch = 40.0f;
        const float width = 480.0f;
        const float height = 72.0f + static_cast<float>(visible) * kRowPitch + 14.0f;
        // The detail page is busy behind the modal, so a light scrim keeps it
        // unmistakable; the panel itself is drawn exactly like the player's,
        // which floats its card over the film with no dimming.
        list.rounded_rect({0.0f, 0.0f, kWidth, kHeight}, 0.0f, Color::rgb(0x000000, 0.55f));
        const Rect card{(kWidth - width) * 0.5f, (kHeight - height) * 0.5f, width, height};
        list.rounded_rect(card, 20.0f, kCoal);
        list.bordered_rect(card, 20.0f, kClear, 1.5f, kWhite.with_alpha(0.18f));
        const char *title = detail_track_menu_ == 1 ? "选择音轨" : "选择字幕";
        ui::text(list, glyphs_.semibold, title, card.x + 24.0f, card.y + 38.0f, 21.0f, kInk);
        if (rows > visible)
        {
            const std::string counter = std::to_string(selected + 1) + " / " + std::to_string(rows);
            ui::text(list, glyphs_.regular, counter, card.x + width - 24.0f, card.y + 38.0f, 16.0f,
                     kFaint, Align::right);
        }

        float row_y = card.y + 66.0f;
        for (int slot = 0; slot < visible; ++slot)
        {
            const int i = first + slot;
            if (i >= rows)
                break;
            const bool focused = i == selected;
            // Mark the stored pick by mapping the row back to its preference
            // value, so an unset subtitle choice (pref < 0, no matching row)
            // shows no dot rather than a wrong one on the first track.
            const bool chosen = detail_track_row_to_pref(detail_track_menu_, i, n) == current_pref;
            if (focused)
                list.rounded_rect({card.x + 12.0f, row_y - 22.0f, width - 24.0f, 36.0f}, 12.0f,
                                  kAccent.with_alpha(0.20f));
            fit_text(list, glyphs_.regular, labels[static_cast<std::size_t>(i)], card.x + 28.0f,
                     row_y + 5.0f, 18.0f, focused ? kInk : kMuted, width - 52.0f);
            // The CJK atlas carries no tick glyph, so the stored pick gets a
            // filled dot on the row's right edge instead.
            if (chosen)
                list.circle(card.x + width - 22.0f, row_y - 4.0f, 5.0f, kAccent);
            row_y += kRowPitch;
        }
        ui::text(list, glyphs_.regular, "上下键选择  X 确认  O 返回", card.x + 24.0f,
                 card.y + height - 12.0f, 16.0f, kFaint);
    }

    void draw_detail(gfx::DrawList &list) const
    {
        const Item &item = detail_item();
        const Color tint = Color::rgb(item.tint);
        const bool series = !detail_episodes_.empty();
        // One server-detail read per frame: the file card, the audio and the
        // subtitle pickers all pull from this rather than each copying the whole
        // Detail (with its episode list) again.
        const auto det = fnvideo::Service::instance().detail();

        // ---- the landscape still ----
        // 飞牛影视 puts the item's landscape art across the top of its detail
        // page. The NAS sends one for most films and series; where it has not
        // arrived yet - or the item has none - the still is painted: a wash of
        // the item's colour, two soft lights and a horizon.
        // The page's own ground first: the still fades into exactly this coal,
        // so there is no seam where the art stops and the page begins.
        list.rounded_rect({0.0f, 0.0f, kWidth, kHeight}, 0.0f, kCoal);
        const Rect still{0.0f, 0.0f, kWidth, 720.0f};
        const Plate art = still_plate(item, still.w, still.h);
        if (art.texture != 0)
        {
            list.image(art.texture, still, art.uv, kWhite, 0.0f);
        }
        else
        {
            list.gradient_rect(still, 0.0f, gfx::mix(tint, kCoal, 0.08f), kCoal);
            list.circle(430.0f, 210.0f, 300.0f, tint.with_alpha(0.20f));
            list.circle(1320.0f, 140.0f, 330.0f, kWhite.with_alpha(0.035f));
            // A horizon and a couple of light streaks: enough structure that
            // the plate reads as a photograph's colour, not as a flat gradient.
            list.gradient_rect_h({0.0f, 320.0f, kWidth, 190.0f}, 0.0f, kClear,
                                 tint.with_alpha(0.14f));
            list.rounded_rect({0.0f, 402.0f, kWidth, 1.0f}, 0.5f, kWhite.with_alpha(0.06f));
            list.rotated_rect({1500.0f, 120.0f, 34.0f, 420.0f}, 17.0f, -0.18f,
                              kWhite.with_alpha(0.035f));
            list.rotated_rect({1620.0f, 90.0f, 12.0f, 380.0f}, 6.0f, -0.18f,
                              kWhite.with_alpha(0.03f));
            list.ring(1500.0f, 250.0f, 210.0f, 1.5f, kWhite.with_alpha(0.06f));
        }
        // Vignette: the corners fall away, which is what a still does.
        list.gradient_rect_h({0.0f, 0.0f, 260.0f, 720.0f}, 0.0f, kCoal.with_alpha(0.65f), kClear);
        list.gradient_rect_h({kWidth - 320.0f, 0.0f, 320.0f, 720.0f}, 0.0f, kClear,
                             kCoal.with_alpha(0.6f));
        list.gradient_rect({0.0f, 0.0f, kWidth, 150.0f}, 0.0f, kCoal.with_alpha(0.55f), kClear);
        // A long, fully transparent-to-coal fade: the art dissolves into the
        // page instead of ending on a line.
        list.gradient_rect({0.0f, 300.0f, kWidth, 420.0f}, 0.0f, kClear, kCoal);

        // The title's transparent logo lockup, the way the web detail page
        // floats it over the hero. Contained inside a box (never cover-cropped,
        // so the artwork keeps its own ratio) at the top-left of the art, kept
        // clear of the score badge under it. Read from the cached path (set only
        // when the answer's guid matched the shown title) so a stale logo from
        // the previous page never bleeds in; a title with no logo shows just the
        // still.
        if (!detail_logo_.empty())
        {
            const std::string lkey = "logo:" + detail_logo_;
            const std::uint32_t ltex = fnvideo::Service::instance().art_texture(lkey);
            int lw = 0;
            int lh = 0;
            if (ltex != 0 && fnvideo::Service::instance().art_size(lkey, &lw, &lh) && lw > 0 &&
                lh > 0)
            {
                constexpr float kLogoMaxW = 460.0f;
                constexpr float kLogoMaxH = 190.0f;
                const float scale = std::min(kLogoMaxW / static_cast<float>(lw),
                                             kLogoMaxH / static_cast<float>(lh));
                const float w = static_cast<float>(lw) * scale;
                const float h = static_cast<float>(lh) * scale;
                list.image(ltex, Rect{96.0f, 150.0f, w, h}, Rect{0.0f, 0.0f, 1.0f, 1.0f}, kWhite,
                           0.0f);
            }
        }

        // The rail is hidden here, so the page says where it came from.
        const std::string crumb = std::string("首页 · ") + (series ? "剧集" : "电影");
        ui::text(list, glyphs_.regular, crumb, 96.0f, 76.0f, 18.0f, kInk.with_alpha(0.5f));

        // ---- the title block, over the bottom of the still ----
        fit_text(list, glyphs_.semibold, heading_of(item), 96.0f, 456.0f, 66.0f, kInk,
                 kWidth - 192.0f);
        // The line under the title: year · genres · country. The type is the
        // real genres the detail call carries (mapped to Chinese in api.cpp);
        // it is left blank until they land rather than falling back to the
        // library category ("电影"), which is a shelf, not a genre. The runtime
        // lives in the 视频信息 block below, so it is not repeated here.
        std::string origin;
        if (!item.year.empty())
            origin = item.year;
        if (!item.genre.empty())
            origin += origin.empty() ? item.genre : " \u00b7 " + item.genre;
        if (!item.country.empty())
            origin += origin.empty() ? item.country : " \u00b7 " + item.country;
        if (origin.empty())
            origin = item.meta;
        fit_text(list, glyphs_.regular, origin, 96.0f, 498.0f, 20.0f, kMuted, kWidth - 192.0f);

        // Score: a gold star badge high on the still, the same shape the poster
        // wall uses, drawn inside the safe area so it never meets the title.
        if (item.rating > 0.0f)
        {
            char rt[16];
            std::snprintf(rt, sizeof(rt), "%.1f", static_cast<double>(item.rating));
            const std::string rs(rt);
            const Color gold = Color::rgb(0xf5c542);
            const float tw = glyphs_.semibold.measure(rs, 15.0f);
            const float star = 15.0f;
            const float pad = 11.0f;
            const Rect rb{96.0f, 110.0f, pad + star + 6.0f + tw + pad, 28.0f};
            list.rounded_rect(rb, 8.0f, kCoal.with_alpha(0.70f));
            list.bordered_rect(rb, 8.0f, kClear, 1.0f, gold.with_alpha(0.55f));
            list.star(rb.x + pad + star * 0.5f, rb.cy(), 7.0f, gold);
            ui::text(list, glyphs_.semibold, rs, rb.x + pad + star + 6.0f, rb.cy() + 5.0f, 15.0f,
                     gold);
        }

        // The metadata chips, on the row the official page keeps under the
        // title: resolution, age rating, countries, Dolby Vision / Atmos and a
        // Chinese audio / subtitle marker. Every value comes from the server
        // (item/list media_stream, /item/{guid}, /stream/list); a chip is drawn
        // only when its field is actually filled, so a blank never guesses.
        {
            std::vector<std::string> chips;
            if (!item.badge.empty())
            {
                chips.push_back(item.badge);
            }
            if (!item.content_rating.empty())
            {
                chips.push_back(item.content_rating);
            }
            if (!item.country.empty())
            {
                chips.push_back(item.country);
            }
            if (item.color_range_type == "DolbyVision")
            {
                chips.push_back("Dolby Vision");
            }
            if (item.audio_type == "DolbyAtmos")
            {
                chips.push_back("Dolby Atmos");
            }
            if (item.chi_subtitle)
            {
                chips.push_back("中文字幕");
            }
            if (item.chi_audio)
            {
                chips.push_back("中文音频");
            }

            constexpr float kChipY = 520.0f;
            constexpr float kChipH = 30.0f;
            constexpr float kChipGap = 10.0f;
            float chip_x = 96.0f;
            for (const std::string &chip : chips)
            {
                const float w = glyphs_.semibold.measure(chip, 14.0f) + 26.0f;
                if (chip_x + w > kWidth - 96.0f)
                {
                    break; // a single row only; the synopsis lives below
                }
                list.bordered_rect({chip_x, kChipY, w, kChipH}, 6.0f, kWhite.with_alpha(0.05f),
                                   1.0f, kWhite.with_alpha(0.22f));
                fit_text(list, glyphs_.semibold, chip, chip_x + w * 0.5f, kChipY + 20.0f, 14.0f,
                         kInk.with_alpha(0.85f), w - 12.0f, Align::center);
                chip_x += w + kChipGap;
            }
        }

        // ---- the actions ----
        // The upstream kit builds a button from a raised surface: a soft drop
        // shadow the body floats on, a fill and a hairline. fnvideo keeps its
        // own palette and its own focus cursor, so it borrows just that surface
        // recipe (ui::Painter::surface) for 播放 / 收藏 / the two pickers - the
        // capsules and the circle now carry the depth the rest of the kit has.
        ui::Painter paint(list, glyphs_, ui_theme(), 0);
        const Rect play{96.0f, 566.0f, 160.0f, 48.0f};
        // The primary action, glossed like the kit's SurfaceStyle::gloss: a
        // drop shadow, a lit-to-deep vertical gradient, a shine over the top
        // half and a darker rim. A flat accent fill only picked up a shadow
        // the dark poster behind it swallowed, so it read as unchanged.
        list.shadow({play.x, play.y + 5.0f, play.w, play.h}, 24.0f, 15.0f,
                    Color::rgb(0x000000, 0.5f));
        list.gradient_rect(play, 24.0f, kAccentHi, kAccentLo);
        list.gradient_rect({play.x + 3.0f, play.y + 3.0f, play.w - 6.0f, play.h * 0.46f}, 21.0f,
                           kWhite.with_alpha(0.22f), kWhite.with_alpha(0.0f));
        list.bordered_rect(play, 24.0f, kClear, 1.2f, Color::rgb(0x2f6fb5));
        list.triangle({play.x + 26.0f, play.y + 15.0f, 13.0f, 16.0f}, kOnAccent, 0.0f, 1.5708f);
        // 继续播放 only while there is a real place to resume to. A film the
        // viewer just watched out is keyed as finished on this console, so it
        // reads 播放 and a press starts from the top (start_detail_playback).
        const bool resumable =
            item.progress > 0.0f && !fnvideo::Service::instance().is_finished(playable_guid());
        ui::text(list, glyphs_.semibold, resumable ? "继续播放" : "播放", play.x + 52.0f,
                 play.y + 31.0f, 20.0f, kOnAccent);

        // 收藏: the same raised surface on a square that is a circle at radius
        // half its side, so the disc picks up a shadow like the capsules.
        paint.surface({273.0f, 566.0f, 48.0f, 48.0f}, 24.0f, kWhite.with_alpha(0.07f),
                      kWhite.with_alpha(0.22f), 1.0f, 1.2f);
        list.star(297.0f, 590.0f, 10.0f, item.is_favorite != 0 ? kAccent : kInk.with_alpha(0.85f));

        // Audio / subtitle pickers: two capsules to the right of the star. A
        // press opens the chooser (draw_detail_track_menu); play() seeds the
        // decoder from the stored choice, so the pick takes effect the moment
        // 播放 starts.
        const auto draw_picker = [&](const Rect &box, const std::string &label, bool hot)
        {
            paint.surface(box, 24.0f, hot ? kAccent.with_alpha(0.18f) : kWhite.with_alpha(0.05f),
                          hot ? kAccent.with_alpha(0.85f) : kWhite.with_alpha(0.16f), 1.0f, 1.2f);
            fit_text(list, glyphs_.semibold, label, box.cx(), box.cy() + 7.0f, 18.0f,
                     hot ? kInk : kMuted.with_alpha(0.9f), box.w - 28.0f, Align::center);
        };
        if (detail_pickers_enabled())
        {
            draw_picker({338.0f, 566.0f, 150.0f, 48.0f}, detail_audio_label(det.item.audio_streams),
                        detail_focus_ == 2);
            draw_picker({500.0f, 566.0f, 150.0f, 48.0f},
                        detail_subtitle_label(det.item.subtitle_streams), detail_focus_ == 3);
        }

        // ---- the synopsis ----
        // The web detail page folds 简介 to a couple of lines and hands the rest
        // to a 展开 control; the same idea here on Square.
        if (!item.synopsis.empty())
        {
            // Collapsed it keeps two lines, unfolded (□) it runs to its full
            // length - the strip / file card below step aside for the frame so
            // nothing collides. The key itself is named in the bottom hint bar
            // with a real Square icon, where every other control here is listed
            // (the CJK atlas carries no □ glyph, so the text block stays clean).
            // Collapsed it keeps two lines in the title column's measure. The
            // web page lets 展开 run to the full content width and the freed
            // lower half of the page (the strip / file card step aside), so
            // unfolded the 简介 widens to the whole band the title uses and
            // reaches twelve lines - enough to show a normal-length synopsis
            // whole instead of ending on an ellipsis mid-sentence.
            if (detail_synopsis_open_)
                ui::paragraph(list, glyphs_.regular, item.synopsis, 96.0f, 648.0f, 20.0f,
                              kWidth - 192.0f, 32.0f, kMuted.with_alpha(0.9f), 12);
            else
                ui::paragraph(list, glyphs_.regular, item.synopsis, 96.0f, 648.0f, 21.0f,
                              kSynopsisCollapsedWidth, 34.0f, kMuted.with_alpha(0.9f),
                              kSynopsisCollapsedLines);
        }
        if (!detail_error_.empty() && !preview_sample() && !detail_synopsis_open_)
        {
            // The row above still draws from the list call; this says why the
            // episode strip and the wide art did not arrive, instead of
            // leaving a blank that looks like a bug.
            fit_text(list, glyphs_.regular, detail_error_, 96.0f, 780.0f, 16.0f,
                     kAccent.with_alpha(0.85f), 1240.0f);
        }

        // The server's own facts about what will actually play. Values come
        // only from the item or its file list - nothing is invented - and a row
        // the server left blank is simply not drawn.
        std::vector<std::pair<const char *, std::string>> rows;
        rows.emplace_back("播放源", std::string("直连 · 原片"));
        if (!item.resolution.empty())
            rows.emplace_back("清晰度", item.resolution);
        if (item.runtime > 0)
            rows.emplace_back("时长", runtime_text(item.runtime));
        if (item.episodes > 0)
            rows.emplace_back("集数", std::to_string(item.episodes) + " 集");
        if (!item.year.empty())
            rows.emplace_back("年份", item.year);
        {
            const auto &files = det.files;
            if (!files.empty())
            {
                if (!files.front().filename.empty())
                    rows.emplace_back("文件", files.front().filename);
                if (!files.front().file_path.empty())
                    rows.emplace_back("路径", files.front().file_path);
            }
        }

        // ---- episodes, or the film's server info ----
        // While the 简介 is unfolded (□) the full description owns this part of
        // the page, so the strip / file card step aside for the frame; 收起
        // brings them straight back.
        if (detail_synopsis_open_)
        {
            // nothing below while the description is unfolded
        }
        else if (series)
        {
            ui::text(list, glyphs_.semibold, "选集", 96.0f, 800.0f, 20.0f, kInk);
            char label[48];
            if (season_focus_count() > 1)
            {
                std::snprintf(label, sizeof(label), "共 %d 集 · %d 季",
                              static_cast<int>(detail_episodes_.size()), season_count());
            }
            else
            {
                std::snprintf(label, sizeof(label), "共 %d 集",
                              static_cast<int>(detail_episodes_.size()));
            }
            ui::text(list, glyphs_.regular, label, 152.0f, 800.0f, 17.0f, kFaint);

            // Seasons first - the row only exists when there is more than one
            // - then that season's episodes. Both rows slide with the focus,
            // so a long series stays walkable inside the space the file card
            // leaves beside them.
            const int season_first = season_window();
            for (int slot = season_first;
                 slot < season_focus_count() && slot - season_first < kSeasonSlots; ++slot)
            {
                const Rect chip = season_chip(slot);
                const SeasonRun &run = detail_seasons_[static_cast<std::size_t>(slot)];
                const bool selected = slot == current_season();
                list.rounded_rect(chip, 10.0f,
                                  selected ? kWhite.with_alpha(0.15f) : kWhite.with_alpha(0.05f));
                char name[24];
                if (run.season > 0)
                    std::snprintf(name, sizeof(name), "第 %d 季", run.season);
                else
                    std::snprintf(name, sizeof(name), "%s", "其他");
                fit_text(list, glyphs_.regular, name, chip.cx(), chip.y + 26.0f, 16.0f,
                         selected ? kInk : kMuted, chip.w - 16.0f, Align::center);
            }

            const SeasonRun &run = current_run();
            const int episode_first = episode_window();
            for (int local = episode_first;
                 local < run.end - run.begin && local - episode_first < kEpisodeSlots; ++local)
            {
                const Rect chip = episode_chip(local);
                const Item &episode = detail_episodes_[static_cast<std::size_t>(run.begin + local)];
                // Each episode carries its own resume bar, so the strip says
                // which one is half watched rather than a guess across the row.
                const float watched = std::clamp(episode.progress, 0.0f, 1.0f);
                const bool current = run.begin + local == episode_;
                list.rounded_rect(chip, 12.0f,
                                  current ? kWhite.with_alpha(0.15f) : kWhite.with_alpha(0.05f));
                char name[24];
                const int number = episode.episode > 0 ? episode.episode : local + 1;
                std::snprintf(name, sizeof(name), "第 %d 集", number);
                fit_text(list, glyphs_.regular, name, chip.cx(), chip.y + 38.0f, 18.0f,
                         watched >= 0.98f ? kInk.with_alpha(0.55f) : kInk, chip.w - 14.0f,
                         Align::center);
                if (watched > 0.0f)
                {
                    list.rounded_rect(
                        {chip.x + 8.0f, chip.y + 48.0f, (chip.w - 16.0f) * watched, 3.0f}, 1.5f,
                        kAccent.with_alpha(0.7f));
                }
            }
        }
        else
        {
            // Where 相似推荐 used to sit: the whole bottom of a film's page now
            // lists what fnOS reports about the file, laid out two columns wide.
            ui::text(list, glyphs_.semibold, "视频信息", 96.0f, 800.0f, 20.0f, kInk);
            const float lines = static_cast<float>((rows.size() + 1) / 2);
            const Rect info{96.0f, 818.0f, 1140.0f, 44.0f + lines * 38.0f};
            list.rounded_rect(info, 16.0f, kWhite.with_alpha(0.035f));
            list.rounded_rect({info.x, info.y, 3.0f, info.h}, 1.5f, kAccent.with_alpha(0.55f));
            const float col = info.w * 0.5f;
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                const float x = info.x + 26.0f + static_cast<float>(i % 2) * col;
                const float y = info.y + 36.0f + static_cast<float>(i / 2) * 38.0f;
                ui::text(list, glyphs_.regular, rows[i].first, x, y, 15.0f, kFaint);
                fit_text(list, glyphs_.regular, rows[i].second, x + 74.0f, y, 16.0f,
                         kMuted.with_alpha(0.9f), col - 92.0f);
            }
        }

        // A series keeps the compact card to the right of the strip; a film's
        // facts already fill the wide panel drawn above.
        if (series)
        {
            const Rect info{kWidth - 96.0f - 560.0f, 790.0f, 560.0f,
                            68.0f + static_cast<float>(rows.size()) * 32.0f};
            list.rounded_rect(info, 16.0f, kWhite.with_alpha(0.035f));
            list.rounded_rect({info.x, info.y, 3.0f, info.h}, 1.5f, kAccent.with_alpha(0.55f));
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                const float y = info.y + 34.0f + static_cast<float>(i) * 32.0f;
                ui::text(list, glyphs_.regular, rows[i].first, info.x + 26.0f, y, 15.0f, kFaint);
                fit_text(list, glyphs_.regular, rows[i].second, info.x + 110.0f, y, 16.0f,
                         kMuted.with_alpha(0.9f), info.w - 136.0f);
            }
        }
    }

    void draw_player_menu(gfx::DrawList &list) const
    {
        if (player_menu_ == PlayerMenu::none)
        {
            return;
        }
        auto &service = fnvideo::Service::instance();
        if (player_menu_ == PlayerMenu::subtitle_search ||
            player_menu_ == PlayerMenu::subtitle_browse)
        {
            draw_subtitle_search(list, service);
            return;
        }
        const bool slider =
            player_menu_ == PlayerMenu::volume || player_menu_ == PlayerMenu::brightness;
        const bool subtitle = player_menu_ == PlayerMenu::subtitle;
        const auto audio_tracks = service.audio_tracks();
        const auto subtitle_tracks = service.subtitle_tracks();
        const bool no_audio = !slider && !subtitle && audio_tracks.empty();
        const int rows = slider     ? 0
                         : subtitle ? static_cast<int>(subtitle_tracks.size()) + 3
                                    : static_cast<int>(audio_tracks.size());
        // A remux can list thirty subtitle tracks; a panel taller than the
        // screen would put the first rows off the top edge. Show a window
        // around the focus instead, with a counter so the rest is discoverable.
        constexpr int kMaxRows = 8;
        const int visible = std::min(rows, kMaxRows);
        const int selected = std::clamp(player_menu_sel_, 0, std::max(0, rows - 1));
        const int first = std::clamp(selected - visible + 1, 0, std::max(0, rows - visible));
        const float width = 480.0f;
        const float height = slider     ? 154.0f
                             : no_audio ? 104.0f
                                        : 72.0f + static_cast<float>(visible) * 40.0f + 14.0f;
        const float x = 96.0f;
        // Sit above the seek bar (964), not on top of it: the viewer still
        // needs to see where the film is while choosing a track.
        const float y = 956.0f - height;

        // Opaque: the panel sits over the film's title and its episode line,
        // and 4% translucency let both ghost through the track list.
        list.rounded_rect({x, y, width, height}, 20.0f, kCoal);
        list.bordered_rect({x, y, width, height}, 20.0f, kClear, 1.5f, kWhite.with_alpha(0.18f));
        const char *title = player_menu_ == PlayerMenu::audio      ? "选择音轨"
                            : player_menu_ == PlayerMenu::subtitle ? "选择字幕"
                            : player_menu_ == PlayerMenu::volume   ? "音量"
                                                                   : "亮度";
        ui::text(list, glyphs_.semibold, title, x + 24.0f, y + 38.0f, 21.0f, kInk);
        if (!slider && rows > visible)
        {
            const std::string counter = std::to_string(selected + 1) + " / " + std::to_string(rows);
            ui::text(list, glyphs_.regular, counter, x + width - 24.0f, y + 38.0f, 16.0f, kFaint,
                     Align::right);
        }

        if (no_audio)
        {
            ui::text(list, glyphs_.regular, "本片没有可选音轨", x + 24.0f, y + 76.0f, 17.0f,
                     kMuted);
            return;
        }

        if (slider)
        {
            const float value =
                player_menu_ == PlayerMenu::volume ? service.volume() : service.brightness();
            char percent[16];
            std::snprintf(percent, sizeof(percent), "%d%%",
                          static_cast<int>(value * 100.0f + 0.5f));
            ui::text(list, glyphs_.regular, percent, x + width - 24.0f, y + 38.0f, 19.0f, kMuted,
                     Align::right);
            const float tx = x + 24.0f;
            const float tw = width - 48.0f;
            const float ty = y + 82.0f;
            const float fraction = player_menu_ == PlayerMenu::volume
                                       ? std::clamp(value, 0.0f, 1.0f)
                                       : std::clamp((value - 0.5f) / 1.0f, 0.0f, 1.0f);
            list.rounded_rect({tx, ty, tw, 6.0f}, 3.0f, kWhite.with_alpha(0.16f));
            list.rounded_rect({tx, ty, tw * fraction, 6.0f}, 3.0f, kAccent);
            list.circle(tx + tw * fraction, ty + 3.0f, 9.0f, kInk);
            // The font has no circle glyph; the footer already names 返回, so
            // this line only needs to explain the left/right adjustment.
            ui::text(list, glyphs_.regular, "十字键左右调整", tx, y + 132.0f, 16.0f, kFaint);
            return;
        }

        float row_y = y + 66.0f;
        for (int slot = 0; slot < visible; ++slot)
        {
            const int i = first + slot;
            const bool focused = i == selected;
            if (focused)
            {
                list.rounded_rect({x + 12.0f, row_y - 22.0f, width - 24.0f, 36.0f}, 12.0f,
                                  kAccent.with_alpha(0.20f));
            }
            std::string label;
            if (subtitle && i == 0)
            {
                label = "关闭字幕";
            }
            else if (subtitle && i == static_cast<int>(subtitle_tracks.size()) + 1)
            {
                // The trailing action rows: the online subtitle search and
                // the NAS-folder browse (mark a file that already sits here).
                label = "搜索在线字幕";
            }
            else if (subtitle && i == static_cast<int>(subtitle_tracks.size()) + 2)
            {
                label = "浏览NAS字幕文件";
            }
            else
            {
                const fnvideo::AudioTrack *audio = nullptr;
                const fnvideo::SubtitleTrack *sub = nullptr;
                if (subtitle)
                {
                    sub = &subtitle_tracks[static_cast<std::size_t>(i - 1)];
                }
                else
                {
                    audio = &audio_tracks[static_cast<std::size_t>(i)];
                }
                if (audio != nullptr)
                {
                    label = detail_stream_name(audio->language, audio->title, i);
                    if (audio->channels > 0)
                    {
                        label += " · " + std::to_string(audio->channels) + "ch";
                    }
                }
                else if (sub != nullptr)
                {
                    // Same label the detail page builds: the language translated
                    // to Chinese, one marker word for a graphics / sidecar track.
                    label = subtitle_display_name(sub->language, sub->title, !sub->text,
                                                  sub->external, i - 1);
                }
            }
            fit_text(list, glyphs_.regular, label, x + 28.0f, row_y + 5.0f, 18.0f,
                     focused ? kInk : kMuted, width - 52.0f);
            row_y += 40.0f;
        }
        if (subtitle && subtitle_tracks.empty())
        {
            ui::text(list, glyphs_.regular, "本片没有字幕轨道", x + 24.0f, y + 112.0f, 15.0f,
                     kFaint);
        }
    }

    // The subtitle panels reached from 选择字幕: the online search and the
    // NAS-folder browse. Both reuse the pull-down's box so a confirmed row is
    // a press away from the track list; the worker fills the shared
    // SubtitlePanel snapshot (busy / error / hits) and this paints it each
    // frame. Confirming downloads an online hit, or marks a folder file.
    void draw_subtitle_search(gfx::DrawList &list, fnvideo::Service &service) const
    {
        const fnvideo::Service::SubtitlePanel panel = service.subtitle_panel();
        // `local` is what the worker set; the menu state says the same, so
        // either one keeps the empty panel labelled before a reply lands.
        const bool local = panel.local || player_menu_ == PlayerMenu::subtitle_browse;
        const int rows = static_cast<int>(panel.hits.size());
        // Two lines per hit (name + meta), so fewer fit than the flat track
        // list; window the rest behind the focus like the subtitle menu does.
        constexpr int kMaxRows = 5;
        const int visible = std::min(rows, kMaxRows);
        const int selected = std::clamp(player_menu_sel_, 0, std::max(0, rows - 1));
        const int first = std::clamp(selected - visible + 1, 0, std::max(0, rows - visible));
        const float width = 520.0f;
        // Searching, an error, or an empty result all hold a single line; a
        // result set grows the box, capped to the window.
        const int body = rows > 0 ? visible : 1;
        const float height = 82.0f + static_cast<float>(body) * (rows > 0 ? 52.0f : 34.0f) + 30.0f;
        const float x = 96.0f;
        const float y = 956.0f - height;

        list.rounded_rect({x, y, width, height}, 20.0f, kCoal);
        list.bordered_rect({x, y, width, height}, 20.0f, kClear, 1.5f, kWhite.with_alpha(0.18f));
        std::string head = local ? "本地字幕 · " : "在线字幕 · 搜索";
        if (local)
        {
            // Show the leaf of the folder currently listed so the viewer can
            // tell which directory the rows belong to.
            const std::string &fld = panel.folder;
            const std::size_t slash = fld.find_last_of('/');
            std::string tail = slash == std::string::npos ? fld : fld.substr(slash + 1);
            if (tail.empty())
                tail = "/";
            head += tail;
        }
        ui::text(list, glyphs_.semibold, head, x + 24.0f, y + 38.0f, 21.0f, kInk);
        if (rows > visible && !panel.busy)
        {
            const std::string counter = std::to_string(selected + 1) + " / " + std::to_string(rows);
            ui::text(list, glyphs_.regular, counter, x + width - 24.0f, y + 38.0f, 16.0f, kFaint,
                     Align::right);
        }

        if (!panel.error.empty())
        {
            fit_text(list, glyphs_.regular, panel.error, x + 24.0f, y + 78.0f, 17.0f, kAccent,
                     width - 48.0f);
        }
        else if (panel.busy)
        {
            const std::string status = panel.status.empty()
                                           ? (local ? "正在浏览 NAS 文件夹…" : "正在搜索字幕…")
                                           : panel.status;
            fit_text(list, glyphs_.regular, status, x + 24.0f, y + 78.0f, 17.0f, kMuted,
                     width - 48.0f);
        }
        else if (rows == 0)
        {
            const std::string status = panel.status.empty()
                                           ? (local ? "文件夹里没有字幕文件" : "没有搜索结果")
                                           : panel.status;
            fit_text(list, glyphs_.regular, status, x + 24.0f, y + 78.0f, 17.0f, kMuted,
                     width - 48.0f);
        }
        else
        {
            float row_y = y + 80.0f;
            for (int slot = 0; slot < visible; ++slot)
            {
                const int i = first + slot;
                const bool focused = i == selected;
                const fnvideo::SubtitleHit &hit = panel.hits[static_cast<std::size_t>(i)];
                if (focused)
                {
                    list.rounded_rect({x + 12.0f, row_y - 18.0f, width - 24.0f, 48.0f}, 12.0f,
                                      kAccent.with_alpha(0.20f));
                }
                std::string label = hit.filename;
                if (hit.is_dir && hit.filename == "..")
                    label = "返回上级目录";
                else if (hit.filename.empty())
                    label = "未命名字幕";
                fit_text(list, glyphs_.regular, label, x + 28.0f, row_y, 18.0f,
                         focused ? kInk : kMuted, width - 56.0f);
                std::string meta;
                if (!hit.format.empty())
                    meta += hit.format;
                if (!hit.source.empty())
                    meta += (meta.empty() ? "" : " · ") + hit.source;
                if (hit.download > 0)
                    meta += std::string(meta.empty() ? "" : " · ") + "下载 " +
                            std::to_string(hit.download);
                if (!meta.empty())
                {
                    fit_text(list, glyphs_.regular, meta, x + 28.0f, row_y + 22.0f, 14.0f, kFaint,
                             width - 56.0f);
                }
                row_y += 52.0f;
            }
        }

        // Words only - the face has no cross/circle glyph, matching the slider
        // panel's footer. The pad still belongs to update_player_menu.
        ui::text(list, glyphs_.regular,
                 local ? "上下选择 · 确定进入/标记 · 返回关闭" : "上下选择 · 确定下载 · 返回关闭",
                 x + 24.0f, y + height - 26.0f, 14.0f, kFaint);
    }

    void draw_subtitles(gfx::DrawList &list) const
    {
        if (player_menu_ != PlayerMenu::none)
        {
            // The pull-down owns the lower third; a subtitle behind it would
            // be half-covered and harder to read than one that waits.
            return;
        }
        auto &service = fnvideo::Service::instance();
        // The service reads the millisecond playhead; `position_` here is only
        // the 0..1 bar fraction.
        const std::string text = service.subtitle_at();
        if (text.empty())
        {
            return;
        }
        std::vector<std::string> lines;
        std::size_t begin = 0;
        while (begin <= text.size() && lines.size() < 3)
        {
            const std::size_t end = text.find('\n', begin);
            std::string line =
                text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
            if (!line.empty())
            {
                lines.push_back(std::move(line));
            }
            if (end == std::string::npos)
            {
                break;
            }
            begin = end + 1;
        }
        if (lines.empty())
        {
            return;
        }
        constexpr float size = 36.0f;
        constexpr float line_h = 46.0f;
        const float cx = kWidth * 0.5f;
        const float last_baseline = player_bar_ ? 950.0f : 984.0f;
        const float first_baseline = last_baseline - static_cast<float>(lines.size() - 1) * line_h;
        // No backing panel now, so the white text gets a dark outline instead:
        // the words are stamped into each of the eight neighbours and then drawn
        // on top. A single offset shadow reads as a blurry smear over a bright
        // sky; a stroke keeps the glyph edge crisp wherever the film goes white.
        float baseline = first_baseline;
        for (const std::string &line : lines)
        {
            for (int oy = -1; oy <= 1; ++oy)
            {
                for (int ox = -1; ox <= 1; ++ox)
                {
                    if (ox == 0 && oy == 0)
                    {
                        continue;
                    }
                    fit_text(list, glyphs_.regular, line, cx + 2.0f * static_cast<float>(ox),
                             baseline + 2.0f * static_cast<float>(oy), size,
                             kCoal.with_alpha(0.85f), kWidth - 160.0f, Align::center);
                }
            }
            fit_text(list, glyphs_.regular, line, cx, baseline, size, kInk, kWidth - 160.0f,
                     Align::center);
            baseline += line_h;
        }
    }

    void draw_player(gfx::DrawList &list) const
    {
        const Item &item = detail_item();
        auto &service = fnvideo::Service::instance();

        // The frame: the decoder's planes once there is one, and the item's
        // colour until then, which reads as a still rather than a black screen.
        const std::uint32_t picture = service.video_texture();
        if (picture != 0)
        {
            int width = 0;
            int height = 0;
            service.video_size(&width, &height);
            // The whole picture, with bars where the aspect leaves room. A
            // 2.4:1 film in a 16:9 screen loses 13% off each side if it is
            // cropped to fill, and that is not a resolution problem to look
            // at - it is the film, cut.
            gfx::Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
            const gfx::Rect box{0.0f, 0.0f, kWidth, kHeight};
            const gfx::Rect shown = contain_rect(width, height, box, &uv);
            list.rounded_rect(box, 0.0f, Color::rgb(0x000000));
            list.video(shown, uv);
            // PGS/DVD sub-picture. The bitmap is authored against the subtitle
            // stream's own canvas - the whole presentation frame, letterbox bars
            // included - which is not the tight rectangle contain_rect carved out
            // for the coded picture. Normalising a 16:9 canvas (Blu-ray subs are
            // authored on 1920x1080) into a scope film's 2.39:1 rect stretches
            // every glyph sideways and squashes it - the subtitles read 矮胖 on
            //  wides. Fit the canvas into the screen with its own aspect instead:
            // the bitmap's pixels stay square and the cue lands where the disc put
            // it, bars and all (Kodi #14866 was the opposite bug - filling the
            // whole screen rather than the picture).
            std::uint32_t sub_tex = 0;
            int sx = 0;
            int sy = 0;
            int sw = 0;
            int sh = 0;
            int cw = 0;
            int ch = 0;
            if (player_menu_ == PlayerMenu::none &&
                service.subtitle_overlay(&sub_tex, &sx, &sy, &sw, &sh, &cw, &ch) && cw > 0 &&
                ch > 0 && sw > 0 && sh > 0)
            {
                gfx::Rect sub_uv{0.0f, 0.0f, 1.0f, 1.0f};
                const gfx::Rect frame = contain_rect(cw, ch, box, &sub_uv);
                const gfx::Rect placed{frame.x + (static_cast<float>(sx) / cw) * frame.w,
                                       frame.y + (static_cast<float>(sy) / ch) * frame.h,
                                       (static_cast<float>(sw) / cw) * frame.w,
                                       (static_cast<float>(sh) / ch) * frame.h};
                list.image(sub_tex, placed, gfx::kFullUv, Color::rgb(0xffffff));
            }
        }
        else
        {
            list.gradient_rect({0.0f, 0.0f, kWidth, kHeight}, 0.0f,
                               gfx::mix(Color::rgb(item.tint), kCoal, 0.18f), kCoal);
            list.circle(kWidth * 0.68f, kHeight * 0.34f, 260.0f, Color::rgb(item.tint, 0.22f));
            list.circle(kWidth * 0.68f, kHeight * 0.34f, 180.0f, kWhite.with_alpha(0.05f));
            list.ring(kWidth * 0.30f, kHeight * 0.58f, 220.0f, 1.0f, kWhite.with_alpha(0.05f));
        }

        const fnvideo::Service::Playback play = service.playback();
        // The picture and the subtitles stay; everything else is chrome. A
        // paused, opening or failed player always keeps its controls, because
        // the state needs to be legible.
        const bool chrome = player_osd_ || player_bar_ || player_menu_ != PlayerMenu::none ||
                            paused_ || play.opening || play.failed;
        if (!chrome)
        {
            draw_subtitles(list);
            return;
        }

        // Scrims, so the OSD stays readable over any picture.
        list.gradient_rect({0.0f, 0.0f, kWidth, 200.0f}, 0.0f, kCoal.with_alpha(0.72f), kClear);
        list.gradient_rect({0.0f, 560.0f, kWidth, 520.0f}, 0.0f, kClear, kCoal.with_alpha(0.94f));

        // Top line: where we are, and what the file is. The badge next to it
        // carries the resolution only: the codec and the audio language have
        // no source on this page, and an invented one would read as fact.
        const bool series = !detail_episodes_.empty();
        char where[160];
        const std::string &heading = heading_of(item);
        if (series)
            std::snprintf(where, sizeof(where), "%s · 第 %d 集", heading.c_str(),
                          episode_number_at(episode_));
        else
            std::snprintf(where, sizeof(where), "%s", heading.c_str());
        fit_text(list, glyphs_.semibold, where, 96.0f, 92.0f, 26.0f, kInk.with_alpha(0.92f),
                 kWidth - 340.0f - 96.0f - 24.0f);
        std::string grade;
        if (picture != 0)
        {
            int frame_w = 0;
            int frame_h = 0;
            service.video_size(&frame_w, &frame_h);
            if (frame_w > 0 && frame_h > 0)
            {
                grade = std::to_string(frame_w);
                grade += " × ";
                grade += std::to_string(frame_h);
                // What the GPU actually got, and which decoder produced it.
                // The pair is the first thing to look at when a film is soft
                // or the wrong colour.
                grade += " · ";
                grade += play_decoder_label();
            }
        }
        if (grade.empty())
        {
            grade = item.badge;
        }
        if (!grade.empty())
        {
            const float pill_w = glyphs_.regular.measure(grade, 15.0f) + 40.0f;
            list.rounded_rect({kWidth - 96.0f - pill_w, 68.0f, pill_w, 34.0f}, 8.0f,
                              kWhite.with_alpha(0.08f));
            ui::text(list, glyphs_.regular, grade, kWidth - 96.0f - pill_w * 0.5f, 91.0f, 15.0f,
                     kInk.with_alpha(0.8f), Align::center);
        }

        if (paused_)
        {
            list.circle(kWidth * 0.5f, kHeight * 0.42f, 66.0f, kCoal.with_alpha(0.55f));
            list.ring(kWidth * 0.5f, kHeight * 0.42f, 66.0f, 1.5f, kWhite.with_alpha(0.28f));
            list.triangle({kWidth * 0.5f - 18.0f, kHeight * 0.42f - 24.0f, 40.0f, 48.0f}, kInk,
                          0.0f, 1.5708f);
        }

        // Title over the OSD.
        fit_text(list, glyphs_.semibold, heading, 96.0f, 900.0f, 30.0f, kInk, kWidth - 192.0f);
        fit_text(list, glyphs_.regular, item.meta, 96.0f, 930.0f, 17.0f, kMuted.with_alpha(0.8f),
                 kWidth - 192.0f);

        // The progress line. It follows the decoder whenever there is one; the
        // sample clock only drives the pages that have no stream behind them.
        const bool live =
            play.playing || play.paused || play.opening || (play.failed && !preview_sample());
        if (play.opening)
        {
            // Opening the ticket is a network wait on a NAS. Without a mark
            // the black frame reads as a hang, and the OSD says nothing.
            const float cy = kHeight * 0.42f;
            list.circle(kWidth * 0.5f, cy, 40.0f, kCoal.with_alpha(0.55f));
            list.ring(kWidth * 0.5f, cy, 40.0f, 3.0f, kWhite.with_alpha(0.18f));
            const float phase = std::fmod(clock_ * 2.2f, 6.2831853f);
            list.arc(kWidth * 0.5f, cy, 40.0f, 3.0f, phase, 1.4f, kAccent.with_alpha(0.9f));
            ui::text(list, glyphs_.regular, "正在打开…", kWidth * 0.5f, cy + 74.0f, 18.0f,
                     kInk.with_alpha(0.85f), Align::center);
        }
        const int length = live && play.duration > 0 ? play.duration : 128 * 60;
        const float at =
            live && play.duration > 0
                ? std::clamp(static_cast<float>(play.position) / static_cast<float>(play.duration),
                             0.0f, 1.0f)
                : position_;
        const float bar_x = 96.0f;
        const float bar_w = kWidth - 192.0f;
        const float bar_y = 964.0f;
        list.rounded_rect({bar_x, bar_y, bar_w, 5.0f}, 2.5f, kWhite.with_alpha(0.22f));
        list.rounded_rect({bar_x, bar_y, bar_w * at, 5.0f}, 2.5f, kAccent);
        list.circle(bar_x + bar_w * at, bar_y + 2.5f, 8.0f, kWhite);
        ui::text(list, glyphs_.regular, clock_text(length * at), bar_x, bar_y + 34.0f, 16.0f,
                 kMuted);
        ui::text(list, glyphs_.regular, clock_text(length), bar_x + bar_w, bar_y + 34.0f, 16.0f,
                 kMuted, Align::right);
        if (play.failed && !play.error.empty() && !preview_sample())
        {
            fit_text(list, glyphs_.regular, play.error, bar_x + bar_w, bar_y + 62.0f, 16.0f,
                     kAccent, bar_w, Align::right);
        }

        draw_subtitles(list);

        // The toolbar. One chip per thing that can be changed without leaving
        // the picture: the two episode steps only exist for a series. Audio
        // and subtitle chips open the pull-down above, and volume/brightness
        // open a slider; all four act on the running film rather than sending
        // the viewer back to the detail page.
        //
        // The chips share the bottom line with the help hints, and a series'
        // nine chips are wider than the space left for both. Only the focused
        // bar draws chips; the resting OSD leaves that line to the hints. Up
        // opens the bar, and that is the only time the chips are actionable.
        if (player_bar_ && player_menu_ == PlayerMenu::none)
        {
            const int chip_count = player_chip_count();
            std::string names[9];
            for (int i = 0; i < chip_count; ++i)
            {
                names[i] = player_chip_label(i);
            }
            float x = 96.0f;
            for (int i = 0; i < chip_count; ++i)
            {
                const float w = glyphs_.regular.measure(names[i], 17.0f) + 40.0f;
                const bool focused = i == player_chip_;
                const bool primary = i == (series ? 1 : 0);
                list.bordered_rect({x, 1006.0f, w, 42.0f}, 21.0f,
                                   kWhite.with_alpha(focused   ? 0.22f
                                                     : primary ? 0.13f
                                                               : 0.06f),
                                   focused ? 2.0f : 0.0f, focused ? kAccent : kClear);
                ui::text(list, glyphs_.regular, names[i], x + w * 0.5f, 1034.0f, 17.0f,
                         focused || primary ? kInk : kMuted, Align::center);
                x += w + 12.0f;
            }
        }

        draw_player_menu(list);

        // A focused bar explains itself through its chips, so it does not
        // also need the hint line underneath them (where the two used to
        // collide). Menus and the resting OSD get the line.
        if (!player_bar_ || player_menu_ != PlayerMenu::none)
        {
            std::array<ui::Hint, 7> hints{};
            int hint_count = 0;
            if (player_menu_ != PlayerMenu::none)
            {
                // The bar's help would be wrong while a pull-down owns the pad:
                // left and right adjust a slider, up and down walk a track list.
                hints[hint_count++] = {ui::Button::cross, "确认"};
                hints[hint_count++] = {ui::Button::dpad,
                                       player_menu_ == PlayerMenu::volume ||
                                               player_menu_ == PlayerMenu::brightness
                                           ? "左右调整"
                                           : "上下选择"};
                hints[hint_count++] = {ui::Button::circle, "返回"};
            }
            else
            {
                hints[hint_count++] = {ui::Button::cross, "播放 / 暂停"};
                hints[hint_count++] = {ui::Button::dpad, "快进快退 · 上开工具栏"};
                hints[hint_count++] = {ui::Button::l1, "快退 / 快进", ui::Button::r1};
                hints[hint_count++] = {ui::Button::left_stick, "上下音量 · 左右快进1分钟"};
                hints[hint_count++] = {ui::Button::right_stick, "上下亮度"};
                hints[hint_count++] = {ui::Button::circle, "返回详情"};
            }
            if (series)
            {
                hints[hint_count++] = {ui::Button::l2, "上一集 / 下一集", ui::Button::r2};
            }
            ui::HintLayout layout;
            layout.size = 30.0f;
            layout.text_size = 18.0f;
            layout.cy = 1034.0f;
            ui::draw_hints(list, glyphs_, ui::GlyphStyle::dark(), hints.data(), hint_count,
                           kWidth - kMargin, true, layout);
        }

        // The transient volume / brightness readout, drawn last so it sits over
        // the chrome and fades on its own once the stick rests.
        if (stick_hud_timer_ > 0.0f && !stick_hud_text_.empty())
        {
            const float alpha = std::clamp(stick_hud_timer_ / 0.35f, 0.0f, 1.0f);
            const float w = 380.0f;
            const float h = 78.0f;
            const float x = (kWidth - w) * 0.5f;
            const float y = 150.0f;
            list.rounded_rect({x, y, w, h}, 18.0f, kCoal.with_alpha(0.82f * alpha));
            list.bordered_rect({x, y, w, h}, 18.0f, kClear, 1.5f, kWhite.with_alpha(0.16f * alpha));
            ui::text(list, glyphs_.semibold, stick_hud_text_, x + 24.0f, y + 32.0f, 21.0f,
                     kInk.with_alpha(0.92f * alpha));
            const float tx = x + 24.0f;
            const float tw = w - 48.0f;
            const float ty = y + 54.0f;
            list.rounded_rect({tx, ty, tw, 6.0f}, 3.0f, kWhite.with_alpha(0.16f * alpha));
            list.rounded_rect({tx, ty, tw * std::clamp(stick_hud_fraction_, 0.0f, 1.0f), 6.0f},
                              3.0f, kAccent.with_alpha(alpha));
        }
    }

    void draw_settings(gfx::DrawList &list) const
    {
        ui::text(list, glyphs_.semibold, "设置", kContentX, 178.0f, 46.0f, kInk);
        ui::text(list, glyphs_.regular, "影视服务的连接与本机信息", kContentX, 212.0f, 18.0f,
                 kMuted.with_alpha(0.7f));

        const Rect panel{kContentX, kSettingsTop, kContentW, kSettingsHeight};
        list.rounded_rect(panel, 26.0f, kPanel.with_alpha(0.55f));

        const char *const rows[] = {"服务器", "账号", "密码", "登录", "退出登录", "缓存", "关于"};
        static_assert(sizeof(rows) / sizeof(rows[0]) == kSettingsRows,
                      "the settings rows and kSettingsRows disagree");
        std::string about = "飞牛影视 ";
        about += version_.empty() ? std::string("版本未知") : version_;
        about += " · 作者 Naye · QQ群 310630593 · 界面框架 ps5-homebrew-ui";
        // The repo sits on its own second line: squeezed into the same row as
        // the name and version it survives only to the ellipsis, and a
        // truncated URL is worse than no URL.
        const std::string about_url = "项目主页 https://github.com/naye1120/fnvideo-ps5";
        // Show what the worker actually has, but prefer the drafts the user is
        // mid-edit on: the form is where a wrong address gets noticed, and it
        // must reflect what will be sent on 登录.
        const std::string server =
            server_text_.empty() ? fnvideo::Service::instance().server() : server_text_;
        const std::string account =
            account_text_.empty() ? fnvideo::Service::instance().account() : account_text_;
        std::string password_display;
        if (settings_edit_ == 2 && !edit_buffer_.empty())
        {
            password_display.assign(edit_buffer_.size(), '*');
        }
        else if (settings_edit_ == 2)
        {
            password_display = "（空）";
        }
        else if (!password_text_.empty() || signed_in_)
        {
            password_display = "••••••••";
        }
        else
        {
            password_display = "—";
        }
        // Resolution and decoder are not here any more: they belong next to
        // the picture, so the player's toolbar owns them.
        const std::string values[] = {server.empty() ? "未配置" : server,
                                      account.empty() ? "未登录" : account,
                                      password_display,
                                      signed_in_ ? "状态：已登录" : "状态：未登录",
                                      "清除这台设备上的令牌",
                                      "封面缓存 128 MB",
                                      about};
        static_assert(sizeof(values) / sizeof(values[0]) == kSettingsRows,
                      "the settings values and kSettingsRows disagree");
        for (int i = 0; i < kSettingsRows; ++i)
        {
            const float y = kSettingsRowTop + static_cast<float>(i) * kSettingsRowPitch;
            const bool hot = settings_row_ == i && settings_edit_ < 0;
            if (hot)
            {
                list.rounded_rect({panel.x + 16.0f, y - 4.0f, panel.w - 32.0f, 78.0f}, 14.0f,
                                  kWhite.with_alpha(0.05f));
                list.rounded_rect({panel.x + 16.0f, y + 8.0f, 4.0f, 54.0f}, 2.0f, kAccent);
            }
            ui::text(list, glyphs_.semibold, rows[i], panel.x + 48.0f, y + 34.0f, 22.0f,
                     hot ? kInk : kMuted);
            if (i == kSettingsRows - 1)
            {
                // 关于 owns two baselines in its row (name/version, then the
                // repo); every other value is one line at the label's height.
                fit_text(list, glyphs_.regular, values[i], panel.x + 300.0f, y + 26.0f, 19.0f,
                         hot ? kInk.with_alpha(0.9f) : kMuted.with_alpha(0.62f),
                         panel.w - 300.0f - 98.0f);
                fit_text(list, glyphs_.regular, about_url, panel.x + 300.0f, y + 52.0f, 16.0f,
                         hot ? kInk.with_alpha(0.9f) : kMuted.with_alpha(0.62f),
                         panel.w - 300.0f - 98.0f);
            }
            else
            {
                fit_text(list, glyphs_.regular, values[i], panel.x + 300.0f, y + 34.0f, 19.0f,
                         hot ? kInk.with_alpha(0.9f) : kMuted.with_alpha(0.62f),
                         panel.w - 300.0f - 98.0f);
            }
            // Only the three text rows open the keyboard; a chevron on 登录 /
            // 缓存 / 关于 would promise an edit that does not exist.
            if (i <= 2)
            {
                list.rounded_rect({panel.x + panel.w - 74.0f, y + 22.0f, 26.0f, 26.0f}, 13.0f,
                                  kWhite.with_alpha(hot ? 0.10f : 0.04f));
                // A drawn chevron: the CJK atlas has no guillemet, and a
                // question mark in a settings row reads as a bug.
                list.triangle({panel.x + panel.w - 66.0f, y + 29.0f, 12.0f, 13.0f},
                              hot ? kInk : kFaint, 0.0f, 1.5708f);
            }
            if (i < kSettingsRows - 1)
                list.rounded_rect({panel.x + 48.0f, y + 76.0f, panel.w - 96.0f, 1.0f}, 0.5f,
                                  kWhite.with_alpha(0.05f));
        }

        // While a field is open the kit keyboard overlays the form and owns
        // focus. The field label and the live draft sit above the keys so the
        // player sees what they are typing; the keys themselves are the kit
        // control, drawn to the DrawList like every other surface.
        if (settings_edit_ >= 0)
        {
            const Rect dim{kContentX, kSettingsTop, kContentW, kSettingsHeight};
            list.rounded_rect(dim, 26.0f, kCoal.with_alpha(0.86f));

            const char *field = settings_edit_ == 0   ? "服务器地址"
                                : settings_edit_ == 1 ? "账号"
                                                      : "密码";
            ui::text(list, glyphs_.semibold, field, kContentX + 40.0f, 296.0f, 22.0f, kAccent);
            std::string shown = edit_buffer_;
            if (settings_edit_ == 2)
                shown.assign(edit_buffer_.size(), '*');
            else if (shown.empty())
                shown = "（空）";
            fit_text(list, glyphs_.regular, shown, kContentX + 40.0f, 340.0f, 30.0f,
                     edit_buffer_.empty() ? kMuted : kInk, kContentW - 80.0f);

            ui::Canvas canvas{list, glyphs_, 0u, clock_};
            keyboard_.draw(canvas);
        }
    }

    void draw_boot(gfx::DrawList &list) const
    {
        const float age = boot_age_;
        // The mark settles first, then the wordmark rises under it. Both are
        // driven by the same clock, so a skipped frame never leaves the splash
        // half-composed.
        const float lift = std::min(1.0f, age / 0.7f);
        const float ease = 1.0f - (1.0f - lift) * (1.0f - lift);
        const float alpha = ease;
        const float cx = kWidth * 0.5f;
        const float cy = kHeight * 0.42f;
        const float ring = 92.0f + 18.0f * (1.0f - ease);

        list.push_opacity(alpha);
        // A slow sweep outside the mark: motion without a spinner that would
        // read as "loading" even after the library is ready.
        list.arc(cx, cy, ring + 30.0f, 2.0f, clock_ * 1.4f, 1.35f, kAccent.with_alpha(0.8f));
        list.ring(cx, cy, ring, 1.0f, kAccent.with_alpha(0.30f));
        list.ring(cx, cy, ring + 18.0f, 1.0f, kAccent.with_alpha(0.10f));
        // A soft halo behind the tile, then the tile itself.
        list.glow({cx - 72.0f, cy - 72.0f, 144.0f, 144.0f}, 32.0f, 28.0f,
                  kAccent.with_alpha(0.22f));
        list.rounded_rect({cx - 52.0f, cy - 52.0f, 104.0f, 104.0f}, 30.0f, kAccent);
        list.triangle({cx - 14.0f, cy - 26.0f, 34.0f, 40.0f}, kOnAccent, 0.0f, 1.5708f);

        const float text_rise = 18.0f * (1.0f - ease);
        ui::text(list, glyphs_.semibold, "飞牛影视", cx, cy + 176.0f + text_rise, 44.0f, kInk,
                 Align::center);
        ui::text(list, glyphs_.regular, "正在连接 NAS · 影视库", cx, cy + 216.0f + text_rise, 19.0f,
                 kMuted.with_alpha(0.8f), Align::center);

        // A hairline progress cue, not a percentage: the wait is short and a
        // number would suggest a precision this launch does not have.
        const float progress = std::min(1.0f, age / 2.8f);
        const float track_w = 220.0f;
        list.rounded_rect({cx - track_w * 0.5f, cy + 246.0f, track_w, 3.0f}, 1.5f,
                          kWhite.with_alpha(0.12f));
        list.rounded_rect({cx - track_w * 0.5f, cy + 246.0f, track_w * progress, 3.0f}, 1.5f,
                          kAccent);
        list.pop_opacity();
    }

    void draw_cursor(gfx::DrawList &list) const
    {
        // The hero is a whole panel, not a tile: a ring around it would read as
        // a mistake, and its own frame already says where the focus is.
        if (!cursor_ready_ || rail_focus_ || zone_ == Zone::settings ||
            (zone_ == Zone::home && row_ == 0) ||
            (zone_ == Zone::library && (library_count() == 0 || search_edit_)))
            return;
        const Rect ring{cursor_.x - 5.0f, cursor_.y - 5.0f, cursor_.w + 10.0f, cursor_.h + 10.0f};
        list.glow(ring, 20.0f, 16.0f, kAccent.with_alpha(0.16f));
        list.bordered_rect(ring, 20.0f, kClear, 2.5f, kWhite.with_alpha(0.92f));
    }

    void draw_hints(gfx::DrawList &list) const
    {
        std::array<ui::Hint, 6> hints{};
        int count = 0;
        if (zone_ == Zone::settings && settings_edit_ >= 0)
        {
            // Mirrors the kit keyboard's standard bindings: Square deletes,
            // Triangle is space, L2 shifts, R2 switches to symbols.
            hints[0] = {ui::Button::cross, "输入 / 完成"};
            hints[1] = {ui::Button::dpad, "移动光标"};
            hints[2] = {ui::Button::circle, "取消编辑"};
            hints[3] = {ui::Button::square, "删除"};
            hints[4] = {ui::Button::triangle, "空格"};
            hints[5] = {ui::Button::l2, "大写"};
            count = 6;
            ui::draw_hints(list, glyphs_, ui::GlyphStyle::dark(), hints.data(), count,
                           kWidth - kMargin, true, ui::HintLayout{32.0f, 19.0f, kHintY});
            return;
        }
        if (zone_ == Zone::library && search_edit_)
        {
            // Same bindings as the settings keyboard, with Circle named for
            // what it closes here.
            hints[0] = {ui::Button::cross, "输入 / 完成"};
            hints[1] = {ui::Button::dpad, "移动光标"};
            hints[2] = {ui::Button::circle, "关闭搜索"};
            hints[3] = {ui::Button::square, "删除"};
            hints[4] = {ui::Button::triangle, "空格"};
            hints[5] = {ui::Button::l2, "大写"};
            count = 6;
            ui::draw_hints(list, glyphs_, ui::GlyphStyle::dark(), hints.data(), count,
                           kWidth - kMargin, true, ui::HintLayout{32.0f, 19.0f, kHintY});
            return;
        }
        if (rail_focus_)
        {
            hints[0] = {ui::Button::cross, "打开这一栏"};
            hints[1] = {ui::Button::dpad, "上下选分类"};
            hints[2] = {ui::Button::circle, "回到内容"};
            count = 3;
            // The rail commits on move, so a library board is already behind the
            // cursor; offer the Options filter shortcut here too, or the hint
            // vanishes exactly when a player walks the categories and wants 筛选.
            if (rail_is_library(rail_) || rail_is_favorites(rail_))
                hints[count++] = {ui::Button::options,
                                  active_filter_count() > 0 ? "重置筛选" : "筛选"};
        }
        else if (zone_ == Zone::home)
        {
            const bool hero_row = row_ == 0;
            hints[0] = {ui::Button::cross, hero_row ? (col_ == 0 ? "播放" : "详情") : "打开详情"};
            hints[1] = {ui::Button::dpad,
                        hero_row ? "左右选按钮 / 上下换行" : "选择卡片 / 左右换行"};
            hints[2] = {ui::Button::l2, "上一栏"};
            hints[3] = {ui::Button::r2, "下一栏"};
            count = 4;
        }
        else if (zone_ == Zone::library)
        {
            // Options is the filter shortcut, so name it in the legend and say
            // what this press will do: open the panel on a clean board, reset
            // the filters once any is active (mirrors update_library).
            hints[0] = {ui::Button::cross, "打开详情"};
            hints[1] = {ui::Button::options, active_filter_count() > 0 ? "重置筛选" : "筛选"};
            hints[2] = {ui::Button::circle, "返回首页"};
            hints[3] = {ui::Button::l2, "上一栏"};
            hints[4] = {ui::Button::r2, "下一栏"};
            count = 5;
        }
        else if (zone_ == Zone::detail)
        {
            // Square folds / unfolds the 剧情简介 (the web page's 展开/收起), so
            // it is offered only when there is a description long enough to
            // fold - or already folded, to give 收起 back.
            const Item &shown = detail_item();
            const bool expandable =
                !shown.synopsis.empty() &&
                glyphs_.regular.font->wrap(shown.synopsis, 21.0f, 1240.0f).size() > 2u;
            hints[0] = {ui::Button::cross, "播放 / 收藏 / 音轨 / 字幕"};
            hints[1] = {ui::Button::dpad, "选按钮与集数"};
            int i = 2;
            if (expandable || detail_synopsis_open_)
                hints[i++] = {ui::Button::square, detail_synopsis_open_ ? "收起简介" : "展开简介"};
            hints[i++] = {ui::Button::circle, "返回"};
            hints[i++] = {ui::Button::l2, "上一栏"};
            hints[i++] = {ui::Button::r2, "下一栏"};
            count = i;
        }
        else
        {
            // 缓存 and 关于 are read-only; a Cross hint on them promised an
            // edit that has no action behind it.
            if (settings_row_ <= 2)
            {
                hints[count++] = {ui::Button::cross, "编辑这一项"};
            }
            else if (settings_row_ == 3)
            {
                hints[count++] = {ui::Button::cross, "登录"};
            }
            else if (settings_row_ == 4)
            {
                hints[count++] = {ui::Button::cross, "退出登录"};
            }
            hints[count++] = {ui::Button::dpad, "选一行"};
            hints[count++] = {ui::Button::circle, "返回首页"};
        }
        ui::draw_hints(list, glyphs_, ui::GlyphStyle::dark(), hints.data(), count, kWidth - kMargin,
                       true, ui::HintLayout{32.0f, 19.0f, kHintY});
    }

    // ---- the library ----

    // One API item as this page draws it. The two models stay separate on
    // purpose: the server speaks of ancestor categories, resolutions and
    // second counts, the page of a meta line, a badge and a 0..1 bar.
    // Invented rows for the preview host: six films and one two-season show,
    // which is everything the wall, the season strip and the file card need
    // in order to lay themselves out. Every field is filled the way
    // view_item() fills it from the server, so a page cannot tell the two
    // apart except by having no network behind it.
    static std::vector<Item> sample_library()
    {
        struct Film
        {
            const char *title;
            const char *year;
            const char *badge;
            float rating;
            int runtime;
            std::uint32_t tint;
        };
        static const Film kFilms[] = {
            {"长夜将尽", "2024", "4K", 8.1f, 126, 0x7a2f1d},
            {"雾港来信", "2023", "1080P", 7.4f, 104, 0x2f5d63},
            {"铁皮屋顶上的夏天", "2022", "1080P", 8.6f, 141, 0x6b4a1f},
            {"北纬四十度", "2025", "4K", 6.9f, 97, 0x243a5e},
            {"候鸟旅馆", "2021", "720P", 7.8f, 88, 0x3d5a2f},
            {"最后一班夜车", "2024", "1080P", 8.2f, 113, 0x5e2a3d},
            // Four rows of films, so the preview's poster wall is longer than
            // the two rows a 1080 screen holds and the scroll window in
            // draw_library() is actually exercised, the way a real 241-item
            // library is.
            {"南方来信", "2023", "1080P", 7.1f, 102, 0x4a3d6b},
            {"旧城墙下的雪", "2022", "4K", 8.4f, 134, 0x2d4a5e},
            {"晚风过境", "2025", "1080P", 6.8f, 95, 0x6b3a2f},
            {"海边理发店", "2021", "720P", 7.9f, 91, 0x3a5e4a},
            {"第七个夏天", "2024", "1080P", 8.0f, 118, 0x5e2f4a},
            {"无声的河", "2020", "4K", 7.6f, 127, 0x2f4a3d},
            {"山雨欲来", "2023", "1080P", 8.5f, 139, 0x6b2f2d},
            {"三个人的车站", "2022", "1080P", 7.3f, 99, 0x3d5e6b},
            {"远山淡影", "2025", "4K", 6.7f, 105, 0x4a5e2f},
            {"风筝季", "2021", "720P", 7.5f, 86, 0x5e4a3d},
            {"冬泳", "2024", "1080P", 8.3f, 121, 0x2f5e4a},
            {"铁轨旁的信", "2023", "1080P", 7.0f, 108, 0x6b5e2f},
            {"午后的雷雨", "2022", "4K", 8.1f, 130, 0x3a2f5e},
            {"渡口", "2020", "1080P", 7.7f, 111, 0x5e6b3a},
            {"雾中列车", "2025", "1080P", 6.9f, 93, 0x2f3a6b},
            {"白鹭洲", "2021", "4K", 8.6f, 143, 0x4a2f3d},
            {"长街短梦", "2024", "1080P", 7.2f, 100, 0x6b4a5e},
        };
        static const char *const kEpisodes[] = {
            "灯塔来信", "退潮之后", "旧码头", "雨季", "北上的船", "雾里的人", "夏末", "归程",
        };

        std::vector<Item> rows;
        for (const Film &film : kFilms)
        {
            Item item;
            item.guid = std::string("sample-film-") + film.title;
            item.series = item.guid;
            item.title = film.title;
            item.year = film.year;
            item.kind = Kind::movie;
            item.genre = "剧情";
            item.resolution = film.badge;
            item.badge = film.badge;
            item.runtime = film.runtime;
            item.rating = film.rating;
            item.tint = film.tint;
            item.synopsis = "样本库的条目，只为让预览主机上的页面有内容可排。";
            item.meta = std::string(film.year) + " · 电影 · " + runtime_text(film.runtime);
            rows.push_back(std::move(item));
        }
        // One show, eight episodes over two seasons, all sharing a series key:
        // the wall draws one card and the detail page a season strip.
        for (std::size_t e = 0; e < std::size(kEpisodes); ++e)
        {
            Item item;
            item.guid = std::string("sample-episode-") + kEpisodes[e];
            item.series = "sample-series";
            item.series_title = "海风旅店";
            item.title = kEpisodes[e];
            item.kind = Kind::show;
            item.genre = "剧情";
            item.season = e < 4 ? 1 : 2;
            item.episode = static_cast<int>(e) + 1;
            item.resolution = "1080P";
            item.badge = "1080P";
            item.runtime = 44;
            item.rating = 8.0f;
            item.tint = tint_for(item.title);
            item.synopsis = "样本剧集，用来铺季/集条。";
            item.meta = std::string("S") + (item.season < 10 ? "0" : "") +
                        std::to_string(item.season) + "E" + (item.episode < 10 ? "0" : "") +
                        std::to_string(item.episode) + " · 44 分";
            rows.push_back(std::move(item));
        }
        return rows;
    }

    static Item view_item(const fnvideo::Item &source)
    {
        Item item;
        item.guid = source.guid;
        // One list row is one card. The list is asked for the top-level kinds
        // only, so a series row already stands for its whole show and its guid
        // is the key the wall groups by - using ancestor_guid here put every
        // film of a library on one card, and every series on another.
        item.series = source.guid;
        item.library = source.ancestor_guid;
        item.title = source.title;
        // A 短剧 folder's files and a series' episodes are named after the
        // file ("S01E55.mp4"); the card names the collection instead, which is
        // what the server puts in parent_title.
        if (!source.parent_title.empty() && named_after_file(source.title))
        {
            item.title = source.parent_title;
            item.series_title = source.parent_title;
        }
        if (source.episode_number > 0)
        {
            // Only a row that is itself an episode belongs to a named series;
            // the list never returns those, but 继续观看 does.
            item.series_title =
                source.parent_title.empty() ? source.ancestor_name : source.parent_title;
        }
        if (source.release_date.size() >= 4)
        {
            item.year = source.release_date.substr(0, 4);
        }
        item.season = source.season_number;
        item.episode = source.episode_number;
        item.episodes = source.local_number_of_episodes > 0 ? source.local_number_of_episodes
                                                            : source.number_of_episodes;
        if (item.episodes <= 0)
        {
            item.episodes = episode_count_from_title(source.title);
        }
        // Real genres from the detail call ("动作 · 冒险 · 喜剧"). A wall row
        // comes from /item/list, which carries none, so this is empty there and
        // the card caption falls back to the year; the detail page fills it in
        // from the server answer. (ancestor_name is the library category -
        // "电影" / "短剧" - not a genre, so it is deliberately not shown here.)
        item.genre = source.genres_text;
        item.synopsis = source.overview;
        item.poster = source.poster;
        item.backdrop = source.backdrop;
        item.kind = kind_of(source);
        item.type = source.type;
        item.category = source.ancestor_category;
        item.is_favorite = source.is_favorite;
        item.watched = source.watched;
        item.badge = badge_text(source);
        item.resolution = source.resolution;
        // Media_stream is on the list row too, so DV / Atmos can show before the
        // detail call lands; the rating / country / Chinese-track chips fill in
        // later, in sync_detail_answer().
        item.content_rating = source.content_rating;
        item.country = country_text(source.countries);
        item.color_range_type = source.color_range;
        item.audio_type = source.audio_type;
        item.chi_audio = source.chi_audio;
        item.chi_subtitle = source.chi_subtitle;
        // The list call carries seconds and the detail call minutes; the card
        // wants minutes either way.
        item.runtime = static_cast<int>(
            source.runtime > 0 ? source.runtime : (source.duration > 0 ? source.duration / 60 : 0));
        item.duration = source.duration;
        item.meta = meta_text(source, item.kind);
        item.rating = static_cast<float>(std::atof(source.vote_average.c_str()));
        item.tint = tint_for(source.title);
        if (source.duration > 0 && source.watched_ts > 0)
        {
            item.progress = std::clamp(static_cast<float>(source.watched_ts) /
                                           static_cast<float>(source.duration),
                                       0.0f, 1.0f);
        }
        // The search box types Latin, so a Han title is reached through its
        // pinyin; the server's Latin original ("Baby Driver") is a second way
        // in. Both are precomputed here so the per-frame filter is a substring
        // test (search_matches), never a table walk while the pad is moving.
        item.search_pinyin = fnvideo::pinyin_of(item.title, &item.search_initials);
        item.search_latin = fnvideo::pinyin_of(source.original_title, nullptr);
        return item;
    }

    // The inverse of view_item(), for the one case the service needs to keep a
    // card: an episode the list call never carried. Only the fields the card
    // is rebuilt from are filled; the service stores it as a plain record.
    static fnvideo::Item service_item(const Item &source)
    {
        fnvideo::Item item;
        item.guid = source.guid;
        item.title = source.title;
        item.ancestor_guid = source.library;
        item.ancestor_name = source.series_title.empty() ? source.genre : source.series_title;
        item.ancestor_category = source.category;
        item.overview = source.synopsis;
        item.poster = source.poster;
        item.backdrop = source.backdrop;
        item.parent_title = source.series_title;
        item.release_date = source.year;
        item.vote_average = std::to_string(source.rating);
        item.resolution = source.resolution;
        item.runtime = source.runtime;
        item.duration = source.duration;
        item.season_number = source.season;
        item.episode_number = source.episode;
        item.local_number_of_episodes = source.episodes;
        item.is_favorite = source.is_favorite;
        item.type = source.kind == Kind::movie ? "Movie" : "TV";
        return item;
    }

    // Copies what the worker last published, but only when it changed: a
    // 241-item library is far too much to rebuild every frame.
    void sync_library()
    {
        auto &service = fnvideo::Service::instance();
        const unsigned revision = service.revision();
        // The preview host's library never changes revision (there is no
        // worker answer to bump it), so the sample has to be allowed through
        // the gate once, not only when the worker publishes.
        const bool wants_sample = preview_sample() && items_.empty();
        if (revision == seen_revision_ && !wants_sample)
        {
            return;
        }
        seen_revision_ = revision;
        shelf_dirty_ = true; // items_ changed, so the projection must be remade

        const fnvideo::Service::View view = service.view();
        status_ = view.status;
        signed_in_ = view.signed_in;
        libraries_ = view.libraries;
        // A new or signed-out server can publish fewer libraries than the rail
        // was pointing into; without this the tab index can fall past the
        // fixed 设置 entry and open the wrong shelf.
        rail_ = std::clamp(rail_, 0, std::max(0, rail_count() - 1));

        const bool sampled = view.items.empty() && preview_sample();
        if (view.items.empty())
        {
            // Nothing from the NAS: not configured, not signed in, or still
            // connecting. Show an empty state, never invented content - unless
            // the preview host asked for the sample library (see
            // preview_sample()), because a photograph of a blank page is no
            // use to anyone reading the design on a PC.
            items_.clear();
            if (sampled)
            {
                items_ = sample_library();
                // The rail is built from the server's libraries, and the
                // preview host has no server, so without this the wall and the
                // detail page could not be photographed at all - every library
                // tab would be missing and the tour would land on 收藏. The
                // sample's own two shelves stand in.
                libraries_.clear();
                libraries_.push_back(fnvideo::Library{"sample-movies", "电影", "Movie", ""});
                libraries_.push_back(fnvideo::Library{"sample-shows", "电视剧", "TV", ""});
                for (Item &item : items_)
                {
                    item.library = item.kind == Kind::movie ? "sample-movies" : "sample-shows";
                }
            }
            else
            {
                wall_.clear();
                home_continue_.clear();
                home_recent_.clear();
                libraries_.clear();
                library_items_ = 0;
                hero_index_ = 0;
                hero_picked_ = false;
                hero_slot_n_ = 0;
                hero_slide_ = 0;
                detail_ = 0;
                detail_guid_.clear();
                episode_ = 0;
                detail_episodes_.clear();
                detail_seasons_.clear();
                season_sel_ = 0;
                search_input_.clear();
                return;
            }
        }

        if (!sampled)
        {
            items_.clear();
            items_.reserve(view.items.size() + view.resume.size());
            for (const auto &source : view.items)
            {
                items_.push_back(view_item(source));
            }
        }

        // 继续观看 points at the same rows by guid; an entry that is not in
        // the library (an episode of a series, say) is appended so the row can
        // still show it. Everything past this point is such a row: it belongs
        // in the home row, never on the poster wall.
        library_items_ = static_cast<int>(items_.size());
        home_continue_.clear();
        for (const auto &entry : view.resume)
        {
            if (home_continue_.size() >= 12)
            {
                break;
            }
            int index = -1;
            for (std::size_t i = 0; i < items_.size(); ++i)
            {
                if (!entry.guid.empty() && items_[i].guid == entry.guid)
                {
                    index = static_cast<int>(i);
                    break;
                }
            }
            if (index < 0)
            {
                items_.push_back(view_item(entry));
                index = static_cast<int>(items_.size()) - 1;
            }
            home_continue_.push_back(index);
        }
        // The preview sample has no server resume list; a real account with no
        // resume points must not be told a dozen films are in progress.
        if (home_continue_.empty() && preview_sample())
        {
            for (int i = 0; i < static_cast<int>(items_.size()) && i < 12; ++i)
            {
                home_continue_.push_back(i);
            }
        }

        // The server has no favourite write endpoint in this client, so a star
        // pressed here is a local mark. Re-apply it after every rebuild of the
        // view, or the next progress tick would silently clear it.
        for (Item &item : items_)
        {
            const auto found = favorite_overrides_.find(item.guid);
            if (found != favorite_overrides_.end())
            {
                item.is_favorite = found->second ? 1 : 0;
            }
        }

        // The third home row is 最近收藏: the items the viewer has starred,
        // whether by the server's flag or a local mark applied just above.
        // Fewer than six is fine; an empty row draws a hint line instead.
        home_recent_.clear();
        for (int i = 0; i < static_cast<int>(items_.size()) && home_recent_.size() < 6; ++i)
        {
            if (items_[static_cast<std::size_t>(i)].is_favorite != 0)
            {
                home_recent_.push_back(i);
            }
        }

        // Re-point the detail page by guid, not by a cached index. The server
        // can insert or remove a row between two view revisions (playback
        // progress bumps them every ten seconds), and the old index would
        // silently start showing a different title mid-film.
        if (items_.empty())
        {
            detail_ = 0;
            detail_guid_.clear();
        }
        else
        {
            int found = -1;
            if (!detail_guid_.empty())
            {
                for (std::size_t i = 0; i < items_.size(); ++i)
                {
                    if (items_[i].guid == detail_guid_)
                    {
                        found = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (found >= 0)
            {
                detail_ = found;
            }
            else
            {
                detail_ = std::clamp(detail_, 0, static_cast<int>(items_.size()) - 1);
                detail_guid_ = items_[static_cast<std::size_t>(detail_)].guid;
            }
        }
        build_wall();
        rebuild_detail_episodes();

        // 随机推荐 is up to three library cards, chosen once per launch, that the
        // banner rotates through. Drawing from wall_ rather than items_ keeps the
        // banner on a series' first row: a raw index could greet the viewer with
        // S01E04 of a show. Re-pick when the wall moved out from under a slot (a
        // 刷新 can retire a card and shift the indices under a stale slot).
        const int hero_pool = static_cast<int>(wall_.size());
        bool slots_valid = hero_picked_ && hero_slot_n_ > 0;
        for (int i = 0; slots_valid && i < hero_slot_n_; ++i)
        {
            if (std::find(wall_.begin(), wall_.end(), hero_slots_[static_cast<std::size_t>(i)]) ==
                wall_.end())
            {
                slots_valid = false;
            }
        }
        if (!slots_valid)
        {
            hero_picked_ = false;
        }
        if (!hero_picked_ && hero_pool > 0)
        {
            hero_picked_ = true;
            hero_slot_n_ = std::min(3, hero_pool);
            hero_slide_ = 0;
            // Mix the clock so two launches in the same second still differ, then
            // walk a hash ladder so the three land spread across the wall.
            std::uint64_t x = hero_seed_ ^ (hero_seed_ >> 33);
            for (int i = 0; i < hero_slot_n_; ++i)
            {
                x *= 0xff51afd7ed558ccdULL;
                x ^= x >> 33;
                hero_slots_[static_cast<std::size_t>(i)] =
                    wall_[static_cast<std::size_t>(x % static_cast<std::uint64_t>(hero_pool))];
            }
            // If the ladder landed twice on one card and the wall has room to
            // differ, nudge the repeat forward so the banner isn't showing twins.
            for (int i = 1; i < hero_slot_n_; ++i)
            {
                for (int j = 0; j < i; ++j)
                {
                    if (hero_slots_[static_cast<std::size_t>(i)] ==
                            hero_slots_[static_cast<std::size_t>(j)] &&
                        hero_pool > hero_slot_n_)
                    {
                        hero_slots_[static_cast<std::size_t>(i)] =
                            wall_[(static_cast<std::size_t>(
                                       hero_slots_[static_cast<std::size_t>(j)]) +
                                   1u + static_cast<std::size_t>(i)) %
                                  static_cast<std::size_t>(hero_pool)];
                    }
                }
            }
            hero_index_ = hero_slots_[0];
        }
        if (items_.empty())
        {
            hero_index_ = 0;
            hero_slot_n_ = 0;
        }
        else if (std::find(wall_.begin(), wall_.end(), hero_index_) == wall_.end())
        {
            // A library change can retire the card the banner was showing.
            hero_index_ = wall_.empty() ? 0 : wall_.front();
        }
        // The wall may have shrunk while the player was on it. Keep the ring
        // on a card that exists and the window on a row that is drawn.
        grid_ = std::clamp(grid_, 0, std::max(0, library_count() - 1));
        clamp_grid_scroll();
        clamp_home_focus();
    }

    // Groups the library into the cards the poster wall draws: one card per
    // series, its first row standing for the rest, so a sixty-episode show is
    // one card rather than sixty. Also the place an episode with no poster of
    // its own borrows the series' artwork.
    void build_wall()
    {
        wall_.clear();
        slot_of_.assign(items_.size(), -1);
        std::unordered_map<std::string, int> slot_of_series;
        for (int i = 0; i < library_items_; ++i)
        {
            Item &item = items_[static_cast<std::size_t>(i)];
            if (item.series.empty())
            {
                item.series = item.guid;
            }
            const auto found = slot_of_series.find(item.series);
            if (found == slot_of_series.end())
            {
                slot_of_series.emplace(item.series, static_cast<int>(wall_.size()));
                wall_.push_back(i);
                slot_of_[static_cast<std::size_t>(i)] = static_cast<int>(wall_.size()) - 1;
            }
            else
            {
                slot_of_[static_cast<std::size_t>(i)] = found->second;
            }
        }

        // Episodes per card, counted per slot: the card says 集数, the detail
        // page says 共 n 集.
        std::vector<int> counts(wall_.size(), 0);
        for (int i = 0; i < library_items_; ++i)
        {
            const Item &item = items_[static_cast<std::size_t>(i)];
            if (item.season > 0 || item.episode > 0)
            {
                ++counts[static_cast<std::size_t>(slot_of_[static_cast<std::size_t>(i)])];
            }
        }
        for (std::size_t slot = 0; slot < wall_.size(); ++slot)
        {
            Item &head = items_[static_cast<std::size_t>(wall_[slot])];
            // A series row from the server already knows how many episodes it
            // holds; the count below only stands in for a library whose rows
            // arrived as episodes (the sample catalogue).
            if (head.episodes <= 0)
            {
                head.episodes = counts[slot];
            }
        }

        // Artwork: the series poster down to its episodes, and back up when
        // only an episode brought one. A wall of blanks would read as broken.
        for (std::size_t slot = 0; slot < wall_.size(); ++slot)
        {
            Item &head = items_[static_cast<std::size_t>(wall_[slot])];
            if (!head.poster.empty())
            {
                continue;
            }
            for (int i = 0; i < library_items_; ++i)
            {
                if (slot_of_[static_cast<std::size_t>(i)] == static_cast<int>(slot) &&
                    !items_[static_cast<std::size_t>(i)].poster.empty())
                {
                    head.poster = items_[static_cast<std::size_t>(i)].poster;
                    break;
                }
            }
        }
        for (int i = 0; i < library_items_; ++i)
        {
            Item &item = items_[static_cast<std::size_t>(i)];
            if (item.poster.empty())
            {
                item.poster = items_[static_cast<std::size_t>(wall_[static_cast<std::size_t>(
                                         slot_of_[static_cast<std::size_t>(i)])])]
                                  .poster;
            }
        }
    }

    // The detail page's episode list for `detail_`, grouped into seasons.
    // A film has none.
    //
    // The list call returns cards, not episodes, so what plays comes from the
    // server's own answer for this item (a series' seasons and their episodes,
    // a 短剧 folder's files), already numbered and sorted by Service. The
    // sample catalogue has no server behind it, so it falls back to the rows
    // the wall folded into this card.
    void rebuild_detail_episodes()
    {
        // The worker's detail answer arrives after the page is already on
        // screen. Rebuilding used to reset season_sel_ to zero, which threw a
        // viewer who had opened season 3 back to season 1 when the answer
        // landed a few frames later.
        const int previous_season = season_sel_;
        const int previous_episode = episode_;
        const int previous_focus = detail_focus_;
        detail_episodes_.clear();
        detail_seasons_.clear();
        const Item &head = detail_item();
        const std::string &key = head.series;
        const fnvideo::Service::Detail detail = fnvideo::Service::instance().detail();
        if (detail.ready && detail.item.guid == head.guid && !detail.episodes.empty())
        {
            // The server listed the episodes for this exact collection guid. A
            // series root carries no `series` field of its own - it *is* the
            // series - so this path must not be gated on head.series. It used
            // to be, which emptied detail_episodes_ for every 剧集: no 选集
            // rendered, and the 播放 press died in the flat<0 guard (the log
            // shows episodes=52 yet never a "play begin").
            detail_episodes_.reserve(detail.episodes.size());
            for (const auto &source : detail.episodes)
            {
                Item episode = view_item(source);
                if (episode.series.empty())
                {
                    episode.series = key;
                }
                detail_episodes_.push_back(std::move(episode));
            }
        }
        else if (!items_.empty() && !key.empty())
        {
            // No matching server answer yet: fall back to the rows the wall
            // folded into this card, matched by series name. Only a row that is
            // an episode joins the strip; a series row carries a season but no
            // episode number.
            for (int i = 0; i < item_count(); ++i)
            {
                const Item &item = items_[static_cast<std::size_t>(i)];
                if (item.series == key && item.episode > 0)
                {
                    detail_episodes_.push_back(item);
                }
            }
            std::stable_sort(detail_episodes_.begin(), detail_episodes_.end(),
                             [](const Item &left, const Item &right)
                             {
                                 if (left.season != right.season)
                                 {
                                     return left.season < right.season;
                                 }
                                 return left.episode < right.episode;
                             });
        }
        else
        {
            episode_ = 0;
            detail_focus_ = 0;
            return;
        }
        if (detail_episodes_.empty())
        {
            if (head.episodes > 0 && !preview_sample())
            {
                // A collection that produced no playable rows: this is exactly
                // the case where a TV detail page shows no 选集 and a 播放 press
                // is refused by the guard. Say whether the server answered with
                // any episodes at all, so the fix targets the fetch/parse path
                // (Service::sync_detail / Api season parsing) instead of guessing.
                sys::log("[HUI] detail episodes empty guid=%s type=%s kind=%d episodes=%d "
                         "series=%s detail.ready=%d detail.guid=%s detail.episodes=%zu",
                         head.guid.c_str(), head.type.c_str(), static_cast<int>(head.kind),
                         head.episodes, head.series.c_str(), detail.ready ? 1 : 0,
                         detail.item.guid.c_str(), detail.episodes.size());
            }
            episode_ = 0;
            detail_focus_ = 0;
            return;
        }
        for (std::size_t at = 0; at < detail_episodes_.size();)
        {
            const int season = detail_episodes_[at].season;
            std::size_t next = at + 1;
            while (next < detail_episodes_.size() && detail_episodes_[next].season == season)
            {
                ++next;
            }
            detail_seasons_.push_back(
                SeasonRun{season, static_cast<int>(at), static_cast<int>(next)});
            at = next;
        }
        // Focus indices are rebuilt around the list, so a stale one would put
        // the ring past the last chip. Keep the viewer's season and episode
        // where the new list still has them.
        season_sel_ = std::clamp(previous_season, 0, season_count() - 1);
        episode_ = std::clamp(previous_episode, 0, static_cast<int>(detail_episodes_.size()) - 1);
        detail_focus_ = std::clamp(previous_focus, 0, detail_last_focus());
    }

    // The worker publishes a detail answer after the page has already drawn,
    // so the strip is rebuilt when the answer's revision moves. Reading the
    // revision costs one integer, which is why this may run every frame.
    void sync_detail_answer()
    {
        // The preview sample is complete as invented; asking the server would
        // put a real login error over an otherwise valid page.
        if (preview_sample())
        {
            return;
        }
        const unsigned revision = fnvideo::Service::instance().detail_revision();
        if (revision == detail_revision_seen_)
        {
            return;
        }
        // Do not consume an answer while the library or the player owns the
        // screen. It used to be marked seen here before the zone check, so a
        // series' episode list that landed during playback was lost forever.
        if (zone_ != Zone::detail)
        {
            return;
        }
        detail_revision_seen_ = revision;
        // The list call only carries a row's own poster; the detail call is
        // what carries the wide backdrop the hero is drawn from - and for a
        // series it is the only place any artwork shows up at all (the worker
        // borrows the first episode's still). Merge it into the row the page
        // is showing, without touching anything else about it.
        const fnvideo::Service::Detail &answer = fnvideo::Service::instance().detail();
        if (!answer.error.empty())
        {
            detail_error_ = answer.error;
            return;
        }
        if (answer.item.guid.empty() || detail_ >= static_cast<int>(items_.size()) ||
            items_[static_cast<std::size_t>(detail_)].guid != answer.item.guid)
        {
            return;
        }
        detail_error_.clear();
        Item &shown = items_[static_cast<std::size_t>(detail_)];
        // Cache the detail's logo path so the per-frame art request can ask for
        // it without copying the whole Detail again (the list row's logo is a
        // poster placeholder and is deliberately ignored here).
        detail_logo_ = answer.item.logo;
        if (shown.poster.empty() && !answer.item.poster.empty())
        {
            shown.poster = answer.item.poster;
        }
        // The list call fills `backdrop` from `poster_list`, which is really the
        // portrait poster - so a wall row's backdrop is a wide-stretched poster,
        // not the landscape key art. The detail call is the only place the real
        // `backdrops` appears, so when it does, it wins outright over that
        // placeholder; this is what makes the hero match the official page.
        if (!answer.item.backdrop.empty())
        {
            shown.backdrop = answer.item.backdrop;
        }
        // The detail call is the only place the rating, the countries and the
        // Chinese audio / subtitle tracks appear, so those chips merge now. Only
        // overwrite a field the detail actually filled.
        if (!answer.item.content_rating.empty())
        {
            shown.content_rating = answer.item.content_rating;
        }
        if (!answer.item.countries.empty())
        {
            shown.country = country_text(answer.item.countries);
        }
        // Genres only exist on the detail call, so the type line fills here.
        if (!answer.item.genres_text.empty())
        {
            shown.genre = answer.item.genres_text;
        }
        if (!answer.item.color_range.empty())
        {
            shown.color_range_type = answer.item.color_range;
        }
        if (!answer.item.audio_type.empty())
        {
            shown.audio_type = answer.item.audio_type;
        }
        shown.chi_audio = answer.item.chi_audio;
        shown.chi_subtitle = answer.item.chi_subtitle;
        if (shown.synopsis.empty() && !answer.item.overview.empty())
        {
            shown.synopsis = answer.item.overview;
        }
        rebuild_detail_episodes();
    }

    // Point the detail page at an item. The episode list belongs to the item,
    // so every path that moves `detail_` goes through here.
    bool open_detail(int index, Zone from = Zone::library)
    {
        if (items_.empty())
        {
            return false;
        }
        detail_back_ = from;
        detail_ = std::clamp(index, 0, static_cast<int>(items_.size()) - 1);
        detail_guid_ = items_[static_cast<std::size_t>(detail_)].guid;
        detail_error_.clear();
        detail_logo_.clear();
        detail_focus_ = 0;
        episode_ = 0;
        season_sel_ = 0;
        detail_synopsis_open_ = false;
        // A series' episodes live a level below the list call, so the worker
        // is asked for this item's own answer; the strip fills in when it
        // lands (see update()). The preview sample has no server, so its
        // invented answer is already complete.
        if (!preview_sample())
        {
            fnvideo::Service::instance().request_detail(detail_item().guid);
        }
        rebuild_detail_episodes();
        return true;
    }

    // ---- detail page: seasons and episodes ----

    int season_count() const
    {
        return static_cast<int>(detail_seasons_.size());
    }

    // The season row only earns a place when there is more than one season;
    // one season would be a label that says nothing.
    int season_focus_count() const
    {
        return season_count() > 1 ? season_count() : 0;
    }

    // Focus indices: 0 播放, 1 收藏, then one chip per season, then that
    // season's episodes.
    int episode_base() const
    {
        return kDetailActions + season_focus_count();
    }

    // The season whose episodes the strip shows: the one the focus is on
    // while that row is walked, the chosen one otherwise.
    int current_season() const
    {
        if (season_count() == 0)
        {
            return 0;
        }
        if (detail_focus_ >= kDetailActions && detail_focus_ < episode_base())
        {
            return std::clamp(detail_focus_ - kDetailActions, 0, season_count() - 1);
        }
        return std::clamp(season_sel_, 0, season_count() - 1);
    }

    const SeasonRun &current_run() const
    {
        static const SeasonRun kNone{};
        if (season_count() == 0)
        {
            return kNone;
        }
        return detail_seasons_[static_cast<std::size_t>(current_season())];
    }

    int detail_last_focus() const
    {
        if (detail_episodes_.empty())
        {
            return kDetailActions - 1;
        }
        return episode_base() + (current_run().end - current_run().begin) - 1;
    }

    // The flat list index the focused episode chip is about, -1 elsewhere.
    int focused_episode() const
    {
        if (detail_focus_ < episode_base())
        {
            return -1;
        }
        const SeasonRun &run = current_run();
        return run.begin + (detail_focus_ - episode_base());
    }

    int episode_number_at(int flat) const
    {
        if (flat < 0 || flat >= static_cast<int>(detail_episodes_.size()))
        {
            return flat + 1;
        }
        const int number = detail_episodes_[static_cast<std::size_t>(flat)].episode;
        return number > 0 ? number : flat + 1;
    }

    // What the wall, the detail page and the player OSD call a collection: an
    // episode is named after its series, a film after itself.
    const std::string &heading_of(const Item &item) const
    {
        return item.series_title.empty() ? item.title : item.series_title;
    }

    // The guid the play button opens: the chosen episode of a series, the
    // item itself for a film.
    std::string playable_guid() const
    {
        if (detail_episodes_.empty())
        {
            return detail_item().guid;
        }
        const int flat = std::clamp(episode_, 0, static_cast<int>(detail_episodes_.size()) - 1);
        return detail_episodes_[static_cast<std::size_t>(flat)].guid;
    }

    // Picks where a series' banner play button starts: the episode already in
    // progress, else the first unwatched one, else the first episode. Films
    // never reach this.
    int hero_resume_episode() const
    {
        int first_unwatched = -1;
        for (int i = 0; i < static_cast<int>(detail_episodes_.size()); ++i)
        {
            const float progress = detail_episodes_[static_cast<std::size_t>(i)].progress;
            if (progress > 0.0f && progress < 1.0f)
            {
                return i;
            }
            if (first_unwatched < 0 && progress <= 0.0f)
            {
                first_unwatched = i;
            }
        }
        return first_unwatched >= 0 ? first_unwatched : 0;
    }

    // Starts the player on a detail row: `flat` is an episode index, or -1 for
    // a film. Both the detail page and the home banner go through here so the
    // resume position, menu state and remembered row cannot drift apart.
    void start_detail_playback(int flat, app::Feedback &feedback)
    {
        const Item &item = detail_item();
        if (detail_episodes_.empty())
        {
            flat = -1;
        }
        else
        {
            flat = std::clamp(flat, 0, static_cast<int>(detail_episodes_.size()) - 1);
        }
        // A collection's own guid is not a playable file. If its episode list
        // has not landed yet, refuse instead of asking the worker for it.
        // But an fnOS "Video" is a single playable file (episodes=0) even though
        // kind_of does not classify it as a movie - it must play. Refusing it
        // here is what made 黑神话钟馗's 播放 button do nothing at all: the press
        // never reached Service::play, so there was no open, no error banner.
        // A collection's own guid is not a playable file, so refuse only when
        // the row really is a multi-episode collection with no episode chosen.
        // The old clause also keyed off the kind/type strings, which silently
        // killed any single file the server filed as "Others"/"TV" or left
        // untyped - "some films' 播放 button does nothing". episodes is the
        // honest signal (0 for a film), so lean on it and log the rest.
        if (flat < 0 && item.episodes > 0)
        {
            sys::log("[HUI] play refused (collection) guid=%s type=%s kind=%d episodes=%d",
                     item.guid.c_str(), item.type.c_str(), static_cast<int>(item.kind),
                     item.episodes);
            feedback.play(audio::Cue::error);
            return;
        }
        const Item &chosen = flat < 0 ? item : detail_episodes_[static_cast<std::size_t>(flat)];
        if (flat >= 0)
        {
            episode_ = flat;
            for (std::size_t slot = 0; slot < detail_seasons_.size(); ++slot)
            {
                const SeasonRun &run = detail_seasons_[slot];
                if (flat >= run.begin && flat < run.end)
                {
                    season_sel_ = static_cast<int>(slot);
                    break;
                }
            }
        }
        const std::string &guid = flat < 0 ? item.guid : chosen.guid;
        // A film watched to the end starts over: un-finish it and jump to
        // from_start. The service holds this flag so 继续观看 and the detail
        // button agree without duplicating state here.
        const bool was_finished =
            !guid.empty() && fnvideo::Service::instance().clear_finished(guid);
        // Ask before resuming a half-watched item. Anything with a real local
        // point (>=15s, under 95%) gets a dialog: 继续 mm:ss / 从头播放.
        // Skipped when `was_finished` (user watched it out: straight from 0),
        // when there's no local point (never opened here) or when it's the
        // tail of the film (a resume would drop them in the credits).
        if (!guid.empty() && !was_finished)
        {
            const int local = fnvideo::Service::instance().local_progress(guid);
            if (local > 15 && chosen.progress > 0.02f && chosen.progress < 0.95f)
            {
                resume_ask_ = true;
                resume_ask_sel_ = 0;
                resume_ask_guid_ = guid;
                resume_ask_seconds_ = local;
                resume_ask_flat_ = flat;
                feedback.play(audio::Cue::tab);
                return;
            }
        }
        launch_playback(guid, chosen, was_finished, feedback);
    }

    // The block that actually hands the film to the player. Split out of
    // start_detail_playback so the resume dialog can commit the choice without
    // re-checking collection guards or re-opening the dialog.
    void launch_playback(const std::string &guid, const Item &chosen, bool from_start,
                         app::Feedback &feedback)
    {
        position_ = from_start ? 0.0f : std::clamp(chosen.progress, 0.0f, 1.0f);
        paused_ = false;
        player_bar_ = false;
        player_chip_ = 0;
        player_menu_ = PlayerMenu::none;
        player_menu_sel_ = 0;
        show_player_osd();
        zone_ = Zone::player;
        if (!guid.empty())
        {
            auto &svc = fnvideo::Service::instance();
            svc.remember(service_item(chosen));
            // A track chosen on the collection page is stored under the
            // collection guid; carry it onto the episode about to play so a
            // series honours the pick too (a film already shares the guid).
            if (guid != detail_guid_)
            {
                const int a = svc.detail_pref_audio(detail_guid_);
                const int s = svc.detail_pref_subtitle(detail_guid_);
                if (a != -1)
                    svc.select_detail_audio(guid, a);
                if (s != -1)
                    svc.select_detail_subtitle(guid, s);
            }
            if (from_start)
                svc.play_from_start(guid);
            else
                svc.play(guid, 0);
        }
        hero_play_pending_ = false;
        feedback.play(audio::Cue::select);
    }

    // The resume dialog. Handles the pad while `resume_ask_` is up (update_pages
    // short-circuits everything else here): left/right moves the choice, Cross
    // commits, Circle backs out without playing. The confirm path rebuilds the
    // chosen Item from `resume_ask_flat_` so a film (flat=-1) and an episode
    // both share launch_playback().
    void update_resume_ask(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            resume_ask_sel_ = resume_ask_sel_ == 0 ? 1 : 0;
            feedback.play(audio::Cue::focus);
            return;
        }
        if (input.is_pressed(Action::back))
        {
            resume_ask_ = false;
            resume_ask_guid_.clear();
            resume_ask_seconds_ = 0;
            resume_ask_flat_ = -1;
            feedback.play(audio::Cue::back);
            return;
        }
        if (input.is_pressed(Action::confirm))
        {
            const std::string guid = resume_ask_guid_;
            const bool from_start = resume_ask_sel_ == 1;
            const int stored = resume_ask_flat_;
            resume_ask_ = false;
            resume_ask_guid_.clear();
            resume_ask_seconds_ = 0;
            resume_ask_flat_ = -1;
            if (guid.empty())
            {
                return;
            }
            const Item &item = detail_item();
            Item chosen = item;
            if (stored >= 0 && stored < static_cast<int>(detail_episodes_.size()))
            {
                chosen = detail_episodes_[static_cast<std::size_t>(stored)];
            }
            launch_playback(guid, chosen, from_start, feedback);
        }
    }

    void draw_resume_ask(gfx::DrawList &list) const
    {
        // Full-screen scrim so the page behind reads as disabled. Uses
        // rounded_rect with radius 0 (only primitive exposed).
        list.rounded_rect({0.0f, 0.0f, kWidth, kHeight}, 0.0f, Color::rgb(0x000000, 0.62f));
        const Rect card{640.0f, 380.0f, 640.0f, 320.0f};
        list.rounded_rect(card, 24.0f, kCoal);
        list.bordered_rect(card, 24.0f, kClear, 1.5f, kWhite.with_alpha(0.16f));
        ui::text(list, glyphs_.semibold, "继续上次播放？", card.cx(), card.y + 62.0f, 32.0f, kInk,
                 Align::center);
        ui::text(list, glyphs_.regular,
                 "上次停在 " + clock_text(static_cast<float>(resume_ask_seconds_)), card.cx(),
                 card.y + 100.0f, 20.0f, kMuted, Align::center);
        // Two equal buttons, side by side and centred under the line. The one
        // under the cursor gets the same glossed-accent treatment as the detail
        // page's 播放 key; the other stays a flat panel. The label sits dead
        // centre - the old x+60 / y+32 left it high and to the left of the
        // body, which is what read as 不协调.
        const auto button = [&](const Rect &r, bool hot, const std::string &label)
        {
            if (hot)
            {
                list.shadow({r.x, r.y + 4.0f, r.w, r.h}, 20.0f, 13.0f, Color::rgb(0x000000, 0.5f));
                list.gradient_rect(r, 20.0f, kAccentHi, kAccentLo);
                list.gradient_rect({r.x + 3.0f, r.y + 3.0f, r.w - 6.0f, r.h * 0.46f}, 17.0f,
                                   kWhite.with_alpha(0.22f), kWhite.with_alpha(0.0f));
                list.bordered_rect(r, 20.0f, kClear, 1.2f, Color::rgb(0x2f6fb5));
            }
            else
            {
                list.rounded_rect(r, 20.0f, kPanel);
                list.bordered_rect(r, 20.0f, kClear, 1.2f, kWhite.with_alpha(0.16f));
            }
            ui::text(list, glyphs_.semibold, label, r.cx(), r.y + r.h * 0.5f + 8.0f, 22.0f,
                     hot ? kOnAccent : kInk, Align::center);
        };
        const float bw = 250.0f;
        const float bh = 64.0f;
        const float gap = 24.0f;
        const float bx0 = card.cx() - (bw * 2.0f + gap) * 0.5f;
        const float by = card.y + 150.0f;
        button({bx0, by, bw, bh}, resume_ask_sel_ == 0, "继续播放");
        button({bx0 + bw + gap, by, bw, bh}, resume_ask_sel_ == 1, "从头播放");
        ui::text(list, glyphs_.regular, "左右键选择  X 确认  O 取消", card.cx(),
                 card.y + card.h - 34.0f, 16.0f, kFaint, Align::center);
    }

    // Moves the detail page's episode list to another episode of the same
    // series, for the player's 上一集 / 下一集, and says whether it moved.
    bool step_episode(int delta, app::Feedback &feedback)
    {
        if (detail_episodes_.empty())
        {
            return false;
        }
        const int next = episode_ + delta;
        if (next < 0 || next >= static_cast<int>(detail_episodes_.size()))
        {
            feedback.play(audio::Cue::error);
            return false;
        }
        episode_ = next;
        for (std::size_t slot = 0; slot < detail_seasons_.size(); ++slot)
        {
            const SeasonRun &run = detail_seasons_[slot];
            if (episode_ >= run.begin && episode_ < run.end)
            {
                season_sel_ = static_cast<int>(slot);
                break;
            }
        }
        if (!detail_seasons_.empty())
        {
            // The focus index is relative to the season being shown. Leaving
            // it on the old run made the next Cross on the detail page open a
            // different episode than the ring highlighted.
            const SeasonRun &run = current_run();
            detail_focus_ =
                std::clamp(episode_base() + episode_ - run.begin, 0, detail_last_focus());
        }
        position_ = 0.0f;
        paused_ = false;
        player_menu_ = PlayerMenu::none;
        // A new episode gets a fresh glance at its title, then the chrome
        // leaves the picture again.
        show_player_osd();
        const std::string guid = detail_episodes_[static_cast<std::size_t>(episode_)].guid;
        if (!guid.empty())
        {
            fnvideo::Service::instance().remember(service_item(detail_episodes_[episode_]));
            fnvideo::Service::instance().play(guid, 0);
        }
        feedback.play(audio::Cue::tab);
        return true;
    }

    // One row of the shelf, clamped so an emptied library cannot index past
    // the end and a stale index cannot point at nothing.
    const Item &item_at(int index) const
    {
        static const Item kEmpty{};
        if (items_.empty())
        {
            return kEmpty;
        }
        return items_[static_cast<std::size_t>(
            std::clamp(index, 0, static_cast<int>(items_.size()) - 1))];
    }

    // The item the detail page is about.
    const Item &detail_item() const
    {
        return item_at(detail_);
    }

    int item_count() const
    {
        return static_cast<int>(items_.size());
    }

    // ---- artwork ----

    // The whole picture inside `box`, bars on whichever sides the aspect
    // leaves empty, and a uv of the full texture. A film is not artwork: a
    // cover crop on a 2.4:1 frame takes 13% off each side, which reads as a
    // wrong picture rather than as a filled tile.
    static gfx::Rect contain_rect(int tex_w, int tex_h, const gfx::Rect &box, gfx::Rect *uv)
    {
        *uv = {0.0f, 0.0f, 1.0f, 1.0f};
        if (tex_w <= 0 || tex_h <= 0 || box.w <= 0.0f || box.h <= 0.0f)
        {
            return box;
        }
        const float image_aspect = static_cast<float>(tex_w) / static_cast<float>(tex_h);
        const float box_aspect = box.w / box.h;
        if (image_aspect > box_aspect)
        {
            const float h = box.w / image_aspect;
            return {box.x, box.y + (box.h - h) * 0.5f, box.w, h};
        }
        const float w = box.h * image_aspect;
        return {box.x + (box.w - w) * 0.5f, box.y, w, box.h};
    }

    // A uv rectangle that fills `box` from a `tex_w` x `tex_h` picture without
    // stretching it: the longer side is cropped, which is what every streaming
    // shelf does with artwork whose aspect is not the tile's.
    static Rect cover_uv(int tex_w, int tex_h, float box_w, float box_h)
    {
        if (tex_w <= 0 || tex_h <= 0 || box_w <= 0.0f || box_h <= 0.0f)
        {
            return {0.0f, 0.0f, 1.0f, 1.0f};
        }
        const float image_aspect = static_cast<float>(tex_w) / static_cast<float>(tex_h);
        const float box_aspect = box_w / box_h;
        if (image_aspect > box_aspect)
        {
            const float keep = box_aspect / image_aspect;
            return {(1.0f - keep) * 0.5f, 0.0f, keep, 1.0f};
        }
        const float keep = image_aspect / box_aspect;
        return {0.0f, (1.0f - keep) * 0.5f, 1.0f, keep};
    }

    // A picture ready to be drawn into a box: the texture, and the uv that
    // fills the box without stretching it. texture is 0 while the picture is
    // still being fetched, and the caller paints its tinted plate instead.
    struct Plate
    {
        std::uint32_t texture = 0;
        Rect uv{0.0f, 0.0f, 1.0f, 1.0f};
    };

    // Looks one cache entry up and works out its crop.
    Plate plate_for(const std::string &key, float box_w, float box_h) const
    {
        if (key.empty())
        {
            return {};
        }
        auto &service = fnvideo::Service::instance();
        const std::uint32_t texture = service.art_texture(key);
        if (texture == 0)
        {
            return {};
        }
        int width = 0;
        int height = 0;
        Plate plate;
        plate.texture = texture;
        if (service.art_size(key, &width, &height))
        {
            plate.uv = cover_uv(width, height, box_w, box_h);
        }
        return plate;
    }

    // The cache key for an item's landscape still. It carries the actual art
    // path, not just the guid: a wall row's `backdrop` begins as the portrait
    // poster (the list has no wide field) and the detail call replaces it with
    // the real key art, and only a path-named entry lets the worker re-fetch
    // once that happens. A guid-only key latched the first (portrait) picture in
    // and never swapped it, leaving the hero a stretched poster.
    std::string still_key(const Item &item) const
    {
        if (item.guid.empty())
        {
            return {};
        }
        const std::string &path = !item.backdrop.empty() ? item.backdrop : item.poster;
        if (path.empty())
        {
            return {};
        }
        return item.guid + "-still:" + path;
    }

    // The landscape still, falling back to the poster when the server only has
    // one picture for the item - which is common for older films.
    Plate still_plate(const Item &item, float box_w, float box_h) const
    {
        // The wide art, or - when the server has none - the larger poster the
        // worker fetched under the same path-named still key (see still_key and
        // request_art); only a total miss drops to the small wall poster.
        const std::string key = still_key(item);
        if (!key.empty())
        {
            Plate plate = plate_for(key, box_w, box_h);
            if (plate.texture != 0)
            {
                return plate;
            }
        }
        return poster_plate(item, box_w, box_h);
    }

    // The portrait poster on its own.
    Plate poster_plate(const Item &item, float box_w, float box_h) const
    {
        if (item.guid.empty())
        {
            return {};
        }
        // Every card draws under its own guid: no two rows ever share a
        // texture, so a show with no artwork of its own can never end up
        // painting another show's poster. The path behind that guid is chosen
        // in art_path_for (its own poster, or a same-titled sibling's).
        return plate_for(item.guid, box_w, box_h);
    }

    // The poster path to fetch for a card. Normally its own. A row the list
    // sent without artwork (a 0-episode duplicate the server keeps beside the
    // real show entry) borrows the poster of another row of the *same* show -
    // matched by title within the same library - so a stray empty twin still
    // paints the right picture. It never borrows across different shows, which
    // is exactly what a whole-library fallback used to get wrong.
    std::string art_path_for(const Item &item) const
    {
        if (!item.poster.empty())
        {
            return item.poster;
        }
        const std::string name = !item.series_title.empty() ? item.series_title : item.title;
        if (name.empty() || item.library.empty())
        {
            return {};
        }
        for (const Item &sibling : items_)
        {
            if (sibling.library != item.library || sibling.poster.empty())
            {
                continue;
            }
            const std::string other =
                !sibling.series_title.empty() ? sibling.series_title : sibling.title;
            if (other == name)
            {
                return sibling.poster;
            }
        }
        return {};
    }

    // Asks the worker for the pictures this page is about to draw. Cheap when
    // they are already here or on their way, so it runs every frame.
    void request_art()
    {
        auto &service = fnvideo::Service::instance();
        const auto ask_poster = [this, &service](const Item &item)
        {
            // Fetch under the card's own guid, from the path art_path_for
            // picks (its own poster, or a same-titled sibling's). Drawing reads
            // the same guid, so request and paint can never disagree.
            if (item.guid.empty())
            {
                return;
            }
            const std::string path = art_path_for(item);
            if (!path.empty())
            {
                service.request_art(item.guid, path, fnvideo::kPosterWidth);
            }
        };
        const auto ask_still = [this, &service, &ask_poster](const Item &item)
        {
            // Fetch under a key that names the exact art path (still_key), so a
            // wall row that began life as a portrait-poster "backdrop" and then
            // received the real wide key art from the detail call gets refetched
            // instead of keeping the stretched poster forever.
            const std::string key = still_key(item);
            if (key.empty())
            {
                ask_poster(item);
                return;
            }
            const bool wide = !item.backdrop.empty();
            service.request_art(key, wide ? item.backdrop : item.poster,
                                wide ? fnvideo::kStillWidth : fnvideo::kDetailStillFallbackWidth);
        };

        if (zone_ == Zone::home)
        {
            ask_still(item_at(hero_index_));
            for (const int index : home_continue_)
            {
                ask_still(item_at(index));
            }
            for (const int index : home_recent_)
            {
                ask_poster(item_at(index));
            }
        }
        else if (zone_ == Zone::library)
        {
            // Prefetch a window ahead of the viewer, not just the visible rows:
            // the web client loads posters up front so a fast scroll never
            // shows a placeholder. The current screen is queued first (FIFO
            // worker), then a few screens *ahead*, so the wall fills
            // top-to-bottom and the next screens are already downloaded by the
            // time the viewer reaches them. The ahead window is deliberately
            // bounded (roughly one cache's worth) rather than the whole
            // library: a 短剧 wall of 150+ cards prefetched in full would
            // thrash the 64-slot GPU cache and re-download what it just evicted.
            // request_art dedups on art_.wants / art_known_, so calling this
            // every frame costs nothing once a picture is resident or in flight.
            const int count = library_count();
            const int first = grid_scroll_now() * kGridColumns;
            const int end = std::min(count, (grid_scroll_now() + kGridRows) * kGridColumns);
            const int ahead = std::min(count, (grid_scroll_now() + kGridRows + 4) * kGridColumns);
            for (int i = first; i < end; ++i)
            {
                ask_poster(items_[static_cast<std::size_t>(library_item(i))]);
            }
            for (int i = end; i < ahead; ++i)
            {
                ask_poster(items_[static_cast<std::size_t>(library_item(i))]);
            }
        }
        else if (zone_ == Zone::detail)
        {
            // The hero still (its larger poster fallback is folded into
            // ask_still). 相似推荐 was removed, so no recommendation art and no
            // separate small poster are asked for here any more.
            ask_still(detail_item());
            // The web detail page overlays the title's transparent logo lockup
            // on the hero. Ask for it from the cached detail-logo path (only the
            // /item/{guid} answer carries a real "logos"; the list row's logo
            // falls back to the poster and never reaches here). Cached in
            // sync_detail_answer so this per-frame path does not copy the Detail.
            if (!detail_logo_.empty())
                service.request_art("logo:" + detail_logo_, detail_logo_, fnvideo::kPosterWidth);
        }
    }

    // ---- state ----
    ui::Fonts glyphs_{};
    const std::string &version_;
    Zone zone_ = Zone::boot;
    float clock_ = 0.0f;
    float boot_age_ = 0.0f;
    float enter_ = 0.0f;
    int rail_ = 0;
    bool rail_focus_ = false;
    // The 刷新片源 button in the top bar is a focus target reached by pressing
    // Up from the hero (home) or the first grid row (a library / 收藏 tab); it
    // never appears on the 搜索 tab, which is a query, not a shelf.
    bool refresh_focus_ = false;
    int row_ = 0;
    int col_ = 0;
    int grid_ = 0;
    int grid_scroll_ = 0; // first visible row of the poster grid
    // The library shelf projection (indices into items_), rebuilt on a change
    // rather than per frame - see rebuild_shelf(). library_count()/library_item()
    // read this so the focus ring and the remote land on the same filtered set.
    std::vector<int> shelf_;
    bool shelf_dirty_ = true;
    int newest_year_ = 0; // most recent release year in the library, for the
                          // 今年 / 近三年 buckets (the app has no wall clock date)
    // The popup filter panel (see update_filter_panel / draw_filter_panel).
    bool panel_open_ = false;
    int panel_row_ = 0;
    int panel_col_ = 0;
    // The quick strip under the heading: 0 筛选, 1 最近添加, 2 高分, 3 未看. It
    // takes the pad when chip_focus_ is up (reached by Up from the first row).
    bool chip_focus_ = false;
    int chip_ = 0;
    // Selected option per filter dimension (0 = 全部). Order matches kFilterDims:
    // 分类,类型,分辨率,动态范围,音频,国家,年份,观看.
    int f_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    // 0 添加日期 (server / wall order), 1 评分, 2 标题, 3 年份.
    int sort_field_ = 0;
    float filter_toast_ = 0.0f; // "已重置筛选" hint, decays each frame (see update)
    int continue_scroll_ = 0;   // first visible card of the 继续观看 row
    int detail_ = 0;
    std::string detail_guid_;  // the title the page is about, stable across view rebuilds
    std::string detail_error_; // why the last detail fetch failed, if it did
    // The detail answer's transparent logo path, cached so request_art can ask
    // for it every frame without copying the Detail again. Cleared on open.
    std::string detail_logo_;
    Zone detail_back_ = Zone::library; // where Circle returns from the detail page
    int detail_focus_ = 0;
    // The detail page's audio / subtitle chooser: a list panel the two action
    // chips open, so a track is picked from the server's own stream list rather
    // than cycled one at a time. 0 = closed, 1 = audio list, 2 = subtitle list;
    // `detail_track_sel_` is the row under the cursor within the open list.
    int detail_track_menu_ = 0;
    int detail_track_sel_ = 0;
    // The 剧情简介 fold, matching the web detail page: collapsed to two lines
    // with a "□ 展开" cue, unfolded to its full length with "□ 收起". Reset
    // whenever a new title is opened (open_detail).
    bool detail_synopsis_open_ = false;
    int episode_ = 0;
    int settings_row_ = 0;
    bool paused_ = false;
    // B2 diagnosis: seconds a stream has spent in the opening state, and the
    // 5-second bucket last logged. Reset whenever the film leaves opening.
    float open_wait_ = 0.0f;
    int open_log_bucket_ = 0;
    // The player's toolbar focus. `player_bar_` false means left/right seek;
    // true means they walk the chips and Cross turns the focused one.
    bool player_bar_ = false;
    int player_chip_ = 0;
    PlayerMenu player_menu_ = PlayerMenu::none;
    int player_menu_sel_ = 0;
    // The whole player OSD, not just the bar: the title, progress, toolbar and
    // hints leave the picture after a few seconds of uninterrupted playback.
    bool player_osd_ = true;
    float player_idle_ = 0.0f;
    // A transient readout for the analog volume / brightness sliders: the
    // toolbar chips only exist while the bar is open, so a stick adjustment
    // needs its own value on screen. It fades once the stick rests.
    float stick_hud_timer_ = 0.0f;
    std::string stick_hud_text_;
    float stick_hud_fraction_ = 0.0f;
    // L1/R1 长按连发状态：shoulder_key_ 记住当前按住的是哪个肩键，
    // shoulder_hold_ 是距下一次允许续跳的剩余时间（秒）。按下当帧立即跳，
    // 之后越过起始延迟后每到一个连发间隔补跳，与左摇杆节奏一致。
    Action shoulder_key_ = Action::count;
    float shoulder_hold_ = 0.0f;
    std::string search_input_; // the 搜索 shelf's query, searched case-insensitively
    bool search_edit_ = false; // true while the search keyboard is open
    float position_ = 0.0f;
    Rect cursor_{};
    bool cursor_ready_ = false;

    // The shelf on screen: the sample catalogue until the NAS answers, then
    // whatever fnvideo::Service last published.
    std::vector<Item> items_;
    // How many of items_ are library cards. The 继续观看 rows appended after
    // them exist only so the home row can draw an episode that is not a card;
    // the wall, the recent row and the recommendations stop here.
    int library_items_ = 0;
    // The poster wall: one entry per collection, plus the series slot every
    // row belongs to. Rebuilt with the library, so the grid index, the focus
    // ring and the art requests all name the same card.
    std::vector<int> wall_;
    std::vector<int> slot_of_;
    // The detail page's episodes in play order, split into seasons.
    std::vector<Item> detail_episodes_;
    std::vector<SeasonRun> detail_seasons_;
    int season_sel_ = 0;
    std::vector<int> home_continue_;
    std::vector<int> home_recent_;
    // Favourites set on this page, keyed by guid. The API exposes the flag but
    // no write call, so this keeps the star stable for the current launch.
    std::unordered_map<std::string, bool> favorite_overrides_;
    // Guids the viewer watched to the end live on the Service side now
    // (`Service::is_finished`), so 继续观看 and the button label agree with
    // the resume dialog without a second copy here.
    // Resume dialog. Opens when a half-watched film is entered (see
    // start_detail_playback) and offers 继续 mm:ss / 从头播放. The pad owns
    // input while `resume_ask_` is up; `resume_ask_flat_` remembers which
    // detail row (film or episode index) the dialog was opened from so the
    // confirm path can rebuild the chosen Item without duplicating state.
    bool resume_ask_ = false;
    int resume_ask_sel_ = 0; // 0 = 继续, 1 = 从头
    std::string resume_ask_guid_;
    int resume_ask_seconds_ = 0;
    int resume_ask_flat_ = -1;
    int hero_index_ = 0;
    bool hero_picked_ = false;
    bool hero_play_pending_ = false; // a series' banner play is waiting for its episode list
    std::uint64_t hero_seed_ = 0;
    // 随机推荐 is a small rotating banner: up to three library cards the hero
    // cycles through on 首页, auto-advancing every few seconds and flippable by
    // hand with L1/R1. hero_index_ stays the "card on screen" the rest of the
    // page reads (play/detail act on it); it is just sourced from the slot now.
    std::array<int, 3> hero_slots_{-1, -1, -1}; // the picked card indices into items_
    int hero_slot_n_ = 0;                       // how many slots are filled (1..3)
    int hero_slide_ = 0;                        // which slot is showing
    float hero_rotate_ = 0.0f;                  // seconds the current slide has been shown
    float hero_swap_ = 1.0f;                    // 0→1 crossfade after a slide change
    unsigned seen_revision_ = 0;
    unsigned detail_revision_seen_ = 0;
    bool signed_in_ = false;
    std::string status_;
    // The NAS's media libraries (电影/电视剧/其他视频/短剧…), kept so the rail
    // can list them in the server's own order. Emptied with the rest of the
    // library; only meaningful once the worker has signed in.
    std::vector<fnvideo::Library> libraries_;

    // Settings editing. `settings_edit_` is -1 when the form is idle and the
    // row index (0=服务器, 1=账号, 2=密码) while a field is open. The three
    // *_text_ members hold what the user has typed; `edit_buffer_` is the live
    // string while a field is open, and `kb_*` track the on-screen keyboard
    // cursor. The drafts are seeded from the service at enter() so the form
    // shows the address the conf file already supplied.
    int settings_edit_ = -1;
    std::string server_text_;
    std::string account_text_;
    std::string password_text_;
    std::string edit_buffer_;
    // The kit on-screen keyboard used by the three text rows. It paints to the
    // DrawList, owns the pad while a field is open, and is the same control the
    // other concepts use.
    ui::Keyboard keyboard_;

    static constexpr std::array<app::TourStep, 47> kTour = {
        app::TourStep{0.6f, 0u, Direction::none, "fnvideo-boot", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{2.0f, 0u, Direction::none, "fnvideo-home", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.6f, 0u, Direction::down, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.7f, 0u, Direction::none, "fnvideo-continue", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        // Left from the first card hands the focus to the rail; one step down
        // is 电影, and confirm opens the poster wall.
        app::TourStep{0.5f, 0u, Direction::left, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.5f, 0u, Direction::down, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.5f, action_bit(Action::confirm), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.8f, 0u, Direction::none, "fnvideo-library", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        // Walk three rows down the wall. A library longer than the two rows
        // on screen slides its window, so the ring has to still be visible;
        // walking back up leaves the page where the rest of the tour expects.
        app::TourStep{0.4f, 0u, Direction::down, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.4f, 0u, Direction::down, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.4f, 0u, Direction::down, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.8f, 0u, Direction::none, "fnvideo-library-scroll", 0.0f, 0.0f, 0u, 0.0f,
                      0.0f},
        app::TourStep{0.4f, 0u, Direction::up, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.4f, 0u, Direction::up, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.4f, 0u, Direction::up, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.6f, action_bit(Action::confirm), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.8f, 0u, Direction::none, "fnvideo-detail", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        // A film has no episodes; back out and open a series instead, so the
        // episode strip and the player's 第 n 集 are on the record too.
        app::TourStep{0.5f, action_bit(Action::back), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.5f, 0u, Direction::left, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.5f, 0u, Direction::down, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.5f, action_bit(Action::confirm), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.5f, action_bit(Action::confirm), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.8f, 0u, Direction::none, "fnvideo-series", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.6f, action_bit(Action::confirm), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.9f, 0u, Direction::none, "fnvideo-player", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        // Up opens the toolbar, two rights reach the resolution chip, and
        // Cross turns it: the picture the player actually uploads and the
        // decoder behind it are the things this screen exists to control.
        app::TourStep{0.5f, 0u, Direction::up, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.4f, 0u, Direction::right, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.4f, 0u, Direction::right, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.6f, 0u, Direction::none, "fnvideo-player-bar", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.5f, action_bit(Action::confirm), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.6f, 0u, Direction::none, "fnvideo-player-4k", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        // Close the bar so the pad seeks again, then back out to the detail
        // page and walk the rail to 设置.
        app::TourStep{0.4f, 0u, Direction::down, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.5f, action_bit(Action::back), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.5f, action_bit(Action::back), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.5f, 0u, Direction::left, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.4f, 0u, Direction::down, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.4f, 0u, Direction::down, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.4f, 0u, Direction::down, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.4f, 0u, Direction::down, nullptr, 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        app::TourStep{0.5f, action_bit(Action::confirm), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.8f, 0u, Direction::none, "fnvideo-settings", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
        // Back to 首页, then four rail tabs: 电影, 电视剧, 收藏, 搜索. Choosing
        // 搜索 opens its keyboard, which is the state worth photographing.
        app::TourStep{0.5f, action_bit(Action::back), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.4f, action_bit(Action::jump_next), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.4f, action_bit(Action::jump_next), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.4f, action_bit(Action::jump_next), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.4f, action_bit(Action::jump_next), Direction::none, nullptr, 0.0f, 0.0f, 0u,
                      0.0f, 0.0f},
        app::TourStep{0.8f, 0u, Direction::none, "fnvideo-search", 0.0f, 0.0f, 0u, 0.0f, 0.0f},
    };
};

} // namespace

std::unique_ptr<app::Concept> make_fnvideo(app::Context &context)
{
    return std::make_unique<Fnvideo>(context);
}

} // namespace hui::concepts

// ps5-homebrew-ui - Application entry point.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, controller and audio, then runs the shell every frame:
// read input, update, play the sounds it asked for, draw, present. Lifecycle
// markers and frame pacing are logged for hardware runs.

#include "app/shell.hpp"
#include "app/tour.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/frame_stats.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/settings.hpp"
#include "core/version.hpp"
#include "demo/catalog.hpp"
#include "fnvideo/service.hpp"
#include "gfx/canvas.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

#include <cstring>
#include <GL/glcorearb.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

extern "C" void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes,
                               std::size_t *blocks, std::size_t *failures);
extern "C" std::size_t hui_heap_capacity();

namespace
{

constexpr const char *kAssets = "/app0/assets";
// Settings live in the title's own save area.
constexpr const char *kDataRoot = "/download0/hui";
// Development output (the tour's pictures and report, the log). A title's
// sandbox has no /data, so it lives in the title's own storage. While the
// title runs, a PC can *read* the folder over FTP as
// /mnt/sandbox/<TITLE_ID>_000/download0/hui/dev, but cannot write there.
constexpr const char *kDevRoot = "/download0/hui/dev";
// Requests therefore come in through the install folder, which a PC can
// write: /data/homebrew/<TITLE_ID>/dev/request.txt is /app0/dev/request.txt
// here. One line: "tour <design id|all> <token>" or "quit - <token>". A
// request is honoured once per token (the last one is remembered in kDevRoot).
constexpr const char *kRequestPath = "/app0/dev/request.txt";
// How long a finished tour stays up so the PC can copy its evidence (the
// storage is unmounted when the title closes) before the app closes itself.
constexpr double kCollectSeconds = 240.0;
// Quarter of a quarter: the title's storage is small and a tour takes
// hundreds of pictures. Enough to compare layout and colour with the PC's.
constexpr int kCaptureWidth = 480;
constexpr int kCaptureHeight = 270;

// Package fingerprint so a device log grab states unambiguously which build is
// actually running: the compile __TIME__ alone left three near-simultaneous
// upload-path experiments indistinguishable. Bump this tag every measurement.
constexpr const char *HUI_PKG_TAG = "v200-seek-event-driven-resume";

void log_heap(std::uint64_t frames)
{
    std::size_t live = 0;
    std::size_t peak = 0;
    std::size_t blocks = 0;
    std::size_t failures = 0;
    hui_heap_stats(&live, &peak, &blocks, &failures);
    hui::sys::log("[HUI] heap frames=%llu live=%zu/%zuMB peak=%zuMB blocks=%zu failures=%zu",
                  static_cast<unsigned long long>(frames), live >> 20, hui_heap_capacity() >> 20,
                  peak >> 20, blocks, failures);
}

// Opens at *resolution (an index into Settings::kResolutions), falling back
// to 1080p; *resolution reports the mode that opened.

// HDR10 scanout is a whole-session decision (EGL's mode is fixed once the
// context opens, so toggling it means close()/open()). It stays off unless the
// operator asks for it with HUI_FNVIDEO_HDR=1 *and* the ps5-opengl EGL really
// hands back a 10-bit PQ surface, which open_display reports through
// display.is_hdr_scanout(). With the flag unset the app is byte-for-byte the
// ordinary SDR build.
//
// A console launch has no way to set an environment variable, so the same
// switch also reads /app0/dev/hdr.txt - the install folder a PC can write over
// FTP (/data/homebrew/<TITLE_ID>/dev/hdr.txt), the channel request.txt already
// uses. Its content is optional: besides acting as the flag it may carry
// "white=<nits>" and "peak=<nits>" overrides. Staying a file rather than a
// request verb is deliberate - HDR must be decided before the display opens.
bool hdr_requested()
{
    const char *on = std::getenv("HUI_FNVIDEO_HDR");
    if (on != nullptr && on[0] == '1')
        return true;
    std::FILE *const handle = std::fopen("/app0/dev/hdr.txt", "rb");
    return handle != nullptr && std::fclose(handle) == 0;
}

// Optional nits overrides: the environment first (the PC host), then the
// hdr.txt marker (the console). Reference white places UI grey and held-back
// SDR inside HDR; the peak hints the target TV's headroom.
float hdr_env_nits(const char *env_name, const char *file_key, float fallback)
{
    const char *value = std::getenv(env_name);
    if (value != nullptr && value[0] != '\0')
    {
        const float parsed = std::strtof(value, nullptr);
        if (parsed > 0.0f)
            return parsed;
    }
    char text[256] = {};
    std::FILE *const handle = std::fopen("/app0/dev/hdr.txt", "rb");
    if (handle == nullptr)
        return fallback;
    const std::size_t read = std::fread(text, 1, sizeof(text) - 1, handle);
    std::fclose(handle);
    text[read] = '\0';
    const char *hit = std::strstr(text, file_key);
    if (hit == nullptr)
        return fallback;
    const float parsed = std::strtof(hit + std::strlen(file_key), nullptr);
    return parsed > 0.0f ? parsed : fallback;
}

bool open_display(hui::ps5::Display &display, int *resolution)
{
    using hui::Settings;
    const bool hdr = hdr_requested();
    const Settings::Resolution &mode = Settings::kResolutions[*resolution];
    if (display.open(mode.width, mode.height, hdr))
    {
        hui::sys::log("[HUI] display mode %s %dx%d", mode.label, display.width(), display.height());
        return true;
    }
    if (*resolution == 0)
        return false;
    hui::sys::log("[HUI] display mode %s failed, using 1080p", mode.label);
    *resolution = 0;
    return display.open(Settings::kResolutions[0].width, Settings::kResolutions[0].height, hdr);
}

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    // 48 MiB, not read_file's 4 MiB default: the Han atlas is 22 MB, and a
    // rejected read silently leaves Chinese to the Latin faces.
    if (!hui::save::read_file(path, &data, 48u << 20) || !font->load(data))
    {
        hui::sys::log("[HUI] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

// The frame as drawn, bottom row first, as a 24-bit BMP (tour pictures).
bool save_picture(const std::string &path, int width, int height)
{
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const std::uint32_t row = (static_cast<std::uint32_t>(width) * 3 + 3) & ~3u;
    const std::uint32_t size = 54 + row * static_cast<std::uint32_t>(height);
    unsigned char header[54] = {'B', 'M'};
    const auto put = [&](int at, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            header[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, static_cast<std::uint32_t>(width));
    put(22, static_cast<std::uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put(34, size - 54);
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> line(row);
    for (int y = 0; ok && y < height; ++y)
    {
        const unsigned char *in = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x)
        {
            line[static_cast<std::size_t>(x) * 3 + 0] = in[x * 4 + 2];
            line[static_cast<std::size_t>(x) * 3 + 1] = in[x * 4 + 1];
            line[static_cast<std::size_t>(x) * 3 + 2] = in[x * 4 + 0];
        }
        ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
    }
    return std::fclose(file) == 0 && ok;
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[HUI] entry");
    sys::log("[HUI] build %s %s tag=%s", __DATE__, __TIME__, HUI_PKG_TAG);
    sys::log("[HUI] storage dir=%d", save::ensure_directory(kDataRoot) ? 1 : 0);

    // The display opens at the saved resolution (1080p if that fails).
    const Settings saved = app::Shell::load_settings(kDataRoot);
    int resolution = ps5::Display::supports_display_modes() ? saved.resolution : 0;
    ps5::Display display;
    if (!open_display(display, &resolution))
    {
        sys::log("[HUI] fatal: display open failed");
        sys::park();
    }

    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font semibold;
    gfx::Font display_font;
    gfx::Font mono;
    gfx::Font pixel;
    gfx::Font hand;
    gfx::Font cjk;
    ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "montserrat-medium.huifont", &display_font, &fonts.display) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
        !load_font(renderer, "press-start-2p.huifont", &pixel, &fonts.pixel) ||
        !load_font(renderer, "patrick-hand.huifont", &hand, &fonts.hand))
    {
        sys::log("[HUI] fatal: renderer init failed");
        sys::park();
    }
    // HDR10 output only when the scanout really is a 10-bit PQ surface: the env
    // flag asked for it and the EGL fell back to nothing. Turning the shader's
    // PQ domain on against an SDR buffer would darken the whole picture, so this
    // keys off is_hdr_scanout(), not the request. Off here leaves every frame
    // on the existing SDR path.
    if (display.is_hdr_scanout())
    {
        const float white = hdr_env_nits("HUI_FNVIDEO_HDR_WHITE", "white=", 203.0f);
        const float peak = hdr_env_nits("HUI_FNVIDEO_HDR_PEAK", "peak=", 1000.0f);
        renderer.batch().set_hdr_output(true, white, peak);
        sys::log("[HUI] hdr: PQ scanout active, white=%.0fnits peak=%.0fnits", white, peak);
    }
    else if (hdr_requested())
    {
        sys::log("[HUI] hdr: requested but no PQ surface obtained, staying SDR");
    }
    // Han glyphs for the film library. Not fatal: the design checks for it and
    // falls back to the Latin faces, which simply cannot show Chinese.
    if (!load_font(renderer, "wenquanyi-cjk.huifont", &cjk, &fonts.cjk))
        sys::log("[HUI] cjk font missing, Chinese text will draw blank");
    demo::Catalog catalog;
    const std::int64_t art_start = sys::monotonic_us();
    if (!catalog.build_covers(renderer, fonts))
        sys::log("[HUI] cover art failed; designs draw without it");
    sys::log("[HUI] cover art %zu covers in %lld ms", catalog.size(),
             static_cast<long long>((sys::monotonic_us() - art_start) / 1000));

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    audio::Mixer mixer;
    // The video player streams into its own mixer slot; attaching is only
    // valid before the audio thread starts reading. There is deliberately no
    // menu music player: the old 飞牛 standby bed kept playing under the film
    // and was the "待机背景音乐" the owner asked to remove.
    fnvideo::Service::instance().attach_audio(mixer);
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(std::string(kAssets) + "/audio/sfx");
    sys::log("[HUI] sounds files=%d rejected=%d", bank.files, bank.rejected);
    for (const std::string &error : bank.errors)
        sys::log("[HUI] sound rejected %s", error.c_str());

    app::Shell shell(fonts, catalog, kDataRoot, renderer.glass_texture());
    const std::string version = read_content_version("/app0/sce_sys/param.json");
    shell.set_version(version);
    sys::log("[HUI] version %s designs=%zu", version.empty() ? "unknown" : version.c_str(),
             shell.concept_count());

    // A tour request turns this launch into an unattended validation run:
    // every design is shown with scripted input, pictures and a report are
    // written to kDevRoot, and the app closes itself a while later.
    std::unique_ptr<app::Tour> tour;
    save::ensure_directory(kDevRoot);
    const std::string handled_path = std::string(kDevRoot) + "/handled.txt";
    const std::string report_path = std::string(kDevRoot) + "/tour/report.txt";
    bool quit_requested = false;
    const auto poll_request = [&]()
    {
        std::string request;
        if (!save::read_file(kRequestPath, &request, 256))
            return;
        char verb[16] = {};
        char argument[64] = {};
        char token[64] = {};
        if (std::sscanf(request.c_str(), "%15s %63s %63s", verb, argument, token) != 3)
            return;
        std::string handled;
        save::read_file(handled_path, &handled, 128);
        if (handled == token)
            return; // this request was honoured by an earlier launch (or frame)
        save::write_atomic(handled_path, token);
        if (std::strcmp(verb, "quit") == 0)
        {
            quit_requested = true;
        }
        else if (std::strcmp(verb, "tour") == 0 && !tour)
        {
            save::ensure_directory(std::string(kDevRoot) + "/tour");
            std::remove(report_path.c_str()); // a new report must not be taken for an old one
            const bool all = std::strcmp(argument, "all") == 0;
            tour = std::make_unique<app::Tour>(shell, all ? std::string() : std::string(argument));
            sys::log("[HUI] tour requested: %s token=%s", argument, token);
        }
    };
    poll_request();
    double collect_wait = -1.0; // seconds a finished tour has waited to be collected

    std::int64_t previous = sys::monotonic_us();
    std::uint64_t frames = 0;
    int watched_state = -1;
    int watched_beats = 0;
    FrameStats stats;
    // The FPS readout averages over half a second so the number is readable.
    double fps_seconds = 0.0;
    int fps_frames = 0;
    PadSample samples[64];
    gfx::Canvas capture;
    // A picture of the screen, written where it survives the title closing.
    // Reading the display surface back is slow, so the frame is drawn a second
    // time into this small off-screen target instead - the same trick the tour
    // uses. It is taken on request rather than on a timer: a capture costs
    // 200-350 ms, and doing that every five seconds is a stutter in the middle
    // of the very thing being measured.
    gfx::Canvas shot;
    constexpr int kShotWidth = 1280;
    constexpr int kShotHeight = 720;
    // Leaving an empty file in the install folder asks for one picture; the
    // request is consumed, so it happens once. Checking for it costs a stat
    // twice a second. The folder is the persistent one the package lives in,
    // not kDevRoot: that is inside the sandbox and is gone once the title
    // closes, so a request dropped there would never be seen.
    const auto shot_wanted = []() -> bool
    {
        const char *path = "/data/homebrew/PPSA99102/.shot";
        if (std::fopen(path, "rb") == nullptr)
            return false;
        std::remove(path);
        return true;
    };
    std::int64_t last_frame_start = sys::monotonic_us();
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        // Animation time is start-to-start (one full frame), not the gap
        // between the previous swap returning and this frame beginning.
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_frame_start) / 1e6f;
        last_frame_start = now;
        if (dt > 0.05f)
            dt = 0.05f; // a hitch must not teleport the animations
        const std::size_t count = pad.read(samples);
        InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                          static_cast<std::uint64_t>(now));
        if (tour)
            input = tour->step(dt);

        // The video library's posters are decoded off this thread and uploaded
        // on it; this also advances local resume points and picture brightness.
        fnvideo::Service::instance().update(renderer);
        const fnvideo::Service::Playback video_pb = fnvideo::Service::instance().playback();
        shell.update(input, dt);
        if (shell.take_settings_changed())
        {
            const Settings &settings = shell.settings();
            mixer.set_bus_gain(audio::Bus::music, Settings::gain(settings.music_volume));
            mixer.set_bus_gain(audio::Bus::sfx, Settings::gain(settings.sfx_volume));
            mixer.set_bus_gain(audio::Bus::ui, Settings::gain(settings.ui_volume));
            InputSettings input_settings = tracker.settings();
            input_settings.swap_confirm = settings.swap_confirm;
            tracker.set_settings(input_settings);
        }
        const app::Feedback &feedback = shell.feedback();
        for (const audio::CueEvent &event : feedback.cues)
        {
            sounds.play(mixer, event.set == audio::SoundSet::count ? shell.sound_set() : event.set,
                        event);
        }
        if (shell.settings().haptics && feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);
        if (shell.settings().light_bar)
        {
            const gfx::Color accent = shell.accent();
            pad.set_light_bar(static_cast<std::uint8_t>(accent.r * 255.0f),
                              static_cast<std::uint8_t>(accent.g * 255.0f),
                              static_cast<std::uint8_t>(accent.b * 255.0f));
        }

        // Queue the design's backdrop, scene and chrome before presenting.
        // Without this the console clears the display and presents an empty
        // layer list: the PC tour still looks right (it composes itself), but
        // the real title shows nothing at all.
        shell.compose(renderer);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (tour && !tour->capture().empty())
        {
            // The picture is the same frame drawn once more into a small
            // off-screen target: reading the display surface back is slow, and
            // a quarter-size picture is enough to compare with the PC render.
            if (capture.texture() == 0)
                capture.create(kCaptureWidth, kCaptureHeight, 1);
            capture.bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(capture.framebuffer(), kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, capture.framebuffer());
            const std::string path = std::string(kDevRoot) + "/tour/" + tour->capture() + ".bmp";
            const bool ok = save_picture(path, kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            sys::log("[HUI] tour picture %s ok=%d", tour->capture().c_str(), ok ? 1 : 0);
            tour->capture_done();
            last_frame_start = sys::monotonic_us(); // saving is slow; the frame was not
        }
        else if (frames % 30 == 0 && shot_wanted())
        {
            // What is actually on the screen. The player writes the picture the
            // decoder handed it to frame.ppm, so a report of "the image looks
            // doubled" can be answered by comparing the two files instead of by
            // asking for a photograph.
            if (shot.texture() == 0)
                shot.create(kShotWidth, kShotHeight, 1);
            shot.bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(shot.framebuffer(), kShotWidth, kShotHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, shot.framebuffer());
            const bool ok =
                save_picture("/data/homebrew/PPSA99102/shot.bmp", kShotWidth, kShotHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            sys::log("[HUI] shot %llu ok=%d", static_cast<unsigned long long>(frames), ok ? 1 : 0);
            last_frame_start = sys::monotonic_us();
        }
        if (!display.swap())
        {
            sys::log("[HUI] fatal: swap failed frame=%llu error=%s",
                     static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        ++frames;
        const std::int64_t presented = sys::monotonic_us();
        const float frame_ms = static_cast<float>(presented - previous) / 1000.0f;
        app::Telemetry &telemetry = shell.telemetry();
        if (frames == 1)
        {
            sys::log("[HUI] first-swap ok shapes=%zu draws=%zu", renderer.last_instances(),
                     renderer.last_draw_calls());
            const bool hidden = sys::hide_splash_screen();
            sys::log("[HUI] ready splash_hidden=%d", hidden ? 1 : 0);
            log_heap(frames);
        }
        else
        {
            stats.add(static_cast<double>(frame_ms));
            telemetry.push(frame_ms);
            if (tour)
                tour->record_frame(frame_ms, renderer.last_draw_calls(), renderer.last_instances());
        }
        previous = presented;
        telemetry.draw_calls = renderer.last_draw_calls();
        telemetry.instances = renderer.last_instances();
        telemetry.voices = mixer.active_voices();
        fps_seconds += static_cast<double>(frame_ms) / 1000.0;
        ++fps_frames;
        if (fps_seconds >= 0.5)
        {
            telemetry.fps = static_cast<float>(fps_frames / fps_seconds);
            telemetry.average_ms = static_cast<float>(fps_seconds * 1000.0 / fps_frames);
            fps_seconds = 0.0;
            fps_frames = 0;
        }

        if (tour && tour->finished())
        {
            std::string report;
            for (const std::string &line : tour->report())
            {
                sys::log("[HUI] %s", line.c_str());
                report += line + "\n";
            }
            save::write_atomic(report_path, report);
            sys::log("[HUI] tour finished designs=%zu", tour->report().size());
            // The storage is unmounted when the title closes, so the app stays
            // up for a while: the PC copies the evidence, then the app closes.
            tour.reset();
            collect_wait = 0.0;
        }
        if (collect_wait >= 0.0)
            collect_wait += static_cast<double>(frame_ms) / 1000.0;
        if (frames % 60 == 0)
        {
            poll_request();
            if (quit_requested || collect_wait > kCollectSeconds)
            {
                sys::log("[HUI] closing: %s",
                         quit_requested ? "quit requested" : "tour evidence window over");
                pad.close();
                sys::quit();
            }
        }
        // Which thread was alive when the title died: this line comes from the
        // render thread and only runs for a short window after the player
        // changes state, so it cannot bury the decoder's own markers. Two
        // threads write the same log, so the last line of a crash run says
        // where the other one had got to.
        const int stream_state = video_pb.failed    ? 5
                                 : video_pb.ended   ? 4
                                 : video_pb.paused  ? 3
                                 : video_pb.playing ? 2
                                 : video_pb.opening ? 1
                                                    : 0;
        if (stream_state != watched_state)
        {
            watched_state = stream_state;
            // Only while a stream is up: an idle title has nothing to
            // attribute, and beats at boot would bury the ladder's tail.
            watched_beats = (video_pb.playing || video_pb.paused) ? 30 : 0;
        }
        if (watched_beats > 0 && frames % 30 == 0)
        {
            --watched_beats;
            // The underrun count next to the clock is what tells a slow network
            // apart from a muted route: the first climbs while the position
            // moves, the second sits still while nothing is heard.
            sys::log("[HUI] play beat state=%d pos=%d dry=%llu frames=%llu", stream_state,
                     video_pb.position, static_cast<unsigned long long>(mixer.stream_underruns()),
                     static_cast<unsigned long long>(frames));
        }
        if (stats.count() == 600)
        {
            char summary[160];
            stats.format(summary, sizeof(summary));
            sys::log("[HUI] %s draws=%zu shapes=%zu", summary, renderer.last_draw_calls(),
                     renderer.last_instances());
            sys::log("[HUI] audio grains=%llu errors=%llu voices=%d underruns=%llu",
                     static_cast<unsigned long long>(audio_out.grains()),
                     static_cast<unsigned long long>(audio_out.errors()), mixer.active_voices(),
                     static_cast<unsigned long long>(mixer.stream_underruns()));
            stats.reset();
            if (frames % 3600 < 600)
                log_heap(frames);
        }
    }
}

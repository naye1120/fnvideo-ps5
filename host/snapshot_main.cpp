// ps5-homebrew-ui - Headless host renderer: runs the tour and writes PNG files.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: hui_snapshots <assets dir> <output dir> [design id|all] [width height]
//
// Runs the same shell, designs and tour script as the console build, through
// Mesa's surfaceless EGL (llvmpipe), with a fixed 60 Hz clock. Only the frames
// the tour marks are rendered, so a full run takes seconds. With
// HUI_STRIP=<from>,<to> it instead writes every frame of the switch between
// two designs (for reviewing the transition).
//
// HUI_REEL=<dir> records instead of photographing: every third simulated frame
// is piped to ffmpeg, which writes animated WebP clips into <dir>.
//   HUI_REEL_SPLIT=1   one clip per tour picture (the Theme Lab: one per theme)
//   otherwise          one clip per design, its whole tour
//   HUI_REEL=<dir> HUI_REEL_SWITCH=1   one clip of L1/R1 cycling through all designs

#include "app/shell.hpp"
#include "app/tour.hpp"
#include "audio/mixer.hpp"
#include "core/save_file.hpp"
#include "core/version.hpp"
#include "demo/catalog.hpp"
#include "fnvideo/service.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "manifest.hpp"
#include "ui/fonts.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr
            ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                         4,
                                         EGL_CONTEXT_MINOR_VERSION,
                                         5,
                                         EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                         EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                         EGL_NONE};
    EGLContext context =
        eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    // 48 MiB, not read_file's 4 MiB default: the Han atlas is 22 MB.
    if (!hui::save::read_file(path, &data, 48u << 20) || !font->load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

// Streams raw frames to ffmpeg and gives the finished clip its name on close
// (the name is only known once the tour reaches the picture that ends it).
class Recorder
{
  public:
    Recorder(std::string directory, int width, int height, int fps)
        : directory_(std::move(directory)), width_(width), height_(height), fps_(fps)
    {
    }
    ~Recorder()
    {
        // Frames after the last picture belong to no clip.
        if (pipe_ != nullptr)
        {
            pclose(pipe_);
            std::remove((directory_ + "/.recording.webp").c_str());
        }
    }
    void frame(const unsigned char *rgba)
    {
        if (pipe_ == nullptr)
        {
            char command[512];
            std::snprintf(command, sizeof(command),
                          "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - "
                          "-vf vflip -c:v libwebp_anim -lossless 0 -q:v 62 -compression_level 5 "
                          "-loop 0 '%s/.recording.webp'",
                          width_, height_, fps_, directory_.c_str());
            pipe_ = popen(command, "w");
            frames_ = 0;
        }
        if (pipe_ != nullptr)
        {
            std::fwrite(rgba, 1, static_cast<std::size_t>(width_) * height_ * 4, pipe_);
            ++frames_;
        }
    }
    void close(const std::string &name)
    {
        if (pipe_ == nullptr)
            return;
        pclose(pipe_);
        pipe_ = nullptr;
        const std::string from = directory_ + "/.recording.webp";
        const std::string to = directory_ + "/" + name + ".webp";
        std::rename(from.c_str(), to.c_str());
        std::fprintf(stderr, "recorded %s: %d frames\n", to.c_str(), frames_);
    }

  private:
    std::string directory_;
    int width_;
    int height_;
    int fps_;
    FILE *pipe_ = nullptr;
    int frames_ = 0;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <assets dir> <output dir> [design id|all] [width height]\n",
                     argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string output = argv[2];
    const std::string only = argc > 3 && std::string(argv[3]) != "all" ? argv[3] : "";
    const int width = argc > 5 ? std::atoi(argv[4]) : 1920;
    const int height = argc > 5 ? std::atoi(argv[5]) : 1080;

    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    std::fprintf(stderr, "GL %s / %s\n", reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    hui::gfx::set_glsl_prefix("#version 450 core\n");

    hui::gfx::Renderer renderer;
    hui::gfx::Font regular;
    hui::gfx::Font semibold;
    hui::gfx::Font display;
    hui::gfx::Font mono;
    hui::gfx::Font pixel;
    hui::gfx::Font hand;
    hui::gfx::Font cjk;
    hui::ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, assets + "/fonts/inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, assets + "/fonts/inter-semibold.huifont", &semibold,
                   &fonts.semibold) ||
        !load_font(renderer, assets + "/fonts/montserrat-medium.huifont", &display,
                   &fonts.display) ||
        !load_font(renderer, assets + "/fonts/dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
        !load_font(renderer, assets + "/fonts/press-start-2p.huifont", &pixel, &fonts.pixel) ||
        !load_font(renderer, assets + "/fonts/patrick-hand.huifont", &hand, &fonts.hand))
        return 1;
    // Han glyphs for the video player's own page; the other designs never ask.
    if (!load_font(renderer, assets + "/fonts/wenquanyi-cjk.huifont", &cjk, &fonts.cjk))
        std::fprintf(stderr, "cjk font missing, Chinese draws blank\n");
    hui::demo::Catalog catalog;
    if (!catalog.build_covers(renderer, fonts))
    {
        std::fprintf(stderr, "cover art failed\n");
        return 1;
    }

    // HUI_ICON=<file.png>: draw the 512x512 launcher icon with the kit and stop.
    if (const char *icon_path = std::getenv("HUI_ICON"))
    {
        constexpr int kIcon = 512;
        GLuint target = 0;
        GLuint storage = 0;
        glGenFramebuffers(1, &target);
        glGenRenderbuffers(1, &storage);
        glBindRenderbuffer(GL_RENDERBUFFER, storage);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kIcon, kIcon);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, storage);
        using hui::gfx::Color;
        hui::gfx::BackdropSpec spec;
        spec.mode = hui::gfx::BackdropMode::aurora;
        spec.colors[0] = Color::rgb(0x0b0e20);
        spec.colors[1] = Color::rgb(0x1b1450);
        spec.colors[2] = Color::rgb(0x3b6cff);
        spec.colors[3] = Color::rgb(0x7cf0c8);
        spec.time = 42.0f;
        hui::gfx::DrawList list;
        // Three cards fanned out, the front one focused: the app in one glance.
        list.rotated_rect({118, 150, 230, 150}, 26, -0.28f, Color::rgb(0xffffff, 0.16f));
        list.rotated_rect({150, 136, 230, 150}, 26, -0.12f, Color::rgb(0xffffff, 0.3f));
        list.shadow({170, 150, 250, 160}, 30, 40, Color::rgb(0x000000, 0.5f));
        list.glow({166, 128, 250, 160}, 30, 26, Color::rgb(0x7cf0c8, 0.55f));
        list.gradient_rect({166, 128, 250, 160}, 30, Color::rgb(0xf4f7ff), Color::rgb(0xcfd8ff));
        list.bordered_rect({160, 122, 262, 172}, 34, Color::rgb(0x000000, 0.0f), 5,
                           Color::rgb(0x7cf0c8));
        list.rounded_rect({196, 164, 96, 18}, 9, Color::rgb(0x1b1450));
        list.rounded_rect({196, 198, 160, 12}, 6, Color::rgb(0x1b1450, 0.45f));
        list.rounded_rect({196, 222, 124, 12}, 6, Color::rgb(0x1b1450, 0.45f));
        list.arc(366, 176, 30, 9, 0.0f, 4.4f, Color::rgb(0x3b6cff));
        hui::ui::text(list, fonts.display, "UI Lab", 256, 402, 82, Color::rgb(0xffffff),
                      hui::gfx::Align::center);
        hui::ui::text(list, fonts.semibold, "HOMEBREW", 256, 446, 24, Color::rgb(0x7cf0c8),
                      hui::gfx::Align::center, 9.0f);
        renderer.begin();
        renderer.backdrop(spec);
        renderer.draw(list);
        renderer.present(target, kIcon, kIcon, 512.0f, 512.0f);
        std::vector<unsigned char> icon(static_cast<std::size_t>(kIcon * kIcon * 4));
        glReadPixels(0, 0, kIcon, kIcon, GL_RGBA, GL_UNSIGNED_BYTE, icon.data());
        for (std::size_t i = 3; i < icon.size(); i += 4)
            icon[i] = 255;
        stbi_flip_vertically_on_write(1);
        return stbi_write_png(icon_path, kIcon, kIcon, 4, icon.data(), kIcon * 4) != 0 ? 0 : 1;
    }

    GLuint framebuffer = 0;
    GLuint color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;

    // Settings are not read from (or written to) a real save: a scratch folder.
    const std::string data_root = output + "/.data";
    hui::save::ensure_directory(output);
    hui::save::ensure_directory(data_root);
    std::remove((data_root + "/settings.bin").c_str());
    hui::app::Shell shell(fonts, catalog, data_root, renderer.glass_texture());
    // The pictures are of the product, so they carry its own number: the same
    // param.json the console reads, from the tree this host runs in.
    const std::string packaged = hui::read_content_version("sce_sys/param.json");
    shell.set_version(packaged.empty() ? "host" : packaged);

    // The video player photographs nothing worth comparing while it is asking
    // for a server: every shelf is a login form. The preview host in
    // src/fnos/http.cpp answers a sign-in with a demo library, so the shelves,
    // the grid and the play page have rows to lay out. No audio device is
    // opened - the mixer only has to exist for the player to attach to.
    hui::audio::Mixer mixer;
    hui::fnvideo::Service &svc = hui::fnvideo::Service::instance();
    svc.attach_audio(mixer);
    svc.ensure_started();

    constexpr float kDt = 1.0f / 60.0f;
    // The console's frame loop ticks the video service between its own updates;
    // this host has a loop of its own, so the answers, the covers and the end of
    // a track have to be said here as well - and in the console's order, before
    // the page is asked to draw. Told afterwards, a finished answer is one frame
    // invisible to the page, and a shelf reads as an empty one.
    const auto tick = [&](const hui::InputFrame &input)
    {
        svc.update(renderer);
        shell.update(input, kDt);
    };
    svc.configure("http://preview.local", "admin", "hunter2");
    // The worker runs on the wall clock while the tour runs on a simulated one,
    // so the sign-in is given the time to come back before any frame is
    // photographed. `busy()` only covers the queue, and an answer that has
    // finished but not landed yet is exactly the state worth waiting out.
    for (int pass = 0; pass < 300 && !svc.view().signed_in; ++pass)
    {
        hui::InputFrame idle;
        idle.connected = true;
        tick(idle);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // HUI_MANIFEST=<file.json>: describe every design and theme (host/manifest.cpp).
    if (const char *manifest = std::getenv("HUI_MANIFEST"))
        return hui::host::write_manifest(manifest, shell) ? 0 : 1;

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
    stbi_flip_vertically_on_write(1);
    bool ok = true;
    const auto render = [&](const std::string &name)
    {
        shell.compose(renderer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        renderer.present(framebuffer, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        // The PNG is opaque whatever the blending left in the alpha channel.
        for (std::size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
        const std::string path = output + "/" + name + ".png";
        ok = stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0 && ok;
        std::fprintf(stderr, "wrote %s: %zu shapes, %zu draw calls, GL error 0x%x\n", path.c_str(),
                     renderer.last_instances(), renderer.last_draw_calls(), glGetError());
    };

    if (const char *strip = std::getenv("HUI_STRIP"))
    {
        int from = 0;
        int to = 1;
        std::sscanf(strip, "%d,%d", &from, &to);
        hui::InputFrame idle;
        idle.connected = true;
        shell.show(static_cast<std::size_t>(from), false);
        for (int frame = 0; frame < 120; ++frame)
            tick(idle);
        shell.show(static_cast<std::size_t>(to), true);
        for (int frame = 0; frame < 36; ++frame)
        {
            tick(idle);
            char name[32];
            std::snprintf(name, sizeof(name), "strip-%02d", frame);
            render(name);
        }
        return ok ? 0 : 1;
    }

    if (const char *reel = std::getenv("HUI_REEL"))
    {
        constexpr int kEvery = 3; // 60 Hz simulated, 20 frames per second recorded
        hui::save::ensure_directory(reel);
        Recorder recorder(reel, width, height, 60 / kEvery);
        const auto record = [&]()
        {
            shell.compose(renderer);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(framebuffer, width, height);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            recorder.frame(pixels.data());
        };
        hui::InputFrame idle;
        idle.connected = true;
        long count = 0;
        if (std::getenv("HUI_REEL_SWITCH") != nullptr)
        {
            // R1 through every design, then the info panel: the app in one clip.
            shell.show(0, false);
            for (std::size_t i = 0; i <= shell.concept_count(); ++i)
            {
                for (int frame = 0; frame < 66; ++frame, ++count)
                {
                    hui::InputFrame input = idle;
                    if (frame == 0 && i > 0)
                        input.pressed = hui::action_bit(hui::Action::page_next);
                    tick(input);
                    if (count % kEvery == 0)
                        record();
                }
            }
            recorder.close("switcher");
            return 0;
        }
        const bool split = std::getenv("HUI_REEL_SPLIT") != nullptr;
        hui::app::Tour tour(shell, only);
        std::size_t design = shell.current();
        char name[96];
        while (!tour.finished() && count < 60 * 60 * 60)
        {
            const hui::InputFrame input = tour.step(kDt);
            if (!split && shell.current() != design)
            {
                std::snprintf(name, sizeof(name), "%s", shell.concept_at(design).info().id);
                recorder.close(name);
                design = shell.current();
            }
            tick(input);
            if (count % kEvery == 0)
                record();
            ++count;
            if (!tour.capture().empty())
            {
                if (split)
                    recorder.close(tour.capture());
                tour.capture_done();
            }
        }
        if (!split)
            recorder.close(shell.concept_at(design).info().id);
        return 0;
    }

    hui::app::Tour tour(shell, only);
    if (tour.finished())
    {
        std::fprintf(stderr, "no design named '%s'\n", only.c_str());
        return 2;
    }
    hui::InputFrame idle_frame;
    idle_frame.connected = true;
    long frames = 0;
    while (!tour.finished() && frames < 60 * 60 * 60)
    {
        const hui::InputFrame input = tour.step(kDt);
        // Nothing is presented here, so the numbers a console would measure
        // are stood in for: 60 Hz with a little jitter and a rare long frame.
        hui::app::Telemetry &telemetry = shell.telemetry();
        const float jitter = static_cast<float>((frames * 7919) % 97) / 97.0f;
        telemetry.push(16.3f + jitter * 0.8f + (frames % 211 == 0 ? 5.5f : 0.0f));
        telemetry.fps = 59.9f;
        telemetry.average_ms = 16.7f;
        telemetry.voices = static_cast<int>(frames / 40 % 4);
        telemetry.draw_calls = renderer.last_draw_calls();
        telemetry.instances = renderer.last_instances();
        // The library answers on the wall clock while the tour counts simulated
        // seconds. A frame is not fed to the page while one of its reads is
        // still open, so a press lands on a page that already has its rows, and
        // one script lays out one page every run.
        for (int pass = 0; pass < 300 && svc.view().busy; ++pass)
        {
            tick(idle_frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        tick(input);
        ++frames;
        // A press starts an entrance animation and usually asks for a fresh
        // read, and both run on clocks the tour does not count. So the frame
        // after a press is followed by a run of idle frames that begins only
        // once the read the press caused has actually shown up on the queue,
        // and ends once the page has then sat with nothing pending for two
        // whole seconds. Every picture is therefore taken past the end of the
        // animation, from the same point of it every run, rather than from
        // wherever the wall clock happened to be when the rows arrived.
        if (input.pressed != 0)
        {
            bool asked = false;
            for (int pass = 0, quiet = 0; pass < 1800 && quiet < 120; ++pass)
            {
                if (svc.view().busy)
                {
                    asked = true;
                    quiet = 0;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                else if (!asked && pass < 60)
                {
                    // A job is only on the queue a poll after the press lands,
                    // so the first second of waiting is grace, not quiet.
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                else
                {
                    ++quiet;
                }
                tick(idle_frame);
            }
        }
        if (!tour.capture().empty())
        {
            render(tour.capture());
            tour.capture_done();
        }
        else
        {
            shell.rehearse(); // as on the console, every frame is drawn
        }
    }
    std::fprintf(stderr, "tour: %ld frames simulated\n", frames);
    return ok ? 0 : 1;
}

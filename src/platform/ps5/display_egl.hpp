// ps5-homebrew-ui - EGL display and OpenGL 4.6 Core context.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <EGL/egl.h>

namespace hui::ps5
{

// Owns the EGL display, window surface and GL 4.6 Core context. ps5-opengl
// presents fullscreen; with runtime display modes the size is chosen before
// EGL starts, so changing it means close() and open() again. Every GL object
// dies with the context and must be recreated by its owner.
class Display
{
  public:
    Display() = default;
    Display(const Display &) = delete;
    Display &operator=(const Display &) = delete;
    ~Display();

    // Opens at width x height (1920x1080, 2560x1440 or 3840x2160) when the
    // SDK supports runtime display modes, else at the SDK's fixed profile.
    // When hdr is requested the window surface is created from a 10-bit
    // BT.2020 PQ config; if the SDK cannot provide one the open falls back to
    // the ordinary RGBA8 config, so asking for HDR never risks a black screen.
    bool open(int width = 1920, int height = 1080, bool hdr = false);
    bool swap();
    void close();
    // Reopens at the last requested size with the HDR target changed. Every GL
    // object dies with the old context and must be rebuilt by its owner.
    bool restart_hdr(bool hdr);
    // True when the SDK can change the display size at runtime.
    static bool supports_display_modes();

    int width() const
    {
        return width_;
    }
    int height() const
    {
        return height_;
    }
    // True once the scanout is actually a 10-bit PQ surface (not merely asked
    // for), and whether the SDK can offer one at all (from the startup probe).
    bool is_hdr_scanout() const
    {
        return hdr_;
    }
    bool is_hdr_capable() const
    {
        return hdr_capable_;
    }
    EGLint last_error() const
    {
        return last_error_;
    }

  private:
    void fail(const char *operation);
    // Tries the 10:10:10:2 + BT.2020 PQ window config; returns it when the SDK
    // has one, and records whether any PQ config exists in hdr_capable_.
    bool choose_pq_config(EGLConfig &config);

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    int width_ = 0;
    int height_ = 0;
    int requested_width_ = 0;
    int requested_height_ = 0;
    bool hdr_ = false;
    bool hdr_capable_ = false;
    EGLint last_error_ = EGL_SUCCESS;
};

const char *egl_error_name(EGLint error);

} // namespace hui::ps5

// ps5-homebrew-ui - The shell: switches between designs and draws shared chrome.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/shell.hpp"

#include "core/save_file.hpp"
#include "platform/ps5/system.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cstdio>

namespace hui::app
{

namespace
{

using gfx::Color;

constexpr float kTransitionSeconds = 0.5f;
constexpr float kBannerSeconds = 3.2f;
constexpr std::uint32_t kShellActions = action_bit(Action::touch);
// L1/R1 switch designs, but only when there is more than one to switch to.
// With a single design in the registry the shell has nowhere to go, and taking
// the press would leave the player's own shoulder buttons dead.
constexpr std::uint32_t kSwitchActions =
    action_bit(Action::page_prev) | action_bit(Action::page_next);

std::string settings_path(const std::string &data_root)
{
    return data_root + "/settings.bin";
}

} // namespace

Settings Shell::load_settings(const std::string &data_root)
{
    Settings settings;
    std::string data;
    if (save::read_file(settings_path(data_root), &data))
    {
        const save::Decoded decoded = save::decode(save::Kind::settings, data);
        if (!decoded.ok || !decode_settings(decoded.payload, &settings))
            sys::log("[HUI] settings unreadable (%s), using defaults", decoded.error.c_str());
    }
    return settings;
}

Shell::Shell(const ui::Fonts &fonts, const demo::Catalog &catalog, std::string data_root,
             std::uint32_t glass_texture)
    : fonts_(fonts), data_root_(std::move(data_root)), settings_(load_settings(data_root_)),
      context_{fonts_, catalog, telemetry_, settings_, version_}, glass_texture_(glass_texture)
{
    for (ConceptFactory factory : concept_registry())
        concepts_.push_back(factory(context_));
    if (!concepts_.empty())
    {
        current_ = static_cast<std::size_t>(settings_.concept_index) % concepts_.size();
        previous_ = current_;
        concepts_[current_]->enter();
    }
    // The switcher banner names the design and counts it out of the registry.
    // With one design in the product there is nothing to count, and its tagline
    // is Han text drawn with the kit's Latin faces: it comes out as a row of
    // question marks. So it only goes up when there is a switcher to explain.
    banner_ = concepts_.size() > 1 ? kBannerSeconds : 0.0f;
}

audio::SoundSet Shell::sound_set() const
{
    return concepts_.empty() ? audio::SoundSet::glass : concepts_[current_]->info().sounds;
}

gfx::Color Shell::accent() const
{
    return concepts_.empty() ? Color::rgb(0xffffff) : concepts_[current_]->info().accent;
}

bool Shell::take_settings_changed()
{
    const bool changed = settings_changed_;
    settings_changed_ = false;
    return changed;
}

void Shell::save_settings()
{
    const std::string error =
        save::write_atomic(settings_path(data_root_),
                           save::encode(save::Kind::settings, 1, encode_settings(settings_)));
    if (!error.empty())
        sys::log("[HUI] settings save failed: %s", error.c_str());
}

void Shell::show(std::size_t index, bool animate)
{
    if (concepts_.empty())
        return;
    index %= concepts_.size();
    if (index == current_)
        return;
    previous_ = current_;
    current_ = index;
    concepts_[current_]->enter();
    banner_ = kBannerSeconds;
    if (animate)
        transition_.start(settings_.reduced_motion ? 0.2f : kTransitionSeconds);
    else
        transition_.running = false;
    settings_.concept_index = static_cast<int>(current_);
    save_delay_ = 1.5f;
}

void Shell::set_info_open(bool open)
{
    info_open_ = open;
}

void Shell::update(const InputFrame &input, float dt)
{
    feedback_.clear();
    if (concepts_.empty())
        return;
    if (!welcomed_)
    {
        welcomed_ = true;
        feedback_.play(audio::Cue::welcome);
    }

    const int count = static_cast<int>(concepts_.size());
    if (count > 1 && (input.is_pressed(Action::page_next) || input.is_pressed(Action::page_prev)))
    {
        direction_ = input.is_pressed(Action::page_next) ? 1 : -1;
        show(static_cast<std::size_t>((static_cast<int>(current_) + direction_ + count) % count),
             true);
        feedback_.play(audio::Cue::tab, 1.0f, direction_ > 0 ? 0.3f : -0.3f);
    }
    // The design-info drawer ("DESIGN nn OF nn / WHAT IT SHOWS / SOURCE") was
    // removed at the owner's request: with a single shipping design it only ever
    // showed a Latin-font row of question marks where the Han title should be,
    // so the touchpad no longer opens anything.

    // The active design sees everything the shell does not use; nothing at
    // all while the info panel covers it.
    const std::uint32_t eaten = kShellActions | (concepts_.size() > 1 ? kSwitchActions : 0u);
    InputFrame forwarded = input;
    forwarded.pressed &= ~eaten;
    forwarded.released &= ~eaten;
    forwarded.held &= ~eaten;
    if (info_open_)
    {
        forwarded = InputFrame{};
        forwarded.connected = input.connected;
    }
    concepts_[current_]->update(forwarded, dt, feedback_);

    if (context_.settings_changed)
    {
        context_.settings_changed = false;
        settings_changed_ = true;
        save_delay_ = 0.75f; // sliders change every frame: save once they rest
    }
    if (save_delay_ > 0.0f)
    {
        save_delay_ -= dt;
        if (save_delay_ <= 0.0f)
            save_settings();
    }

    transition_.update(dt);
    banner_ = std::max(0.0f, banner_ - dt);
    banner_show_.target = banner_ > 0.0f && !info_open_ ? 1.0f : 0.0f;
    banner_show_.update(dt, 10.0f);
    info_show_.target = info_open_ ? 1.0f : 0.0f;
    info_show_.update(dt, settings_.reduced_motion ? 40.0f : 14.0f);
    telemetry_.uptime += static_cast<double>(dt);
}

void Shell::record(Frame &frame, const Concept &design, float opacity, float dx, float scale)
{
    frame.reset();
    frame.glass_texture = glass_texture_;
    if (opacity < 1.0f || dx != 0.0f || scale != 1.0f)
    {
        for (gfx::DrawList *list : {&frame.scene, &frame.overlay})
        {
            list->push_opacity(opacity);
            list->push_transform(scale, gfx::kVirtualWidth * 0.5f, gfx::kVirtualHeight * 0.5f, dx,
                                 0.0f);
        }
    }
    design.draw(frame);
}

void Shell::rehearse()
{
    if (!concepts_.empty())
        record(frames_[0], *concepts_[current_], 1.0f, 0.0f, 1.0f);
}

void Shell::draw_chrome()
{
    const ConceptInfo &info = concepts_[current_]->info();
    const ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
    const Color white = Color::rgb(0xffffff);
    char text[96];

    // ---- switcher banner: "L1  03 / 12  Name  R1", then the tagline ----
    const float banner = banner_show_.value;
    if (banner > 0.01f)
    {
        std::snprintf(text, sizeof(text), "%02zu / %02zu", current_ + 1, concepts_.size());
        const float count_w = fonts_.mono.measure(text, 22);
        const float name_w = fonts_.semibold.measure(info.name, 28);
        const float l1 = ui::button_width(ui::Button::l1, 36);
        const float width = 28 + l1 + 22 + count_w + 20 + name_w + 22 + l1 + 28;
        const float x = 960 - width * 0.5f;
        const float y = 34 - 30 * (1.0f - banner);
        chrome_.push_opacity(banner);
        chrome_.shadow({x, y + 10, width, 64}, 32, 30, Color::rgb(0x000000, 0.45f));
        chrome_.bordered_rect({x, y, width, 64}, 32, Color::rgb(0x0b0d16, 0.88f), 1.5f,
                              info.accent.with_alpha(0.55f));
        float cursor = x + 28;
        ui::draw_button(chrome_, fonts_, glyphs, ui::Button::l1, cursor, y + 32, 36);
        cursor += l1 + 22;
        ui::text(chrome_, fonts_.mono, text, cursor, y + 40, 22, white.with_alpha(0.6f));
        cursor += count_w + 20;
        ui::text(chrome_, fonts_.semibold, info.name, cursor, y + 42, 28, white);
        cursor += name_w + 22;
        ui::draw_button(chrome_, fonts_, glyphs, ui::Button::r1, cursor, y + 32, 36);
        const float tag_w = fonts_.regular.measure(info.tagline, 20) + 40;
        chrome_.rounded_rect({960 - tag_w * 0.5f, y + 74, tag_w, 36}, 18,
                             Color::rgb(0x0b0d16, 0.7f));
        ui::text(chrome_, fonts_.regular, info.tagline, 960, y + 99, 20, white.with_alpha(0.82f),
                 gfx::Align::center);
        chrome_.pop_opacity();
    }

    // The design-info drawer is gone by request; `info_open_` is never set, so
    // nothing draws here. The FPS overlay below is the only remaining chrome.

    if (settings_.show_fps && telemetry_.fps > 0.0f)
    {
        std::snprintf(text, sizeof(text), "%3.0f FPS  %4.1f ms  %zu draws  %zu shapes",
                      static_cast<double>(telemetry_.fps),
                      static_cast<double>(telemetry_.average_ms), telemetry_.draw_calls,
                      telemetry_.instances);
        const float w = fonts_.mono.measure(text, 18) + 28;
        chrome_.rounded_rect({20, 1036, w, 30}, 8, Color::rgb(0x000000, 0.55f));
        ui::text(chrome_, fonts_.mono, text, 34, 1058, 18, white.with_alpha(0.9f));
    }
}

} // namespace hui::app

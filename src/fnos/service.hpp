// ps5-homebrew-ui - Everything the player screen needs, in one object the
// frame loop owns: the NAS library, the decoder thread and the play queue.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A concept is built by a factory that receives no services, so the screens
// reach this through instance(). It is created at start-up, before the shell,
// because the player must attach to the mixer before the audio thread runs.

#pragma once

#include "fnos/art.hpp"
#include "fnos/library.hpp"
#include "fnos/player.hpp"

#include <string>
#include <vector>

namespace hui::gfx
{
class Renderer;
}

namespace hui::audio
{
class Mixer;
}

namespace hui::fnos
{

// Repeat modes, in the order the repeat button cycles through them.
enum class Repeat : int
{
    off = 0,
    all = 1,
    one = 2,
};

// The queue is a copy of the shelf it came from: leaving for another shelf
// must not change what keeps playing.
class Service
{
  public:
    static Service &instance();

    // Attaches the player (call before the audio thread starts), then opens
    // the library worker and reads the saved session.
    bool start(audio::Mixer &mixer, const std::string &data_root);
    // The last line the previous launch wrote before it stopped: a title that
    // died leaves no other trace on a machine with no console attached.
    const std::string &crash_trace() const
    {
        return crash_trace_;
    }
    // The steps before that last line, oldest first: which thread the death
    // was on shows in the run of markers, not in one of them.
    const std::vector<std::string> &crash_lines() const
    {
        return crash_lines_;
    }
    // Uploads covers, moves finished answers onto the shelves and follows the
    // end of a track. Once per frame, on the render thread.
    void update(gfx::Renderer &renderer);

    Library &library()
    {
        return library_;
    }
    ArtCache &art()
    {
        return art_;
    }
    Player &player()
    {
        return player_;
    }

    // ---- the queue ----
    void play_queue(const std::vector<Track> &tracks, int index);
    void toggle_play();
    void next();
    void previous();
    void seek_to(int seconds);
    void stop();
    // 0..1; the player's gain is this squared, so the middle feels like half.
    void set_volume(float level);
    float volume() const
    {
        return volume_;
    }
    void toggle_shuffle();
    bool shuffle() const
    {
        return shuffle_;
    }
    void cycle_repeat();
    Repeat repeat() const
    {
        return repeat_;
    }
    int index() const
    {
        return index_;
    }
    int queue_size() const
    {
        return static_cast<int>(queue_.size());
    }
    const std::vector<Track> &queue() const
    {
        return queue_;
    }
    const Track *current() const;

  private:
    Service() = default;
    void play_at(int index);
    // Asks the library worker for the next song's address while this one is
    // still playing out.
    void arm_prefetch();

    Library library_;
    std::string crash_trace_;
    std::vector<std::string> crash_lines_;
    ArtCache art_;
    Player player_;
    std::vector<Track> queue_;
    int index_ = -1;
    // The queue index whose address the worker has already been asked for, so
    // one song buys one fetch and not one per frame.
    int prefetched_ = -1;
    bool shuffle_ = false;
    Repeat repeat_ = Repeat::off;
    float volume_ = 1.0f;
    // Where a random next track may not land on twice in a row.
    int last_shuffle_ = -1;
};

} // namespace hui::fnos

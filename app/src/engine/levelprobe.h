// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Peak metering for the sinks in the mixer graph.
//
// PipeWire nodes do not publish a level, so the only way to know what is
// actually flowing through a channel is to listen to it. Each watched sink
// gets one capture stream on its monitor ports which does nothing but track
// the peak sample -- the same thing pavucontrol does for its meters.
//
// This matters more than it looks: a link existing in the graph does not mean
// it carries signal, and every routing bug in this project so far has looked
// correct in `pw-link -l` and silent in the ears. A meter per channel is the
// cheapest way to see the difference.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace waveline {

class LevelProbe {
public:
    LevelProbe();
    ~LevelProbe();
    LevelProbe(const LevelProbe &) = delete;
    LevelProbe &operator=(const LevelProbe &) = delete;

    bool start(std::string &error);
    void stop();

    // Listens to `sinkName`'s monitor and reports under `key`. Safe to call
    // before the sink exists: the stream stays idle until it appears.
    // sourceIsSink=false when `nodeName` is already a source (a virtual
    // microphone), so the tap reads it directly instead of asking for a
    // monitor it does not have.
    bool watch(const std::string &key, const std::string &nodeName,
               std::string &error, bool sourceIsSink = true);

    // Drops a tap. Needed because a capture stream whose target does not exist
    // silently falls back to the default source -- the microphone -- so every
    // meter for an absent node reads the same wrong thing.
    bool unwatch(const std::string &key);

    // Peak amplitude since the last call, 0..1, with a decay so the value is
    // readable at any poll rate. Unknown keys read 0.
    std::vector<std::pair<std::string, float>> levels() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline

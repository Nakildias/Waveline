// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Per-channel EQ and low-cut as a PipeWire filter node.

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "eqspec.h"

namespace waveline {

struct ChannelFxSettings {
    bool lowCut = false;
    int lowCutHz = 80;   // 80 or 120
    bool eq = false;
    float lowDb = 0.0f;
    float midDb = 0.0f;
    float highDb = 0.0f;
    // Which EQ the `eq` switch turns on: the three fixed bands above, or the
    // ten parametric ones below. Both are kept whichever is selected, so
    // switching back and forth does not throw a curve away.
    bool eqAdvanced = false;
    EqBands bands = defaultEqBands();

    bool active() const {
        if (lowCut) return true;
        if (!eq) return false;
        // An advanced EQ with nothing switched on is a pass-through, and
        // saying so lets the whole chain bypass rather than run ten neutral
        // filters on every channel someone once opened the panel for.
        return !eqAdvanced || anyEqBandOn(bands);
    }
};

class EffectsFilter {
public:
    EffectsFilter();
    ~EffectsFilter();
    EffectsFilter(const EffectsFilter &) = delete;
    EffectsFilter &operator=(const EffectsFilter &) = delete;

    // asSource publishes the node as an Audio/Source, which is how a channel's
    // microphone chain becomes selectable in Discord and friends.
    bool start(const std::string &nodeName, const std::string &description,
               int channels, std::string &error, bool asSource = false);
    void stop();

    void setSettings(const ChannelFxSettings &s);
    ChannelFxSettings settings() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline

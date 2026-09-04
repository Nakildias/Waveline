// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Sums application audio (stereo) with a mono microphone send into one stereo bus.

#pragma once

#include <memory>
#include <string>

namespace waveline {

class ChannelInputMixer {
public:
    ChannelInputMixer();
    ~ChannelInputMixer();
    ChannelInputMixer(const ChannelInputMixer &) = delete;
    ChannelInputMixer &operator=(const ChannelInputMixer &) = delete;

    bool start(const std::string &nodeName, const std::string &description,
               std::string &error);
    void stop();

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline

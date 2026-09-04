// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Routes chosen applications into the Sound Sharing sink so their playback
// reaches the Stream and Monitor mixes alongside the microphone.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace waveline {

class PwEngine;
struct PwNode;

struct SharedApp {
    uint32_t nodeId = 0;
    std::string appName;
};

class SoundShareRouter {
public:
    explicit SoundShareRouter(PwEngine &engine);
    ~SoundShareRouter();

    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    void setApps(std::vector<std::string> patterns);
    const std::vector<std::string> &apps() const { return apps_; }

    bool isShared(const std::string &appName) const;

    bool start(std::string &error);
    void stop();

    bool shareStream(uint32_t nodeId, const std::string &appName,
                     std::string &error);
    bool unshareStream(uint32_t nodeId, const std::string &appName,
                       std::string &error);

    // Route one stream when Sound Sharing is enabled and the app is listed.
    // Returns true when the stream was moved.
    bool routeNode(const PwNode &node, std::string &error);
    void routeAll();

    std::vector<SharedApp> shared() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    bool enabled_ = false;
    std::vector<std::string> apps_;
};

}  // namespace waveline

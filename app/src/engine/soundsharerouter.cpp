// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "soundsharerouter.h"

#include "mixergraph.h"
#include "pwengine.h"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace waveline {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool matchesAny(const std::vector<std::string> &patterns, const std::string &app) {
    const std::string hay = lower(app);
    for (const auto &p : patterns) {
        if (hay.find(lower(p)) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

struct SoundShareRouter::Impl {
    PwEngine *eng = nullptr;
    mutable std::mutex mutex;
    std::map<uint32_t, SharedApp> active;
};

SoundShareRouter::SoundShareRouter(PwEngine &engine)
    : d_(std::make_unique<Impl>()) {
    d_->eng = &engine;
}

SoundShareRouter::~SoundShareRouter() { stop(); }

void SoundShareRouter::setApps(std::vector<std::string> patterns) {
    apps_ = std::move(patterns);
}

bool SoundShareRouter::isShared(const std::string &appName) const {
    return matchesAny(apps_, appName);
}

std::vector<SharedApp> SoundShareRouter::shared() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    std::vector<SharedApp> out;
    out.reserve(d_->active.size());
    for (const auto &[id, a] : d_->active) out.push_back(a);
    return out;
}

bool SoundShareRouter::shareStream(uint32_t nodeId, const std::string &appName,
                                   std::string &error) {
    if (!d_->eng) {
        error = "no engine";
        return false;
    }
    if (!d_->eng->setStreamTarget(nodeId, MixerGraph::kSoundShareSink, error))
        return false;
    std::lock_guard<std::mutex> lock(d_->mutex);
    d_->active[nodeId] = {nodeId, appName};
    return true;
}

bool SoundShareRouter::unshareStream(uint32_t nodeId, const std::string &appName,
                                     std::string &error) {
    if (!d_->eng) {
        error = "no engine";
        return false;
    }
    // Send it back to System so it is not left on a sink the user cannot hear
    // directly.
    if (!d_->eng->setStreamTarget(nodeId, "waveline-ch-system", error)) return false;
    std::lock_guard<std::mutex> lock(d_->mutex);
    d_->active.erase(nodeId);
    (void)appName;
    return true;
}

bool SoundShareRouter::routeNode(const PwNode &n, std::string &error) {
    if (!enabled_) return false;
    if (n.mediaClass != "Stream/Output/Audio") return false;
    if (n.isOurs) return false;
    const std::string name = n.appName.empty() ? n.name : n.appName;
    if (name.empty()) return false;
    if (name.rfind("waveline-", 0) == 0) return false;
    if (!matchesAny(apps_, name)) return false;
    return shareStream(n.id, name, error);
}

void SoundShareRouter::routeAll() {
    if (!d_->eng) return;
    for (const auto &n : d_->eng->nodes()) {
        std::string err;
        routeNode(n, err);
    }
}

bool SoundShareRouter::start(std::string &error) {
    if (!d_->eng) {
        error = "no engine";
        return false;
    }
    routeAll();
    (void)error;
    return true;
}

void SoundShareRouter::stop() {
    if (!d_->eng) return;
    std::vector<uint32_t> ids;
    {
        std::lock_guard<std::mutex> lock(d_->mutex);
        ids.reserve(d_->active.size());
        for (const auto &[id, _] : d_->active) ids.push_back(id);
        d_->active.clear();
    }
    for (uint32_t id : ids) {
        std::string err;
        d_->eng->setStreamTarget(id, "waveline-ch-system", err);
    }
}

}  // namespace waveline

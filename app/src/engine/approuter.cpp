// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "approuter.h"

#include "appidentity.h"
#include "pwengine.h"
#include "steamdetector.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>

namespace waveline {
namespace {

std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A copy, not a pointer: nodes() returns a vector by value, so anything
// pointing into it is dangling by the time the caller looks at it.
std::optional<PwNode> findNode(PwEngine *eng, uint32_t nodeId) {
    if (!eng) return std::nullopt;
    for (const auto &n : eng->nodes()) {
        if (n.id == nodeId) return n;
    }
    return std::nullopt;
}

}  // namespace

struct AppRouter::Impl {
    PwEngine *eng = nullptr;
    mutable std::mutex mutex;
    std::map<uint32_t, RoutedApp> routed;
    std::vector<RoutingRule> rules;
    std::string fallback = "system";
    std::map<std::string, std::string> manualOverrides;
    std::map<uint32_t, std::string> manualNodePins;
    // Application display name -> the per-app gain stage sink it plays into.
    std::map<std::string, std::string> stageSinks;
};

std::string manualOverrideFor(const std::map<std::string, std::string> &overrides,
                              const PwNode &n) {
    for (const std::string &key : appIdentityKeyCandidates(n)) {
        if (const auto it = overrides.find(key); it != overrides.end())
            return it->second;
    }
    return {};
}

AppRouter::AppRouter(PwEngine &engine) : d_(std::make_unique<Impl>()) {
    d_->eng = &engine;
    d_->rules = defaultRules();
}

AppRouter::~AppRouter() { stop(); }

std::vector<RoutingRule> AppRouter::defaultRules() {
    return {
        {"discord", "voice"},   {"zoom", "voice"},     {"teams", "voice"},
        {"slack", "voice"},     {"mumble", "voice"},
        {"spotify", "music"},   {"rhythmbox", "music"},{"clementine", "music"},
        {"vlc", "music"},       {"mpv", "music"},
        {"firefox", "browser"}, {"chrome", "browser"}, {"chromium", "browser"},
        {"zen", "browser"},     {"librewolf", "browser"},
        {"steam", "game"},      {"lutris", "game"},    {"heroic", "game"},
    };
}

void AppRouter::setRules(std::vector<RoutingRule> rules) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    d_->rules = std::move(rules);
}

void AppRouter::setFallbackChannel(const std::string &channelId) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    d_->fallback = channelId;
}

void AppRouter::setManualOverrides(std::map<std::string, std::string> overrides) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    d_->manualOverrides = std::move(overrides);
}

std::vector<RoutingRule> AppRouter::rules() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return d_->rules;
}

std::map<std::string, std::string> AppRouter::manualOverrides() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    return d_->manualOverrides;
}

std::string AppRouter::channelFor(const std::string &appName) const {
    const std::string hay = lower(appName);
    std::lock_guard<std::mutex> lock(d_->mutex);
    for (const auto &r : d_->rules) {
        if (hay.find(lower(r.pattern)) != std::string::npos) return r.channelId;
    }
    return d_->fallback;
}

std::string AppRouter::channelForNode(const PwNode &n) const {
    std::string fallback;
    {
        std::lock_guard<std::mutex> lock(d_->mutex);
        if (const std::string pinned = manualOverrideFor(d_->manualOverrides, n);
            !pinned.empty())
            return pinned;
        fallback = d_->fallback;
    }

    if (isSteamGameProcess(n.processId, n.processBinary)) return "game";
    const std::string name = appDisplayName(n);
    const std::string byName = channelFor(name);
    if (byName != fallback) return byName;
    if (n.processId == 0) return {};
    return fallback;
}

std::string AppRouter::assignedChannel(const PwNode &n) const {
    {
        std::lock_guard<std::mutex> lock(d_->mutex);
        if (const std::string pinned = manualOverrideFor(d_->manualOverrides, n);
            !pinned.empty())
            return pinned;
    }
    return channelOf(n.id);
}

std::vector<RoutedApp> AppRouter::routed() const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    std::vector<RoutedApp> out;
    out.reserve(d_->routed.size());
    for (const auto &[id, a] : d_->routed) out.push_back(a);
    return out;
}

std::string AppRouter::channelOf(uint32_t nodeId) const {
    std::lock_guard<std::mutex> lock(d_->mutex);
    auto it = d_->routed.find(nodeId);
    return it == d_->routed.end() ? std::string() : it->second.channelId;
}

std::vector<StreamContention> AppRouter::misroutedStreams() const {
    std::vector<StreamContention> out;
    if (!d_->eng) return out;

    // The engine reads links under its own lock, so the routing table is
    // copied first rather than held while calling into it.
    std::vector<RoutedApp> want;
    {
        std::lock_guard<std::mutex> lock(d_->mutex);
        for (const auto &[id, a] : d_->routed) {
            (void)id;
            if (!a.channelId.empty()) want.push_back(a);
        }
    }

    for (const RoutedApp &a : want) {
        // The routing table is never pruned, and PipeWire reuses node ids. A
        // stale entry matched against whatever later inherited its id would
        // report a stream as stolen purely because a previous tenant of that
        // id had been sent somewhere else. Only a node that is still there and
        // still the same application counts.
        const auto node = findNode(d_->eng, a.nodeId);
        if (!node || appDisplayName(*node) != a.appName) continue;

        PwNode sink;
        if (!d_->eng->streamSinkNode(a.nodeId, sink)) continue;
        const std::string expected = "waveline-ch-" + a.channelId;
        if (sink.name == expected) continue;
        // The sound-sharing router moves streams onto its own sink on purpose,
        // and a stream on its way to a channel we just removed is our own
        // doing too. Neither is somebody else taking it.
        if (sink.name.rfind("waveline-", 0) == 0) continue;

        StreamContention c;
        c.nodeId = a.nodeId;
        c.appName = a.appName;
        c.channelId = a.channelId;
        c.sinkName = sink.name;
        c.sinkLabel = !sink.description.empty() ? sink.description : sink.name;
        out.push_back(std::move(c));
    }
    return out;
}

void AppRouter::setStageSink(const std::string &appName,
                             const std::string &sinkName) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    d_->stageSinks[appName] = sinkName;
}

void AppRouter::clearStageSink(const std::string &appName) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    d_->stageSinks.erase(appName);
}

bool AppRouter::moveStream(uint32_t nodeId, const std::string &channelId,
                           std::string &error) {
    if (!d_->eng) { error = "no engine"; return false; }
    std::string sink = "waveline-ch-" + channelId;
    if (const auto n = findNode(d_->eng, nodeId)) {
        std::lock_guard<std::mutex> lock(d_->mutex);
        const auto it = d_->stageSinks.find(appDisplayName(*n));
        if (it != d_->stageSinks.end()) sink = it->second;
    }
    if (!d_->eng->setStreamTarget(nodeId, sink, error)) return false;
    std::lock_guard<std::mutex> lock(d_->mutex);
    d_->routed[nodeId].nodeId = nodeId;
    d_->routed[nodeId].channelId = channelId;
    if (const auto n = findNode(d_->eng, nodeId))
        d_->routed[nodeId].appName = appDisplayName(*n);
    return true;
}

bool AppRouter::pinStream(uint32_t nodeId, const std::string &channelId,
                          std::string &error) {
    if (const auto n = findNode(d_->eng, nodeId)) {
        std::lock_guard<std::mutex> lock(d_->mutex);
        d_->manualNodePins[nodeId] = channelId;
        for (const std::string &key : appIdentityKeyCandidates(*n))
            d_->manualOverrides[key] = channelId;
    }
    return moveStream(nodeId, channelId, error);
}

bool AppRouter::refreshManualPinForNode(const PwNode &n) {
    std::lock_guard<std::mutex> lock(d_->mutex);
    std::string channel;
    if (const auto it = d_->manualNodePins.find(n.id); it != d_->manualNodePins.end())
        channel = it->second;
    if (channel.empty()) channel = manualOverrideFor(d_->manualOverrides, n);
    if (channel.empty()) return false;

    bool changed = false;
    for (const std::string &key : appIdentityKeyCandidates(n)) {
        if (!isStableIdentityKey(key)) continue;
        if (const auto it = d_->manualOverrides.find(key);
            it == d_->manualOverrides.end() || it->second != channel) {
            d_->manualOverrides[key] = channel;
            changed = true;
        }
    }
    return changed;
}

bool AppRouter::routeNode(const PwNode &n, std::string &error) {
    if (n.mediaClass != "Stream/Output/Audio") return false;
    if (n.isOurs) return false;
    const std::string raw = n.appName.empty() ? n.name : n.appName;
    if (raw.empty()) return false;
    if (raw.rfind("waveline-", 0) == 0) return false;
    const std::string channel = channelForNode(n);
    if (channel.empty()) return false;
    return moveStream(n.id, channel, error);
}

void AppRouter::routeAll() {
    if (!d_->eng) return;
    for (const auto &n : d_->eng->nodes()) {
        std::string err;
        routeNode(n, err);
    }
}

bool AppRouter::start(std::string &error) {
    if (!d_->eng) { error = "no engine"; return false; }
    routeAll();
    (void)error;
    return true;
}

void AppRouter::stop() {}

}  // namespace waveline

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Stage 4: send each application to the channel it belongs on.
//
// A rule matches a substring of an application's name (or its node name) and
// names a channel. When a new playback stream appears, the first matching rule
// wins and the stream is moved to that channel's sink; anything unmatched goes
// to the fallback channel.
//
// Moving is done by writing target.object into PipeWire's "default" metadata,
// which is the same mechanism `pactl move-sink-input` uses. Creating links by
// hand would fight the session manager, which re-routes streams on its own
// schedule.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace waveline {

class PwEngine;
struct PwNode;

struct RoutingRule {
    std::string pattern;   // matched case-insensitively as a substring
    std::string channelId; // "music", "voice", ...
};

struct RoutedApp {
    uint32_t nodeId = 0;
    std::string appName;
    std::string channelId;  // where it was sent, empty if untouched
};

// A stream this router moved onto a channel that is playing somewhere else.
//
// Routing is a write to one shared metadata key, so whoever writes last wins
// and nothing in PipeWire records who that was. This does not claim to know:
// it reports where the audio actually ended up, which is both observable and
// the thing a user has to act on. For every tool that does this -- EasyEffects
// and friends, or wireplumber restoring a stale saved target -- the
// destination names the culprit anyway.
struct StreamContention {
    uint32_t nodeId = 0;
    std::string appName;       // the application losing its routing
    std::string channelId;     // the channel it was supposed to be on
    std::string sinkName;      // node.name of where it actually went
    std::string sinkLabel;     // that node's description, for the message
};

class AppRouter {
public:
    explicit AppRouter(PwEngine &engine);
    ~AppRouter();

    // Wave Link's defaults, which is what people coming from Windows expect.
    static std::vector<RoutingRule> defaultRules();

    void setRules(std::vector<RoutingRule> rules);
    std::vector<RoutingRule> rules() const;

    // Channel used when nothing matches.
    void setFallbackChannel(const std::string &channelId);

    // Starts watching for new streams. Existing streams are routed too.
    bool start(std::string &error);
    void stop();

    // Which channel a rule set would pick for a given application name.
    std::string channelFor(const std::string &appName) const;

    // Name rules plus Steam-game detection from the stream's process metadata.
    std::string channelForNode(const PwNode &node) const;

    // Move one stream by hand, overriding automatic routing (the Apps tab).
    bool pinStream(uint32_t nodeId, const std::string &channelId,
                   std::string &error);

    // Move one stream without recording a manual override (used by routeNode).
    bool moveStream(uint32_t nodeId, const std::string &channelId,
                    std::string &error);

    void setManualOverrides(std::map<std::string, std::string> overrides);
    std::map<std::string, std::string> manualOverrides() const;

    // An application with its own gain stage plays into that stage's sink
    // instead of the channel sink. The channel is still what the app is
    // assigned to -- the stage's output is what feeds it -- so this changes
    // where streams are sent without changing what they are routed *to*.
    // Without it, routeNode would send the stream straight to the channel on
    // every restart and quietly bypass the level the user set.
    void setStageSink(const std::string &appName, const std::string &sinkName);
    void clearStageSink(const std::string &appName);

    // Resolved channel for the Apps tab: manual pin, else last routed target.
    std::string assignedChannel(const PwNode &node) const;

    // Re-key a manual pin once process metadata becomes available.
    bool refreshManualPinForNode(const PwNode &node);

    // Route one stream if automatic routing is active. Returns true when the
    // stream was moved.
    bool routeNode(const PwNode &node, std::string &error);
    void routeAll();

    std::vector<RoutedApp> routed() const;

    // Where we last sent this stream, or empty if we never moved it.
    std::string channelOf(uint32_t nodeId) const;

    // Every stream we moved that is not where we put it. A stream still
    // settling, or idle and linked to nothing, is not reported -- only one
    // that is demonstrably playing into a different sink.
    std::vector<StreamContention> misroutedStreams() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline

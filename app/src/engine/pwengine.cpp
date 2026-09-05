// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "pwengine.h"

#include "rtsched.h"

#include "appidentity.h"

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <pipewire/client.h>
#include <pipewire/node.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/json.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>  // strcasecmp
#include <mutex>
#include <set>
#include <thread>

namespace waveline {
namespace {

// Loopback modules created by addPath. Volume lives on the *capture* side node
// so that changing it does not disturb the link to the target.
struct Path {
    pw_impl_module *module = nullptr;
    std::string captureName;   // node we set volume on
    std::string target;
    float volume = 1.0f;
    bool muted = false;
    int channels = 2;
    bool sourceIsSink = true;
    std::string source;
    std::string description;
};

std::string quote(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

std::string positionFor(int channels) {
    return channels == 1 ? "[ MONO ]" : "[ FL FR ]";
}

}  // namespace

struct PwEngine::Impl {
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;
    spa_hook registryListener{};
    spa_hook coreListener{};

    // Sinks are server-side objects, held alive by their proxy. Destroying the
    // proxy destroys the node, which is the lifetime we want.
    std::vector<pw_proxy *> sinkProxies;
    // Server-side nodes we can destroy again, by node.name.
    std::map<std::string, pw_proxy *> namedProxies;
    // Manual port links created by linkPorts(), keyed so retries cannot stack
    // duplicates and so individual links can be dropped when a filter node is
    // torn down without clearing the whole graph.
    std::map<std::string, pw_proxy *> manualLinks;
    // Live PipeWire links (from registry), keyed by link id. Used to verify a
    // link-factory call actually connected before trusting manualLinks cache.
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> pipewireLinks;
    std::map<std::string, Path> paths;

    mutable std::mutex nodesMutex;
    std::map<uint32_t, PwNode> nodes;

    // PipeWire Client globals carry application.process.*; playback stream nodes
    // (object.register=false) only have client.id and inherit process info here.
    struct ClientProc {
        uint32_t processId = 0;
        std::string processBinary;
        std::string appName;
    };
    std::map<uint32_t, ClientProc> clients;

    // link-factory takes port object ids, not names. pw-link resolves names
    // itself, which is why hand-written links worked while ours silently did
    // nothing: passing names produced a link object that referred to nothing.
    struct PortRef {
        uint32_t nodeId = 0;
        std::string name;      // port.name, e.g. "capture_MONO", "input"
        bool isOutput = false;
    };
    std::map<uint32_t, PortRef> ports;
    std::function<void()> onGraphChanged;
    std::function<void()> onDefaultSourceChanged;
    std::function<void(const PwNode &)> onNodeAdded;
    std::function<void(const PwNode &)> onNodeRemoved;
    // Bound stream nodes: PipeWire fills application.process.* after the first
    // registry announce, so we listen for property updates on playback streams.
    std::map<uint32_t, pw_proxy *> streamProxies;
    // Client globals carry process metadata for their stream nodes; bind so we
    // see the full property set, not just the sparse registry announce.
    std::map<uint32_t, pw_proxy *> clientProxies;
    pw_metadata *metadata = nullptr;
    spa_hook metadataListener{};
    // Registry ids of the session manager's two metadata objects, so their
    // removal can be recognised and the replacements bound. 0 = not bound.
    uint32_t metadataId = 0;
    uint32_t settingsId = 0;
    // True once "default" has been bound at least once, which is what tells a
    // session-manager restart apart from ordinary startup.
    bool hadMetadata = false;
    std::function<void()> onSessionManagerRestarted;
    std::string defaultSink;     // from default.audio.sink
    std::string defaultSource;   // from default.audio.source

    // PipeWire's "settings" metadata: the graph clock. Mirrored here rather
    // than inferred, and writable -- clock.force-quantum is how the mixer's
    // latency control takes effect without restarting anything.
    //
    // The values start at zero, not at PipeWire's defaults. A plausible-
    // looking default is indistinguishable from an answer once it has been
    // drawn on screen, and this is a diagnostics surface whose entire value is
    // that it does not make things up. `known` is what says the difference.
    pw_metadata *settings = nullptr;
    spa_hook settingsListener{};
    PwEngine::GraphClock clock{};

    // Node ids we created, so the UI can distinguish our virtual devices from
    // the user's real hardware.
    std::vector<std::string> ourNames;

    bool findPortIdLocked(const std::string &nodeName, const std::string &portName,
                          bool wantOutput, uint32_t &portId) const {
        uint32_t nodeId = 0;
        if (!findNodeIdLocked(nodeName, nodeId)) return false;
        bool found = false;
        for (const auto &[pid, p] : ports) {
            if (p.nodeId != nodeId || p.isOutput != wantOutput ||
                p.name != portName)
                continue;
            if (!found || pid > portId) {
                portId = pid;
                found = true;
            }
        }
        return found;
    }

    bool hasPipewireLinkLocked(uint32_t outPort, uint32_t inPort) const {
        for (const auto &[_, ends] : pipewireLinks) {
            if (ends.first == outPort && ends.second == inPort) return true;
        }
        return false;
    }

    bool hasIncomingPipewireLinkLocked(uint32_t inPort) const {
        for (const auto &[_, ends] : pipewireLinks) {
            if (ends.second == inPort) return true;
        }
        return false;
    }

    // Recreating a path loads a new module under the *same* node names, and the
    // old globals linger in the registry until PipeWire gets round to removing
    // them. Taking the first match means taking the dead one -- link-factory
    // then happily builds a link onto ports that no longer exist, and the
    // channel goes silent with every link in `pw-link -l` looking correct.
    // Ids are monotonic, so the highest is always the live node.
    bool findNodeIdLocked(const std::string &name, uint32_t &id) const {
        bool found = false;
        for (const auto &[nid, n] : nodes) {
            if (n.name != name) continue;
            if (!found || nid > id) {
                id = nid;
                found = true;
            }
        }
        return found;
    }
};

namespace {

int onMetadataProperty(void *data, uint32_t /*subject*/, const char *key,
                       const char * /*type*/, const char *value) {
    auto *impl = static_cast<PwEngine::Impl *>(data);
    if (!key || !value) return 0;
    const bool isSink = std::strcmp(key, "default.audio.sink") == 0;
    // Tracked as well as the sink because a microphone we have no profile for
    // has no name we can predict: "whatever the user's default source is" is
    // the only correct answer, and this is where PipeWire says so.
    const bool isSource = std::strcmp(key, "default.audio.source") == 0;
    if (!isSink && !isSource) return 0;
    // The value is JSON like {"name":"alsa_output...."}. Pulling the one field
    // out by hand avoids dragging in a JSON parser for a single key.
    const char *n = std::strstr(value, "\"name\"");
    if (!n) return 0;
    const char *open = std::strchr(n + 6, '"');
    if (!open) return 0;
    const char *close = std::strchr(open + 1, '"');
    if (!close) return 0;
    const std::string value_(open + 1, close - open - 1);
    bool sourceChanged = false;
    {
        std::lock_guard<std::mutex> lock(impl->nodesMutex);
        std::string &slot = isSink ? impl->defaultSink : impl->defaultSource;
        sourceChanged = isSource && slot != value_;
        slot = value_;
    }
    // Announced outside the lock: the handler goes on to read the node list,
    // which takes the same mutex.
    if (sourceChanged && impl->onDefaultSourceChanged)
        impl->onDefaultSourceChanged();
    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_metadata_events kMetadataEvents = {
    .version = PW_VERSION_METADATA_EVENTS,
    .property = onMetadataProperty,
};
#pragma GCC diagnostic pop

// The graph clock, from the "settings" metadata. clock.quantum and clock.rate
// are the configured defaults; the force-* pair is an override that wins
// whenever it is non-zero.
//
// The two are kept apart rather than folded together, because "512" and
// "512, and pinned there so nothing can renegotiate it" are different facts
// about a machine and only one of them is a promise. The diagnostics view
// shows which it is.
int onSettingsProperty(void *data, uint32_t /*subject*/, const char *key,
                       const char * /*type*/, const char *value) {
    auto *impl = static_cast<PwEngine::Impl *>(data);
    if (!key || !value) return 0;
    // A forced value legitimately goes back to 0, meaning "released"; unlike
    // the others, that zero is an answer and must be stored.
    const uint32_t n = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
    std::lock_guard<std::mutex> lock(impl->nodesMutex);
    if (std::strcmp(key, "clock.quantum") == 0) {
        if (n) impl->clock.quantum = n;
    } else if (std::strcmp(key, "clock.rate") == 0) {
        if (n) impl->clock.rate = n;
    } else if (std::strcmp(key, "clock.min-quantum") == 0) {
        if (n) impl->clock.minQuantum = n;
    } else if (std::strcmp(key, "clock.max-quantum") == 0) {
        if (n) impl->clock.maxQuantum = n;
    } else if (std::strcmp(key, "clock.force-quantum") == 0) {
        impl->clock.forcedQuantum = n;
    } else if (std::strcmp(key, "clock.force-rate") == 0) {
        impl->clock.forcedRate = n;
    } else {
        return 0;
    }
    impl->clock.known = true;
    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_metadata_events kSettingsEvents = {
    .version = PW_VERSION_METADATA_EVENTS,
    .property = onSettingsProperty,
};
#pragma GCC diagnostic pop

// Applies a path's volume to its capture node. MUST be called with the thread
// loop lock held -- registry callbacks already run holding it, so they must not
// take it again.
void applyVolumeLocked(PwEngine::Impl *impl, uint32_t nodeId, const Path &p);

// A custom node prop written by hand in a WirePlumber rule. SPA-JSON carries
// booleans as text and a person editing a .conf writes whatever looks right, so
// the spellings are accepted rather than requiring one. A device profile that
// says "yes" and is silently ignored is worse than one that fails loudly, and
// there is nothing here to fail loudly with.
bool isTruthyProp(const char *v) {
    if (!v) return false;
    return strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
           strcasecmp(v, "on") == 0 || std::strcmp(v, "1") == 0;
}

void applyNodeProps(PwNode &n, const spa_dict *props) {
    if (!props) return;
    const char *v;
    if ((v = spa_dict_lookup(props, PW_KEY_NODE_NAME))) n.name = v;
    if ((v = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION))) n.description = v;
    if ((v = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS))) n.mediaClass = v;
    if ((v = spa_dict_lookup(props, PW_KEY_APP_NAME))) n.appName = v;
    if ((v = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID)))
        n.processId = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    if ((v = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY)))
        n.processBinary = v;
    if ((v = spa_dict_lookup(props, PW_KEY_CLIENT_ID)))
        n.clientId = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    // Which ALSA PCM is behind this node, so its latency can be read from the
    // kernel rather than computed. Present on hardware nodes only, and absent
    // on our own filters and on every application stream.
    if ((v = spa_dict_lookup(props, "api.alsa.path"))) n.alsaPath = v;
    if ((v = spa_dict_lookup(props, "api.alsa.pcm.card")))
        n.alsaCard = static_cast<int>(std::strtol(v, nullptr, 10));
    if ((v = spa_dict_lookup(props, "waveline.hidden-latency")))
        n.hidesLatency = isTruthyProp(v);
    if ((v = spa_dict_lookup(props, "api.alsa.headroom")))
        n.alsaHeadroom = static_cast<int>(std::strtol(v, nullptr, 10));
}

void resolveClientProcess(PwNode &n, const PwEngine::Impl &impl) {
    if (n.clientId == 0) return;
    const auto it = impl.clients.find(n.clientId);
    if (it == impl.clients.end()) return;
    const auto &client = it->second;

    if (n.processId == 0 && client.processId != 0)
        n.processId = client.processId;

    if ((n.processBinary.empty() || isGenericAppLabel(n.processBinary)) &&
        !client.processBinary.empty())
        n.processBinary = client.processBinary;

    if ((n.appName.empty() || isGenericAppLabel(n.appName)) &&
        !client.appName.empty() && !isGenericAppLabel(client.appName))
        n.appName = client.appName;
}

void applyClientProps(PwEngine::Impl &impl, uint32_t clientId,
                      const spa_dict *props) {
    PwEngine::Impl::ClientProc cp;
    if (props) {
        const char *v;
        if ((v = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID)))
            cp.processId = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        if (cp.processId == 0 &&
            (v = spa_dict_lookup(props, "pipewire.sec.pid")))
            cp.processId = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        if ((v = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY)))
            cp.processBinary = v;
        if ((v = spa_dict_lookup(props, PW_KEY_APP_NAME)))
            cp.appName = v;
    }
    std::vector<PwNode> updated;
    {
        std::lock_guard<std::mutex> lock(impl.nodesMutex);
        impl.clients[clientId] = std::move(cp);
        for (auto &[nid, node] : impl.nodes) {
            if (node.clientId != clientId) continue;
            if (node.mediaClass != "Stream/Output/Audio") continue;
            const uint32_t prevPid = node.processId;
            const std::string prevBin = node.processBinary;
            resolveClientProcess(node, impl);
            if ((prevPid == 0 && node.processId != 0) ||
                ((prevBin.empty() || isGenericAppLabel(prevBin)) &&
                 !node.processBinary.empty() && !isGenericAppLabel(node.processBinary)))
                updated.push_back(node);
        }
    }
    if (impl.onNodeAdded) {
        for (const PwNode &n : updated) impl.onNodeAdded(n);
    }
}

struct ClientWatch {
    PwEngine::Impl *impl = nullptr;
    spa_hook listener{};
    uint32_t id = 0;
};

void onClientInfo(void *data, const struct pw_client_info *info) {
    auto *watch = static_cast<ClientWatch *>(data);
    if (!(info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS) || !info->props) return;
    applyClientProps(*watch->impl, watch->id, info->props);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_client_events kClientEvents = {
    .version = PW_VERSION_CLIENT_EVENTS,
    .info = onClientInfo,
};
#pragma GCC diagnostic pop

void bindClientWatch(PwEngine::Impl *impl, uint32_t id, const spa_dict *props) {
    if (impl->clientProxies.count(id)) return;

    auto *proxy = static_cast<pw_proxy *>(pw_registry_bind(
        impl->registry, id, PW_TYPE_INTERFACE_Client, PW_VERSION_CLIENT,
        sizeof(ClientWatch)));
    if (!proxy) return;

    auto *watch = static_cast<ClientWatch *>(pw_proxy_get_user_data(proxy));
    watch->impl = impl;
    watch->id = id;
    pw_client_add_listener(reinterpret_cast<pw_client *>(proxy), &watch->listener,
                           &kClientEvents, watch);
    impl->clientProxies[id] = proxy;

    if (props) applyClientProps(*impl, id, props);
}

struct StreamWatch {
    PwEngine::Impl *impl = nullptr;
    spa_hook listener{};
    uint32_t id = 0;
};

void onStreamNodeInfo(void *data, const struct pw_node_info *info) {
    auto *watch = static_cast<StreamWatch *>(data);
    if (!(info->change_mask & PW_NODE_CHANGE_MASK_PROPS) || !info->props) return;

    bool notify = false;
    PwNode copy;
    {
        std::lock_guard<std::mutex> lock(watch->impl->nodesMutex);
        auto it = watch->impl->nodes.find(watch->id);
        if (it == watch->impl->nodes.end()) return;
        const uint32_t prevPid = it->second.processId;
        applyNodeProps(it->second, info->props);
        resolveClientProcess(it->second, *watch->impl);
        if (it->second.description.empty()) it->second.description = it->second.name;
        copy = it->second;
        notify = prevPid == 0 && it->second.processId != 0;
    }
    if (notify && watch->impl->onNodeAdded) watch->impl->onNodeAdded(copy);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_node_events kStreamNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = onStreamNodeInfo,
};
#pragma GCC diagnostic pop

// Our own nodes: track scheduling state so a node that PipeWire never gave a
// driver can be told apart from a healthy one. Neither fact is in the registry
// announce -- state arrives with the info event, and node.driver-id only shows
// up in the info props once the server has placed the node in a driver group.
void onOwnNodeInfo(void *data, const struct pw_node_info *info) {
    auto *watch = static_cast<StreamWatch *>(data);
    std::lock_guard<std::mutex> lock(watch->impl->nodesMutex);
    auto it = watch->impl->nodes.find(watch->id);
    if (it == watch->impl->nodes.end()) return;

    if (info->change_mask & PW_NODE_CHANGE_MASK_STATE)
        it->second.state = static_cast<int>(info->state);
    if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) && info->props) {
        const char *v = spa_dict_lookup(info->props, "node.driver-id");
        it->second.driverId =
            v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0;
        // api.alsa.path is NOT in the registry announce -- that dict is a
        // sparse subset, and this only turns up here, on the bound node's info
        // event. Reading it in applyNodeProps() alone left every hardware
        // source with an empty path, so nothing was ever handed to the delay
        // probe and the diagnostics view listed no devices at all.
        if ((v = spa_dict_lookup(info->props, "api.alsa.path")))
            it->second.alsaPath = v;
        if ((v = spa_dict_lookup(info->props, "api.alsa.pcm.card")))
            it->second.alsaCard = static_cast<int>(std::strtol(v, nullptr, 10));
        if ((v = spa_dict_lookup(info->props, "waveline.hidden-latency")))
            it->second.hidesLatency = isTruthyProp(v);
        // Same story as api.alsa.path above: only on the bound node's info.
        if ((v = spa_dict_lookup(info->props, "api.alsa.headroom")))
            it->second.alsaHeadroom = static_cast<int>(std::strtol(v, nullptr, 10));
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_node_events kOwnNodeEvents = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = onOwnNodeInfo,
};
#pragma GCC diagnostic pop

void bindOwnNodeWatch(PwEngine::Impl *impl, uint32_t id, const PwNode &n) {
    if (!n.isOurs) return;
    if (impl->streamProxies.count(id)) return;

    auto *proxy = static_cast<pw_proxy *>(pw_registry_bind(
        impl->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
        sizeof(StreamWatch)));
    if (!proxy) return;

    auto *watch = static_cast<StreamWatch *>(pw_proxy_get_user_data(proxy));
    watch->impl = impl;
    watch->id = id;
    pw_node_add_listener(reinterpret_cast<pw_node *>(proxy), &watch->listener,
                         &kOwnNodeEvents, watch);
    impl->streamProxies[id] = proxy;
}

// Hardware capture sources: watch which driver group PipeWire put them in.
//
// This binding used to exist to read SPA_PARAM_Latency and turn it into a
// number for the UI. That number is now measured from the kernel instead (see
// engine/alsadelay.h), and what is worth watching here is the fact the whole
// investigation turned on: a capture node's driver group is assigned by the
// server, changes when unrelated hardware is plugged in, and is invisible
// unless something asks. The reused info handler records node.driver-id, which
// is what the diagnostics view reports.
// Sinks are watched too, and not only for symmetry: api.alsa.headroom arrives
// on the bound node's info event exactly like api.alsa.path, so without this
// every output device reads back headroom -1 and the mixer cannot tell whether
// a headroom rule has been applied yet. It also gives sinks a driver-id, which
// is the other half of "which device is clocking the graph".
void bindDeviceWatch(PwEngine::Impl *impl, uint32_t id, const PwNode &n) {
    if (n.isOurs) return;
    if (n.mediaClass != "Audio/Source" && n.mediaClass != "Audio/Sink") return;
    if (impl->streamProxies.count(id)) return;

    auto *proxy = static_cast<pw_proxy *>(pw_registry_bind(
        impl->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
        sizeof(StreamWatch)));
    if (!proxy) return;

    auto *watch = static_cast<StreamWatch *>(pw_proxy_get_user_data(proxy));
    watch->impl = impl;
    watch->id = id;
    // No pw_node_subscribe_params: nothing here reads a param any more, and
    // subscribing pushes every Latency/Props/Format change at us for every
    // capture device on the machine.
    pw_node_add_listener(reinterpret_cast<pw_node *>(proxy), &watch->listener,
                         &kOwnNodeEvents, watch);
    impl->streamProxies[id] = proxy;
}

void bindStreamWatch(PwEngine::Impl *impl, uint32_t id, const PwNode &n) {
    if (n.isOurs || n.mediaClass != "Stream/Output/Audio") return;
    if (impl->streamProxies.count(id)) return;

    auto *proxy = static_cast<pw_proxy *>(pw_registry_bind(
        impl->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
        sizeof(StreamWatch)));
    if (!proxy) return;

    auto *watch = static_cast<StreamWatch *>(pw_proxy_get_user_data(proxy));
    watch->impl = impl;
    watch->id = id;
    pw_node_add_listener(reinterpret_cast<pw_node *>(proxy), &watch->listener,
                         &kStreamNodeEvents, watch);
    impl->streamProxies[id] = proxy;
}

void onRegistryGlobal(void *data, uint32_t id, uint32_t /*permissions*/,
                      const char *type, uint32_t /*version*/,
                      const spa_dict *props) {
    auto *impl = static_cast<PwEngine::Impl *>(data);

    if (type && std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *name = props ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : nullptr;
        // Both of these objects belong to the session manager, not to us, and
        // they die with it. Binding once and holding the pointer forever meant
        // that after `systemctl --user restart wireplumber` every write here
        // went to a destroyed proxy and was silently dropped -- which is what
        // left applications sitting on the wrong sink until wavelined itself
        // was restarted. Track the id so the replacement can be picked up.
        if (name && std::strcmp(name, "default") == 0 && !impl->metadata) {
            impl->metadata = static_cast<pw_metadata *>(pw_registry_bind(
                impl->registry, id, PW_TYPE_INTERFACE_Metadata,
                PW_VERSION_METADATA, 0));
            if (impl->metadata) {
                impl->metadataId = id;
                pw_metadata_add_listener(impl->metadata, &impl->metadataListener,
                                         &kMetadataEvents, impl);
                // Only on a *re*-bind. The first one is startup, where the
                // graph is being built anyway and routing runs regardless.
                if (impl->hadMetadata && impl->onSessionManagerRestarted)
                    impl->onSessionManagerRestarted();
                impl->hadMetadata = true;
            }
        }
        if (name && std::strcmp(name, "settings") == 0 && !impl->settings) {
            impl->settings = static_cast<pw_metadata *>(pw_registry_bind(
                impl->registry, id, PW_TYPE_INTERFACE_Metadata,
                PW_VERSION_METADATA, 0));
            if (impl->settings) {
                impl->settingsId = id;
                pw_metadata_add_listener(impl->settings, &impl->settingsListener,
                                         &kSettingsEvents, impl);
            }
        }
        return;
    }

    if (type && std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
        uint32_t outPort = 0, inPort = 0;
        if (props) {
            const char *v;
            if ((v = spa_dict_lookup(props, "link.output.port")))
                outPort = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
            if ((v = spa_dict_lookup(props, "link.input.port")))
                inPort = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        }
        if (outPort && inPort) {
            std::lock_guard<std::mutex> lock(impl->nodesMutex);
            impl->pipewireLinks[id] = {outPort, inPort};
        }
        return;
    }

    if (type && std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        PwEngine::Impl::PortRef p;
        if (props) {
            const char *v;
            if ((v = spa_dict_lookup(props, PW_KEY_NODE_ID))) p.nodeId = atoi(v);
            if ((v = spa_dict_lookup(props, PW_KEY_PORT_NAME))) p.name = v;
            if ((v = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)))
                p.isOutput = (std::strcmp(v, "out") == 0);
        }
        std::lock_guard<std::mutex> lock(impl->nodesMutex);
        impl->ports[id] = std::move(p);
        return;
    }

    if (type && std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        bindClientWatch(impl, id, props);
        return;
    }

    if (!type || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

    PwNode n;
    n.id = id;
    if (props) applyNodeProps(n, props);
    if (n.description.empty()) n.description = n.name;
    for (const auto &ours : impl->ourNames)
        if (ours == n.name) { n.isOurs = true; break; }

    PwNode copy;
    const std::string nodeName = n.name;
    {
        std::lock_guard<std::mutex> lock(impl->nodesMutex);
        resolveClientProcess(n, *impl);
        copy = n;
        impl->nodes[id] = std::move(n);
    }
    bindStreamWatch(impl, id, copy);
    bindDeviceWatch(impl, id, copy);
    bindOwnNodeWatch(impl, id, copy);
    if (impl->onNodeAdded) impl->onNodeAdded(copy);

    // A path's volume cannot be set until its node exists, and the node appears
    // asynchronously well after the module is loaded. Anything requested in the
    // meantime is applied here, the moment the node shows up.
    for (auto &[handle, path] : impl->paths) {
        if (path.captureName == nodeName) {
            applyVolumeLocked(impl, id, path);
            break;
        }
    }

    if (impl->onGraphChanged) impl->onGraphChanged();
}

void onRegistryGlobalRemove(void *data, uint32_t id) {
    auto *impl = static_cast<PwEngine::Impl *>(data);
    // The session manager went away and took its metadata with it. Drop the
    // proxy and the listener so the next "default"/"settings" global is bound
    // instead of ignored; holding the stale pointer is what made every routing
    // write a no-op after a WirePlumber restart.
    if (id == impl->metadataId && impl->metadata) {
        spa_hook_remove(&impl->metadataListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(impl->metadata));
        impl->metadata = nullptr;
        impl->metadataId = 0;
    }
    if (id == impl->settingsId && impl->settings) {
        spa_hook_remove(&impl->settingsListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(impl->settings));
        impl->settings = nullptr;
        impl->settingsId = 0;
    }
    if (auto it = impl->streamProxies.find(id); it != impl->streamProxies.end()) {
        pw_proxy_destroy(it->second);
        impl->streamProxies.erase(it);
    }
    if (auto it = impl->clientProxies.find(id); it != impl->clientProxies.end()) {
        pw_proxy_destroy(it->second);
        impl->clientProxies.erase(it);
    }
    PwNode removed;
    bool erased;
    {
        std::lock_guard<std::mutex> lock(impl->nodesMutex);
        impl->pipewireLinks.erase(id);
        impl->ports.erase(id);
        impl->clients.erase(id);
        auto nit = impl->nodes.find(id);
        if (nit != impl->nodes.end()) {
            removed = nit->second;
            impl->nodes.erase(nit);
            for (auto pit = impl->ports.begin(); pit != impl->ports.end();) {
                if (pit->second.nodeId == id)
                    pit = impl->ports.erase(pit);
                else
                    ++pit;
            }
            erased = true;
        } else {
            erased = false;
        }
    }
    if (erased && impl->onNodeRemoved) impl->onNodeRemoved(removed);
    if (erased && impl->onGraphChanged) impl->onGraphChanged();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = onRegistryGlobal,
    .global_remove = onRegistryGlobalRemove,
};
#pragma GCC diagnostic pop

}  // namespace

PwEngine::PwEngine() : d_(std::make_unique<Impl>()) {}

PwEngine::~PwEngine() { stop(); }

void PwEngine::setOnGraphChanged(std::function<void()> cb) {
    d_->onGraphChanged = std::move(cb);
}

void PwEngine::setOnDefaultSourceChanged(std::function<void()> cb) {
    d_->onDefaultSourceChanged = std::move(cb);
}

void PwEngine::setOnNodeAdded(std::function<void(const PwNode &)> cb) {
    d_->onNodeAdded = std::move(cb);
}

void PwEngine::setOnNodeRemoved(std::function<void(const PwNode &)> cb) {
    d_->onNodeRemoved = std::move(cb);
}

void PwEngine::setOnSessionManagerRestarted(std::function<void()> cb) {
    d_->onSessionManagerRestarted = std::move(cb);
}

bool PwEngine::setStreamTarget(uint32_t nodeId, const std::string &sinkName,
                               std::string &error) {
    if (!running_) { error = "engine not started"; return false; }
    pw_thread_loop_lock(d_->loop);
    if (!d_->metadata) {
        pw_thread_loop_unlock(d_->loop);
        error = "default metadata not available yet";
        return false;
    }
    pw_metadata_set_property(d_->metadata, nodeId, "target.object", "Spa:String",
                             sinkName.c_str());
    pw_core_sync(d_->core, PW_ID_CORE, 0);
    pw_thread_loop_unlock(d_->loop);
    return true;
}

bool PwEngine::streamSinkNode(uint32_t streamNodeId, PwNode &out) const {
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    for (const auto &[linkId, ends] : d_->pipewireLinks) {
        (void)linkId;
        const auto outPort = d_->ports.find(ends.first);
        if (outPort == d_->ports.end() || outPort->second.nodeId != streamNodeId)
            continue;
        const auto inPort = d_->ports.find(ends.second);
        if (inPort == d_->ports.end()) continue;
        const auto node = d_->nodes.find(inPort->second.nodeId);
        if (node == d_->nodes.end()) continue;
        // The first link is enough: a stereo stream has two of them and both
        // land on the same sink. A stream split across two sinks is not a
        // thing PipeWire's session managers produce.
        out = node->second;
        return true;
    }
    return false;
}

bool PwEngine::streamSourceNode(uint32_t streamNodeId, PwNode &out) const {
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    for (const auto &[linkId, ends] : d_->pipewireLinks) {
        (void)linkId;
        const auto inPort = d_->ports.find(ends.second);
        if (inPort == d_->ports.end() || inPort->second.nodeId != streamNodeId)
            continue;
        const auto outPort = d_->ports.find(ends.first);
        if (outPort == d_->ports.end()) continue;
        const auto node = d_->nodes.find(outPort->second.nodeId);
        if (node == d_->nodes.end()) continue;
        // As above: a stereo capture has two links and both come from the same
        // device.
        out = node->second;
        return true;
    }
    return false;
}

bool PwEngine::start(std::string &error) {
    if (running_) return true;

    pw_init(nullptr, nullptr);

    d_->loop = pw_thread_loop_new("waveline-engine", nullptr);
    if (!d_->loop) { error = "pw_thread_loop_new failed"; return false; }

    pw_thread_loop_lock(d_->loop);

    d_->context = pw_context_new(pw_thread_loop_get_loop(d_->loop), realtimeContextProps(), 0);
    if (!d_->context) {
        error = "pw_context_new failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    d_->core = pw_context_connect(d_->context, nullptr, 0);
    if (!d_->core) {
        error = "cannot connect to PipeWire (is the service running?)";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    d_->registry = pw_core_get_registry(d_->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(d_->registry, &d_->registryListener, &kRegistryEvents,
                             d_.get());

    pw_thread_loop_unlock(d_->loop);

    if (pw_thread_loop_start(d_->loop) < 0) {
        error = "pw_thread_loop_start failed";
        return false;
    }
    running_ = true;
    return true;
}

void PwEngine::stop() {
    if (!d_ || !d_->loop) return;

    if (running_) {
        pw_thread_loop_lock(d_->loop);
        // Destroy paths before sinks: a loopback whose target vanished first
        // logs errors on the way out.
        for (auto &[name, p] : d_->paths)
            if (p.module) pw_impl_module_destroy(p.module);
        d_->paths.clear();
        for (auto &[_, link] : d_->manualLinks) pw_proxy_destroy(link);
        d_->manualLinks.clear();
        for (auto &[_, p] : d_->streamProxies) pw_proxy_destroy(p);
        d_->streamProxies.clear();
        for (auto &[_, p] : d_->clientProxies) pw_proxy_destroy(p);
        d_->clientProxies.clear();
        for (auto *p : d_->sinkProxies) pw_proxy_destroy(p);
        d_->sinkProxies.clear();
        if (d_->registry) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(d_->registry));
        d_->registry = nullptr;
        pw_thread_loop_unlock(d_->loop);
        pw_thread_loop_stop(d_->loop);
        running_ = false;
    }

    if (d_->core) { pw_core_disconnect(d_->core); d_->core = nullptr; }
    if (d_->context) { pw_context_destroy(d_->context); d_->context = nullptr; }
    pw_thread_loop_destroy(d_->loop);
    d_->loop = nullptr;
}

bool PwEngine::addNullSink(const std::string &name, const std::string &description,
                           int channels, std::string &error) {
    if (!running_) { error = "engine not started"; return false; }

    // Created through the server's "adapter" factory rather than by loading
    // module-adapter into our own context. A module loaded client-side builds a
    // node that lives only in this process and is never exported, so it never
    // appears in pactl and no application can play into it.
    //
    // monitor.channel-volumes keeps the monitor ports following the sink
    // volume, which is what makes a channel strip behave as people expect.
    std::string args =
        "{ factory.name = support.null-audio-sink"
        " node.name = " + quote(name) +
        " node.description = " + quote(description) +
        " media.class = Audio/Sink"
        " audio.position = " + positionFor(channels) +
        " monitor.channel-volumes = true"
        " monitor.passthrough = true"
        " node.autoconnect = false"
        " session.suspend-timeout-seconds = 0"
        // The session manager remembers a volume for every node it sees and
        // stamps it back the moment the node reappears. For a mixer that owns
        // its own levels that is not a nicety, it is a fight: the daemon sets a
        // monitor output to 20%, WirePlumber restores the 100% it had filed
        // under that node name, and the next start is deafening. These two say
        // "this node's levels are not yours to keep".
        " state.restore-props = false"
        " state.restore-target = false"
        " }";

    pw_thread_loop_lock(d_->loop);
    pw_properties *props = pw_properties_new_string(args.c_str());
    auto *proxy = static_cast<pw_proxy *>(pw_core_create_object(
        d_->core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
        props ? &props->dict : nullptr, 0));
    if (props) pw_properties_free(props);
    if (proxy) {
        d_->sinkProxies.push_back(proxy);
        d_->ourNames.push_back(name);
    }
    // Round-trip so the object exists before anything targets it.
    if (proxy) pw_core_sync(d_->core, PW_ID_CORE, 0);
    pw_thread_loop_unlock(d_->loop);

    if (!proxy) {
        error = "failed to create sink " + name;
        return false;
    }
    return true;
}

bool PwEngine::addVirtualSource(const std::string &name,
                               const std::string &description, int channels,
                               std::string &error) {
    if (!running_) { error = "engine not started"; return false; }

    // Same factory as a null sink; only media.class differs. That one word is
    // what makes it appear in `pactl list sources` for applications to pick.
    std::string args =
        "{ factory.name = support.null-audio-sink"
        " node.name = " + quote(name) +
        " node.description = " + quote(description) +
        " media.class = Audio/Source/Virtual"
        " audio.position = " + positionFor(channels) +
        " audio.rate = 48000"
        " node.autoconnect = false"
        " session.suspend-timeout-seconds = 0"
        " }";

    pw_thread_loop_lock(d_->loop);
    pw_properties *props = pw_properties_new_string(args.c_str());
    auto *proxy = static_cast<pw_proxy *>(pw_core_create_object(
        d_->core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
        props ? &props->dict : nullptr, 0));
    if (props) pw_properties_free(props);
    if (proxy) {
        d_->namedProxies[name] = proxy;
        d_->ourNames.push_back(name);
        pw_core_sync(d_->core, PW_ID_CORE, 0);
    }
    pw_thread_loop_unlock(d_->loop);

    if (!proxy) { error = "failed to create virtual source " + name; return false; }
    return true;
}

bool PwEngine::removeNode(const std::string &name) {
    if (!running_) return false;
    pw_thread_loop_lock(d_->loop);
    auto it = d_->namedProxies.find(name);
    const bool found = it != d_->namedProxies.end();
    if (found) {
        pw_proxy_destroy(it->second);
        d_->namedProxies.erase(it);
        pw_core_sync(d_->core, PW_ID_CORE, 0);
    }
    pw_thread_loop_unlock(d_->loop);
    return found;
}

bool PwEngine::addPath(const std::string &handle, const std::string &source,
                       const std::string &target, const std::string &description,
                       int channels, bool sourceIsSink, std::string &error) {
    PathSpec spec;
    spec.handle = handle;
    spec.source = source;
    spec.target = target;
    spec.description = description;
    spec.inChannels = channels;
    spec.outChannels = channels;
    spec.sourceIsSink = sourceIsSink;
    return addPath(spec, error);
}

bool PwEngine::removePath(const std::string &handle) {
    auto it = d_->paths.find(handle);
    if (it == d_->paths.end()) return false;
    // Erased under the loop lock: the registry callback walks this map on the
    // loop thread to apply deferred volumes, and erasing from under it is how
    // you get a crash that only happens while re-targeting the monitor mix.
    pw_thread_loop_lock(d_->loop);
    if (it->second.module) pw_impl_module_destroy(it->second.module);
    d_->paths.erase(it);
    pw_thread_loop_unlock(d_->loop);
    return true;
}

bool PwEngine::addPath(const PathSpec &spec, std::string &error) {
    if (!running_) { error = "engine not started"; return false; }
    if (d_->paths.count(spec.handle)) {
        error = "path already exists: " + spec.handle;
        return false;
    }

    const std::string capName = spec.handle + "-in";
    const std::string playName =
        spec.playbackName.empty() ? spec.handle + "-out" : spec.playbackName;

    // A loopback gives us a capture end and a playback end with a volume in
    // between -- exactly one channel strip. The two ends may have different
    // channel counts, which is how a mono microphone reaches both sides of a
    // stereo mix.
    // A published virtual source must remix. Its consumer is an arbitrary
    // application asking for whatever channel layout it likes, and pinning the
    // stream with stream.dont-remix left audioconvert to reconcile the mismatch
    // during client-node negotiation -- which segfaulted the daemon, and the
    // connecting application with it.
    const std::string remix = (spec.remix || spec.virtualSource)
                                  ? std::string()
                                  : std::string("   stream.dont-remix = true");
    // Both ends pinned to the graph rate, again so negotiation has nothing to
    // reconcile. Published recipes for virtual sources all do this. Monitor
    // fan-out also pins: two hardware sinks must not renegotiate the graph
    // rate against each other (that is the multi-Monitor "out of tune guitar"
    // warble).
    const std::string rate = (spec.virtualSource || spec.pinRate)
                                 ? std::string("   audio.rate = 48000"
                                               "   node.lock-rate = true"
                                               "   node.lock-quantum = true")
                                 : std::string();
    // Playback end only: primary Monitor out may drive the graph; extras must
    // not, or two ALSA clocks fight and adaptive resampling pitch-warbles
    // every source in the mix (capture cards included).
    const std::string playbackDriver =
        spec.followerOnly   ? std::string("   priority.driver = 0")
        : spec.pinRate      ? std::string("   priority.driver = 30000")
                            : std::string();
    // Do not try to split the two ends of a loopback into separate driver
    // groups. Both attempts were measured on a live graph, 2026-08-09:
    //
    //   node.group per end      no effect at all. The properties applied, the
    //                           graph did not re-partition, the target sink
    //                           stayed in the capture side's group.
    //   + node.link-group       constant crackle at every quantum, and at 80 ms
    //                           a dropout roughly every second.
    //
    // The second one is the answer to the first: module-loopback hands buffers
    // straight across when both ends run in one cycle, and it is the shared
    // group that guarantees they do. Separate the ends and the two sides run on
    // unrelated cycles with nothing sized to bridge them.
    //
    // So a Monitor fan-out really does put every target device in one driver
    // group, on one elected clock, and that is module-loopback working as
    // designed rather than something to undo here.
    const std::string sticky =
        spec.stickyTarget
            ? std::string("   node.dont-reconnect = true"
                          "   node.dont-fallback = true"
                          "   node.dont-move = true")
            : std::string();

    // Both ends of a loopback are Stream/*/Audio nodes, so the session manager
    // treats them as ordinary application streams and saves their volume by
    // node name. That is wrong for us twice over: the volume that means
    // something is the one this engine sets on the capture end, and a stale
    // saved value on the playback end silently attenuates a whole channel.
    //
    // It is not hypothetical -- it is how waveline-music-stream-out came to sit
    // at 0.015625 while every other channel was at 1.0, putting the Music
    // channel 36 dB down into the Stream mix with every link in `pw-link -l`
    // present and correct.
    const std::string noRestore =
        "   state.restore-props = false"
        "   state.restore-target = false";

    std::string args =
        "{ node.description = " + quote(spec.description) +
        " capture.props = {"
        "   node.name = " + quote(capName) +
        "   node.description = " + quote(spec.description) +
        "   media.class = Stream/Input/Audio"
        "   audio.position = " + positionFor(spec.inChannels)
        + (spec.source.empty()
               // Nothing to target: keep the session manager away from it
               // entirely so it cannot substitute the default source.
               ? std::string("   node.autoconnect = false")
               : "   target.object = " + quote(spec.source))
        + (spec.sourceIsSink && !spec.source.empty()
               ? std::string("   stream.capture.sink = true")
               : std::string())
        + "   node.passive = false" + remix + rate + noRestore +
        " }"
        " playback.props = {"
        "   node.name = " + quote(playName) +
        "   node.description = " + quote(spec.description)
        // A virtual source is the endpoint applications record from, so it
        // takes no target and must not be told not to autoconnect -- that flag
        // is for streams we link by hand.
        + (spec.virtualSource
               ? std::string("   media.class = Audio/Source/Virtual")
               : std::string("   media.class = Stream/Output/Audio"))
        + "   audio.position = " + positionFor(spec.outChannels)
        + (spec.virtualSource ? std::string()
                              : "   target.object = " + quote(spec.target))
        + "   node.passive = false" + remix + rate + playbackDriver + sticky +
              noRestore +
        " } }";

    pw_thread_loop_lock(d_->loop);
    auto *m = pw_context_load_module(d_->context, "libpipewire-module-loopback",
                                     args.c_str(), nullptr);
    if (m) {
        Path p;
        p.module = m;
        p.captureName = capName;
        p.target = spec.target;
        p.source = spec.source;
        p.channels = spec.inChannels;
        p.description = spec.description;
        p.sourceIsSink = spec.sourceIsSink;
        p.volume = spec.volume;
        p.muted = spec.muted;
        d_->paths[spec.handle] = std::move(p);
        d_->ourNames.push_back(capName);
        d_->ourNames.push_back(playName);
    }
    pw_thread_loop_unlock(d_->loop);

    if (!m) {
        error = "failed to create path " + spec.handle;
        return false;
    }
    return true;
}

namespace {

void applyVolumeLocked(PwEngine::Impl *impl, uint32_t nodeId, const Path &p) {
    auto *node = static_cast<pw_node *>(
        pw_registry_bind(impl->registry, nodeId, PW_TYPE_INTERFACE_Node,
                         PW_VERSION_NODE, 0));
    if (!node) return;

    float vols[SPA_AUDIO_MAX_CHANNELS];
    const int n = p.channels > 0 ? p.channels : 2;
    for (int i = 0; i < n; ++i) vols[i] = p.muted ? 0.0f : p.volume;

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod *pod = static_cast<const spa_pod *>(spa_pod_builder_add_object(
        &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
        SPA_PROP_mute, SPA_POD_Bool(p.muted),
        SPA_PROP_channelVolumes,
        SPA_POD_Array(sizeof(float), SPA_TYPE_Float, n, vols)));

    pw_node_set_param(node, SPA_PARAM_Props, 0, pod);
    pw_core_sync(impl->core, PW_ID_CORE, 0);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(node));
}

}  // namespace

bool PwEngine::setPathVolume(const std::string &handle, float volume) {
    auto it = d_->paths.find(handle);
    if (it == d_->paths.end()) return false;
    // Record it regardless: if the node is not registered yet the value is
    // applied from the registry callback when it appears.
    it->second.volume = volume;

    pw_thread_loop_lock(d_->loop);
    uint32_t id = 0;
    bool found;
    {
        std::lock_guard<std::mutex> lock(d_->nodesMutex);
        found = d_->findNodeIdLocked(it->second.captureName, id);
    }
    if (found) applyVolumeLocked(d_.get(), id, it->second);
    pw_thread_loop_unlock(d_->loop);
    return true;
}

bool PwEngine::setPathMuted(const std::string &handle, bool muted) {
    auto it = d_->paths.find(handle);
    if (it == d_->paths.end()) return false;
    it->second.muted = muted;
    return setPathVolume(handle, it->second.volume);
}

bool PwEngine::setPathLevel(const std::string &handle, float volume, bool muted) {
    auto it = d_->paths.find(handle);
    if (it == d_->paths.end()) return false;
    // Record the mute first, then let setPathVolume() emit the single Props
    // object that carries both.
    it->second.muted = muted;
    return setPathVolume(handle, volume);
}

bool PwEngine::setNodeVolume(const std::string &nodeName, float volume, bool muted) {
    if (!running_) return false;
    pw_thread_loop_lock(d_->loop);
    uint32_t id = 0;
    bool found;
    {
        std::lock_guard<std::mutex> lock(d_->nodesMutex);
        found = d_->findNodeIdLocked(nodeName, id);
    }
    if (found) {
        Path p;
        p.volume = volume;
        p.muted = muted;
        p.channels = 2;
        applyVolumeLocked(d_.get(), id, p);
    }
    pw_thread_loop_unlock(d_->loop);
    return found;
}

bool PwEngine::setNodeMuted(const std::string &nodeName, bool muted) {
    if (!running_) return false;
    pw_thread_loop_lock(d_->loop);
    uint32_t id = 0;
    bool found;
    {
        std::lock_guard<std::mutex> lock(d_->nodesMutex);
        found = d_->findNodeIdLocked(nodeName, id);
    }
    if (found) {
        auto *node = static_cast<pw_node *>(
            pw_registry_bind(d_->registry, id, PW_TYPE_INTERFACE_Node,
                             PW_VERSION_NODE, 0));
        if (node) {
            uint8_t buffer[512];
            spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            // Mute alone: no channelVolumes in the object, so the device keeps
            // the level it was left at.
            const spa_pod *pod = static_cast<const spa_pod *>(
                spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props,
                                           SPA_PARAM_Props, SPA_PROP_mute,
                                           SPA_POD_Bool(muted)));
            pw_node_set_param(node, SPA_PARAM_Props, 0, pod);
            pw_core_sync(d_->core, PW_ID_CORE, 0);
            pw_proxy_destroy(reinterpret_cast<pw_proxy *>(node));
        } else {
            found = false;
        }
    }
    pw_thread_loop_unlock(d_->loop);
    return found;
}

bool PwEngine::setNodeVolumeById(uint32_t nodeId, float volume, bool muted,
                                 int channels) {
    if (!running_ || nodeId == 0) return false;
    Path p;
    p.volume = volume;
    p.muted = muted;
    p.channels = channels > 0 ? channels : 2;
    pw_thread_loop_lock(d_->loop);
    applyVolumeLocked(d_.get(), nodeId, p);
    pw_thread_loop_unlock(d_->loop);
    return true;
}

bool PwEngine::setPathTarget(const std::string &handle, const std::string &target,
                             std::string &error) {
    auto it = d_->paths.find(handle);
    if (it == d_->paths.end()) { error = "no such path: " + handle; return false; }

    // A loopback's target is fixed at load time, so re-pointing means
    // recreating the module. Cheap, and it keeps one code path for both.
    const Path old = it->second;
    pw_thread_loop_lock(d_->loop);
    if (old.module) pw_impl_module_destroy(old.module);
    pw_thread_loop_unlock(d_->loop);
    d_->paths.erase(it);

    if (!addPath(handle, old.source, target, old.description, old.channels,
                 old.sourceIsSink, error))
        return false;
    setPathVolume(handle, old.volume);
    setPathMuted(handle, old.muted);
    return true;
}

bool PwEngine::linkPorts(const std::string &outNode, const std::string &outPort,
                         const std::string &inNode, const std::string &inPort,
                         std::string &error, bool asyncLink,
                         int activationTimeoutMs) {
    if (!running_) { error = "engine not started"; return false; }

    const std::string key =
        outNode + ":" + outPort + ">" + inNode + ":" + inPort +
        (asyncLink ? ":async" : "");

    uint32_t outId = 0, inId = 0;
    pw_proxy *staleProxy = nullptr;
    {
        std::lock_guard<std::mutex> lock(d_->nodesMutex);
        if (!d_->findPortIdLocked(outNode, outPort, true, outId)) {
            error = "output port not found: " + outNode + ":" + outPort;
            return false;
        }
        if (!d_->findPortIdLocked(inNode, inPort, false, inId)) {
            error = "input port not found: " + inNode + ":" + inPort;
            return false;
        }
        if (d_->hasPipewireLinkLocked(outId, inId)) return true;
        if (auto it = d_->manualLinks.find(key); it != d_->manualLinks.end()) {
            staleProxy = it->second;
            d_->manualLinks.erase(it);
        }
    }

    if (staleProxy) {
        pw_thread_loop_lock(d_->loop);
        pw_proxy_destroy(staleProxy);
        pw_core_sync(d_->core, PW_ID_CORE, 0);
        pw_thread_loop_unlock(d_->loop);
    }

    std::string args =
        "{ link.output.port = " + std::to_string(outId) +
        " link.input.port = " + std::to_string(inId) +
        " object.linger = false";
    if (asyncLink) args += " link.async = true";
    args += " }";

    pw_thread_loop_lock(d_->loop);
    pw_properties *props = pw_properties_new_string(args.c_str());
    auto *proxy = static_cast<pw_proxy *>(pw_core_create_object(
        d_->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
        props ? &props->dict : nullptr, 0));
    if (props) pw_properties_free(props);
    if (proxy) {
        std::lock_guard<std::mutex> lock(d_->nodesMutex);
        d_->manualLinks.emplace(key, proxy);
    }
    pw_core_sync(d_->core, PW_ID_CORE, 0);
    pw_thread_loop_unlock(d_->loop);

    if (!proxy) {
        error = "link " + outNode + ":" + outPort + " -> " + inNode + ":" + inPort +
                " failed";
        return false;
    }

    bool linked = false;
    const int maxAttempts = std::max(1, (activationTimeoutMs + 19) / 20);
    for (int attempt = 0; attempt < maxAttempts && !linked; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(d_->nodesMutex);
            linked = d_->hasPipewireLinkLocked(outId, inId);
        }
        if (linked) break;
        pw_thread_loop_lock(d_->loop);
        pw_core_sync(d_->core, PW_ID_CORE, 0);
        pw_thread_loop_unlock(d_->loop);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (linked) return true;

    pw_thread_loop_lock(d_->loop);
    if (proxy) pw_proxy_destroy(proxy);
    pw_core_sync(d_->core, PW_ID_CORE, 0);
    pw_thread_loop_unlock(d_->loop);
    {
        std::lock_guard<std::mutex> lock(d_->nodesMutex);
        d_->manualLinks.erase(key);
    }
    error = "link did not activate: " + outNode + ":" + outPort + " -> " + inNode +
            ":" + inPort;
    return false;
}

void PwEngine::clearLinks() {
    if (!running_) return;
    std::vector<std::string> drop;
    {
        std::lock_guard<std::mutex> lock(d_->nodesMutex);
        drop.reserve(d_->manualLinks.size());
        for (const auto &[key, _] : d_->manualLinks) drop.push_back(key);
    }
    // Through dropManualLinks so a rewire waits for the teardown to land before
    // it starts re-linking the same port pairs.
    dropManualLinks(drop);
}

bool PwEngine::nodeStalled(const std::string &name) const {
    if (!running_ || name.empty()) return false;

    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    uint32_t nodeId = 0;
    if (!d_->findNodeIdLocked(name, nodeId)) return false;
    auto it = d_->nodes.find(nodeId);
    if (it == d_->nodes.end()) return false;

    // No info event yet; say nothing rather than cry wolf on a node that is
    // still being set up.
    if (it->second.state == PwNode::kStateUnknown) return false;
    if (it->second.state == PW_NODE_STATE_ERROR) return true;
    // Suspended *and* in no driver group. Suspended alone is how an idle path
    // waits for work; it is the missing driver that makes it unschedulable.
    return it->second.state == PW_NODE_STATE_SUSPENDED && it->second.driverId == 0;
}

bool PwEngine::hasManualLinkTo(const std::string &inNode,
                               const std::string &inPort) const {
    if (!running_ || inNode.empty()) return false;

    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    uint32_t nodeId = 0;
    if (!d_->findNodeIdLocked(inNode, nodeId)) return false;
    for (const auto &[pid, p] : d_->ports) {
        if (p.nodeId != nodeId || p.isOutput) continue;
        if (!inPort.empty() && p.name != inPort) continue;
        if (d_->hasIncomingPipewireLinkLocked(pid)) return true;
    }
    return false;
}

namespace {

// "outNode:outPort>inNode:inPort" as built by linkPorts, with the ":async"
// suffix that async edges carry stripped off the input side.
struct LinkKeyParts {
    std::string outNode, outPort, inNode, inPort;
};

bool splitLinkKey(const std::string &key, LinkKeyParts &parts) {
    const auto gt = key.find('>');
    if (gt == std::string::npos) return false;
    const std::string outSide = key.substr(0, gt);
    std::string inSide = key.substr(gt + 1);
    static constexpr const char kAsync[] = ":async";
    constexpr std::size_t kAsyncLen = sizeof(kAsync) - 1;
    if (inSide.size() >= kAsyncLen &&
        inSide.compare(inSide.size() - kAsyncLen, kAsyncLen, kAsync) == 0)
        inSide.resize(inSide.size() - kAsyncLen);
    const auto outColon = outSide.find(':');
    const auto inColon = inSide.find(':');
    if (outColon == std::string::npos || inColon == std::string::npos) return false;
    parts.outNode = outSide.substr(0, outColon);
    parts.outPort = outSide.substr(outColon + 1);
    parts.inNode = inSide.substr(0, inColon);
    parts.inPort = inSide.substr(inColon + 1);
    return true;
}

}  // namespace

// Destroys `drop` and waits for PipeWire to take the links out of the registry.
// The wait is the point: linkPorts() reads the registry to decide a pair is
// already linked, so without it the next link onto the same pair is skipped and
// nothing gets wired -- which is why retargeting Audio Sharing from one input
// device to another only took effect on the second attempt.
void PwEngine::dropManualLinks(const std::vector<std::string> &drop) {
    if (drop.empty()) return;

    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    pairs.reserve(drop.size());

    pw_thread_loop_lock(d_->loop);
    {
        std::lock_guard<std::mutex> lock(d_->nodesMutex);
        for (const std::string &key : drop) {
            auto it = d_->manualLinks.find(key);
            if (it == d_->manualLinks.end()) continue;
            LinkKeyParts p;
            uint32_t outId = 0, inId = 0;
            if (splitLinkKey(key, p) &&
                d_->findPortIdLocked(p.outNode, p.outPort, true, outId) &&
                d_->findPortIdLocked(p.inNode, p.inPort, false, inId))
                pairs.emplace_back(outId, inId);
            pw_proxy_destroy(it->second);
            d_->manualLinks.erase(it);
        }
    }
    pw_core_sync(d_->core, PW_ID_CORE, 0);
    pw_thread_loop_unlock(d_->loop);

    waitForLinksDropped(pairs);
}

void PwEngine::waitForLinksDropped(
    const std::vector<std::pair<uint32_t, uint32_t>> &portPairs) {
    if (portPairs.empty()) return;

    // ~250ms ceiling. Giving up leaves the old behaviour rather than hanging a
    // rewire on a link the server never reports gone.
    for (int attempt = 0; attempt < 25; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(d_->nodesMutex);
            bool remaining = false;
            for (const auto &[outId, inId] : portPairs) {
                if (d_->hasPipewireLinkLocked(outId, inId)) {
                    remaining = true;
                    break;
                }
            }
            if (!remaining) return;
        }
        pw_thread_loop_lock(d_->loop);
        pw_core_sync(d_->core, PW_ID_CORE, 0);
        pw_thread_loop_unlock(d_->loop);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void PwEngine::destroyRegistryObject(uint32_t id) {
    if (!running_ || id == 0 || id == PW_ID_CORE) return;
    pw_thread_loop_lock(d_->loop);
    if (d_->registry) {
        pw_registry_destroy(d_->registry, id);
        // Synced here so the global_remove lands before the caller looks at
        // the registry again -- the removal is what drives every handler that
        // undoes the device, and a caller that re-reads first would see the
        // node still present and conclude the destroy did nothing.
        pw_core_sync(d_->core, PW_ID_CORE, 0);
    }
    pw_thread_loop_unlock(d_->loop);
}

void PwEngine::forgetLinksTo(const std::string &inNode, const std::string &inPort) {
    if (!running_ || inNode.empty()) return;

    std::vector<std::string> drop;
    {
        std::lock_guard<std::mutex> lock(d_->nodesMutex);
        for (const auto &[key, _] : d_->manualLinks) {
            LinkKeyParts p;
            if (!splitLinkKey(key, p)) continue;
            if (p.inNode != inNode) continue;
            if (!inPort.empty() && p.inPort != inPort) continue;
            drop.push_back(key);
        }
    }
    dropManualLinks(drop);
}

void PwEngine::forgetLinksForNode(const std::string &nodeName) {
    if (!running_ || nodeName.empty()) return;

    std::vector<std::string> drop;
    {
        std::lock_guard<std::mutex> lock(d_->nodesMutex);
        for (const auto &[key, _] : d_->manualLinks) {
            LinkKeyParts p;
            if (!splitLinkKey(key, p)) continue;
            if (p.outNode == nodeName || p.inNode == nodeName) drop.push_back(key);
        }
    }
    dropManualLinks(drop);
}

bool PwEngine::hasPathNodes(const std::string &handle) const {
    if (!running_) return false;
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    auto it = d_->paths.find(handle);
    if (it == d_->paths.end()) return false;
    uint32_t id = 0;
    const std::string playName = handle + "-out";
    const char *port = it->second.channels == 1 ? "input_MONO" : "input_FL";
    return d_->findNodeIdLocked(it->second.captureName, id) &&
           d_->findNodeIdLocked(playName, id) &&
           d_->findPortIdLocked(it->second.captureName, port, false, id);
}

// A mono path's capture port is input_MONO, not input_FL. Asking for input_FL
// on the microphone paths meant hasPathNodes() was permanently false for them,
// so every rewire tore down and rebuilt a pair of loopbacks that were working
// perfectly well.
std::string PwEngine::pathCapturePort(const std::string &handle) const {
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    auto it = d_->paths.find(handle);
    if (it != d_->paths.end() && it->second.channels == 1) return "input_MONO";
    return "input_FL";
}

bool PwEngine::pathCaptureReady(const std::string &handle) const {
    return hasPathNodes(handle);
}

bool PwEngine::pathExists(const std::string &handle) const {
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    return d_->paths.count(handle) > 0;
}

std::string PwEngine::pathTarget(const std::string &handle) const {
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    const auto it = d_->paths.find(handle);
    return it == d_->paths.end() ? std::string() : it->second.target;
}

// Destroying a live loopback is never free: the replacement can fail to load
// (it used to, constantly, once the process ran out of descriptors) and then
// the channel has no path at all and no amount of retrying brings it back.
//
// So the module is only ever torn down when it is genuinely gone. If we still
// hold it, the registry is simply behind -- wait for it instead.
bool PwEngine::repairPath(const PathSpec &spec, std::string &error) {
    const std::string capName = spec.handle + "-in";
    const bool held = pathExists(spec.handle);

    if (held) {
        if (hasPathNodes(spec.handle) ||
            waitForPort(capName, pathCapturePort(spec.handle), false, 2000)) {
            // Refresh levels even when the module is kept — callers pass the
            // desired volume on every repair after a master rebuild.
            setPathVolume(spec.handle, spec.volume);
            setPathMuted(spec.handle, spec.muted);
            return true;
        }
        removePath(spec.handle);
    }

    const std::string port =
        spec.inChannels == 1 ? "input_MONO" : "input_FL";
    // A loopback's ports appear when its capture stream has negotiated, and
    // how long that takes depends on how busy the graph already is -- not on
    // anything about this path. Building the full graph is a dozen loopbacks
    // and thirty filter nodes, so the paths created last are racing the most
    // loaded server, and on a slow machine one of them loses.
    //
    // It was a hard two-second wait and then a failed startup, which on a
    // Steam Deck meant wavelined never came up at all: the daemon died, the
    // unit restarted it, and it lost the same race again. Which path drew the
    // short straw moved around from boot to boot -- the tell that this is
    // load, not a path that cannot be built.
    //
    // So: try again rather than give up, the way the mic path already does.
    // The retry costs nothing on a machine that was going to win the race
    // anyway, because the wait ends the moment the port shows up.
    constexpr int kPortWaitMs = 4000;
    constexpr int kAttempts = 3;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (!addPath(spec, error)) return false;
        sync();
        if (waitForPort(capName, port, false, kPortWaitMs)) {
            setPathVolume(spec.handle, spec.volume);
            setPathMuted(spec.handle, spec.muted);
            return true;
        }
        // Nothing to keep: a loopback whose capture end never negotiated is
        // not going to start later, and leaving it behind would make the next
        // addPath fail on "path already exists".
        removePath(spec.handle);
    }
    error = "path capture not ready: " + capName;
    return false;
}

bool PwEngine::hasPort(const std::string &node, const std::string &port,
                       bool wantOutput) const {
    if (!running_) return false;
    uint32_t id = 0;
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    return d_->findPortIdLocked(node, port, wantOutput, id);
}

uint32_t PwEngine::portId(const std::string &node, const std::string &port,
                          bool wantOutput) const {
    if (!running_) return 0;
    uint32_t id = 0;
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    return d_->findPortIdLocked(node, port, wantOutput, id) ? id : 0;
}

std::vector<std::string> PwEngine::outputPortNames(const std::string &node) const {
    std::vector<std::string> out;
    if (!running_ || node.empty()) return out;
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    uint32_t nodeId = 0;
    if (!d_->findNodeIdLocked(node, nodeId)) return out;
    for (const auto &[_, p] : d_->ports) {
        if (p.nodeId != nodeId || !p.isOutput || p.name.empty()) continue;
        out.push_back(p.name);
    }
    return out;
}

std::string PwEngine::resolveMidiOutputPort(const std::string &node,
                                            const std::string &preferredPort) const {
    const std::vector<std::string> ports = outputPortNames(node);
    if (ports.empty()) return {};

    auto hasPort = [&](const std::string &name) {
        return std::find(ports.begin(), ports.end(), name) != ports.end();
    };
    if (!preferredPort.empty() && hasPort(preferredPort)) return preferredPort;

    static const char *const kNames[] = {"output", "Midi Out", "out", "playback"};
    for (const char *name : kNames) {
        if (hasPort(name)) return name;
    }

    auto score = [](const std::string &port) {
        if (port.find("Transport") != std::string::npos) return -1;
        if (port.find("(USB MIDI)") != std::string::npos) return 4;
        const bool capture = port.find("(capture)") != std::string::npos;
        const bool through = port.find("Through") != std::string::npos;
        if (capture && !through) return 3;
        if (capture) return 2;
        if (!through) return 1;
        return 0;
    };

    std::string best;
    int bestScore = -1;
    for (const std::string &port : ports) {
        const int s = score(port);
        if (s > bestScore) {
            bestScore = s;
            best = port;
        }
    }
    return best;
}

bool PwEngine::waitForPort(const std::string &node, const std::string &port,
                           bool wantOutput, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (hasPort(node, port, wantOutput)) return true;
        sync();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return hasPort(node, port, wantOutput);
}

void PwEngine::sync() {
    if (!running_) return;
    pw_thread_loop_lock(d_->loop);
    pw_core_sync(d_->core, PW_ID_CORE, 0);
    pw_thread_loop_unlock(d_->loop);
}

std::string PwEngine::defaultSinkName() const {
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    // The real default, as PipeWire reports it. Picking "the first alsa_output
    // we happen to see" sent the monitor mix to an idle S/PDIF port instead of
    // the speakers actually in use.
    if (!d_->defaultSink.empty() &&
        d_->defaultSink.rfind("waveline-", 0) != 0)  // never target our own sinks
        return d_->defaultSink;
    for (const auto &[id, n] : d_->nodes) {
        if (n.isOurs || n.mediaClass != "Audio/Sink") continue;
        if (n.name.rfind("alsa_output.", 0) == 0) return n.name;
    }
    return {};
}

std::string PwEngine::defaultSourceName() const {
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    // Same reasoning as defaultSinkName: take PipeWire's own answer, and never
    // our own virtual sources -- the processed microphone this graph publishes
    // is frequently the default, and feeding it back into its own input is a
    // loop rather than a microphone.
    if (!d_->defaultSource.empty() &&
        d_->defaultSource.rfind("waveline-", 0) != 0)
        return d_->defaultSource;
    for (const auto &[id, n] : d_->nodes) {
        if (n.isOurs || n.mediaClass != "Audio/Source") continue;
        if (n.name.rfind("alsa_input.", 0) == 0) return n.name;
    }
    return {};
}

std::vector<PwNode> PwEngine::nodes() const {
    std::vector<PwNode> out;
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    out.reserve(d_->nodes.size());
    for (const auto &[id, n] : d_->nodes) {
        PwNode copy = n;
        resolveClientProcess(copy, *d_);
        out.push_back(std::move(copy));
    }
    return out;
}

PwEngine::GraphClock PwEngine::graphClock() const {
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    return d_->clock;
}

std::string PwEngine::driverNodeName() const {
    std::lock_guard<std::mutex> lock(d_->nodesMutex);
    // Every node in a driver group carries the id of the node driving it, and
    // the driver itself carries none. So the group's driver is found by taking
    // a member's driver-id and looking it up -- the driver does not announce
    // itself, it is only ever pointed at.
    //
    // Two things make the naive version of that wrong, and it reported a
    // *follower* as the system clock until both were handled.
    //
    // Node ids are reused. A session restart destroys and recreates every
    // hardware node, and our registry mirror can still hold a dead entry whose
    // stale driver-id now names a live node that has nothing to do with it --
    // the same hazard findNodeIdLocked() exists for. Taking the first entry we
    // happened to iterate meant trusting whichever was oldest.
    //
    // So: vote. Every current hardware node points at its driver, they
    // overwhelmingly agree, and a stale straggler cannot outvote the live
    // graph. Then verify the winner really is a driver -- a genuine one either
    // carries no driver-id or points at itself -- because the failure being
    // guarded against is precisely naming a node that is following someone
    // else.
    // Only *cross* votes count: a node naming some other node as its driver.
    //
    // A node whose driver-id is its own id has told us nothing. It is either a
    // lone driver of a group with one member, or -- far more often here -- a
    // stale initial reading. Every hardware node is announced self-driving
    // before the graph is wired, and PipeWire does not reliably push a fresh
    // info event when it later joins a group, so our mirror keeps the value
    // from bind time. Measured on this machine: pw-dump showed all six inputs
    // driven by node 737, while the mirror had five of them still pointing at
    // themselves and only one carrying 737.
    //
    // Counting those self-votes produced a six-way tie at one vote each, and
    // the tie broke on lowest node id -- so the diagnostics confidently named
    // a *follower* as the system clock, which is worse than saying nothing.
    // Dropping them leaves only the nodes that actually observed a driver, and
    // one honest cross-vote beats any number of stale self-references.
    std::map<uint32_t, int> votes;
    for (const auto &[id, n] : d_->nodes) {
        if (n.isOurs || n.driverId == 0 || n.driverId == id) continue;
        if (n.name.rfind("alsa_", 0) != 0) continue;
        ++votes[n.driverId];
    }
    // Our own filters are asked only if no hardware node answered. They sit in
    // the same group and give the same answer, but mid-rewire they can be in a
    // group of their own, and reporting a Waveline filter as the system's
    // clock explains nothing to anybody.
    if (votes.empty()) {
        for (const auto &[id, n] : d_->nodes) {
            if (n.driverId == 0 || n.driverId == id) continue;
            ++votes[n.driverId];
        }
    }

    while (!votes.empty()) {
        const auto best = std::max_element(
            votes.begin(), votes.end(),
            [](const auto &a, const auto &b) { return a.second < b.second; });
        const uint32_t candidate = best->first;
        const auto it = d_->nodes.find(candidate);
        if (it != d_->nodes.end() &&
            (it->second.driverId == 0 || it->second.driverId == candidate))
            return it->second.name;
        // Either the id no longer exists or it names a follower. Both mean the
        // votes for it came from stale entries; drop it and ask the rest.
        votes.erase(best);
    }
    return {};
}

bool PwEngine::setForcedQuantum(uint32_t frames, std::string &error) {
    pw_thread_loop_lock(d_->loop);
    pw_metadata *settings = d_->settings;
    if (!settings) {
        pw_thread_loop_unlock(d_->loop);
        error = "PipeWire settings metadata is not available yet";
        return false;
    }
    char value[32];
    std::snprintf(value, sizeof(value), "%u", frames);
    // Subject 0 and a null type, exactly as `pw-metadata -n settings 0
    // clock.force-quantum N` does it. The server applies it to the running
    // graph: nodes reconfigure, and nothing is reopened or rewired.
    pw_metadata_set_property(settings, 0, "clock.force-quantum", nullptr, value);
    pw_thread_loop_unlock(d_->loop);
    return true;
}

}  // namespace waveline

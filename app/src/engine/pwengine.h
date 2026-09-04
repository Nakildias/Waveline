// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Thin C++ wrapper over libpipewire for building and holding an audio graph.
//
// The graph is built out of PipeWire modules loaded into *our* context, which
// means the nodes live exactly as long as this process. object.linger is
// deliberately never set: a crashed or killed daemon must not leave orphaned
// sinks behind for the user to clean up by hand.
//
// Everything here runs under a pw_thread_loop. Any call that touches the core
// must hold its lock, so the public API does that internally and callers never
// see it.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_impl_module;
struct pw_registry;
struct spa_hook;

namespace waveline {

// A node PipeWire has told us about. Enough to populate an output picker or
// route an application, without exposing SPA types to the UI.
struct PwNode {
    uint32_t id = 0;
    std::string name;         // node.name
    std::string description;  // node.description, falls back to name
    std::string mediaClass;   // Audio/Sink, Audio/Source, Stream/Output/Audio...
    std::string appName;      // application.name, for streams
    uint32_t processId = 0;   // application.process.id
    std::string processBinary; // application.process.binary
    uint32_t clientId = 0;    // client.id, for streams that inherit process.* from Client
    bool isOurs = false;      // created by this engine
    // Live scheduling state, filled in for our own nodes once bound. See
    // PwEngine::nodeStalled(). The "not told yet" sentinel cannot be -1:
    // PW_NODE_STATE_ERROR is -1, and conflating the two hides exactly the
    // nodes worth reporting.
    static constexpr int kStateUnknown = -2;
    int state = kStateUnknown;  // pw_node_state once known
    uint32_t driverId = 0;      // node.driver-id; 0 when PipeWire assigned none
    // The ALSA PCM behind this node, from api.alsa.path ("hw:5"). Empty for
    // everything that is not a hardware device.
    //
    // Deliberately an identity rather than a latency. This used to carry a
    // computed latencyUs, built from SPA_PARAM_Latency plus the graph clock
    // plus the node's own node.latency request -- an arithmetic answer that
    // ranked two identical microphones 4x apart because one of them happened
    // to ask for a short quantum and the other did not. The number is now
    // measured from the kernel instead; see engine/alsadelay.h. What the
    // engine owes the rest of the system is which PCM to go and read.
    std::string alsaPath;
    // api.alsa.pcm.card, or -1. This is the identity the latency probe uses:
    // alsaPath above is for display only, because PipeWire writes whichever
    // ALSA device name it opened ("hw:6" on one microphone, "front:5" on the
    // next) and only the card index is always an integer.
    int alsaCard = -1;
    // waveline.hidden-latency, set by a device's own WirePlumber rule. True for
    // hardware that processes audio before handing it over -- a conference
    // camera, a headset with onboard AEC -- where the delay you hear is
    // decided somewhere this machine cannot see, and any figure measured here
    // would be a fraction of the truth presented as the whole of it.
    bool hidesLatency = false;
    // api.alsa.headroom in frames, -1 when the node is not an ALSA device.
    //
    // Read back rather than assumed, because it is fixed when the device is
    // opened: changing it means writing a WirePlumber rule and restarting
    // WirePlumber, and "did that actually take" is otherwise unanswerable from
    // inside the mixer. See MixerService::EffectiveOutputHeadroom().
    int alsaHeadroom = -1;
};

class PwEngine {
public:
    PwEngine();
    ~PwEngine();
    PwEngine(const PwEngine &) = delete;
    PwEngine &operator=(const PwEngine &) = delete;

    // Connects to PipeWire and starts the thread loop.
    bool start(std::string &error);
    void stop();
    bool running() const { return running_; }

    // Creates a virtual sink applications can play into. Returns false and
    // fills error on failure.
    bool addNullSink(const std::string &name, const std::string &description,
                     int channels, std::string &error);

    // A virtual *recording device*: applications select it as a microphone and
    // we feed its input ports. Built with the server's adapter factory, like
    // addNullSink -- a client-side module-loopback published as
    // Audio/Source/Virtual segfaults libspa-audioconvert on PipeWire 1.6.8
    // (stock pw-loopback crashes the same way, so it is not ours to fix).
    bool addVirtualSource(const std::string &name, const std::string &description,
                          int channels, std::string &error);

    // Destroys a node created by addNullSink/addVirtualSource.
    bool removeNode(const std::string &name);

    // Routes audio from one node to another with its own volume control.
    //
    // sourceIsSink must be true when `source` names a sink and we want its
    // monitor. PipeWire needs stream.capture.sink for that; without it the
    // capture end silently attaches to the default *source* -- the microphone --
    // and every path quietly carries the wrong audio.
    struct PathSpec {
        std::string handle;
        std::string source;       // empty = link it yourself with linkPorts()
        std::string target;
        std::string description;
        int inChannels = 2;
        int outChannels = 2;
        bool sourceIsSink = true;
        // A mono source feeding a stereo mix must be remixed, or it arrives on
        // the left channel only. Off preserves exact channel placement.
        bool remix = false;
        // Publish the playback end as a virtual *source* instead of pointing it
        // at a target. This is the only way a filter chain becomes selectable as
        // a recording device: pipewire-pulse does not export raw pw_filter DSP
        // nodes, so applications never see them however their media.class is
        // set. The loopback's playback end is a properly negotiated node and
        // does show up in pactl.
        bool virtualSource = false;
        // Optional override for the playback node's name, so a published source
        // is not called "<handle>-out".
        std::string playbackName;
        // Applied when the capture node appears (and stored on the Path so a
        // later setPathMuted cannot stamp unity over a pending level).
        float volume = 1.0f;
        bool muted = false;
        // Pin both ends to 48 kHz / lock quantum. Needed for Monitor fan-out:
        // two ALSA sinks with independent clocks otherwise make PipeWire's
        // adaptive resampler hunt and pitch-warble the whole graph.
        bool pinRate = false;
        // Asks PipeWire not to consider this path's playback end as the graph
        // driver (priority.driver = 0 instead of 30000).
        //
        // Measured to be inert: a loopback end is a stream, `node.driver` is
        // false on it, and only driver-capable nodes are ever candidates -- the
        // election happens purely among the ALSA devices, on their own
        // priority.driver. Kept because the property is harmless and honest
        // about intent -- but nothing here decides which clock a Monitor
        // fan-out runs on. See addPath() for the two ways that were tried.
        bool followerOnly = false;
        // Never fall back to the default sink if the assigned target is gone
        // (headphones unplug must not dump mic-monitor onto speakers).
        bool stickyTarget = false;
    };

    bool addPath(const PathSpec &spec, std::string &error);
    bool removePath(const std::string &handle);

    // Convenience for the common symmetric case.
    bool addPath(const std::string &handle, const std::string &source,
                 const std::string &target, const std::string &description,
                 int channels, bool sourceIsSink, std::string &error);

    // 0.0 .. 1.0+, applied to the loopback created by addPath.
    bool setPathVolume(const std::string &handle, float volume);
    bool setPathMuted(const std::string &handle, bool muted);
    // Both in one write. The Props object carries mute and channelVolumes
    // together, so setting them separately binds the node -- and syncs the
    // core -- twice for no reason. Matters when re-asserting levels across
    // every path at once, where the doubled proxy churn is audible.
    bool setPathLevel(const std::string &handle, float volume, bool muted);

    // Volume and mute on one of our nodes, looked up by node.name.
    bool setNodeVolume(const std::string &nodeName, float volume, bool muted);
    // Mute flag only. Used on the real output devices at shutdown, where
    // writing a volume as well would overwrite whatever the user had set.
    bool setNodeMuted(const std::string &nodeName, bool muted);

    // Volume and mute on a PipeWire node by registry id (application streams).
    bool setNodeVolumeById(uint32_t nodeId, float volume, bool muted,
                           int channels = 2);

    // Re-points an existing path at a different output. Used for "send the
    // monitor mix to these headphones instead".
    bool setPathTarget(const std::string &handle, const std::string &target,
                       std::string &error);

    // Explicit port-to-port link. Needed for nodes that cannot be targeted by
    // name -- a pw_filter exposes raw DSP ports and is never auto-connected.
    // Port names are matched by suffix, so "capture_MONO" finds
    // "<node>:capture_MONO".
    bool linkPorts(const std::string &outNode, const std::string &outPort,
                   const std::string &inNode, const std::string &inPort,
                   std::string &error, bool asyncLink = false,
                   int activationTimeoutMs = 1000);

    // True when one of our nodes exists but PipeWire is not scheduling it: no
    // driver was assigned and it is not running. Such a node passes no audio no
    // matter how its links read, which is the failure that hides behind a graph
    // where every link is present -- a mix meters happily while its output is
    // silent. An idle node that still belongs to a driver group is *not*
    // stalled; suspending an idle path is normal.
    bool nodeStalled(const std::string &name) const;

    bool hasPort(const std::string &node, const std::string &port, bool wantOutput) const;
    // Registry object id for a port. USB MIDI ports keep the same node/name
    // across replug but receive a new id, which makes this a useful generation.
    uint32_t portId(const std::string &node, const std::string &port,
                    bool wantOutput) const;

    // Output port names registered for a node (empty when unknown or none yet).
    std::vector<std::string> outputPortNames(const std::string &node) const;

    // Pick a MIDI source port on a bridge/device node. Prefers hardware capture
    // ports (ALSA seq names them "... (capture)") over Through/Transport ports.
    std::string resolveMidiOutputPort(const std::string &node,
                                      const std::string &preferredPort = {}) const;

    // Poll until a port appears in the registry (filter/loopback nodes register late).
    bool waitForPort(const std::string &node, const std::string &port, bool wantOutput,
                     int timeoutMs = 200);

    // True when the loopback's capture node is present in the registry.
    bool hasPathNodes(const std::string &handle) const;
    bool pathCaptureReady(const std::string &handle) const;

    // True when we still hold the loopback module, whatever the registry says.
    bool pathExists(const std::string &handle) const;
    // Where a path currently plays. Empty when there is no such path.
    std::string pathTarget(const std::string &handle) const;
    // "input_FL" or, for a mono path, "input_MONO".
    std::string pathCapturePort(const std::string &handle) const;

    // Recreate a path only when it is genuinely gone; an existing module is
    // waited for, never torn down.
    bool repairPath(const PathSpec &spec, std::string &error);

    // Drop every manual link created by linkPorts(). Call before re-wiring
    void clearLinks();

    // Destroy a registry object we do not own -- specifically, an ALSA node
    // whose card has been unplugged and that PipeWire left behind anyway. The
    // same operation `pw-cli destroy <id>` performs.
    //
    // This is deliberately narrow. Nothing else in this engine reaches for
    // another client's objects, and an orphan is the one case where it is
    // justified: the node is provably dead (its /proc/asound/card<N> is gone),
    // it cannot be revived, and while it exists it is a running driver
    // candidate whose hardware raises no interrupts, and a second object
    // answering to the same node.name as the device's replacement. Leaving it
    // costs a stalled graph and a monitor path that re-attaches to the corpse
    // on replug; see MixerService::sweepDeadHardwareNodes().
    //
    // Best-effort. The server may refuse, and the caller must work either way.
    void destroyRegistryObject(uint32_t id);

    // Drop cached manual links touching a node. Needed when a pw_filter node
    // is destroyed out from under linkPorts(): the PipeWire link vanishes but
    // the cache still thinks it exists, so the next linkPorts() is a no-op.
    void forgetLinksForNode(const std::string &nodeName);

    // Drop manual links into a specific input port. Used to swap one mix
    // leg's source without tearing down the whole graph.
    void forgetLinksTo(const std::string &inNode, const std::string &inPort = {});

    // True when PipeWire reports an active link into inNode[:inPort].
    bool hasManualLinkTo(const std::string &inNode,
                         const std::string &inPort = {}) const;

    // Wait for the registry to settle after creating nodes or clearing links.
    void sync();

    // The system's current default-ish hardware sink, so the monitor mix can be
    // sent somewhere audible without the user having to name a device.
    std::string defaultSinkName() const;
    // PipeWire's default *capture* device. What the mixer feeds its microphone
    // path from when the installed device profile pins no particular ALSA node
    // -- i.e. on any microphone we have no special knowledge of.
    std::string defaultSourceName() const;

    std::vector<PwNode> nodes() const;

    // ---- the graph clock ---------------------------------------------------
    //
    // Read from, and written to, PipeWire's "settings" metadata: the same
    // place `pw-metadata -n settings` shows and the same route pw-metadata
    // takes. This is deliberately not a config file. clock.force-quantum
    // applies live -- the graph reconfigures, and nothing is reopened, no
    // session is restarted and no device is rewired -- which is the only
    // reason the mixer can offer a latency control at all rather than an
    // instruction to edit a file and log out.
    //
    // The earlier binding to this metadata existed to resolve a node's Latency
    // param into a time, and the plan was to delete it along with that
    // arithmetic. It stays, for the opposite reason: not to infer a number we
    // could not observe, but to own and report one we can.
    struct GraphClock {
        uint32_t rate = 0;          // clock.rate, the configured default
        uint32_t quantum = 0;       // clock.quantum, the configured default
        uint32_t forcedRate = 0;    // clock.force-rate, 0 when not forced
        uint32_t forcedQuantum = 0; // clock.force-quantum, 0 when not forced
        uint32_t minQuantum = 0;
        uint32_t maxQuantum = 0;
        // True once the metadata has actually answered. Everything above is a
        // guess until then, and a guess must not be drawn as a measurement.
        bool known = false;
    };
    GraphClock graphClock() const;

    // Pins the graph quantum, or releases it back to negotiation when frames
    // is 0. Returns false when the settings metadata is not available -- which
    // happens when PipeWire is still starting, so callers should be prepared
    // to try again rather than treat it as fatal.
    bool setForcedQuantum(uint32_t frames, std::string &error);

    // node.name of whatever is currently driving the graph, or empty when
    // nothing is. This is the single line that would have turned the whole
    // "latency seems random" investigation into a glance: it is the thing that
    // silently changes when an unrelated device is plugged in.
    std::string driverNodeName() const;

    // Called on the loop thread whenever the node list changes.
    void setOnGraphChanged(std::function<void()> cb);

    // Called for each node as it appears. Used to route new application
    // streams the moment they start playing.
    // Fired when the user changes their default *input* device, and only when
    // the name actually changes. Runs on the PipeWire thread loop, so the
    // handler must not touch anything that is not thread-safe.
    void setOnDefaultSourceChanged(std::function<void()> cb);

    void setOnNodeAdded(std::function<void(const PwNode &)> cb);
    void setOnNodeRemoved(std::function<void(const PwNode &)> cb);

    // The session manager was replaced -- WirePlumber restarted. Its "default"
    // metadata is where every stream routing decision is written, and it is
    // destroyed and recreated empty, so everything routed has to be routed
    // again. No node is added or removed when this happens, which is why
    // nothing else notices. Runs on the PipeWire thread loop.
    void setOnSessionManagerRestarted(std::function<void()> cb);

    // Sends an application's stream to a sink, by writing target.object into
    // PipeWire's "default" metadata -- the same route `pactl move-sink-input`
    // takes. Creating links directly would be undone by the session manager.
    bool setStreamTarget(uint32_t nodeId, const std::string &sinkName,
                         std::string &error);

    // Where a playback stream's audio is *actually* going, read from the links
    // in the graph.
    //
    // Deliberately not "what did we ask for". target.object is one shared
    // metadata key on a last-writer-wins basis, so reading our own write back
    // would only ever confirm that we wrote it. Another session manager or
    // effects daemon that moves the stream after us leaves that key looking
    // exactly as we left it while the audio goes somewhere else entirely --
    // which is the case this exists to catch. Links cannot lie.
    //
    // False when the stream is linked to nothing, which is the ordinary state
    // for a moment after it appears and for as long as it is idle.
    bool streamSinkNode(uint32_t streamNodeId, PwNode &out) const;

    // The mirror of the above for a *recording* stream: which node is feeding
    // it, read from the links rather than from what anything asked for.
    //
    // This is what separates "an application is using a microphone" from "an
    // application is capturing a sink's monitor". Both are Stream/Input/Audio
    // and both look identical from the node alone; only the far end of the
    // link says which one it is, because a monitor capture is linked from an
    // Audio/Sink and a microphone capture from an Audio/Source.
    //
    // False when the stream is linked to nothing -- the ordinary state for the
    // moment after it appears, and for as long as it is idle.
    bool streamSourceNode(uint32_t streamNodeId, PwNode &out) const;

    // Public only so the C registry callbacks can name it; not part of the API.
    struct Impl;

private:
    // Destroy cached manual links by key, then wait for them to leave the
    // registry. Shared by forgetLinksTo() and forgetLinksForNode().
    void dropManualLinks(const std::vector<std::string> &drop);

    // pw_proxy_destroy() only asks the server to drop a link; the global stays
    // in our registry mirror until the remove event arrives. linkPorts() reads
    // that mirror to decide a port pair is already linked, so re-linking a pair
    // we just tore down has to wait for the removal to land.
    void waitForLinksDropped(
        const std::vector<std::pair<uint32_t, uint32_t>> &portPairs);

    std::unique_ptr<Impl> d_;
    bool running_ = false;
};

}  // namespace waveline

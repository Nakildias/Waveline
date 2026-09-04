// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The Wave Link-style topology, built on PwEngine.
//
//   App  ->  waveline-ch-<id>  (a sink apps play into)
//              |-> in-mix <- mic-send <- mic-fx (global mic path)
//              |-> in-nc -> in-fx -> out-nc -> out-fx -> paths -> mixes
//
//   hardware mic -> [mic-nc] -> [mic-fx] -> mic-stream/monitor -> mixes
//
//   waveline-monitor-mix -> monitor-out (volume) -> whichever real output you pick

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "captureselector.h"
#include "dspprobe.h"
#include "fluidsynthfilter.h"
#include "channelinputmixer.h"
#include "creativefxfilter.h"
#include "duckingfilter.h"
#include "dynamicsfilter.h"
#include "effectsfilter.h"
#include "gainfilter.h"
#include "lufslimiterfilter.h"
#include "masterbus.h"
#include "noisefilter.h"
#include "pwengine.h"

namespace waveline {

enum class Mix { Stream, Monitor };
enum class FxStage { Input, Output };

struct Channel {
    std::string id;           // "system", "voice", ...
    std::string name;         // "System"
    float streamVolume = 1.0f;
    float monitorVolume = 1.0f;
    bool streamMuted = false;
    bool monitorMuted = false;
    float micSend = 1.0f;     // gain of this channel's published microphone
    bool micMuted = false;
    // Where the Monitor mix takes this channel from. False = the sink's own
    // monitor (dry, always audible). True = the end of the FX chain, so the
    // headphones hear the effects -- which is the routing that currently
    // delivers silence, and the reason this is a per-channel switch rather
    // than a global one: it is how we find where the chain gives out.
    bool monitorFx = false;
    // Route the Monitor mix through the chain tail for ear-protection limiting
    // even when monitorFx is off (green). The filter bypasses in software when
    // disabled, so toggling never needs a full rewire.
    bool lufsLimiter = false;
    // Publish "<brand> <name> Microphone" as a recording device, fed from
    // the raw mic through this channel's input NC/EQ. Off by default.
    bool micSource = false;
    // Software monitor of that published microphone in the Monitor mix. Only
    // meaningful while micSource is on; there is no hardware path for it.
    bool micMonitor = false;
    // Which master buses supply capture for this channel's mic chain. Several
    // are allowed; they sum at the head of the chain, so the channel publishes
    // one recording device carrying all of them. Never empty.
    std::vector<std::string> masterMicIds{kPrimaryMasterId};
    // Take each of those devices from the end of its own chain -- its own noise
    // suppression, EQ and dynamics -- and sum that into the published mic,
    // instead of running this channel's mic chain. The only way two devices can
    // reach one recording device carrying *different* effects.
    bool micUseDeviceFx = false;
    std::vector<DuckingSourceRef> duckingSources;
};

// What one signal chain's DSP stages are costing, in the order the audio goes
// through them. The graph is the only thing that knows which filter nodes make
// up a given input device, which is the whole reason this lives here rather
// than in the probe: the registry is keyed by node name, and mapping twenty
// names like waveline-voice-creative back onto "the Voice microphone" by hand
// is exactly the work that made per-client CPU figures useless for this.
struct DspChainLoad {
    std::string id;
    std::string name;
    // Only the stages actually attached, so a chain built without noise
    // suppression reports six stages rather than seven with a hole in it.
    std::vector<DspStageStats> stages;
};

struct MonitorOutputEntry {
    std::string sink;
    std::string description;  // last known friendly name (for offline UI)
    float volume = 1.0f;
    bool muted = false;
    // Which loopback this output owns: waveline-monitor-out-<slot>. Held for
    // as long as the entry lives and never derived from list position, so
    // adding or removing some *other* output cannot rename this one's nodes --
    // renaming them means destroying and recreating them, and a recreated
    // loopback plays at unity until its level is pushed back onto it.
    int slot = -1;
};

struct MasterBusChain {
    std::unique_ptr<CaptureSelector> selector;
    std::unique_ptr<FluidSynthFilter> synth;
    std::unique_ptr<GainFilter> gain;
    std::unique_ptr<NoiseFilter> nc;
    std::unique_ptr<EffectsFilter> fx;
    std::unique_ptr<CreativeFxFilter> creative;
    std::unique_ptr<DynamicsFilter> dyn;
    bool selectorReady = false;
    bool synthReady = false;
    bool gainReady = false;
    bool ncReady = false;
    bool fxReady = false;
    bool creativeReady = false;
    bool dynReady = false;
    bool sourceReady = false;
    std::string dryTailNode;
    std::string dryTailPort;
    std::string fxTailNode;
    std::string fxTailPort;
};

struct MasterBusRuntime {
    std::string id;
    std::string name;
    std::string busType = "capture";
    std::string captureMatch;
    std::string midiPortMatch;
    std::string soundfontPath;
    MasterBusChain chain;
    bool micMonitorFx = false;
    bool softwareMonitor = false;
    bool micStereo = true;
    bool wantSoftwareGain = false;
    std::string captureNode;
    std::string midiNode;
    std::string midiLinkedMatch;
    uint32_t midiSourcePortId = 0;
    // Capture links are append-only. Removing a live hardware link can stall
    // PipeWire's graph clock and silence every output loopback.
    std::vector<std::string> selectorInputs;
    // Survives path destroy/recreate (rebuild). Pushed onto loopbacks in
    // wireMasterPaths — PipeWire modules always come up at unity.
    float streamVolume = 1.0f;
    float monitorVolume = 1.0f;
    bool streamMuted = false;
    bool monitorMuted = false;
};

class MixerGraph {
public:
    explicit MixerGraph(PwEngine &engine) : eng_(engine) {}

    // What every node this graph publishes is called in other people's device
    // lists: "Wave:3 Stream Mix" with a Wave:3 installed, "Waveline Stream Mix"
    // otherwise. Comes from the device profile; see device/deviceprofile.h.
    //
    // Must be set before build(). A PipeWire node's description is fixed when
    // the node is created, so changing this afterwards renames nothing already
    // running -- and half-renamed is worse than either name.
    void setBrand(std::string brand) { if (!brand.empty()) brand_ = std::move(brand); }
    const std::string &brand() const { return brand_; }

    // Put a gain stage of our own at the head of the microphone chain.
    //
    // For microphones with no vendor protocol, which is otherwise every
    // microphone with no gain control at all: a hardware capture device's
    // volume lives on its PipeWire *device route*, owned by the session
    // manager, and writing the node's Props does nothing -- so there is no
    // preamp to reach and one has to be provided.
    //
    // Off for hardware that has a real preamp. Stacking a software gain on top
    // of an analogue one gives two controls on a single signal with no way to
    // tell which of them is turned down.
    void setSoftwareMicGain(bool on);
    // Linear, and applied at the head of the chain, so it reaches both mixes,
    // the published microphone and every channel's own microphone alike.
    void setMicInputGain(float linear);
    void setMasterInputGain(const std::string &id, float linear);

    // ALSA node name prefix of the microphone to take input from, from the
    // device profile. Empty -- the case for any microphone we have no profile
    // for -- means "PipeWire's default source". Also set before build().
    void setMicNodeMatch(std::string match);
    // What was actually picked, and empty when nothing suitable was found --
    // which is a normal state, not an error. Worth reporting: on the generic
    // profile it is the one thing about the graph that was not predictable.
    const std::string &micNode() const;
    bool hasMic() const;

    bool build(std::string &error, bool withNoiseSuppression = true);

    NoiseFilter *noiseFilter();

    // The noise suppression model, for the master filter and every per-channel
    // one alike. One setting for the whole mixer rather than per channel: the
    // engines differ enormously in CPU cost, and mixing them per channel would
    // make "why is the daemon eating a core" impossible to reason about.
    //
    // Applies to filters already running, without disturbing the graph. Any
    // filter that will not switch is left on the engine it had; `error` carries
    // the first failure and the return value is false, but the mixer keeps
    // working either way.
    bool setNoiseEngine(NoiseEngine engine, std::string &error);
    // What is actually running.
    NoiseEngine noiseEngine() const { return engine_; }
    // What was last asked for, which is not the same thing: a request that
    // could not be honoured leaves this set and the one above on the fallback.
    // Config saves *this*, so an engine that is temporarily missing -- a model
    // on a drive that has not mounted yet, a library removed by an upgrade --
    // does not quietly erase the user's choice the next time settings are
    // written.
    NoiseEngine requestedNoiseEngine() const { return requestedEngine_; }

    // Re-picks the microphone after the user changed their default input.
    // True when it changed and the graph needs rewiring. See the definition.
    bool reconsiderMicNode();

    bool wireMicPaths(std::string &error);
    // Post-rewire sanity check: every mix loopback input must have a manual link.
    bool verifyMixWiring(std::string &error) const;

    // Paths whose nodes exist and are linked but that PipeWire is not
    // scheduling, so they carry no audio. verifyMixWiring() cannot see this: it
    // asks whether links exist, and they do. Covers the Monitor outputs as
    // well, which nothing else verifies -- a dead Monitor output is exactly the
    // case where the mixer looks healthy and sounds silent. Returns node names.
    std::vector<std::string> stalledPaths() const;
    // Same check for one input device — used after RebuildMasterCapture so a
    // partial heal does not tear down the whole graph with scheduleRewire().
    bool verifyMasterMixWiring(const std::string &id, std::string &error) const;
    bool wireChannelFx(const std::string &channelId, std::string &error);
    bool wireAllChannelFx(std::string &error);
    bool wireNoiseFilter(std::string &error) { return wireMicPaths(error); }

    EffectsFilter *micEffects();
    DynamicsFilter *micDynamics();
    EffectsFilter *channelEffects(const std::string &channelId, FxStage stage);
    DynamicsFilter *channelDynamics(const std::string &channelId, FxStage stage);
    NoiseFilter *channelNoiseFilter(const std::string &channelId, FxStage stage);

    bool setMicEffects(const ChannelFxSettings &settings);
    bool setMicDynamics(const DynamicsSettings &settings);
    bool setMicMonitorFx(bool on);
    bool micMonitorFx() const;
    // Swap only the microphone's Monitor mix input between dry and processed.
    bool rewireMicMonitor(std::string &error);
    bool setChannelEffects(const std::string &channelId, FxStage stage,
                           const ChannelFxSettings &settings);
    bool setChannelDynamics(const std::string &channelId, FxStage stage,
                            const DynamicsSettings &settings);
    bool setChannelDucking(const std::string &channelId, const DuckingSettings &settings);
    bool setChannelLufsLimiter(const std::string &channelId,
                               const LufsLimiterSettings &settings);
    bool setChannelCreativeFx(const std::string &channelId, FxStage stage,
                              const CreativeFxSettings &settings);
    bool setMasterCreativeFx(const std::string &id, const CreativeFxSettings &settings);
    void setChannelNoiseSuppression(const std::string &channelId, FxStage stage,
                                    bool on, float intensity = 1.0f);
    // Returns true only when a new NC filter node was created (needs rewire).
    bool ensureChannelNoiseFilter(const std::string &channelId, FxStage stage);
    // Gain of this channel's own published microphone, 0..1+. Named "micSend"
    // for continuity with the config key; it is a level, not a send.
    bool setChannelMicSend(const std::string &channelId, float level);
    float channelMicSend(const std::string &channelId) const;
    bool setChannelMicMuted(const std::string &channelId, bool muted);
    bool channelMicMuted(const std::string &channelId) const;

    // Needs a rewire to take effect: it changes which node feeds the path.
    bool setChannelMonitorFx(const std::string &channelId, bool on);
    bool channelMonitorFx(const std::string &channelId) const;

    // Swap only this channel's Monitor mix input between dry and the FX chain
    // tail. Does not disturb Stream routing or any other channel.
    bool rewireChannelMonitor(const std::string &channelId, std::string &error);

    // Builds or drops the channel's microphone chain and published source.
    // Does not wire it -- call rewireChannelMicSource() after enabling.
    bool ensureChannelMicSource(const std::string &channelId);
    // Link or unlink only this channel's microphone publish path. Does not
    // touch app-audio Stream/Monitor routing.
    bool rewireChannelMicSource(const std::string &channelId, std::string &error);
    // Hear this channel's published microphone in the Monitor mix. Mutes the
    // dedicated path rather than rewiring; needs micSource and a wire pass to
    // connect the tap in the first place.
    bool setChannelMicMonitor(const std::string &channelId, bool on);
    bool channelMicMonitor(const std::string &channelId) const;
    // Ids that do not name a bus are dropped; an empty result falls back to the
    // primary input device, so a channel always has a capture source.
    bool setChannelMasterMics(const std::string &channelId,
                              const std::vector<std::string> &masterIds);
    std::vector<std::string> channelMasterMics(const std::string &channelId) const;
    bool setChannelMasterMic(const std::string &channelId, const std::string &masterId);
    const std::string &channelMasterMic(const std::string &channelId) const;
    bool setChannelMicUseDeviceFx(const std::string &channelId, bool on);
    bool channelMicUseDeviceFx(const std::string &channelId) const;

    bool setChannelMicSource(const std::string &channelId, bool on);
    bool channelMicSource(const std::string &channelId) const;

    const std::vector<Channel> &channels() const { return channels_; }
    Channel *channel(const std::string &id);

    bool setVolume(const std::string &channelId, Mix mix, float volume);
    bool setMuted(const std::string &channelId, Mix mix, bool muted);

    bool setMicVolume(Mix mix, float volume);
    bool setMicMuted(Mix mix, bool muted);

    bool setSoundShareVolume(Mix mix, float volume);
    bool setSoundShareMuted(Mix mix, bool muted);
    static constexpr const char *kSoundShareSink = "waveline-sound-share";

    bool setSoftwareMonitor(bool on);
    bool softwareMonitor() const { return softwareMonitor_; }

    bool setMicStereo(bool on, std::string &error);
    bool micStereo() const;

    bool setMonitorOutput(const std::string &sinkName, std::string &error);
    const std::string &monitorOutput() const;
    const std::vector<MonitorOutputEntry> &monitorOutputs() const {
        return monitorOutputs_;
    }
    bool setMonitorOutputs(const std::vector<MonitorOutputEntry> &outputs,
                           std::string &error);
    bool setMonitorOutputAt(size_t index, const std::string &sinkName, std::string &error);
    bool addMonitorOutput(const std::string &sinkName, std::string &error);
    bool removeMonitorOutput(size_t index, std::string &error);
    // True when an output other than exceptIndex already plays to this sink.
    // Pass monitorOutputs().size() as exceptIndex to test a sink for a new one.
    bool monitorSinkTaken(const std::string &sinkName, size_t exceptIndex) const;
    bool setMonitorOutputVolume(size_t index, float volume);
    bool setMonitorOutputMuted(size_t index, bool muted);
    // True when the assigned sink is currently in the PipeWire registry.
    bool monitorOutputOnline(size_t index) const;
    // Drop/recreate paths for assigned sinks (offline slots stay assigned).
    bool refreshMonitorOutputBindings(std::string &error);
    static constexpr size_t kMaxMonitorOutputs = 5;

    bool setMonitorMasterVolume(float volume);
    bool setMonitorMasterMuted(bool muted);
    float monitorMasterVolume() const { return monitorMaster_; }
    // Re-push per-output Monitor levels (after rewire / restart settles).
    void applyAllMonitorOutputGains();
    bool setStreamMixVolume(float volume);
    float streamMixVolume() const { return streamMixVolume_; }
    bool setStreamMixMuted(bool muted);
    bool streamMixMuted() const { return streamMixMuted_; }

    bool ensureChannelPaths(std::string &error);
    bool ensureChannelFilters(std::string &error);
    // Push a channel's stored fader levels back onto its mix loopbacks. The
    // master-bus equivalent of this (applyMasterPathLevels) is why master
    // levels survive a rewire and channel levels used not to.
    void applyChannelPathLevels(const std::string &channelId);
    void applyAllChannelPathLevels();

    std::vector<std::string> channelSinkNames() const;

    // Multi-master bus API.
    bool addMasterBus(const std::string &id, const std::string &name,
                      const std::string &busType, std::string &error);
    bool removeMasterBus(const std::string &id, std::string &error);
    bool setMasterCaptureMatch(const std::string &id, const std::string &match);
    bool setMasterMidiPortMatch(const std::string &id, const std::string &match);
    bool setMasterSoundfontPath(const std::string &id, const std::string &path);
    bool setMasterName(const std::string &id, const std::string &name);
    bool wireMasterPaths(const std::string &id, std::string &error);
    // Push stored mix levels onto loopbacks after create/repair/rebuild.
    void applyMasterPathLevels(const std::string &id);
    bool rewireMasterCapture(const std::string &id, std::string &error);
    // After the graph is quiet: reconnect only the ALSA→first-filter hop so a
    // poisoned adaptive resampler from startup/hotplug cannot stick. Does not
    // tear down DSP or mix links (peers stay audible).
    bool relinkMasterHwCapture(const std::string &id, std::string &error);
    // Start a newly appeared capture device against the existing selector
    // while deliberately outputting silence. The settled rebuild follows once
    // its hardware clock has had time to negotiate.
    bool primeMasterHwCapture(const std::string &id, std::string &error);
    void silenceMasterCapture(const std::string &id);
    // Nuclear: destroy/recreate the whole master chain. Prefer relink when the
    // DSP is already up.
    bool rebuildMasterHwCapture(const std::string &id, std::string &error);
    bool rebuildAllMasterHwCaptures(std::string &error);
    bool rewireMasterMonitor(const std::string &id, std::string &error);
    // Per-input-device and per-channel DSP cost, in signal order. Both read
    // whatever the probe has collected; both are empty lists of stages while
    // profiling is off, because nothing has been counting. See dspprobe.h.
    std::vector<DspChainLoad> masterDspChains() const;
    std::vector<DspChainLoad> channelDspChains() const;

    const std::vector<MasterBusRuntime> &masterBuses() const { return masterBuses_; }
    MasterBusRuntime *masterBus(const std::string &id);
    const MasterBusRuntime *masterBus(const std::string &id) const;
    bool setMasterMicMonitorFx(const std::string &id, bool on);
    bool setMasterSoftwareMonitor(const std::string &id, bool on);
    bool masterSoftwareMonitor(const std::string &id) const;
    bool rewireMasterSoftwareMonitor(const std::string &id, std::string &error);
    bool setMasterMicStereo(const std::string &id, bool on, std::string &error);
    EffectsFilter *masterEffects(const std::string &id);
    DynamicsFilter *masterDynamics(const std::string &id);
    CreativeFxFilter *masterCreativeFx(const std::string &id);
    NoiseFilter *masterNoiseFilter(const std::string &id);
    bool setMasterEffects(const std::string &id, const ChannelFxSettings &settings);
    bool setMasterDynamics(const std::string &id, const DynamicsSettings &settings);
    bool setMasterVolume(const std::string &id, Mix mix, float volume);
    bool setMasterMuted(const std::string &id, Mix mix, bool muted);
    bool syncMasterStreamPath(const std::string &id, std::string &error);
    bool ensureMidiStreamPath(const std::string &id, std::string &error);
    bool rebuildMasterMidiChain(const std::string &id, std::string &error);
    std::string findCaptureNode(const std::string &captureMatch) const;
    std::string findMidiNode(const std::string &midiMatch) const;
    std::string masterMidiOutputPort(const std::string &node,
                                     const std::string &portHint = {}) const;
    bool isMasterMidi(const MasterBusRuntime &bus) const;
    bool waitForMidiNode(MasterBusRuntime &bus, int timeoutMs, std::string &error);
    bool linkMasterMidiInput(MasterBusRuntime &bus, std::string &error);
    bool linkPendingMidiInputs(std::string &error);
    bool linkPendingMidiAudioPaths(std::string &error);
    bool rewireMasterMidiInput(const std::string &id, std::string &error);
    void invalidateMasterMidiInput(const std::string &id);

    static constexpr const char *kStreamMix = "waveline-stream-mix";
    static constexpr const char *kMonitorMix = "waveline-monitor-mix";
    static constexpr const char *kMicFxNode = "waveline-mic-fx";
    static constexpr const char *kMicDynNode = "waveline-mic-dynamics";
    static constexpr const char *kMicGainNode = "waveline-mic-gain";
    // The input device as applications see it, after NC and EQ.
    static constexpr const char *kMicSourceNode = "waveline-mic";

private:
    struct ChannelChain {
        std::unique_ptr<ChannelInputMixer> mixer;
        std::unique_ptr<GainFilter> micSend;
        std::unique_ptr<NoiseFilter> inputNc;
        std::unique_ptr<EffectsFilter> inputFx;
        std::unique_ptr<CreativeFxFilter> inputCreative;
        std::unique_ptr<DynamicsFilter> inputDyn;
        std::unique_ptr<NoiseFilter> outputNc;
        std::unique_ptr<EffectsFilter> outputFx;
        std::unique_ptr<CreativeFxFilter> outputCreative;
        std::unique_ptr<DuckingFilter> outputDuck;
        std::unique_ptr<DynamicsFilter> outputDyn;
        std::unique_ptr<LufsLimiterFilter> outputLufs;
        bool mixerReady = false;
        bool micSendReady = false;
        bool inputNcReady = false;
        bool inputFxReady = false;
        bool inputCreativeReady = false;
        bool inputDynReady = false;
        bool outputNcReady = false;
        bool outputFxReady = false;
        bool outputCreativeReady = false;
        bool outputDuckReady = false;
        bool outputDynReady = false;
        bool outputLufsReady = false;
        std::string fxTailNode;
        std::string fxTailPortL;
        std::string fxTailPortR;
        bool fxTailMono = false;
        bool fxChainWired = false;
    };

    // Display name for a published node. Node *names* stay "waveline-..."
    // whatever microphone is installed -- they are the graph's internal
    // identifiers and scripts key off them -- while the descriptions people
    // actually see carry the device's brand.
    std::string disp(const std::string &suffix) const {
        return suffix.empty() ? brand_ : brand_ + " " + suffix;
    }

    MasterBusRuntime *primaryMaster();
    const MasterBusRuntime *primaryMaster() const;

    bool isUsableMicNode(const PwNode &n) const;
    std::string masterCapturePort(const MasterBusRuntime &bus) const;
    bool resolveMasterChain(const MasterBusRuntime &bus, std::string &dryNode,
                            std::string &dryPort, std::string &fxNode,
                            std::string &fxPort) const;
    bool syncMasterCaptureNode(MasterBusRuntime &bus);
    bool waitForCaptureNode(MasterBusRuntime &bus, int timeoutMs, std::string &error);
    std::string resolveMasterCaptureNode(const MasterBusRuntime &bus) const;
    bool reconsiderMasterNode(MasterBusRuntime &bus);
    // First software filter that consumes the hardware capture for this bus.
    bool masterHwFirstHop(const MasterBusRuntime &bus, std::string &inNode,
                          std::string &inPort) const;
    void disconnectMasterMonitorLinks(const std::string &id);
    void disconnectChannelMicMonitorLinks(const std::string &channelId);

    static std::string sinkName(const std::string &id) {
        return "waveline-ch-" + id;
    }
    static std::string pathName(const std::string &id, Mix mix) {
        return "waveline-" + id + (mix == Mix::Stream ? "-stream" : "-monitor");
    }
    static std::string mixNodeName(const std::string &id) {
        return "waveline-ch-" + id + "-in-mix";
    }
    static std::string micSendNodeName(const std::string &id) {
        return "waveline-ch-" + id + "-mic-send";
    }
    static std::string ncNodeName(const std::string &id, FxStage stage) {
        return std::string("waveline-ch-") + id + (stage == FxStage::Input ? "-in-nc" : "-out-nc");
    }
    static std::string micSourceNode(const std::string &id) {
        return "waveline-" + id + "-mic";
    }
    static std::string channelMicMonitorPath(const std::string &id) {
        return "waveline-" + id + "-mic-monitor";
    }
    static std::string fxNodeName(const std::string &id, FxStage stage) {
        return std::string("waveline-ch-") + id + (stage == FxStage::Input ? "-in-fx" : "-out-fx");
    }
    static std::string dynNodeName(const std::string &id, FxStage stage) {
        return std::string("waveline-ch-") + id + (stage == FxStage::Input ? "-in-dyn" : "-out-dyn");
    }
    static std::string creativeNodeName(const std::string &id, FxStage stage) {
        return std::string("waveline-ch-") + id +
               (stage == FxStage::Input ? "-in-creative" : "-out-creative");
    }
    static std::string duckNodeName(const std::string &id) {
        return "waveline-ch-" + id + "-out-duck";
    }
    static std::string lufsNodeName(const std::string &id) {
        return "waveline-ch-" + id + "-out-lufs";
    }

    void wireDuckingSidechain(const std::string &channelId, const std::string &duckNode);
    // Link one sidechain port to whatever node the source ref names.
    void linkDuckingSidechainSource(const std::string &duckNode, size_t index,
                                    const DuckingSourceRef &src);
    // Swap sidechain links in place when the user edits the source list. Only
    // the ports whose assignment changed are touched: the program path through
    // the duck node is never unlinked, so audio keeps flowing. A full rewire
    // here would clear every link in the graph and cut all audio for seconds.
    void relinkDuckingSidechains(const std::string &channelId,
                                 const std::vector<DuckingSourceRef> &oldSources);
    void rewireDuckingSidechainsForMic(const std::string &micChannelId);

    bool createMasterChainNodes(MasterBusRuntime &bus, bool withNoiseSuppression,
                                std::string &error);
    bool createMasterVirtualSource(MasterBusRuntime &bus, std::string &error);
    void destroyMasterChain(MasterBusRuntime &bus);
    bool buildPrimaryMasterPaths(std::string &error);
    bool selectMasterCaptureInput(MasterBusRuntime &bus, std::string &error,
                                  bool activate = true);

    PwEngine &eng_;
    std::string brand_ = "Waveline";
    std::string micNodeMatch_;
    bool pendingWantMicGain_ = false;
    NoiseEngine engine_ = NoiseEngine::RnNoise;
    NoiseEngine requestedEngine_ = NoiseEngine::RnNoise;
    std::vector<MasterBusRuntime> masterBuses_;
    std::unordered_map<std::string, ChannelChain> chChains_;
    std::vector<Channel> channels_;
    bool buildFxChannelPaths(const std::string &id, const std::string &label,
                               std::string &error);
    bool startChannelChain(const std::string &id, const std::string &label,
                           std::string &error);
    bool wireChannelPaths(const std::string &channelId, const std::string &node,
                          const std::string &portL, const std::string &portR,
                          bool mono, std::string &error);
    bool wireChannelMixPath(const std::string &channelId, Mix mix,
                            const std::string &node, const std::string &portL,
                            const std::string &portR, bool mono,
                            std::string &error);
    bool wireChannelPassthrough(const std::string &channelId, std::string &error);
    // Mic -> per-channel gain -> the channel's Stream capture node. Stream
    // only: the Monitor mix must never carry the microphone twice.
    bool applyChannelMicGain(const std::string &channelId);
    // Raw hardware mic -> this channel's in-nc/in-fx -> a published Audio/Source
    // that applications can record from.
    void wireChannelMicSource(const std::string &channelId);
    bool ensurePathsForChannel(const std::string &channelId, std::string &error);
    bool ensurePrimaryMasterPaths(const MasterBusRuntime &bus, std::string &error);

    bool rebuildMonitorOutputs(std::string &error);
    static std::string monitorOutputHandle(int slot);
    // Handle of the loopback entry `index` owns; empty when it owns none.
    std::string monitorOutputHandleAt(size_t index) const;
    // Lowest slot no entry holds, -1 when all kMaxMonitorOutputs are taken.
    int freeMonitorSlot() const;
    // Gives every entry a slot of its own, keeping the ones already assigned.
    void assignMonitorSlots();
    // True when this output currently has a loopback loaded.
    bool monitorOutputPathLive(size_t index) const;
    // Destroys one output's loopback and releases the clock-driver role with it.
    void dropMonitorOutputPath(size_t index);
    // Creates one monitor output's loopback at its stored level. Used both by
    // the full rebuild and by unmuting an output that had none.
    bool createMonitorOutputPath(size_t index, std::string &error);
    void applyMonitorOutputGain(size_t index);
    void applyStreamMixGain();
    bool isUsableSinkNode(const PwNode &n) const;
    bool sinkPresent(const std::string &name) const;
    std::string lookupSinkDescription(const std::string &name) const;

    std::vector<MonitorOutputEntry> monitorOutputs_;
    // Slot of the monitor loopback that currently drives the graph clock, -1
    // when none does. Claimed by the first path created and released when that
    // path is destroyed; never re-derived from list position, so removing an
    // output cannot make another one restart just to change its driver flag.
    int monitorDriverSlot_ = -1;
    float monitorMaster_ = 1.0f;
    bool monitorMasterMuted_ = false;
    float streamMixVolume_ = 1.0f;
    bool streamMixMuted_ = false;
    bool softwareMonitor_ = true;
    bool built_ = false;
};

}  // namespace waveline

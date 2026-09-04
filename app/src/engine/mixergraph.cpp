// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "mixergraph.h"

#include <cstdio>
#include <cstring>

#include <algorithm>
#include <thread>

namespace waveline {
namespace {

const std::vector<std::pair<const char *, const char *>> kDefaultChannels = {
    {"system", "System"},
    {"voice", "Voice"},
    {"music", "Music"},
    {"video", "Video"},
    {"browser", "Browser"},
    {"game", "Game"},
    {"sfx", "SFX"},
};

struct MidiMatchParts {
    std::string node;
    std::string port;
};

MidiMatchParts splitMidiMatch(const std::string &match) {
    const auto sep = match.find('|');
    if (sep == std::string::npos) return {match, {}};
    return {match.substr(0, sep), match.substr(sep + 1)};
}

}  // namespace

MasterBusRuntime *MixerGraph::primaryMaster() {
    for (auto &b : masterBuses_)
        if (isPrimaryMaster(b.id)) return &b;
    return masterBuses_.empty() ? nullptr : &masterBuses_.front();
}

const MasterBusRuntime *MixerGraph::primaryMaster() const {
    for (const auto &b : masterBuses_)
        if (isPrimaryMaster(b.id)) return &b;
    return masterBuses_.empty() ? nullptr : &masterBuses_.front();
}

MasterBusRuntime *MixerGraph::masterBus(const std::string &id) {
    for (auto &b : masterBuses_)
        if (b.id == id) return &b;
    return nullptr;
}

const MasterBusRuntime *MixerGraph::masterBus(const std::string &id) const {
    for (const auto &b : masterBuses_)
        if (b.id == id) return &b;
    return nullptr;
}

std::vector<DspChainLoad> MixerGraph::masterDspChains() const {
    std::vector<DspChainLoad> out;
    out.reserve(masterBuses_.size());
    for (const MasterBusRuntime &bus : masterBuses_) {
        DspChainLoad load;
        load.id = bus.id;
        load.name = bus.name.empty() ? bus.id : bus.name;
        // Signal order, not creation order: the point of the breakdown is to
        // see where in the chain the time goes, and a list sorted by anything
        // else has to be read twice to answer that.
        std::vector<std::string> nodes;
        if (isMasterMidi(bus))
            nodes.push_back(masterSynthNode(bus.id));
        else
            nodes.push_back(masterCaptureSelectorNode(bus.id));
        nodes.push_back(masterGainNode(bus.id));
        nodes.push_back(masterNcNode(bus.id));
        nodes.push_back(masterFxNode(bus.id));
        nodes.push_back(masterCreativeNode(bus.id));
        nodes.push_back(masterDynNode(bus.id));
        load.stages = dspStages(nodes);
        out.push_back(std::move(load));
    }
    return out;
}

std::vector<DspChainLoad> MixerGraph::channelDspChains() const {
    std::vector<DspChainLoad> out;
    out.reserve(channels_.size());
    for (const Channel &ch : channels_) {
        DspChainLoad load;
        load.id = ch.id;
        load.name = ch.name.empty() ? ch.id : ch.name;
        const std::vector<std::string> nodes = {
            mixNodeName(ch.id),
            micSendNodeName(ch.id),
            ncNodeName(ch.id, FxStage::Input),
            fxNodeName(ch.id, FxStage::Input),
            creativeNodeName(ch.id, FxStage::Input),
            dynNodeName(ch.id, FxStage::Input),
            ncNodeName(ch.id, FxStage::Output),
            fxNodeName(ch.id, FxStage::Output),
            creativeNodeName(ch.id, FxStage::Output),
            duckNodeName(ch.id),
            dynNodeName(ch.id, FxStage::Output),
            lufsNodeName(ch.id),
        };
        load.stages = dspStages(nodes);
        out.push_back(std::move(load));
    }
    return out;
}

const std::string &MixerGraph::micNode() const {
    static const std::string kEmpty;
    const MasterBusRuntime *p = primaryMaster();
    return p ? p->captureNode : kEmpty;
}

bool MixerGraph::hasMic() const {
    const MasterBusRuntime *p = primaryMaster();
    return p && !p->captureNode.empty();
}

void MixerGraph::setSoftwareMicGain(bool on) {
    pendingWantMicGain_ = on;
    if (auto *p = primaryMaster()) p->wantSoftwareGain = on;
}

void MixerGraph::setMicNodeMatch(std::string match) {
    micNodeMatch_ = std::move(match);
    if (auto *p = primaryMaster()) p->captureMatch = micNodeMatch_;
}

NoiseFilter *MixerGraph::noiseFilter() {
    MasterBusRuntime *p = primaryMaster();
    return (p && p->chain.ncReady && p->chain.nc) ? p->chain.nc.get() : nullptr;
}

EffectsFilter *MixerGraph::micEffects() {
    MasterBusRuntime *p = primaryMaster();
    return (p && p->chain.fxReady && p->chain.fx) ? p->chain.fx.get() : nullptr;
}

DynamicsFilter *MixerGraph::micDynamics() {
    MasterBusRuntime *p = primaryMaster();
    return (p && p->chain.dynReady && p->chain.dyn) ? p->chain.dyn.get() : nullptr;
}

bool MixerGraph::micMonitorFx() const {
    const MasterBusRuntime *p = primaryMaster();
    return p && p->micMonitorFx;
}

bool MixerGraph::micStereo() const {
    const MasterBusRuntime *p = primaryMaster();
    return !p || p->micStereo;
}

EffectsFilter *MixerGraph::masterEffects(const std::string &id) {
    MasterBusRuntime *b = masterBus(id);
    return (b && b->chain.fxReady && b->chain.fx) ? b->chain.fx.get() : nullptr;
}

DynamicsFilter *MixerGraph::masterDynamics(const std::string &id) {
    MasterBusRuntime *b = masterBus(id);
    return (b && b->chain.dynReady && b->chain.dyn) ? b->chain.dyn.get() : nullptr;
}

CreativeFxFilter *MixerGraph::masterCreativeFx(const std::string &id) {
    MasterBusRuntime *b = masterBus(id);
    return (b && b->chain.creativeReady && b->chain.creative) ? b->chain.creative.get()
                                                              : nullptr;
}

NoiseFilter *MixerGraph::masterNoiseFilter(const std::string &id) {
    MasterBusRuntime *b = masterBus(id);
    return (b && b->chain.ncReady && b->chain.nc) ? b->chain.nc.get() : nullptr;
}

Channel *MixerGraph::channel(const std::string &id) {
    auto it = std::find_if(channels_.begin(), channels_.end(),
                           [&](const Channel &c) { return c.id == id; });
    return it == channels_.end() ? nullptr : &*it;
}

std::vector<std::string> MixerGraph::channelSinkNames() const {
    std::vector<std::string> out;
    out.reserve(channels_.size());
    for (const auto &c : channels_) out.push_back(sinkName(c.id));
    return out;
}

EffectsFilter *MixerGraph::channelEffects(const std::string &channelId,
                                          FxStage stage) {
    auto it = chChains_.find(channelId);
    if (it == chChains_.end()) return nullptr;
    if (stage == FxStage::Input)
        return it->second.inputFxReady ? it->second.inputFx.get() : nullptr;
    return it->second.outputFxReady ? it->second.outputFx.get() : nullptr;
}

DynamicsFilter *MixerGraph::channelDynamics(const std::string &channelId,
                                            FxStage stage) {
    auto it = chChains_.find(channelId);
    if (it == chChains_.end()) return nullptr;
    if (stage == FxStage::Input)
        return it->second.inputDynReady ? it->second.inputDyn.get() : nullptr;
    return it->second.outputDynReady ? it->second.outputDyn.get() : nullptr;
}

NoiseFilter *MixerGraph::channelNoiseFilter(const std::string &channelId,
                                            FxStage stage) {
    auto it = chChains_.find(channelId);
    if (it == chChains_.end()) return nullptr;
    if (stage == FxStage::Input)
        return it->second.inputNcReady ? it->second.inputNc.get() : nullptr;
    return it->second.outputNcReady ? it->second.outputNc.get() : nullptr;
}

bool MixerGraph::setNoiseEngine(NoiseEngine engine, std::string &error) {
    requestedEngine_ = engine;

    std::vector<NoiseFilter *> filters;
    for (auto &bus : masterBuses_) {
        if (bus.chain.ncReady && bus.chain.nc) filters.push_back(bus.chain.nc.get());
    }
    for (auto &[id, chain] : chChains_) {
        (void)id;
        if (chain.inputNcReady && chain.inputNc) filters.push_back(chain.inputNc.get());
        if (chain.outputNcReady && chain.outputNc) filters.push_back(chain.outputNc.get());
    }

    for (size_t i = 0; i < filters.size(); ++i) {
        std::string err;
        if (filters[i]->setEngine(engine, err)) continue;

        error = err;
        std::string ignored;
        for (size_t j = 0; j < i; ++j) filters[j]->setEngine(engine_, ignored);
        return false;
    }

    engine_ = engine;
    return true;
}

bool MixerGraph::createMasterChainNodes(MasterBusRuntime &bus,
                                        bool withNoiseSuppression,
                                        std::string &error) {
    MasterBusChain &chain = bus.chain;
    const std::string &id = bus.id;
    const bool primary = isPrimaryMaster(id);
    const bool midi = isMasterMidi(bus);

    if (midi) {
        chain.synth = std::make_unique<FluidSynthFilter>();
        std::string synthErr;
        if (chain.synth->start(masterSynthNode(id),
                               disp(primary ? "MIDI Synth" : bus.name + " Synth"),
                               synthErr)) {
            chain.synthReady = true;
            if (!bus.soundfontPath.empty())
                chain.synth->setSoundfontPath(bus.soundfontPath);
        } else {
            error = "MIDI synth for " + id + ": " + synthErr;
            chain.synth.reset();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    } else {
        chain.selector = std::make_unique<CaptureSelector>();
        std::string selectorErr;
        if (chain.selector->start(
                masterCaptureSelectorNode(id),
                disp(primary ? "Microphone Capture Selector"
                             : bus.name + " Capture Selector"),
                selectorErr)) {
            chain.selectorReady = true;
        } else {
            error = "capture selector for " + id + ": " + selectorErr;
            chain.selector.reset();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    if (bus.wantSoftwareGain) {
        chain.gain = std::make_unique<GainFilter>();
        std::string gainErr;
        if (chain.gain->start(masterGainNode(id),
                              disp(primary ? "Microphone Gain" : bus.name + " Gain"),
                              1, gainErr))
            chain.gainReady = true;
        else
            chain.gain.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    if (!midi && withNoiseSuppression) {
        chain.nc = std::make_unique<NoiseFilter>();
        std::string ncError;
        if (chain.nc->start(masterNcNode(id),
                            disp(primary ? "Microphone (Noise Suppressed)"
                                         : bus.name + " (Noise Suppressed)"),
                            ncError, false, engine_))
            chain.ncReady = true;
        else
            chain.nc.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    chain.fx = std::make_unique<EffectsFilter>();
    std::string fxErr;
    if (chain.fx->start(masterFxNode(id),
                        disp(primary ? "Microphone EQ" : bus.name + " EQ"),
                        1, fxErr))
        chain.fxReady = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    chain.creative = std::make_unique<CreativeFxFilter>();
    std::string creativeErr;
    if (chain.creative->start(masterCreativeNode(id),
                              disp(primary ? "Microphone Creative FX"
                                           : bus.name + " Creative FX"),
                              1, creativeErr))
        chain.creativeReady = true;
    else
        chain.creative.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    chain.dyn = std::make_unique<DynamicsFilter>();
    std::string dynErr;
    if (chain.dyn->start(masterDynNode(id),
                         disp(primary ? "Microphone Dynamics" : bus.name + " Dynamics"),
                         1, dynErr))
        chain.dynReady = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    return chain.fxReady || chain.dynReady || chain.ncReady || chain.gainReady ||
           chain.synthReady;
}

bool MixerGraph::createMasterVirtualSource(MasterBusRuntime &bus, std::string &error) {
    MasterBusChain &chain = bus.chain;
    if (!(chain.fxReady || chain.dynReady || chain.ncReady || chain.synthReady))
        return true;
    const bool primary = isPrimaryMaster(bus.id);
    std::string srcErr;
    if (eng_.addVirtualSource(masterSourceNode(bus.id),
                              disp(primary ? "Microphone (Processed)"
                                           : bus.name + " (Processed)"),
                              1, srcErr)) {
        chain.sourceReady = true;
        return true;
    }
    error = srcErr;
    return false;
}

void MixerGraph::destroyMasterChain(MasterBusRuntime &bus) {
    const std::string &id = bus.id;
    eng_.forgetLinksForNode(masterCaptureSelectorNode(id));
    eng_.forgetLinksForNode(masterSynthNode(id));
    eng_.forgetLinksForNode(masterGainNode(id));
    eng_.forgetLinksForNode(masterNcNode(id));
    eng_.forgetLinksForNode(masterFxNode(id));
    eng_.forgetLinksForNode(masterCreativeNode(id));
    eng_.forgetLinksForNode(masterDynNode(id));
    eng_.forgetLinksForNode(masterSourceNode(id));
    if (!isPrimaryMaster(id)) {
        eng_.removePath(pathName(id, Mix::Monitor));
        eng_.removePath(pathName(id, Mix::Stream));
    }
    eng_.removeNode(masterCaptureSelectorNode(id));
    eng_.removeNode(masterSynthNode(id));
    eng_.removeNode(masterGainNode(id));
    eng_.removeNode(masterNcNode(id));
    eng_.removeNode(masterFxNode(id));
    eng_.removeNode(masterCreativeNode(id));
    eng_.removeNode(masterDynNode(id));
    eng_.removeNode(masterSourceNode(id));
    bus.chain = MasterBusChain{};
    bus.selectorInputs.clear();
}

bool MixerGraph::buildPrimaryMasterPaths(std::string &error) {
    const MasterBusRuntime *primary = primaryMaster();
    if (!primary) return false;
    for (Mix mix : {Mix::Monitor, Mix::Stream}) {
        PwEngine::PathSpec spec;
        spec.handle = pathName("mic", mix);
        spec.target = (mix == Mix::Stream) ? kStreamMix : kMonitorMix;
        spec.description = (mix == Mix::Stream) ? "Microphone → Stream"
                                                : "Microphone → Monitor";
        spec.inChannels = primary->micStereo ? 2 : 1;
        spec.outChannels = primary->micStereo ? 2 : 1;
        spec.remix = false;
        spec.source.clear();
        spec.sourceIsSink = false;
        if (mix == Mix::Stream) {
            spec.volume = primary->streamVolume;
            spec.muted = primary->streamMuted;
        } else {
            spec.volume = primary->monitorVolume;
            spec.muted = !softwareMonitor_ || primary->monitorMuted;
        }
        if (!eng_.addPath(spec, error)) return false;
    }
    applyMasterPathLevels(primary->id);
    return true;
}

bool MixerGraph::startChannelChain(const std::string &id, const std::string &label,
                                   std::string &error) {
    ChannelChain chain;

    std::string mixErr;
    chain.mixer = std::make_unique<ChannelInputMixer>();
    if (chain.mixer->start(mixNodeName(id), disp(label + " Input Mix"), mixErr))
        chain.mixerReady = true;
    else
        chain.mixer.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::string sendErr;
    chain.micSend = std::make_unique<GainFilter>();
    if (chain.micSend->start(micSendNodeName(id),
                             disp(label + " Mic Send"), 1, sendErr)) {
        chain.micSendReady = true;
        chain.micSend->setGain(1.0f);
    } else {
        chain.micSend.reset();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::string outNcErr;
    chain.outputNc = std::make_unique<NoiseFilter>();
    if (chain.outputNc->start(ncNodeName(id, FxStage::Output),
                              disp(label + " Output NC"), outNcErr,
                              false, engine_)) {
        chain.outputNcReady = true;
        chain.outputNc->setEnabled(false);
    } else {
        chain.outputNc.reset();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::string outFxErr;
    chain.outputFx = std::make_unique<EffectsFilter>();
    if (chain.outputFx->start(fxNodeName(id, FxStage::Output),
                              disp(label + " Output EQ"), 2, outFxErr))
        chain.outputFxReady = true;
    else
        chain.outputFx.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::string outCreativeErr;
    chain.outputCreative = std::make_unique<CreativeFxFilter>();
    if (chain.outputCreative->start(creativeNodeName(id, FxStage::Output),
                                    disp(label + " Output Creative FX"), 2,
                                    outCreativeErr))
        chain.outputCreativeReady = true;
    else
        chain.outputCreative.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::string outDuckErr;
    chain.outputDuck = std::make_unique<DuckingFilter>();
    if (chain.outputDuck->start(duckNodeName(id), disp(label + " Output Ducking"),
                                outDuckErr))
        chain.outputDuckReady = true;
    else
        chain.outputDuck.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::string outDynErr;
    chain.outputDyn = std::make_unique<DynamicsFilter>();
    if (chain.outputDyn->start(dynNodeName(id, FxStage::Output),
                               disp(label + " Output Dynamics"), 2, outDynErr))
        chain.outputDynReady = true;
    else
        chain.outputDyn.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    std::string outLufsErr;
    chain.outputLufs = std::make_unique<LufsLimiterFilter>();
    if (chain.outputLufs->start(lufsNodeName(id), disp(label + " Output LUFS Limiter"),
                                outLufsErr))
        chain.outputLufsReady = true;
    else
        chain.outputLufs.reset();

    const bool ok = chain.mixerReady || chain.outputFxReady || chain.outputDynReady;
    if (!ok) {
        error = "no effects chain for channel " + id;
        return false;
    }
    chChains_.emplace(id, std::move(chain));
    return true;
}

bool MixerGraph::build(std::string &error, bool withNoiseSuppression) {
    if (built_) return true;

    if (!eng_.addNullSink(kStreamMix, disp("Stream Mix"), 2, error)) return false;
    if (!eng_.addNullSink(kMonitorMix, disp("Monitor Mix"), 2, error)) return false;

    for (const auto &[id, name] : kDefaultChannels) {
        Channel c;
        c.id = id;
        c.name = name;
        if (!eng_.addNullSink(sinkName(c.id), disp(name), 2,
                              error))
            return false;
        if (!buildFxChannelPaths(c.id, name, error)) return false;
        {
            std::string chainErr;
            startChannelChain(c.id, name, chainErr);
        }
        channels_.push_back(std::move(c));
    }

    if (!eng_.addNullSink(kSoundShareSink, disp("Sound Sharing"), 2, error))
        return false;
    if (!eng_.addPath(pathName("sound-share", Mix::Stream), kSoundShareSink,
                      kStreamMix, "Sound Sharing → Stream", 2, true, error))
        return false;
    if (!eng_.addPath(pathName("sound-share", Mix::Monitor), kSoundShareSink,
                      kMonitorMix, "Sound Sharing → Monitor", 2, true, error))
        return false;

    MasterBusRuntime primary;
    primary.id = kPrimaryMasterId;
    primary.name = "Input #1";
    primary.captureMatch = micNodeMatch_;
    primary.captureNode = findCaptureNode(primary.captureMatch);
    primary.wantSoftwareGain = pendingWantMicGain_;
    primary.micStereo = true;
    primary.micMonitorFx = false;

    if (!createMasterChainNodes(primary, withNoiseSuppression, error)) return false;
    if (!createMasterVirtualSource(primary, error)) return false;
    masterBuses_.push_back(std::move(primary));

    if (!buildPrimaryMasterPaths(error)) return false;

    if (monitorOutputs_.empty()) {
        const std::string dflt = eng_.defaultSinkName();
        if (!dflt.empty()) {
            MonitorOutputEntry e;
            e.sink = dflt;
            e.description = lookupSinkDescription(dflt);
            if (e.description.empty()) e.description = dflt;
            monitorOutputs_.push_back(std::move(e));
        }
    }
    // Deliberately no monitor loopback yet, even though there is now an entry
    // to build one from.
    //
    // build() runs before the profile is read, so the only level available here
    // is the struct default -- unity. A path created now plays the whole
    // Monitor mix into the default output at full scale, and goes on doing it
    // until applyProfile() arrives with the saved levels a second or more
    // later. That is not a click, it is the mixer shouting into somebody's
    // headphones on every single start.
    //
    // MixerService::applyProfile() builds them, once it knows how loud they are
    // supposed to be. Until then the monitor mix goes nowhere, which is the
    // right way round: silence is recoverable, the other thing is not.
    applyStreamMixGain();

    built_ = true;
    return true;
}

bool MixerGraph::resolveMasterChain(const MasterBusRuntime &bus,
                                    std::string &dryNode, std::string &dryPort,
                                    std::string &fxNode, std::string &fxPort) const {
    if (isMasterMidi(bus)) {
        if (!bus.chain.synthReady) return false;
        const std::string &id = bus.id;
        const std::string synthNode = masterSynthNode(id);
        if (!eng_.hasPort(synthNode, "output", true)) return false;

        const MasterBusChain &chain = bus.chain;
        const std::string gainNode = masterGainNode(id);
        const std::string ncNode = masterNcNode(id);
        const std::string fxNodeName = masterFxNode(id);
        const std::string creativeNode = masterCreativeNode(id);
        const std::string dynNode = masterDynNode(id);

        std::string srcNode = synthNode;
        std::string srcPort = "output";
        if (chain.gainReady && eng_.hasPort(gainNode, "input", false)) {
            srcNode = gainNode;
            srcPort = "output";
        }
        dryNode = srcNode;
        dryPort = srcPort;

        if (chain.ncReady && eng_.hasPort(ncNode, "input", false)) {
            srcNode = ncNode;
            srcPort = "output";
        }
        if (chain.fxReady && eng_.hasPort(fxNodeName, "input", false)) {
            srcNode = fxNodeName;
            srcPort = "output";
        }
        if (chain.creativeReady && eng_.hasPort(creativeNode, "input", false)) {
            srcNode = creativeNode;
            srcPort = "output";
        }
        if (chain.dynReady && eng_.hasPort(dynNode, "input", false)) {
            srcNode = dynNode;
            srcPort = "output";
        }
        fxNode = srcNode;
        fxPort = srcPort;
        return true;
    }

    if (bus.captureNode.empty()) return false;

    const MasterBusChain &chain = bus.chain;
    const std::string &id = bus.id;
    const std::string gainNode = masterGainNode(id);
    const std::string ncNode = masterNcNode(id);
    const std::string fxNodeName = masterFxNode(id);
    const std::string creativeNode = masterCreativeNode(id);
    const std::string dynNode = masterDynNode(id);

    std::string srcNode = bus.captureNode;
    std::string srcPort = masterCapturePort(bus);
    if (chain.selectorReady) {
        srcNode = masterCaptureSelectorNode(id);
        srcPort = "output";
        if (!eng_.hasPort(srcNode, srcPort, true)) return false;
    } else if (!eng_.hasPort(srcNode, srcPort, true)) {
        return false;
    }
    if (chain.gainReady && eng_.hasPort(gainNode, "input", false)) {
        srcNode = gainNode;
        srcPort = "output";
    }
    dryNode = srcNode;
    dryPort = srcPort;

    if (chain.ncReady && eng_.hasPort(ncNode, "input", false)) {
        srcNode = ncNode;
        srcPort = "output";
    }
    if (chain.fxReady && eng_.hasPort(fxNodeName, "input", false)) {
        srcNode = fxNodeName;
        srcPort = "output";
    }
    if (chain.creativeReady && eng_.hasPort(creativeNode, "input", false)) {
        srcNode = creativeNode;
        srcPort = "output";
    }
    if (chain.dynReady && eng_.hasPort(dynNode, "input", false)) {
        srcNode = dynNode;
        srcPort = "output";
    }
    fxNode = srcNode;
    fxPort = srcPort;
    return true;
}

bool MixerGraph::masterHwFirstHop(const MasterBusRuntime &bus, std::string &inNode,
                                  std::string &inPort) const {
    inPort = "input";
    if (bus.chain.gainReady) {
        inNode = masterGainNode(bus.id);
        return true;
    }
    if (bus.chain.ncReady) {
        inNode = masterNcNode(bus.id);
        return true;
    }
    if (bus.chain.fxReady) {
        inNode = masterFxNode(bus.id);
        return true;
    }
    if (bus.chain.dynReady) {
        inNode = masterDynNode(bus.id);
        return true;
    }
    inNode.clear();
    return false;
}

bool MixerGraph::selectMasterCaptureInput(MasterBusRuntime &bus,
                                          std::string &error,
                                          bool activate) {
    if (!bus.chain.selectorReady || !bus.chain.selector) return false;
    if (bus.captureNode.empty()) {
        error = "capture node not ready for " + bus.id;
        return false;
    }

    auto it = std::find(bus.selectorInputs.begin(), bus.selectorInputs.end(),
                        bus.captureNode);
    std::size_t index = 0;
    if (it == bus.selectorInputs.end()) {
        if (bus.selectorInputs.size() >= CaptureSelector::kMaxInputs) {
            error = "capture selector is full for " + bus.id;
            return false;
        }
        index = bus.selectorInputs.size();
        bus.selectorInputs.push_back(bus.captureNode);
    } else {
        index = static_cast<std::size_t>(
            std::distance(bus.selectorInputs.begin(), it));
    }

    const std::string selectorNode = masterCaptureSelectorNode(bus.id);
    const std::string selectorPort = CaptureSelector::inputPort(index);
    if (!eng_.waitForPort(selectorNode, selectorPort, false, 1000)) {
        error = "capture selector input not ready for " + bus.id;
        return false;
    }
    const std::string capturePort = masterCapturePort(bus);
    if (!eng_.hasPort(bus.captureNode, capturePort, true) &&
        !eng_.waitForPort(bus.captureNode, capturePort, true, 1000)) {
        error = "capture port not ready on " + bus.captureNode;
        return false;
    }
    if (!eng_.linkPorts(bus.captureNode, capturePort, selectorNode, selectorPort,
                        error, /*asyncLink=*/true))
        return false;

    // The new hardware edge is active before selection changes. Existing
    // capture links remain untouched, avoiding the PipeWire clock stall caused
    // by deleting a live ALSA link. A physical-replug warm-up deliberately
    // leaves the selector on silence until the settled rebuild.
    if (activate) bus.chain.selector->select(index);
    return true;
}

void MixerGraph::silenceMasterCapture(const std::string &id) {
    MasterBusRuntime *bus = masterBus(id);
    if (bus && bus->chain.selectorReady && bus->chain.selector)
        bus->chain.selector->selectSilence();
}

bool MixerGraph::primeMasterHwCapture(const std::string &id, std::string &error) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) {
        error = "no such input device: " + id;
        return false;
    }
    if (bus->captureMatch.empty()) return true;

    bus->captureNode.clear();
    if (!waitForCaptureNode(*bus, 1000, error)) return false;

    if (!bus->chain.selectorReady || !bus->chain.selector)
        return relinkMasterHwCapture(id, error);

    bus->chain.selector->selectSilence();
    return selectMasterCaptureInput(*bus, error, /*activate=*/false);
}

bool MixerGraph::relinkMasterHwCapture(const std::string &id, std::string &error) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) {
        error = "no such input device: " + id;
        return false;
    }
    if (bus->captureMatch.empty()) return true;

    bus->captureNode.clear();
    if (!waitForCaptureNode(*bus, 400, error)) return false;

    if (bus->chain.selectorReady)
        return selectMasterCaptureInput(*bus, error);

    std::string inNode, inPort;
    if (!masterHwFirstHop(*bus, inNode, inPort)) {
        // DSP not up yet (new master / cold partial wire) — full path wire.
        return wireMasterPaths(id, error);
    }

    // Drop edges from this ALSA node only. Leave gain→…→mix alone so peers
    // sharing Monitor/Stream do not see their mix inputs torn down.
    eng_.forgetLinksForNode(bus->captureNode);

    const std::string capPort = masterCapturePort(*bus);
    if (!eng_.hasPort(bus->captureNode, capPort, true) &&
        !eng_.waitForPort(bus->captureNode, capPort, true, 1000)) {
        error = "capture port not ready on " + bus->captureNode;
        return false;
    }
    if (!eng_.waitForPort(inNode, inPort, false, 1000)) {
        error = "first hop not ready on " + inNode;
        return false;
    }
    return eng_.linkPorts(bus->captureNode, capPort, inNode, inPort, error,
                          /*asyncLink=*/true);
}

bool MixerGraph::rebuildMasterHwCapture(const std::string &id, std::string &error) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) {
        error = "no such input device: " + id;
        return false;
    }
    if (bus->captureNode.empty() && bus->captureMatch.empty()) return true;

    float preservedGain = 1.0f;
    if (bus->chain.gainReady && bus->chain.gain)
        preservedGain = bus->chain.gain->gain();
    const bool wantNc = bus->chain.ncReady;

    if (!bus->captureNode.empty())
        eng_.forgetLinksForNode(bus->captureNode);

    // Full consumer recreate for *this* master only — peers stay linked.
    destroyMasterChain(*bus);
    bus->wantSoftwareGain = true;
    eng_.sync();

    if (!createMasterChainNodes(*bus, wantNc, error)) return false;
    {
        std::string srcErr;
        createMasterVirtualSource(*bus, srcErr);
    }
    if (!wireMasterPaths(id, error)) return false;
    if (bus->chain.gainReady && bus->chain.gain)
        bus->chain.gain->setGain(preservedGain);
    return true;
}

bool MixerGraph::rebuildAllMasterHwCaptures(std::string &error) {
    error.clear();
    std::string last;
    for (const auto &bus : masterBuses_) {
        std::string err;
        if (!rebuildMasterHwCapture(bus.id, err) && last.empty()) last = err;
        eng_.sync();
    }
    if (!last.empty()) {
        error = last;
        return false;
    }
    return true;
}

bool MixerGraph::wireMasterPaths(const std::string &id, std::string &error) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) {
        error = "no such input device: " + id;
        return false;
    }

    if (isMasterMidi(*bus)) {
        if (!bus->midiPortMatch.empty()) {
            bus->midiNode.clear();
            std::string midiErr;
            // MIDI hardware may appear late; never block the whole graph on it.
            if (waitForMidiNode(*bus, 150, midiErr)) {
                std::string linkErr;
                if (!linkMasterMidiInput(*bus, linkErr))
                    error = linkErr;
            }
        }
    } else if (!bus->captureMatch.empty()) {
        bus->captureNode.clear();
        // Short wait: callers debounce before rebuild. Blocking the daemon
        // thread for multi-second polls is what made replug feel like a lockup.
        if (!waitForCaptureNode(*bus, 400, error))
            return false;
    } else {
        syncMasterCaptureNode(*bus);
    }

    if (isMasterMidi(*bus)) {
        if (!bus->chain.synthReady) {
            error = "MIDI synth not ready for " + id;
            return false;
        }
    } else if (bus->captureNode.empty()) {
        if (!bus->captureMatch.empty()) {
            error = "capture node not ready for " + id;
            return false;
        }
        return true;
    } else {
        const std::string capPort = masterCapturePort(*bus);
        if (!eng_.hasPort(bus->captureNode, capPort, true) &&
            !eng_.waitForPort(bus->captureNode, capPort, true, 2000)) {
            error = "capture port not ready on " + bus->captureNode;
            return false;
        }
        if (bus->chain.selectorReady && !selectMasterCaptureInput(*bus, error))
            return false;
    }

    if (bus->wantSoftwareGain && !bus->chain.gainReady) {
        bus->chain.gain = std::make_unique<GainFilter>();
        std::string gainErr;
        const bool primary = isPrimaryMaster(id);
        if (bus->chain.gain->start(masterGainNode(id),
                                   disp(primary ? "Microphone Gain" : bus->name + " Gain"),
                                   1, gainErr))
            bus->chain.gainReady = true;
        else
            bus->chain.gain.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    std::string dryNode, dryPort, fxNode, fxPort;
    if (!resolveMasterChain(*bus, dryNode, dryPort, fxNode, fxPort)) {
        error = "master chain not ready for " + id;
        return false;
    }

    bus->chain.dryTailNode = dryNode;
    bus->chain.dryTailPort = dryPort;
    bus->chain.fxTailNode = fxNode;
    bus->chain.fxTailPort = fxPort;

    const MasterBusChain &chain = bus->chain;
    const std::string gainNode = masterGainNode(id);
    const std::string ncNode = masterNcNode(id);
    const std::string fxNodeName = masterFxNode(id);
    const std::string creativeNodeName = masterCreativeNode(id);
    const std::string dynNodeName = masterDynNode(id);
    const std::string srcNodeName = masterSourceNode(id);

    eng_.forgetLinksForNode(gainNode);
    eng_.forgetLinksForNode(ncNode);
    eng_.forgetLinksForNode(fxNodeName);
    eng_.forgetLinksForNode(creativeNodeName);
    eng_.forgetLinksForNode(dynNodeName);
    eng_.forgetLinksForNode(srcNodeName);
    if (isPrimaryMaster(id)) {
        for (Mix mix : {Mix::Monitor, Mix::Stream}) {
            eng_.forgetLinksTo(pathName("mic", mix) + "-in");
        }
    } else {
        eng_.forgetLinksTo(pathName(id, Mix::Monitor) + "-in");
        eng_.forgetLinksTo(pathName(id, Mix::Stream) + "-in");
    }
    eng_.sync();

    std::string srcNode;
    std::string srcPort;
    if (isMasterMidi(*bus)) {
        srcNode = masterSynthNode(id);
        srcPort = "output";
        if (!eng_.waitForPort(srcNode, srcPort, true, 400)) {
            error = "MIDI synth output not ready";
            return false;
        }
    } else {
        srcNode = bus->chain.selectorReady ? masterCaptureSelectorNode(id)
                                           : bus->captureNode;
        srcPort = bus->chain.selectorReady ? "output" : masterCapturePort(*bus);
    }
    // First hop from ALSA hardware must be async. A sync link pulls the mic's
    // capture clock into the shared graph driver; when an unrelated peer
    // (e.g. OBS stopping an SC0710 monitor) leaves and the quantum
    // renegotiates, Wave:3 capture rubber-bands until the node is reopened.
    // Async isolates the hardware clock (+1 quantum latency on that edge).
    const std::string hwCapture = bus->captureNode;
    auto linkFromSrc = [&](const std::string &inNode,
                           const std::string &inPort) -> bool {
        const bool asyncHw = (srcNode == hwCapture);
        const int timeoutMs = isMasterMidi(*bus) ? 250 : 1000;
        return eng_.linkPorts(srcNode, srcPort, inNode, inPort, error, asyncHw,
                              timeoutMs);
    };
    const int chainWaitMs = isMasterMidi(*bus) ? 400 : 3000;

    if (chain.gainReady &&
        eng_.waitForPort(gainNode, "input", false, chainWaitMs)) {
        if (!linkFromSrc(gainNode, "input"))
            return false;
        srcNode = gainNode;
        srcPort = "output";
    }

    if (chain.ncReady && eng_.waitForPort(ncNode, "input", false, chainWaitMs)) {
        if (!linkFromSrc(ncNode, "input"))
            return false;
        srcNode = ncNode;
        srcPort = "output";
    }
    if (chain.fxReady && eng_.waitForPort(fxNodeName, "input", false, chainWaitMs)) {
        if (!linkFromSrc(fxNodeName, "input"))
            return false;
        srcNode = fxNodeName;
        srcPort = "output";
    }
    if (chain.creativeReady &&
        eng_.waitForPort(creativeNodeName, "input", false, chainWaitMs)) {
        if (!linkFromSrc(creativeNodeName, "input"))
            return false;
        srcNode = creativeNodeName;
        srcPort = "output";
    }
    if (chain.dynReady &&
        eng_.waitForPort(dynNodeName, "input", false, chainWaitMs)) {
        if (!linkFromSrc(dynNodeName, "input"))
            return false;
        srcNode = dynNodeName;
        srcPort = "output";
    }

    if (chain.sourceReady &&
        eng_.waitForPort(srcNodeName, "input_MONO", false, chainWaitMs))
        eng_.linkPorts(fxNode, fxPort, srcNodeName, "input_MONO", error);

    if (isPrimaryMaster(id)) {
        if (!ensurePrimaryMasterPaths(*bus, error)) return false;
        eng_.sync();

        const std::string monitorNode = bus->micMonitorFx ? fxNode : dryNode;
        const std::string monitorPort = bus->micMonitorFx ? fxPort : dryPort;
        for (Mix mix : {Mix::Monitor, Mix::Stream}) {
            const std::string in = pathName("mic", mix) + "-in";
            const std::string &node = (mix == Mix::Monitor) ? monitorNode : fxNode;
            const std::string &port = (mix == Mix::Monitor) ? monitorPort : fxPort;
            if (mix == Mix::Monitor && !softwareMonitor_) {
                disconnectMasterMonitorLinks(id);
                continue;
            }
            const std::string handle = pathName("mic", mix);
            const std::string capPort = eng_.pathCapturePort(handle);
            bool linked = false;
            for (int attempt = 0; attempt < 2 && !linked; ++attempt) {
                if (!eng_.waitForPort(in, capPort, false, 3000)) {
                    eng_.sync();
                    continue;
                }
                eng_.sync();
                if (bus->micStereo) {
                    linked = eng_.linkPorts(node, port, in, "input_FL", error) &&
                             eng_.linkPorts(node, port, in, "input_FR", error);
                } else {
                    linked = eng_.linkPorts(node, port, in, "input_MONO", error);
                }
                if (!linked) eng_.sync();
            }
            if (!linked) {
                error = std::string(mix == Mix::Stream ? "stream" : "monitor") +
                        " mix path not ready: " + in;
                return false;
            }
        }
        eng_.setPathMuted(pathName("mic", Mix::Monitor), !softwareMonitor_);
    } else {
        if (!syncMasterStreamPath(id, error)) return false;

        const std::string streamHandle = pathName(id, Mix::Stream);
        const std::string monHandle = masterMonitorPath(id);
        const bool chainReady = chain.gainReady || chain.ncReady || chain.fxReady ||
                                chain.dynReady;
        if (chainReady) {
            PwEngine::PathSpec spec;
            spec.handle = monHandle;
            spec.target = kMonitorMix;
            spec.description = disp(bus->name + " → Monitor");
            spec.inChannels = bus->micStereo ? 2 : 1;
            spec.outChannels = 2;
            spec.remix = false;
            spec.source.clear();
            spec.sourceIsSink = false;
            spec.volume = bus->monitorVolume;
            spec.muted = !bus->softwareMonitor || bus->monitorMuted;
            std::string pathErr;
            if (!eng_.repairPath(spec, pathErr)) {
                error = pathErr;
                return false;
            }
        }

        // Levels before links so unity-gain loopbacks are never audible.
        applyMasterPathLevels(id);

        const std::string streamIn = streamHandle + "-in";
        const std::string capPort = eng_.pathCapturePort(streamHandle);
        bool streamLinked = false;
        if (eng_.waitForPort(streamIn, capPort, false, chainWaitMs)) {
            if (bus->micStereo) {
                streamLinked =
                    eng_.linkPorts(fxNode, fxPort, streamIn, "input_FL", error) &&
                    eng_.linkPorts(fxNode, fxPort, streamIn, "input_FR", error);
            } else {
                streamLinked =
                    eng_.linkPorts(fxNode, fxPort, streamIn, "input_MONO", error);
            }
        }
        if (!streamLinked) {
            error = "stream mix path not ready: " + streamIn;
            return false;
        }

        if (chainReady) {
            if (bus->softwareMonitor) {
                const std::string monIn = monHandle + "-in";
                const std::string monitorNode = bus->micMonitorFx ? fxNode : dryNode;
                const std::string monitorPort = bus->micMonitorFx ? fxPort : dryPort;
                const std::string monCapPort = eng_.pathCapturePort(monHandle);
                bool monitorLinked = false;
                if (eng_.waitForPort(monIn, monCapPort, false, chainWaitMs)) {
                    if (bus->micStereo) {
                        monitorLinked =
                            eng_.linkPorts(monitorNode, monitorPort, monIn, "input_FL",
                                           error) &&
                            eng_.linkPorts(monitorNode, monitorPort, monIn, "input_FR",
                                           error);
                    } else {
                        monitorLinked = eng_.linkPorts(monitorNode, monitorPort, monIn,
                                                       "input_MONO", error);
                    }
                }
                if (!monitorLinked) {
                    error = "monitor mix path not ready: " + monIn;
                    return false;
                }
            } else {
                disconnectMasterMonitorLinks(id);
            }
        }
        eng_.setPathMuted(monHandle, !bus->softwareMonitor || bus->monitorMuted);
    }

    // Again after linking — PipeWire often rewrites Props once the stream runs.
    applyMasterPathLevels(id);
    return true;
}

bool MixerGraph::wireMicPaths(std::string &error) {
    error.clear();
    std::string last;
    for (auto &bus : masterBuses_) {
        // MIDI is optional and its pw_filter ports commonly settle after the
        // capture graph on cold boot. A MIDI timeout must never fail the global
        // microphone rewire and trigger clearLinks() retry storms.
        if (isMasterMidi(bus)) continue;
        // Nor must an input device that is simply not plugged in. Unplugged
        // hardware is a normal state, not a wiring failure: reporting it as one
        // fails the whole rewire, and the retry ladder answers with up to eight
        // more clearLinks() passes -- every channel in the graph torn down and
        // rebuilt, over about a minute, because one absent microphone could not
        // be wired. It also left captureHotplugArmed_ unset, so the device did
        // not get wired when it came back either.
        //
        // Absent means no node in the registry matches at all. A device that is
        // present but whose ports have not settled still fails through
        // wireMasterPaths() below, because that one is transient and a retry is
        // exactly the right answer to it.
        if (!bus.captureMatch.empty() && findCaptureNode(bus.captureMatch).empty()) {
            // Clear the remembered node: verifyMasterMixWiring() skips a bus
            // with no capture node, and would otherwise demand links to the
            // device that just went away.
            bus.captureNode.clear();
            fprintf(stderr,
                    "waveline: master '%s' capture '%s' is not connected -- "
                    "leaving it unwired until it appears\n",
                    bus.id.c_str(), bus.captureMatch.c_str());
            continue;
        }
        std::string err;
        if (!wireMasterPaths(bus.id, err)) last = err;
    }
    if (!last.empty()) {
        error = last;
        return false;
    }
    // Do not rebuild the ALSA hop here. Startup/hotplug still has quantum
    // moving; MixerService schedules per-master rebuildCaptureHops after quiet.
    return true;
}

bool MixerGraph::verifyMasterMixWiring(const std::string &id, std::string &error) const {
    error.clear();
    const MasterBusRuntime *bus = masterBus(id);
    if (!bus) return true;

    auto needLink = [&](const std::string &inNode, const std::string &port) -> bool {
        if (eng_.hasManualLinkTo(inNode, port)) return true;
        error = "missing link to " + inNode + ":" + port;
        return false;
    };
    auto needPathLinks = [&](const std::string &handle, bool stereo) -> bool {
        if (!eng_.pathExists(handle)) {
            error = "missing mix path: " + handle;
            return false;
        }
        const std::string in = handle + "-in";
        if (stereo) {
            return needLink(in, "input_FL") && needLink(in, "input_FR");
        }
        return needLink(in, "input_MONO");
    };

    if (isMasterMidi(*bus)) {
        if (!bus->chain.synthReady) {
            error = "MIDI synth not ready for " + id;
            return false;
        }
        const std::string streamHandle = pathName(id, Mix::Stream);
        if (!needPathLinks(streamHandle, bus->micStereo)) return false;
        if (masterSoftwareMonitor(id)) {
            if (!needPathLinks(masterMonitorPath(id), bus->micStereo)) return false;
        }
        return true;
    }

    if (bus->captureNode.empty()) return true;

    const std::string streamHandle =
        isPrimaryMaster(id) ? pathName("mic", Mix::Stream) : pathName(id, Mix::Stream);
    if (!needPathLinks(streamHandle, bus->micStereo)) return false;

    if (masterSoftwareMonitor(id)) {
        const std::string monHandle =
            isPrimaryMaster(id) ? pathName("mic", Mix::Monitor) : masterMonitorPath(id);
        if (!needPathLinks(monHandle, bus->micStereo)) return false;
    }
    return true;
}

std::vector<std::string> MixerGraph::stalledPaths() const {
    std::vector<std::string> out;
    auto check = [&](const std::string &handle) {
        if (!eng_.pathExists(handle)) return;
        for (const std::string &end : {handle + "-in", handle + "-out"})
            if (eng_.nodeStalled(end)) out.push_back(end);
    };

    // Monitor outputs first: this is the leg between the Monitor mix and the
    // speakers, and the one whose failure is inaudible in the meters.
    for (int slot = 0; slot < static_cast<int>(kMaxMonitorOutputs); ++slot)
        check(monitorOutputHandle(slot));

    for (const auto &bus : masterBuses_) {
        if (isPrimaryMaster(bus.id)) {
            check(pathName("mic", Mix::Stream));
            check(pathName("mic", Mix::Monitor));
        } else {
            check(pathName(bus.id, Mix::Stream));
            check(masterMonitorPath(bus.id));
        }
    }
    for (const auto &c : channels_) {
        check(pathName(c.id, Mix::Stream));
        check(pathName(c.id, Mix::Monitor));
    }
    return out;
}

bool MixerGraph::verifyMixWiring(std::string &error) const {
    error.clear();

    for (const auto &bus : masterBuses_) {
        // MIDI synth ports settle asynchronously; the MIDI timer wires these
        // without tearing down capture devices. Including them here causes
        // rewire retry storms on cold boot.
        if (isMasterMidi(bus)) continue;
        if (!verifyMasterMixWiring(bus.id, error)) return false;
    }

    auto needLink = [&](const std::string &inNode, const std::string &port) -> bool {
        if (eng_.hasManualLinkTo(inNode, port)) return true;
        error = "missing link to " + inNode + ":" + port;
        return false;
    };
    auto needPathLinks = [&](const std::string &handle, bool stereo) -> bool {
        const std::string in = handle + "-in";
        if (stereo) {
            return needLink(in, "input_FL") && needLink(in, "input_FR");
        }
        return needLink(in, "input_MONO");
    };

    for (const auto &c : channels_) {
        if (!needPathLinks(pathName(c.id, Mix::Stream), true)) return false;
        if (!needPathLinks(pathName(c.id, Mix::Monitor), true)) return false;
    }
    return true;
}

bool MixerGraph::ensurePrimaryMasterPaths(const MasterBusRuntime &bus,
                                          std::string &error) {
    error.clear();
    for (Mix mix : {Mix::Monitor, Mix::Stream}) {
        PwEngine::PathSpec spec;
        spec.handle = pathName("mic", mix);
        spec.target = (mix == Mix::Stream) ? kStreamMix : kMonitorMix;
        spec.description = (mix == Mix::Stream) ? "Microphone → Stream"
                                                : "Microphone → Monitor";
        spec.inChannels = bus.micStereo ? 2 : 1;
        spec.outChannels = bus.micStereo ? 2 : 1;
        spec.remix = false;
        spec.source.clear();
        spec.sourceIsSink = false;
        if (mix == Mix::Stream) {
            spec.volume = bus.streamVolume;
            spec.muted = bus.streamMuted;
        } else {
            spec.volume = bus.monitorVolume;
            spec.muted = !softwareMonitor_ || bus.monitorMuted;
        }
        if (!eng_.repairPath(spec, error)) return false;
    }
    return true;
}

bool MixerGraph::rewireMasterMonitor(const std::string &id, std::string &error) {
    if (!isPrimaryMaster(id)) {
        error = "monitor rewire is primary-only";
        return false;
    }
    return rewireMicMonitor(error);
}

bool MixerGraph::wireChannelMixPath(const std::string &channelId, Mix mix,
                                    const std::string &node,
                                    const std::string &portL,
                                    const std::string &portR, bool mono,
                                    std::string &error) {
    const std::string in = pathName(channelId, mix) + "-in";
    const std::string capPort = eng_.pathCapturePort(pathName(channelId, mix));
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!eng_.waitForPort(in, capPort, false, 3000)) {
            eng_.sync();
            continue;
        }
        eng_.sync();
        bool ok = false;
        if (mono) {
            ok = eng_.linkPorts(node, "output", in, "input_FL", error) &&
                 eng_.linkPorts(node, "output", in, "input_FR", error);
        } else {
            ok = eng_.linkPorts(node, portL, in, "input_FL", error) &&
                 eng_.linkPorts(node, portR, in, "input_FR", error);
        }
        if (ok) return true;
        eng_.sync();
    }
    return false;
}

bool MixerGraph::wireChannelPaths(const std::string &channelId,
                                  const std::string &node, const std::string &portL,
                                  const std::string &portR, bool mono,
                                  std::string &error) {
    const bool monitor =
        wireChannelMixPath(channelId, Mix::Monitor, node, portL, portR, mono, error);
    const bool stream =
        wireChannelMixPath(channelId, Mix::Stream, node, portL, portR, mono, error);
    if (monitor && stream) return true;
    if (error.empty())
        error = "channel " + channelId + ": " +
                (monitor ? "stream" : "monitor") + " path not wired";
    return false;
}

bool MixerGraph::wireChannelPassthrough(const std::string &channelId,
                                        std::string &error) {
    const std::string sink = sinkName(channelId);
    if (wireChannelPaths(channelId, sink, "monitor_FL", "monitor_FR", false, error))
        return true;
    eng_.sync();
    return wireChannelPaths(channelId, sink, "monitor_FL", "monitor_FR", false, error);
}

bool MixerGraph::ensurePathsForChannel(const std::string &channelId,
                                       std::string &error) {
    Channel *c = channel(channelId);
    if (!c) return true;
    error.clear();
    for (Mix mix : {Mix::Monitor, Mix::Stream}) {
        PwEngine::PathSpec spec;
        spec.handle = pathName(channelId, mix);
        spec.target = (mix == Mix::Stream) ? kStreamMix : kMonitorMix;
        spec.description = c->name + (mix == Mix::Stream ? " → Stream" : " → Monitor");
        spec.inChannels = 2;
        spec.outChannels = 2;
        spec.remix = false;
        spec.source.clear();
        spec.sourceIsSink = false;
        // The fader, not unity. repairPath() writes spec.volume even when it
        // keeps an existing module, so leaving these at their defaults reset
        // every channel to 100% unmuted on each rewire -- including the one
        // that runs right after the profile loads at startup.
        spec.volume = (mix == Mix::Stream) ? c->streamVolume : c->monitorVolume;
        spec.muted = (mix == Mix::Stream) ? c->streamMuted : c->monitorMuted;
        std::string err;
        if (!eng_.repairPath(spec, err)) {
            error = err;
            return false;
        }
    }
    eng_.sync();
    return true;
}

void MixerGraph::wireChannelMicSource(const std::string &channelId) {
    const Channel *c = channel(channelId);
    if (!c || !c->micSource) return;
    auto it = chChains_.find(channelId);
    if (it == chChains_.end()) return;
    const ChannelChain &chain = it->second;
    const std::string ncNode = ncNodeName(channelId, FxStage::Input);
    const std::string fxNode = fxNodeName(channelId, FxStage::Input);
    const std::string creativeNode = creativeNodeName(channelId, FxStage::Input);
    const std::string dynNode = dynNodeName(channelId, FxStage::Input);

    // One entry per selected input device, all linked into whatever stage comes
    // first in this channel's chain. PipeWire sums links arriving at the same
    // input port, so two devices become one recording device with no mixer node
    // of our own. Everything downstream of that first stage is a single hop and
    // is wired exactly as it was for one device.
    struct MicSource {
        std::string node;
        std::string port;
        bool hw = false;  // still on the ALSA node: that hop has to be async
    };
    std::vector<MicSource> sources;
    // A MIDI instrument must never meet the speech denoiser -- createMasterChainNodes
    // refuses to build one for a MIDI bus for exactly this reason. Measured on a
    // piano through a channel's mic chain: 0.187 in, 0.0016 out, about -41dB, so
    // "MIDI on a channel microphone" read as completely broken. The sources are
    // summed before the filter, so this is all-or-nothing for the channel: one
    // MIDI device in the list takes the whole mic chain around it.
    bool anyMidi = false;
    // "Use device effects": take each device from the end of its own chain and
    // sum those, so every device keeps the processing its own strip gives it.
    // The channel's mic chain is skipped entirely below -- there is only one of
    // it, so running it would flatten the per-device differences that are the
    // whole point of this mode.
    const bool deviceFx = c->micUseDeviceFx;
    for (const std::string &masterId : c->masterMicIds) {
        const MasterBusRuntime *master = masterBus(masterId);
        if (!master) continue;
        const MasterBusChain &mchain = master->chain;
        MicSource s;
        if (deviceFx) {
            // Tails are resolved in wireMasterPaths, which runs before channels
            // are wired. If one is missing the device is mid-rebuild: leave it
            // out and pick it up on the next pass rather than falling back to
            // its raw capture, which is the opposite of what this mode asks for.
            if (mchain.fxTailNode.empty() || mchain.fxTailPort.empty()) continue;
            s.node = mchain.fxTailNode;
            s.port = mchain.fxTailPort;
            // The tail is one of our own filters, never the ALSA node, so this
            // hop is always sync.
            sources.push_back(std::move(s));
            continue;
        }
        if (isMasterMidi(*master)) {
            anyMidi = true;
            // A MIDI instrument has no capture node at all -- its audio starts
            // at the synth. Asking for a capture node here is what made a MIDI
            // device on a channel microphone silently produce nothing.
            if (!mchain.synthReady) continue;
            s.node = masterSynthNode(master->id);
            s.port = "output";
        } else {
            if (master->captureNode.empty()) continue;
            s.node = mchain.selectorReady ? masterCaptureSelectorNode(master->id)
                                          : master->captureNode;
            s.port = mchain.selectorReady ? "output" : masterCapturePort(*master);
            s.hw = (s.node == master->captureNode);
        }
        // Past the device's own input gain, so the channel hears it at the level
        // its strip is set to. Same hop wireMasterPaths takes.
        const std::string mgainNode = masterGainNode(master->id);
        if (mchain.gainReady && eng_.hasPort(mgainNode, "output", true)) {
            s.node = mgainNode;
            s.port = "output";
            s.hw = false;
        }
        sources.push_back(std::move(s));
    }
    if (sources.empty()) return;

    std::string src = sources.front().node;
    std::string port = sources.front().port;
    bool srcIsHw = sources.front().hw;
    // False until a stage of this channel's own chain has been linked; up to
    // that point every device feeds the port, after it there is one source.
    bool advanced = false;
    std::string err;
    auto linkFromSrc = [&](const std::string &inNode,
                           const std::string &inPort) -> bool {
        if (advanced) return eng_.linkPorts(src, port, inNode, inPort, err, srcIsHw);
        bool any = false;
        for (const MicSource &s : sources)
            if (eng_.linkPorts(s.node, s.port, inNode, inPort, err, s.hw)) any = true;
        return any;
    };
    auto advanceTo = [&](const std::string &node, const std::string &outPort) {
        src = node;
        port = outPort;
        srcIsHw = false;
        advanced = true;
    };
    const std::string gainNode = micSendNodeName(channelId);
    if (chain.micSendReady && eng_.hasPort(gainNode, "input", false) &&
        eng_.hasPort(gainNode, "output", true)) {
        if (linkFromSrc(gainNode, "input")) advanceTo(gainNode, "output");
    }
    if (!deviceFx && !anyMidi && chain.inputNcReady &&
        eng_.hasPort(ncNode, "input", false) && eng_.hasPort(ncNode, "output", true)) {
        if (linkFromSrc(ncNode, "input")) advanceTo(ncNode, "output");
    }
    if (!deviceFx && chain.inputFxReady && eng_.hasPort(fxNode, "input_FL", false)) {
        linkFromSrc(fxNode, "input_FL");
        // Right channel only needs async when still on hardware (mono capture
        // duplicated); after mic-send/gain, stay sync with the left hop.
        linkFromSrc(fxNode, "input_FR");
        advanceTo(fxNode, "output_FL");
    }
    if (!deviceFx && chain.inputCreativeReady &&
        eng_.hasPort(creativeNode, "input_FL", false)) {
        if (src == fxNode) {
            eng_.linkPorts(fxNode, "output_FL", creativeNode, "input_FL", err);
            eng_.linkPorts(fxNode, "output_FR", creativeNode, "input_FR", err);
        } else {
            linkFromSrc(creativeNode, "input_FL");
            linkFromSrc(creativeNode, "input_FR");
        }
        advanceTo(creativeNode, "output_FL");
    }
    if (!deviceFx && chain.inputDynReady && eng_.hasPort(dynNode, "input_FL", false)) {
        if (src == fxNode || src == creativeNode) {
            eng_.linkPorts(src, "output_FL", dynNode, "input_FL", err);
            eng_.linkPorts(src, "output_FR", dynNode, "input_FR", err);
        } else {
            linkFromSrc(dynNode, "input_FL");
            linkFromSrc(dynNode, "input_FR");
        }
        advanceTo(dynNode, "output_FL");
    }

    const std::string tailNode = src;
    const bool tailStereo =
        (tailNode == fxNode || tailNode == creativeNode || tailNode == dynNode);

    // A mono tail feeds both sides. Without this the publish step only ran for a
    // stereo tail, so a mic chain that ends mono -- which is every chain in
    // "use device effects", and any chain whose stereo stages are switched off
    // -- published silence.
    auto linkTailTo = [&](const std::string &inNode) {
        if (tailStereo) {
            eng_.linkPorts(tailNode, "output_FL", inNode, "input_FL", err);
            eng_.linkPorts(tailNode, "output_FR", inNode, "input_FR", err);
            return;
        }
        linkFromSrc(inNode, "input_FL");
        linkFromSrc(inNode, "input_FR");
    };

    const std::string pub = micSourceNode(channelId);
    if (eng_.waitForPort(pub, "input_FL", false, 3000)) linkTailTo(pub);

    const std::string monHandle = channelMicMonitorPath(channelId);
    const std::string monIn = monHandle + "-in";
    const std::string capPort = eng_.pathCapturePort(monHandle);
    if (c->micMonitor && eng_.waitForPort(monIn, capPort, false, 3000)) {
        linkTailTo(monIn);
    } else {
        disconnectChannelMicMonitorLinks(channelId);
    }
    eng_.setPathMuted(monHandle, !c->micMonitor);
}

bool MixerGraph::wireChannelFx(const std::string &channelId, std::string &error) {
    const std::string sink = sinkName(channelId);
    auto tryLink = [this](const std::string &outNode, const std::string &outPort,
                          const std::string &inNode, const std::string &inPort) {
        std::string err;
        return eng_.linkPorts(outNode, outPort, inNode, inPort, err);
    };

    const std::string monIn = pathName(channelId, Mix::Monitor) + "-in";
    const std::string streamIn = pathName(channelId, Mix::Stream) + "-in";
    eng_.forgetLinksTo(monIn, "input_FL");
    eng_.forgetLinksTo(monIn, "input_FR");
    eng_.forgetLinksTo(streamIn, "input_FL");
    eng_.forgetLinksTo(streamIn, "input_FR");
    eng_.sync();

    bool fxWired = false;
    std::string fxOutNode, fxOutPortL, fxOutPortR;
    bool fxOutMono = false;
    auto it = chChains_.find(channelId);
    if (it != chChains_.end()) {
        const ChannelChain &chain = it->second;
        const std::string mixNode = mixNodeName(channelId);
        const std::string outNcNode = ncNodeName(channelId, FxStage::Output);
        const std::string outFxNode = fxNodeName(channelId, FxStage::Output);
        const std::string outCreativeNode = creativeNodeName(channelId, FxStage::Output);
        const std::string outDuckNode = duckNodeName(channelId);
        const std::string outDynNode = dynNodeName(channelId, FxStage::Output);
        const std::string outLufsNode = lufsNodeName(channelId);

        const bool useMixer =
            chain.mixerReady && eng_.hasPort(mixNode, "input_FL", false);
        const bool hasOutNc =
            chain.outputNcReady && eng_.hasPort(outNcNode, "input", false) &&
            eng_.hasPort(outNcNode, "output", true);
        const bool hasOutFx =
            chain.outputFxReady && eng_.hasPort(outFxNode, "input_FL", false) &&
            eng_.hasPort(outFxNode, "output_FL", true);
        const bool hasOutCreative =
            chain.outputCreativeReady &&
            eng_.hasPort(outCreativeNode, "input_FL", false) &&
            eng_.hasPort(outCreativeNode, "output_FL", true);
        const bool hasOutDuck =
            chain.outputDuckReady && eng_.hasPort(outDuckNode, "input_FL", false) &&
            eng_.hasPort(outDuckNode, "output_FL", true);
        const bool hasOutDyn =
            chain.outputDynReady && eng_.hasPort(outDynNode, "input_FL", false) &&
            eng_.hasPort(outDynNode, "output_FL", true);
        const bool hasOutLufs =
            chain.outputLufsReady && eng_.hasPort(outLufsNode, "input_FL", false) &&
            eng_.hasPort(outLufsNode, "output_FL", true);

        bool anchored = false;
        if (useMixer) {
            anchored = tryLink(sink, "monitor_FL", mixNode, "input_FL") &&
                       tryLink(sink, "monitor_FR", mixNode, "input_FR");
        }

        const bool useMixOut =
            anchored && eng_.hasPort(mixNode, "output_FL", true);
        std::string pathOutNode = useMixOut ? mixNode : sink;
        std::string pathOutPortL = useMixOut ? "output_FL" : "monitor_FL";
        std::string pathOutPortR = useMixOut ? "output_FR" : "monitor_FR";
        bool pathOutMono = false;
        if (!useMixOut) anchored = true;

        if (hasOutNc) {
            const std::string ncIn = pathOutMono ? pathOutPortL : pathOutPortL;
            if (tryLink(pathOutNode, ncIn, outNcNode, "input")) {
                pathOutNode = outNcNode;
                pathOutPortL = pathOutPortR = "output";
                pathOutMono = true;
            }
        }

        if (hasOutFx) {
            bool linked = false;
            if (pathOutMono) {
                linked = tryLink(pathOutNode, "output", outFxNode, "input_FL") &&
                         tryLink(pathOutNode, "output", outFxNode, "input_FR");
            } else {
                linked = tryLink(pathOutNode, pathOutPortL, outFxNode, "input_FL") &&
                         tryLink(pathOutNode, pathOutPortR, outFxNode, "input_FR");
            }
            if (linked) {
                pathOutNode = outFxNode;
                pathOutPortL = "output_FL";
                pathOutPortR = "output_FR";
                pathOutMono = false;
            }
        }

        if (hasOutCreative) {
            bool linked = false;
            if (pathOutMono) {
                linked = tryLink(pathOutNode, "output", outCreativeNode, "input_FL") &&
                         tryLink(pathOutNode, "output", outCreativeNode, "input_FR");
            } else {
                linked = tryLink(pathOutNode, pathOutPortL, outCreativeNode, "input_FL") &&
                         tryLink(pathOutNode, pathOutPortR, outCreativeNode, "input_FR");
            }
            if (linked) {
                pathOutNode = outCreativeNode;
                pathOutPortL = "output_FL";
                pathOutPortR = "output_FR";
                pathOutMono = false;
            }
        }

        if (hasOutDuck) {
            bool linked = false;
            if (pathOutMono) {
                linked = tryLink(pathOutNode, "output", outDuckNode, "input_FL") &&
                         tryLink(pathOutNode, "output", outDuckNode, "input_FR");
            } else {
                linked = tryLink(pathOutNode, pathOutPortL, outDuckNode, "input_FL") &&
                         tryLink(pathOutNode, pathOutPortR, outDuckNode, "input_FR");
            }
            if (linked) {
                pathOutNode = outDuckNode;
                pathOutPortL = "output_FL";
                pathOutPortR = "output_FR";
                pathOutMono = false;
                wireDuckingSidechain(channelId, outDuckNode);
            }
        }

        if (hasOutDyn) {
            bool linked = false;
            if (pathOutMono) {
                linked = tryLink(pathOutNode, "output", outDynNode, "input_FL") &&
                         tryLink(pathOutNode, "output", outDynNode, "input_FR");
            } else {
                linked = tryLink(pathOutNode, pathOutPortL, outDynNode, "input_FL") &&
                         tryLink(pathOutNode, pathOutPortR, outDynNode, "input_FR");
            }
            if (linked) {
                pathOutNode = outDynNode;
                pathOutPortL = "output_FL";
                pathOutPortR = "output_FR";
                pathOutMono = false;
            }
        }

        if (hasOutLufs) {
            bool linked = false;
            if (pathOutMono) {
                linked = tryLink(pathOutNode, "output", outLufsNode, "input_FL") &&
                         tryLink(pathOutNode, "output", outLufsNode, "input_FR");
            } else {
                linked = tryLink(pathOutNode, pathOutPortL, outLufsNode, "input_FL") &&
                         tryLink(pathOutNode, pathOutPortR, outLufsNode, "input_FR");
            }
            if (linked) {
                pathOutNode = outLufsNode;
                pathOutPortL = "output_FL";
                pathOutPortR = "output_FR";
                pathOutMono = false;
            }
        }

        fxOutNode = pathOutNode;
        fxOutPortL = pathOutPortL;
        fxOutPortR = pathOutPortR;
        fxOutMono = pathOutMono;
        it->second.fxTailNode = fxOutNode;
        it->second.fxTailPortL = fxOutPortL;
        it->second.fxTailPortR = fxOutPortR;
        it->second.fxTailMono = fxOutMono;
        fxWired = anchored &&
                  wireChannelMixPath(channelId, Mix::Stream, pathOutNode,
                                     pathOutPortL, pathOutPortR, pathOutMono,
                                     error);
        it->second.fxChainWired = fxWired;
    }

    wireChannelMicSource(channelId);

    const Channel *c = channel(channelId);
    bool monitor = false;
    if (c && (c->monitorFx || c->lufsLimiter) && fxWired) {
        monitor = wireChannelMixPath(channelId, Mix::Monitor, fxOutNode, fxOutPortL,
                                     fxOutPortR, fxOutMono, error);
    }
    if (!monitor)
        monitor = wireChannelMixPath(channelId, Mix::Monitor, sink, "monitor_FL",
                                     "monitor_FR", false, error);
    const bool stream =
        fxWired || wireChannelMixPath(channelId, Mix::Stream, sink, "monitor_FL",
                                      "monitor_FR", false, error);
    if (monitor && stream) {
        error.clear();
        return true;
    }
    if (error.empty())
        error = "channel " + channelId + ": " +
                (monitor ? "stream" : "monitor") + " path not wired";
    return false;
}

bool MixerGraph::wireAllChannelFx(std::string &error) {
    error.clear();
    std::string last;
    for (const auto &c : channels_) {
        std::string err;
        if (!wireChannelFx(c.id, err)) last = err;
    }
    if (!last.empty()) {
        error = last;
        return false;
    }
    return true;
}

bool MixerGraph::setMicEffects(const ChannelFxSettings &settings) {
    return setMasterEffects(kPrimaryMasterId, settings);
}

bool MixerGraph::setMicDynamics(const DynamicsSettings &settings) {
    return setMasterDynamics(kPrimaryMasterId, settings);
}

bool MixerGraph::setMasterEffects(const std::string &id,
                                  const ChannelFxSettings &settings) {
    if (auto *fx = masterEffects(id)) {
        fx->setSettings(settings);
        return true;
    }
    return false;
}

bool MixerGraph::setMasterDynamics(const std::string &id,
                                   const DynamicsSettings &settings) {
    if (auto *dyn = masterDynamics(id)) {
        dyn->setSettings(settings);
        return true;
    }
    return false;
}

bool MixerGraph::setMicMonitorFx(bool on) {
    return setMasterMicMonitorFx(kPrimaryMasterId, on);
}

bool MixerGraph::setMasterMicMonitorFx(const std::string &id, bool on) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) return false;
    if (bus->micMonitorFx == on) return false;
    bus->micMonitorFx = on;
    return true;
}

bool MixerGraph::setMasterSoftwareMonitor(const std::string &id, bool on) {
    if (isPrimaryMaster(id)) return setSoftwareMonitor(on);
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) return false;
    const bool changed = bus->softwareMonitor != on;
    bus->softwareMonitor = on;
    eng_.setPathMuted(masterMonitorPath(id), !on);
    if (!on) {
        disconnectMasterMonitorLinks(id);
    } else {
        std::string err;
        rewireMasterSoftwareMonitor(id, err);
    }
    return changed;
}

bool MixerGraph::masterSoftwareMonitor(const std::string &id) const {
    if (isPrimaryMaster(id)) return softwareMonitor_;
    const MasterBusRuntime *bus = masterBus(id);
    return bus ? bus->softwareMonitor : false;
}

bool MixerGraph::rewireMasterSoftwareMonitor(const std::string &id, std::string &error) {
    error.clear();
    if (isPrimaryMaster(id)) return rewireMicMonitor(error);

    MasterBusRuntime *bus = masterBus(id);
    if (!bus) return true;

    const bool chainReady = isMasterMidi(*bus) ? bus->chain.synthReady
                                               : !bus->captureNode.empty();
    if (!chainReady) return true;

    if (!bus->softwareMonitor) {
        disconnectMasterMonitorLinks(id);
        eng_.setPathMuted(masterMonitorPath(id), true);
        return true;
    }

    std::string dryNode, dryPort, fxNode, fxPort;
    if (!resolveMasterChain(*bus, dryNode, dryPort, fxNode, fxPort)) return true;

    const std::string monHandle = masterMonitorPath(id);
    const std::string monIn = monHandle + "-in";
    if (bus->micStereo) {
        eng_.forgetLinksTo(monIn, "input_FL");
        eng_.forgetLinksTo(monIn, "input_FR");
    } else {
        eng_.forgetLinksTo(monIn, "input_MONO");
    }
    // Proxy destruction is asynchronous. If we recreate immediately, the
    // registry can still report the old edge and linkPorts() treats that stale
    // edge as success; it disappears moments later and monitoring stays off
    // until the user toggles it. Wait for the old monitor edge to actually
    // leave before creating its replacement.
    for (int attempt = 0; attempt < 40; ++attempt) {
        const bool oldLinkPresent =
            bus->micStereo
                ? (eng_.hasManualLinkTo(monIn, "input_FL") ||
                   eng_.hasManualLinkTo(monIn, "input_FR"))
                : eng_.hasManualLinkTo(monIn, "input_MONO");
        if (!oldLinkPresent) break;
        eng_.sync();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    const std::string &node = bus->micMonitorFx ? fxNode : dryNode;
    const std::string &port = bus->micMonitorFx ? fxPort : dryPort;
    const std::string capPort = eng_.pathCapturePort(monHandle);
    if (!eng_.waitForPort(monIn, capPort, false, 3000)) {
        error = "input device monitor path not ready: " + id;
        return false;
    }

    if (bus->micStereo) {
        return eng_.linkPorts(node, port, monIn, "input_FL", error) &&
               eng_.linkPorts(node, port, monIn, "input_FR", error);
    }
    return eng_.linkPorts(node, port, monIn, "input_MONO", error);
}

bool MixerGraph::rewireMicMonitor(std::string &error) {
    error.clear();
    MasterBusRuntime *primary = primaryMaster();
    if (!primary || primary->captureNode.empty()) return true;
    if (!softwareMonitor_) {
        disconnectMasterMonitorLinks(kPrimaryMasterId);
        eng_.setPathMuted(pathName("mic", Mix::Monitor), true);
        return true;
    }

    std::string dryNode, dryPort, fxNode, fxPort;
    if (!resolveMasterChain(*primary, dryNode, dryPort, fxNode, fxPort)) return true;

    primary->chain.dryTailNode = dryNode;
    primary->chain.dryTailPort = dryPort;
    primary->chain.fxTailNode = fxNode;
    primary->chain.fxTailPort = fxPort;

    const std::string monIn = pathName("mic", Mix::Monitor) + "-in";
    if (primary->micStereo) {
        eng_.forgetLinksTo(monIn, "input_FL");
        eng_.forgetLinksTo(monIn, "input_FR");
    } else {
        eng_.forgetLinksTo(monIn, "input_MONO");
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        const bool oldLinkPresent =
            primary->micStereo
                ? (eng_.hasManualLinkTo(monIn, "input_FL") ||
                   eng_.hasManualLinkTo(monIn, "input_FR"))
                : eng_.hasManualLinkTo(monIn, "input_MONO");
        if (!oldLinkPresent) break;
        eng_.sync();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    const std::string &node = primary->micMonitorFx ? fxNode : dryNode;
    const std::string &port = primary->micMonitorFx ? fxPort : dryPort;
    const std::string capPort = eng_.pathCapturePort(pathName("mic", Mix::Monitor));
    if (!eng_.waitForPort(monIn, capPort, false, 3000)) {
        error = "mic monitor path not ready";
        return false;
    }

    if (primary->micStereo) {
        return eng_.linkPorts(node, port, monIn, "input_FL", error) &&
               eng_.linkPorts(node, port, monIn, "input_FR", error);
    }
    return eng_.linkPorts(node, port, monIn, "input_MONO", error);
}

bool MixerGraph::setChannelEffects(const std::string &channelId, FxStage stage,
                                   const ChannelFxSettings &settings) {
    if (auto *fx = channelEffects(channelId, stage)) {
        fx->setSettings(settings);
        return true;
    }
    return false;
}

bool MixerGraph::setChannelDynamics(const std::string &channelId, FxStage stage,
                                    const DynamicsSettings &settings) {
    if (auto *dyn = channelDynamics(channelId, stage)) {
        dyn->setSettings(settings);
        return true;
    }
    return false;
}

bool MixerGraph::setChannelDucking(const std::string &channelId,
                                   const DuckingSettings &settings) {
    auto it = chChains_.find(channelId);
    if (it == chChains_.end() || !it->second.outputDuckReady || !it->second.outputDuck)
        return false;
    it->second.outputDuck->setSettings(settings);

    Channel *c = channel(channelId);
    if (!c) return true;
    // A disabled duck filter passes audio straight through, so its sidechain
    // links cost nothing. Leaving them -- and the remembered source list --
    // alone means the effects switch and the ducking checkbox never churn the
    // graph, and re-enabling finds the wiring already in place.
    if (!settings.enabled) return true;

    std::vector<DuckingSourceRef> oldSources = c->duckingSources;
    c->duckingSources = settings.sources;
    if (c->duckingSources.size() > static_cast<size_t>(kMaxDuckingSources))
        c->duckingSources.resize(kMaxDuckingSources);
    // Adding, removing or swapping a source only moves sidechain links. Doing
    // it here keeps the caller from having to schedule a whole-graph rewire,
    // which would cut every channel's audio for the length of the rebuild.
    if (oldSources != c->duckingSources) relinkDuckingSidechains(channelId, oldSources);
    return true;
}

bool MixerGraph::setChannelLufsLimiter(const std::string &channelId,
                                       const LufsLimiterSettings &settings) {
    Channel *c = channel(channelId);
    if (c) c->lufsLimiter = settings.enabled;
    auto it = chChains_.find(channelId);
    if (it == chChains_.end() || !it->second.outputLufsReady || !it->second.outputLufs)
        return c != nullptr;
    it->second.outputLufs->setSettings(settings);
    return true;
}

bool MixerGraph::setChannelCreativeFx(const std::string &channelId, FxStage stage,
                                      const CreativeFxSettings &settings) {
    auto it = chChains_.find(channelId);
    if (it == chChains_.end()) return false;
    if (stage == FxStage::Input) {
        if (!it->second.inputCreativeReady || !it->second.inputCreative) return false;
        it->second.inputCreative->setSettings(settings);
        return true;
    }
    if (!it->second.outputCreativeReady || !it->second.outputCreative) return false;
    it->second.outputCreative->setSettings(settings);
    return true;
}

bool MixerGraph::setMasterCreativeFx(const std::string &id,
                                     const CreativeFxSettings &settings) {
    if (auto *fx = masterCreativeFx(id)) {
        fx->setSettings(settings);
        return true;
    }
    return false;
}

void MixerGraph::wireDuckingSidechain(const std::string &channelId,
                                      const std::string &duckNode) {
    const Channel *c = channel(channelId);
    if (!c) return;

    for (size_t i = 0; i < c->duckingSources.size() && i < kMaxDuckingSources; ++i)
        linkDuckingSidechainSource(duckNode, i, c->duckingSources[i]);
}

void MixerGraph::linkDuckingSidechainSource(const std::string &duckNode, size_t index,
                                            const DuckingSourceRef &src) {
    if (index >= static_cast<size_t>(kMaxDuckingSources)) return;
    const std::string port = "sidechain_" + std::to_string(index);
    if (!eng_.hasPort(duckNode, port, false)) return;

    std::string err;
    switch (src.kind) {
    case DuckingSourceKind::ChannelMic: {
        if (src.channelId.empty()) break;
        const Channel *srcCh = channel(src.channelId);
        if (!srcCh || !srcCh->micSource) break;
        const std::string dynNode = dynNodeName(src.channelId, FxStage::Input);
        if (eng_.hasPort(dynNode, "output_FL", true)) {
            eng_.linkPorts(dynNode, "output_FL", duckNode, port, err);
            break;
        }
        const std::string fxNode = fxNodeName(src.channelId, FxStage::Input);
        if (eng_.hasPort(fxNode, "output_FL", true))
            eng_.linkPorts(fxNode, "output_FL", duckNode, port, err);
        break;
    }
    case DuckingSourceKind::ChannelAudio: {
        if (src.channelId.empty()) break;
        const std::string sink = sinkName(src.channelId);
        auto chainIt = chChains_.find(src.channelId);
        const std::string mixNode = mixNodeName(src.channelId);
        if (chainIt != chChains_.end() && chainIt->second.mixerReady &&
            eng_.hasPort(mixNode, "output_FL", true)) {
            eng_.linkPorts(mixNode, "output_FL", duckNode, port, err);
        } else if (eng_.hasPort(sink, "monitor_FL", true)) {
            eng_.linkPorts(sink, "monitor_FL", duckNode, port, err);
        }
        break;
    }
    case DuckingSourceKind::MasterMic:
    default: {
        std::string masterId = src.channelId.empty() ? kPrimaryMasterId : src.channelId;
        const MasterBusRuntime *bus = masterBus(masterId);
        if (!bus) {
            masterId = kPrimaryMasterId;
            bus = primaryMaster();
        }
        if (!bus) break;
        const MasterBusChain &mc = bus->chain;
        const std::string dyn = masterDynNode(masterId);
        const std::string fx = masterFxNode(masterId);
        const std::string gain = masterGainNode(masterId);
        if (mc.dynReady && eng_.hasPort(dyn, "output", true)) {
            eng_.linkPorts(dyn, "output", duckNode, port, err);
        } else if (mc.fxReady && eng_.hasPort(fx, "output", true)) {
            eng_.linkPorts(fx, "output", duckNode, port, err);
        } else if (mc.gainReady && eng_.hasPort(gain, "output", true)) {
            eng_.linkPorts(gain, "output", duckNode, port, err);
        }
        break;
    }
    }
}

// Only the ports whose source actually changed are unlinked and re-linked. The
// duck node's program in/out links are left alone -- it sits in the output path
// whether or not any sidechain is attached -- so app audio never stops.
// Dropping a sidechain link just makes that port's DSP buffer null, which the
// processor reads as silence and releases from through its normal envelope.
void MixerGraph::relinkDuckingSidechains(const std::string &channelId,
                                         const std::vector<DuckingSourceRef> &oldSources) {
    auto it = chChains_.find(channelId);
    if (it == chChains_.end() || !it->second.outputDuckReady) return;
    const Channel *c = channel(channelId);
    if (!c) return;

    const std::string duckNode = duckNodeName(channelId);
    const size_t n = std::min<size_t>(
        kMaxDuckingSources, std::max(oldSources.size(), c->duckingSources.size()));
    for (size_t i = 0; i < n; ++i) {
        const bool hadOld = i < oldSources.size();
        const bool hasNew = i < c->duckingSources.size();
        if (hadOld && hasNew && oldSources[i] == c->duckingSources[i]) continue;

        const std::string port = "sidechain_" + std::to_string(i);
        if (!eng_.hasPort(duckNode, port, false)) continue;
        if (hadOld) eng_.forgetLinksTo(duckNode, port);
        if (hasNew) linkDuckingSidechainSource(duckNode, i, c->duckingSources[i]);
    }
}

void MixerGraph::rewireDuckingSidechainsForMic(const std::string &micChannelId) {
    for (const auto &c : channels_) {
        for (const auto &src : c.duckingSources) {
            if (src.kind != DuckingSourceKind::ChannelMic || src.channelId != micChannelId)
                continue;
            auto it = chChains_.find(c.id);
            if (it == chChains_.end() || !it->second.outputDuckReady) continue;
            wireDuckingSidechain(c.id, duckNodeName(c.id));
        }
    }
}

void MixerGraph::setChannelNoiseSuppression(const std::string &channelId, FxStage stage,
                                            bool on, float intensity) {
    if (auto *nc = channelNoiseFilter(channelId, stage)) {
        nc->setEnabled(on);
        nc->setIntensity(intensity);
    }
}

bool MixerGraph::ensureChannelNoiseFilter(const std::string &channelId, FxStage stage) {
    if (channelNoiseFilter(channelId, stage)) return false;
    auto it = chChains_.find(channelId);
    if (it == chChains_.end()) return false;
    Channel *c = channel(channelId);
    if (!c) return false;
    ChannelChain &chain = it->second;
    std::string err;
    if (stage == FxStage::Input) {
        chain.inputNc = std::make_unique<NoiseFilter>();
        if (!chain.inputNc->start(ncNodeName(channelId, FxStage::Input),
                                  disp(c->name + " Input NC"), err,
                                  false, engine_)) {
            chain.inputNc.reset();
            return false;
        }
        chain.inputNcReady = true;
        chain.inputNc->setEnabled(false);
        return true;
    }
    chain.outputNc = std::make_unique<NoiseFilter>();
    if (!chain.outputNc->start(ncNodeName(channelId, FxStage::Output),
                               disp(c->name + " Output NC"), err,
                               false, engine_)) {
        chain.outputNc.reset();
        return false;
    }
    chain.outputNcReady = true;
    chain.outputNc->setEnabled(false);
    return true;
}

bool MixerGraph::setChannelMicSend(const std::string &channelId, float level) {
    Channel *c = channel(channelId);
    if (!c) return false;
    c->micSend = level;
    return applyChannelMicGain(channelId);
}

bool MixerGraph::setChannelMicMuted(const std::string &channelId, bool muted) {
    Channel *c = channel(channelId);
    if (!c) return false;
    c->micMuted = muted;
    return applyChannelMicGain(channelId);
}

bool MixerGraph::channelMicMuted(const std::string &channelId) const {
    for (const auto &c : channels_)
        if (c.id == channelId) return c.micMuted;
    return false;
}

bool MixerGraph::applyChannelMicGain(const std::string &channelId) {
    const Channel *c = channel(channelId);
    if (!c) return false;
    auto it = chChains_.find(channelId);
    if (it == chChains_.end() || !it->second.micSendReady || !it->second.micSend)
        return false;
    it->second.micSend->setGain(c->micMuted ? 0.0f : c->micSend);
    return true;
}

float MixerGraph::channelMicSend(const std::string &channelId) const {
    for (const auto &c : channels_)
        if (c.id == channelId) return c.micSend;
    return 0.0f;
}

bool MixerGraph::setChannelMonitorFx(const std::string &channelId, bool on) {
    Channel *c = channel(channelId);
    if (!c) return false;
    c->monitorFx = on;
    return true;
}

bool MixerGraph::rewireChannelMonitor(const std::string &channelId, std::string &error) {
    error.clear();
    const Channel *c = channel(channelId);
    if (!c) {
        error = "no such channel: " + channelId;
        return false;
    }

    const bool wantFx = c->monitorFx || c->lufsLimiter;
    const std::string monIn = pathName(channelId, Mix::Monitor) + "-in";
    eng_.forgetLinksTo(monIn, "input_FL");
    eng_.forgetLinksTo(monIn, "input_FR");
    eng_.sync();

    if (wantFx) {
        auto it = chChains_.find(channelId);
        if (it != chChains_.end() && it->second.fxChainWired) {
            const ChannelChain &chain = it->second;
            if (wireChannelMixPath(channelId, Mix::Monitor, chain.fxTailNode,
                                   chain.fxTailPortL, chain.fxTailPortR,
                                   chain.fxTailMono, error))
                return true;
        }
    }

    const std::string sink = sinkName(channelId);
    return wireChannelMixPath(channelId, Mix::Monitor, sink, "monitor_FL",
                              "monitor_FR", false, error);
}

bool MixerGraph::channelMonitorFx(const std::string &channelId) const {
    for (const auto &c : channels_)
        if (c.id == channelId) return c.monitorFx;
    return false;
}

bool MixerGraph::setChannelMicSource(const std::string &channelId, bool on) {
    Channel *c = channel(channelId);
    if (!c) return false;
    c->micSource = on;
    return ensureChannelMicSource(channelId);
}

bool MixerGraph::ensureChannelMicSource(const std::string &channelId) {
    Channel *c = channel(channelId);
    if (!c) return false;
    auto it = chChains_.find(channelId);
    if (it == chChains_.end()) return false;
    ChannelChain &chain = it->second;

    if (!c->micSource) {
        const bool had = chain.inputNcReady || chain.inputFxReady ||
                         chain.inputCreativeReady || chain.inputDynReady;
        eng_.forgetLinksForNode(micSourceNode(channelId));
        eng_.forgetLinksForNode(ncNodeName(channelId, FxStage::Input));
        eng_.forgetLinksForNode(fxNodeName(channelId, FxStage::Input));
        eng_.forgetLinksForNode(creativeNodeName(channelId, FxStage::Input));
        eng_.forgetLinksForNode(dynNodeName(channelId, FxStage::Input));
        eng_.forgetLinksForNode(micSendNodeName(channelId));
        const std::string monIn = channelMicMonitorPath(channelId) + "-in";
        eng_.forgetLinksTo(monIn, "input_FL");
        eng_.forgetLinksTo(monIn, "input_FR");
        eng_.removeNode(micSourceNode(channelId));
        eng_.removePath(channelMicMonitorPath(channelId));
        chain.inputNc.reset();
        chain.inputFx.reset();
        chain.inputCreative.reset();
        chain.inputDyn.reset();
        chain.inputNcReady = chain.inputFxReady = chain.inputCreativeReady =
            chain.inputDynReady = false;
        return had;
    }

    bool changed = false;
    if (!chain.inputFxReady) {
        std::string err;
        chain.inputNc = std::make_unique<NoiseFilter>();
        if (chain.inputNc->start(ncNodeName(channelId, FxStage::Input),
                                 disp(c->name + " Mic NC"), err,
                                 false, engine_)) {
            chain.inputNcReady = true;
            chain.inputNc->setEnabled(false);
        } else {
            chain.inputNc.reset();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        chain.inputFx = std::make_unique<EffectsFilter>();
        if (chain.inputFx->start(fxNodeName(channelId, FxStage::Input),
                                 disp(c->name + " Mic EQ"), 2, err))
            chain.inputFxReady = true;
        else
            chain.inputFx.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        chain.inputCreative = std::make_unique<CreativeFxFilter>();
        if (chain.inputCreative->start(creativeNodeName(channelId, FxStage::Input),
                                       disp(c->name + " Mic Creative FX"), 2, err))
            chain.inputCreativeReady = true;
        else
            chain.inputCreative.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        chain.inputDyn = std::make_unique<DynamicsFilter>();
        if (chain.inputDyn->start(dynNodeName(channelId, FxStage::Input),
                                  disp(c->name + " Mic Dynamics"), 2, err))
            chain.inputDynReady = true;
        else
            chain.inputDyn.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        if (chain.inputFxReady || chain.inputDynReady)
            eng_.addVirtualSource(micSourceNode(channelId),
                                  disp(c->name + " Microphone"), 2, err);
        changed = true;
    }

    {
        PwEngine::PathSpec spec;
        spec.handle = channelMicMonitorPath(channelId);
        spec.target = kMonitorMix;
        spec.description = disp(c->name + " Mic → Monitor");
        spec.inChannels = 2;
        spec.outChannels = 2;
        spec.remix = false;
        spec.source.clear();
        spec.sourceIsSink = false;
        std::string pathErr;
        if (eng_.repairPath(spec, pathErr)) changed = true;
        eng_.setPathMuted(channelMicMonitorPath(channelId), !c->micMonitor);
    }
    return changed;
}

bool MixerGraph::rewireChannelMicSource(const std::string &channelId, std::string &error) {
    error.clear();
    const Channel *c = channel(channelId);
    if (!c || !c->micSource) return true;

    eng_.forgetLinksForNode(micSendNodeName(channelId));
    eng_.forgetLinksForNode(ncNodeName(channelId, FxStage::Input));
    eng_.forgetLinksForNode(fxNodeName(channelId, FxStage::Input));
    eng_.forgetLinksForNode(creativeNodeName(channelId, FxStage::Input));
    eng_.forgetLinksForNode(dynNodeName(channelId, FxStage::Input));
    eng_.forgetLinksForNode(micSourceNode(channelId));
    const std::string monIn = channelMicMonitorPath(channelId) + "-in";
    eng_.forgetLinksTo(monIn, "input_FL");
    eng_.forgetLinksTo(monIn, "input_FR");
    eng_.sync();

    wireChannelMicSource(channelId);
    rewireDuckingSidechainsForMic(channelId);
    return true;
}

bool MixerGraph::channelMicSource(const std::string &channelId) const {
    for (const auto &c : channels_)
        if (c.id == channelId) return c.micSource;
    return false;
}

bool MixerGraph::setChannelMicMonitor(const std::string &channelId, bool on) {
    Channel *c = channel(channelId);
    if (!c || !c->micSource) return false;
    const bool changed = c->micMonitor != on;
    c->micMonitor = on;
    eng_.setPathMuted(channelMicMonitorPath(channelId), !on);
    if (!on) {
        disconnectChannelMicMonitorLinks(channelId);
    } else {
        // Re-feed the monitor path; links were dropped while it was off.
        wireChannelMicSource(channelId);
    }
    return changed;
}

void MixerGraph::disconnectChannelMicMonitorLinks(const std::string &channelId) {
    const std::string monIn = channelMicMonitorPath(channelId) + "-in";
    eng_.forgetLinksTo(monIn, "input_FL");
    eng_.forgetLinksTo(monIn, "input_FR");
}

bool MixerGraph::channelMicMonitor(const std::string &channelId) const {
    for (const auto &c : channels_)
        if (c.id == channelId) return c.micMonitor;
    return false;
}

bool MixerGraph::setChannelMasterMics(const std::string &channelId,
                                      const std::vector<std::string> &masterIds) {
    Channel *c = channel(channelId);
    if (!c) return false;

    std::vector<std::string> keep;
    for (const std::string &id : masterIds) {
        if (!masterBus(id)) continue;
        if (std::find(keep.begin(), keep.end(), id) != keep.end()) continue;
        keep.push_back(id);
    }
    if (keep.empty()) keep.push_back(kPrimaryMasterId);
    if (keep == c->masterMicIds) return false;
    c->masterMicIds = std::move(keep);
    return true;
}

std::vector<std::string> MixerGraph::channelMasterMics(
    const std::string &channelId) const {
    for (const auto &c : channels_)
        if (c.id == channelId) return c.masterMicIds;
    return {kPrimaryMasterId};
}

bool MixerGraph::setChannelMasterMic(const std::string &channelId,
                                     const std::string &masterId) {
    return setChannelMasterMics(channelId, {masterId});
}

bool MixerGraph::setChannelMicUseDeviceFx(const std::string &channelId, bool on) {
    Channel *c = channel(channelId);
    if (!c || c->micUseDeviceFx == on) return false;
    c->micUseDeviceFx = on;
    return true;
}

bool MixerGraph::channelMicUseDeviceFx(const std::string &channelId) const {
    for (const auto &c : channels_)
        if (c.id == channelId) return c.micUseDeviceFx;
    return false;
}

const std::string &MixerGraph::channelMasterMic(const std::string &channelId) const {
    static const std::string kDefault = kPrimaryMasterId;
    for (const auto &c : channels_)
        if (c.id == channelId) return c.masterMicIds.empty() ? kDefault
                                                             : c.masterMicIds.front();
    return kDefault;
}

bool MixerGraph::setVolume(const std::string &channelId, Mix mix, float volume) {
    Channel *c = channel(channelId);
    if (!c) return false;
    (mix == Mix::Stream ? c->streamVolume : c->monitorVolume) = volume;
    return eng_.setPathVolume(pathName(channelId, mix), volume);
}

bool MixerGraph::setMuted(const std::string &channelId, Mix mix, bool muted) {
    Channel *c = channel(channelId);
    if (!c) return false;
    (mix == Mix::Stream ? c->streamMuted : c->monitorMuted) = muted;
    return eng_.setPathMuted(pathName(channelId, mix), muted);
}

bool MixerGraph::setMasterVolume(const std::string &id, Mix mix, float volume) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) return false;
    if (mix == Mix::Stream) bus->streamVolume = volume;
    else bus->monitorVolume = volume;
    return eng_.setPathVolume(pathName(id, mix), volume);
}

bool MixerGraph::setMasterMuted(const std::string &id, Mix mix, bool muted) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) return false;
    if (mix == Mix::Monitor) {
        bus->monitorMuted = muted;
        // Fader mute and software-monitor share this loopback. Turning the
        // fader unmute must not defeat software-monitor=off (that was the
        // "ghost monitoring" bug: applyProfile wrote monitorMuted=false and
        // reopened every master into the Monitor mix).
        const bool sw =
            isPrimaryMaster(id) ? softwareMonitor_ : bus->softwareMonitor;
        return eng_.setPathMuted(pathName(id, mix), !sw || muted);
    }
    bus->streamMuted = muted;
    return eng_.setPathMuted(pathName(id, mix), muted);
}

void MixerGraph::applyMasterPathLevels(const std::string &id) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) return;
    const std::string streamHandle = pathName(id, Mix::Stream);
    const std::string monHandle = pathName(id, Mix::Monitor);
    const bool sw = isPrimaryMaster(id) ? softwareMonitor_ : bus->softwareMonitor;
    // Twice: module-loopback often overwrites the first Props write while the
    // capture node is still finishing format negotiation.
    for (int i = 0; i < 2; ++i) {
        eng_.setPathVolume(streamHandle, bus->streamVolume);
        eng_.setPathMuted(streamHandle, bus->streamMuted);
        eng_.setPathVolume(monHandle, bus->monitorVolume);
        eng_.setPathMuted(monHandle, !sw || bus->monitorMuted);
        eng_.sync();
    }
}

// One write per mix, no core sync -- the shape applyMonitorOutputGain() uses,
// not applyMasterPathLevels(). The master version repeats itself and syncs on
// each pass, which is affordable for two or three buses and emphatically not
// for every channel: each write binds a node proxy and syncs the core inside
// applyVolumeLocked(), and doing that a few hundred times in half a second
// starves the thread loop badly enough to drop audio.
void MixerGraph::applyChannelPathLevels(const std::string &channelId) {
    const Channel *c = channel(channelId);
    if (!c) return;
    eng_.setPathLevel(pathName(channelId, Mix::Stream), c->streamVolume,
                      c->streamMuted);
    eng_.setPathLevel(pathName(channelId, Mix::Monitor), c->monitorVolume,
                      c->monitorMuted);
}

void MixerGraph::applyAllChannelPathLevels() {
    for (const auto &c : channels_) applyChannelPathLevels(c.id);
}

void MixerGraph::disconnectMasterMonitorLinks(const std::string &id) {
    const std::string monIn =
        (isPrimaryMaster(id) ? pathName("mic", Mix::Monitor)
                             : masterMonitorPath(id)) +
        "-in";
    eng_.forgetLinksTo(monIn, "input_FL");
    eng_.forgetLinksTo(monIn, "input_FR");
    eng_.forgetLinksTo(monIn, "input_MONO");
}

bool MixerGraph::rebuildMasterMidiChain(const std::string &id, std::string &error) {
    error.clear();
    MasterBusRuntime *bus = masterBus(id);
    if (!bus || !isMasterMidi(*bus)) return true;

    const std::string soundfont = bus->soundfontPath;
    destroyMasterChain(*bus);
    eng_.sync();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    bus->wantSoftwareGain = true;
    if (!createMasterChainNodes(*bus, false, error)) return false;
    if (!soundfont.empty() && bus->chain.synth)
        bus->chain.synth->setSoundfontPath(soundfont);
    if (!createMasterVirtualSource(*bus, error)) return false;
    eng_.sync();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    return ensureMidiStreamPath(id, error);
}

bool MixerGraph::ensureMidiStreamPath(const std::string &id, std::string &error) {
    error.clear();
    MasterBusRuntime *bus = masterBus(id);
    if (!bus || !isMasterMidi(*bus)) return true;
    const std::string handle = pathName(id, Mix::Stream);
    // The loopback's channel count is fixed when the module loads, so a bus
    // that changed micStereo needs the path rebuilt at the new width. Only
    // tear it down when the width actually disagrees -- rebuilding a healthy
    // path on every profile load is what churns the graph on startup.
    if (eng_.pathExists(handle)) {
        const std::string want = bus->micStereo ? "input_FL" : "input_MONO";
        if (eng_.pathCapturePort(handle) != want) eng_.removePath(handle);
    }
    return syncMasterStreamPath(id, error);
}

bool MixerGraph::syncMasterStreamPath(const std::string &id, std::string &error) {
    error.clear();
    if (isPrimaryMaster(id)) return true;
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) {
        error = "no such input device: " + id;
        return false;
    }

    const std::string handle = pathName(id, Mix::Stream);
    if (eng_.pathExists(handle) && eng_.hasPathNodes(handle)) {
        eng_.setPathVolume(handle, bus->streamVolume);
        eng_.setPathMuted(handle, bus->streamMuted);
        return true;
    }

    if (eng_.pathExists(handle)) eng_.removePath(handle);

    PwEngine::PathSpec spec;
    spec.handle = handle;
    spec.target = kStreamMix;
    spec.description = disp(bus->name + " → Stream");
    spec.inChannels = bus->micStereo ? 2 : 1;
    spec.outChannels = 2;
    spec.remix = false;
    spec.source.clear();
    spec.sourceIsSink = false;
    spec.volume = bus->streamVolume;
    spec.muted = bus->streamMuted;
    if (!eng_.addPath(spec, error)) return false;
    eng_.sync();
    eng_.setPathVolume(handle, bus->streamVolume);
    eng_.setPathMuted(handle, bus->streamMuted);
    return true;
}

bool MixerGraph::setMicVolume(Mix mix, float volume) {
    return setMasterVolume(kPrimaryMasterId, mix, volume);
}

bool MixerGraph::setMicMuted(Mix mix, bool muted) {
    return setMasterMuted(kPrimaryMasterId, mix, muted);
}

bool MixerGraph::setSoundShareVolume(Mix mix, float volume) {
    return eng_.setPathVolume(pathName("sound-share", mix), volume);
}

bool MixerGraph::setSoundShareMuted(Mix mix, bool muted) {
    return eng_.setPathMuted(pathName("sound-share", mix), muted);
}

bool MixerGraph::buildFxChannelPaths(const std::string &id,
                                     const std::string &label, std::string &error) {
    for (Mix mix : {Mix::Monitor, Mix::Stream}) {
        PwEngine::PathSpec spec;
        spec.handle = pathName(id, mix);
        spec.target = (mix == Mix::Stream) ? kStreamMix : kMonitorMix;
        spec.description = label + (mix == Mix::Stream ? " → Stream" : " → Monitor");
        spec.inChannels = 2;
        spec.outChannels = 2;
        spec.remix = false;
        spec.source.clear();
        spec.sourceIsSink = false;
        // Unity only for a channel that does not exist yet (first build, before
        // the profile is loaded); a rebuild of a live channel keeps its fader.
        if (const Channel *c = channel(id)) {
            spec.volume = (mix == Mix::Stream) ? c->streamVolume : c->monitorVolume;
            spec.muted = (mix == Mix::Stream) ? c->streamMuted : c->monitorMuted;
        }
        if (!eng_.repairPath(spec, error)) return false;
    }
    return true;
}

bool MixerGraph::ensureChannelPaths(std::string &error) {
    error.clear();
    std::string last;
    for (const auto &c : channels_) {
        std::string err;
        if (!ensurePathsForChannel(c.id, err)) last = err;
    }
    error = last;
    return last.empty();
}

bool MixerGraph::ensureChannelFilters(std::string &error) {
    error.clear();
    for (const auto &c : channels_) {
        auto it = chChains_.find(c.id);
        if (it == chChains_.end()) {
            std::string startErr;
            if (!startChannelChain(c.id, c.name, startErr)) {
                error = startErr;
                continue;
            }
            it = chChains_.find(c.id);
            if (it == chChains_.end()) continue;
        }
        ChannelChain &chain = it->second;

        if (!chain.outputNcReady) {
            chain.outputNc = std::make_unique<NoiseFilter>();
            std::string ncErr;
            if (chain.outputNc->start(ncNodeName(c.id, FxStage::Output),
                                      disp(c.name + " Output NC"), ncErr,
                                      false, engine_)) {
                chain.outputNcReady = true;
                chain.outputNc->setEnabled(false);
            } else {
                chain.outputNc.reset();
            }
        }
        if (!chain.micSendReady) {
            chain.micSend = std::make_unique<GainFilter>();
            std::string sendErr;
            if (chain.micSend->start(micSendNodeName(c.id),
                                     disp(c.name + " Mic Send"), 1, sendErr)) {
                chain.micSendReady = true;
                chain.micSend->setGain(c.micSend);
            } else {
                chain.micSend.reset();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        if (!chain.outputFxReady) {
            chain.outputFx = std::make_unique<EffectsFilter>();
            std::string fxErr;
            if (chain.outputFx->start(fxNodeName(c.id, FxStage::Output),
                                      disp(c.name + " Output EQ"), 2, fxErr))
                chain.outputFxReady = true;
            else
                chain.outputFx.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        if (!chain.outputCreativeReady) {
            chain.outputCreative = std::make_unique<CreativeFxFilter>();
            std::string creativeErr;
            if (chain.outputCreative->start(creativeNodeName(c.id, FxStage::Output),
                                            disp(c.name + " Output Creative FX"), 2,
                                            creativeErr))
                chain.outputCreativeReady = true;
            else
                chain.outputCreative.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        if (!chain.outputDuckReady) {
            chain.outputDuck = std::make_unique<DuckingFilter>();
            std::string duckErr;
            if (chain.outputDuck->start(duckNodeName(c.id), disp(c.name + " Output Ducking"),
                                        duckErr))
                chain.outputDuckReady = true;
            else
                chain.outputDuck.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        if (!chain.outputDynReady) {
            chain.outputDyn = std::make_unique<DynamicsFilter>();
            std::string dynErr;
            if (chain.outputDyn->start(dynNodeName(c.id, FxStage::Output),
                                       disp(c.name + " Output Dynamics"), 2, dynErr))
                chain.outputDynReady = true;
            else
                chain.outputDyn.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        if (!chain.outputLufsReady) {
            chain.outputLufs = std::make_unique<LufsLimiterFilter>();
            std::string lufsErr;
            if (chain.outputLufs->start(lufsNodeName(c.id),
                                        disp(c.name + " Output LUFS Limiter"), lufsErr))
                chain.outputLufsReady = true;
            else
                chain.outputLufs.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        if (!chain.mixerReady) {
            chain.mixer = std::make_unique<ChannelInputMixer>();
            std::string mixErr;
            if (chain.mixer->start(mixNodeName(c.id), disp(c.name + " Input Mix"),
                                   mixErr))
                chain.mixerReady = true;
            else
                chain.mixer.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }
    eng_.sync();
    return true;
}

bool MixerGraph::setSoftwareMonitor(bool on) {
    softwareMonitor_ = on;
    eng_.setPathMuted(pathName("mic", Mix::Monitor), !on);
    if (!on) {
        disconnectMasterMonitorLinks(kPrimaryMasterId);
    } else {
        std::string err;
        rewireMicMonitor(err);
    }
    return true;
}

bool MixerGraph::setMicStereo(bool on, std::string &error) {
    return setMasterMicStereo(kPrimaryMasterId, on, error);
}

bool MixerGraph::setMasterMicStereo(const std::string &id, bool on, std::string &error) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) {
        error = "no such input device: " + id;
        return false;
    }
    if (on == bus->micStereo) return true;
    bus->micStereo = on;

    if (isPrimaryMaster(id)) {
        eng_.removePath(pathName("mic", Mix::Stream));
        eng_.removePath(pathName("mic", Mix::Monitor));
        if (!buildPrimaryMasterPaths(error)) return false;
    } else {
        const std::string streamHandle = pathName(id, Mix::Stream);
        const std::string monHandle = masterMonitorPath(id);
        if (eng_.pathExists(streamHandle)) eng_.removePath(streamHandle);
        if (eng_.pathExists(monHandle)) eng_.removePath(monHandle);
    }

    if (!wireMasterPaths(id, error)) return false;

    if (isPrimaryMaster(id) && !softwareMonitor_)
        eng_.setPathMuted(pathName("mic", Mix::Monitor), true);
    return true;
}

bool MixerGraph::addMasterBus(const std::string &id, const std::string &name,
                              const std::string &busType, std::string &error) {
    if (isPrimaryMaster(id)) {
        error = "cannot add primary input device";
        return false;
    }
    if (masterBuses_.size() >= kMaxMasterBuses) {
        error = "maximum input devices reached";
        return false;
    }
    if (masterBus(id)) {
        error = "input device already exists: " + id;
        return false;
    }
    if (!built_) {
        error = "graph not built";
        return false;
    }

    MasterBusRuntime bus;
    bus.id = id;
    bus.name = name.empty() ? id : name;
    bus.busType = busType.empty() ? "capture" : busType;
    bus.wantSoftwareGain = true;
    const bool midi = isMidiBusType(bus.busType);
    if (!createMasterChainNodes(bus, !midi, error)) return false;
    if (!createMasterVirtualSource(bus, error)) return false;
    eng_.sync();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    masterBuses_.push_back(std::move(bus));
    if (midi) {
        // Create the stream loopback early so later wiring only has to link
        // the synth chain once filter ports appear.
        std::string pathErr;
        ensureMidiStreamPath(id, pathErr);
        // MIDI filters register asynchronously. Keep device addition quick and
        // let the MIDI timer wire this bus without touching capture devices.
        return true;
    }
    if (!wireMasterPaths(id, error)) {
        destroyMasterChain(masterBuses_.back());
        masterBuses_.pop_back();
        return false;
    }
    return true;
}

bool MixerGraph::removeMasterBus(const std::string &id, std::string &error) {
    if (isPrimaryMaster(id)) {
        error = "cannot remove primary input device";
        return false;
    }
    auto it = std::find_if(masterBuses_.begin(), masterBuses_.end(),
                           [&](const MasterBusRuntime &b) { return b.id == id; });
    if (it == masterBuses_.end()) {
        error = "no such input device: " + id;
        return false;
    }
    for (auto &c : channels_) {
        auto &ids = c.masterMicIds;
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        if (ids.empty()) ids.push_back(kPrimaryMasterId);
    }
    destroyMasterChain(*it);
    masterBuses_.erase(it);
    return true;
}

bool MixerGraph::setMasterCaptureMatch(const std::string &id, const std::string &match) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) return false;
    const bool matchChanged = bus->captureMatch != match;
    if (matchChanged) bus->captureNode.clear();
    bus->captureMatch = match;
    if (isPrimaryMaster(id)) micNodeMatch_ = match;
    return syncMasterCaptureNode(*bus) || matchChanged;
}

bool MixerGraph::syncMasterCaptureNode(MasterBusRuntime &bus) {
    const std::string want = resolveMasterCaptureNode(bus);
    const bool portMissing =
        !bus.captureNode.empty() &&
        !eng_.hasPort(bus.captureNode, masterCapturePort(bus), true);
    const bool matchMismatch =
        !bus.captureMatch.empty() && !bus.captureNode.empty() &&
        bus.captureNode.rfind(bus.captureMatch, 0) != 0;

    if (!want.empty()) {
        if (want == bus.captureNode && !portMissing && !matchMismatch) return false;
        bus.captureNode = want;
    } else if (bus.captureMatch.empty()) {
        if (bus.captureNode.empty()) return false;
        bus.captureNode.clear();
    } else if (matchMismatch || portMissing) {
        bus.captureNode.clear();
    } else {
        return false;
    }

    if (isPrimaryMaster(bus.id)) {
        if (bus.captureNode.empty())
            fprintf(stderr, "waveline: no usable microphone found\n");
        else
            fprintf(stderr, "waveline: microphone input is '%s'\n",
                    bus.captureNode.c_str());
    } else if (!bus.captureNode.empty()) {
        fprintf(stderr, "waveline: master '%s' capture is '%s'\n", bus.id.c_str(),
                bus.captureNode.c_str());
    }
    return true;
}

bool MixerGraph::waitForCaptureNode(MasterBusRuntime &bus, int timeoutMs,
                                    std::string &error) {
    if (bus.captureMatch.empty()) {
        bus.captureNode.clear();
        return true;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        eng_.sync();
        const std::string found = findCaptureNode(bus.captureMatch);
        if (!found.empty()) {
            bus.captureNode = found;
            const std::string capPort = masterCapturePort(bus);
            if (eng_.hasPort(found, capPort, true) ||
                eng_.waitForPort(found, capPort, true, 500))
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    error = "capture device not ready: " + bus.captureMatch;
    bus.captureNode.clear();
    return false;
}

bool MixerGraph::rewireMasterCapture(const std::string &id, std::string &error) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus) {
        error = "no such input device: " + id;
        return false;
    }

    bus->captureNode.clear();
    if (!bus->captureMatch.empty() && !waitForCaptureNode(*bus, 400, error))
        return false;
    return wireMasterPaths(id, error);
}

bool MixerGraph::setMasterName(const std::string &id, const std::string &name) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus || name.empty()) return false;
    bus->name = name;
    return true;
}

// Keyed on the entry's slot, not its position in the list: an output keeps the
// same loopback -- the same module, the same node names, the same level already
// pushed onto it -- no matter what happens to the outputs around it.
std::string MixerGraph::monitorOutputHandle(int slot) {
    return "waveline-monitor-out-" + std::to_string(slot);
}

std::string MixerGraph::monitorOutputHandleAt(size_t index) const {
    if (index >= monitorOutputs_.size()) return {};
    const int slot = monitorOutputs_[index].slot;
    return slot < 0 ? std::string() : monitorOutputHandle(slot);
}

int MixerGraph::freeMonitorSlot() const {
    for (int slot = 0; slot < static_cast<int>(kMaxMonitorOutputs); ++slot) {
        bool taken = false;
        for (const MonitorOutputEntry &e : monitorOutputs_)
            if (e.slot == slot) taken = true;
        if (!taken) return slot;
    }
    return -1;
}

// Entries arriving from a profile carry no slot, and two entries sharing one
// would be two outputs fighting over a single loopback -- the later claim loses
// and takes a slot of its own.
void MixerGraph::assignMonitorSlots() {
    for (size_t i = 0; i < monitorOutputs_.size(); ++i) {
        int &slot = monitorOutputs_[i].slot;
        if (slot >= 0 && slot < static_cast<int>(kMaxMonitorOutputs)) {
            bool dup = false;
            for (size_t j = 0; j < i; ++j)
                if (monitorOutputs_[j].slot == slot) dup = true;
            if (!dup) continue;
        }
        slot = -1;  // released first: freeMonitorSlot() scans this entry too
        slot = freeMonitorSlot();
    }
}

bool MixerGraph::monitorOutputPathLive(size_t index) const {
    const std::string handle = monitorOutputHandleAt(index);
    return !handle.empty() && eng_.pathExists(handle);
}

void MixerGraph::dropMonitorOutputPath(size_t index) {
    const std::string handle = monitorOutputHandleAt(index);
    if (handle.empty()) return;
    eng_.removePath(handle);
    if (monitorOutputs_[index].slot == monitorDriverSlot_) monitorDriverSlot_ = -1;
}

const std::string &MixerGraph::monitorOutput() const {
    static const std::string kEmpty;
    return monitorOutputs_.empty() ? kEmpty : monitorOutputs_.front().sink;
}

bool MixerGraph::isUsableSinkNode(const PwNode &n) const {
    if (n.isOurs || n.mediaClass != "Audio/Sink") return false;
    if (n.name.rfind("waveline-", 0) == 0) return false;
    return true;
}

bool MixerGraph::sinkPresent(const std::string &name) const {
    if (name.empty()) return false;
    for (const auto &n : eng_.nodes()) {
        if (isUsableSinkNode(n) && n.name == name) return true;
    }
    return false;
}

std::string MixerGraph::lookupSinkDescription(const std::string &name) const {
    for (const auto &n : eng_.nodes()) {
        if (!isUsableSinkNode(n) || n.name != name) continue;
        return n.description.empty() ? n.name : n.description;
    }
    return {};
}

bool MixerGraph::monitorOutputOnline(size_t index) const {
    if (index >= monitorOutputs_.size()) return false;
    return sinkPresent(monitorOutputs_[index].sink);
}

// Called from every rewire. It used to be a straight rebuildMonitorOutputs(),
// which tore down every monitor loopback and loaded a new module in its place.
//
// That is how a monitor output ends up at full scale and stays there. A new
// loopback runs at unity until its stored level is applied, and the level is
// applied by node name -- so while the old node is still in the registry and
// the new one is not, the write lands on the node that is going away and the
// one now playing keeps unity. Measured: 1.0, then 0.21, then 1.0 again inside
// 400 ms, and 1.0 from then on. A rewire happens on hotplug and on every retry
// ladder, so "it blasts again on restart" was this, not the start itself.
//
// A path that is alive and already pointed at the right sink is left exactly
// where it is; only what is missing, stale or unwanted is touched.
bool MixerGraph::refreshMonitorOutputBindings(std::string &error) {
    assignMonitorSlots();
    for (size_t i = 0; i < monitorOutputs_.size(); ++i) {
        const MonitorOutputEntry &e = monitorOutputs_[i];
        const std::string handle = monitorOutputHandleAt(i);
        const bool want = !e.sink.empty() && sinkPresent(e.sink) && !e.muted &&
                          !monitorMasterMuted_;
        const bool have = !handle.empty() && eng_.pathExists(handle);

        if (have && want && eng_.pathTarget(handle) == e.sink) {
            // Healthy. Re-assert the level -- a rewire may have recreated the
            // mix nodes under it -- and leave the module alone.
            applyMonitorOutputGain(i);
            continue;
        }
        if (have) dropMonitorOutputPath(i);
        if (want && !createMonitorOutputPath(i, error)) return false;
    }
    return true;
}

void MixerGraph::applyMonitorOutputGain(size_t index) {
    if (index >= monitorOutputs_.size()) return;
    const MonitorOutputEntry &e = monitorOutputs_[index];
    const bool muted = e.muted || monitorMasterMuted_;
    const float gain = muted ? 0.0f : e.volume * monitorMaster_;
    const std::string handle = monitorOutputHandleAt(index);
    if (handle.empty()) return;
    // One write, not two. Mute and channelVolumes live in the same Props
    // object, and sending them separately binds a node proxy and syncs the core
    // twice -- which also made ordering matter, because setPathMuted re-applies
    // the Path's stored level and could stamp unity from a loopback whose real
    // level had not arrived yet. Sending both together has no order to get
    // wrong, and this is re-pushed several times per device hotplug.
    eng_.setPathLevel(handle, gain, muted);
}

void MixerGraph::applyAllMonitorOutputGains() {
    for (size_t i = 0; i < monitorOutputs_.size(); ++i) applyMonitorOutputGain(i);
}

void MixerGraph::applyStreamMixGain() {
    float gain = streamMixVolume_;
    if (streamMixMuted_) gain = 0.0f;
    eng_.setNodeVolume(kStreamMix, gain, streamMixMuted_);
}

// One monitor output's loopback, at the level that output is set to.
//
// The volume is seeded into the PathSpec *and* pushed again as soon as the
// capture node has a port, because a loopback module runs at unity from the
// moment it is loaded until something tells it otherwise: the gap between the
// two is the whole reason a monitor output could be briefly, painfully loud.
bool MixerGraph::createMonitorOutputPath(size_t index, std::string &error) {
    if (index >= monitorOutputs_.size()) return false;
    if (monitorOutputs_[index].slot < 0) assignMonitorSlots();
    MonitorOutputEntry &e = monitorOutputs_[index];
    if (e.slot < 0) {
        error = "no free monitor output slot";
        return false;
    }
    if (e.sink.empty() || !sinkPresent(e.sink)) return true;

    // Exactly one monitor loopback drives the clock; two ALSA clocks both
    // driving pitch-warble everything in the graph. The role goes to the first
    // path created and is released when that path is destroyed, rather than
    // being pinned to list position -- otherwise removing output #2 would have
    // to restart output #1 to hand it a title it already held.
    const bool driver = monitorDriverSlot_ < 0 || monitorDriverSlot_ == e.slot;

    PwEngine::PathSpec spec;
    spec.handle = monitorOutputHandle(e.slot);
    spec.source = kMonitorMix;
    spec.target = e.sink;
    // Position at the moment the loopback is loaded. A node description is
    // fixed once the node exists, so an output that moves up the list after a
    // removal keeps the number it was born with in other people's device lists
    // -- which is cheaper than restarting a healthy output to renumber it.
    spec.description = "Monitor Mix → Output " + std::to_string(index + 1);
    spec.inChannels = 2;
    spec.outChannels = 2;
    spec.sourceIsSink = true;
    spec.pinRate = true;
    spec.followerOnly = !driver;
    spec.stickyTarget = true;
    spec.volume = e.volume * monitorMaster_;
    spec.muted = e.muted || monitorMasterMuted_;
    if (!eng_.addPath(spec, error)) return false;
    if (driver) monitorDriverSlot_ = e.slot;

    // The node the volume lands on is the capture end; waiting for its port is
    // waiting for the earliest moment the level can be applied at all.
    const std::string capNode = spec.handle + "-in";
    eng_.waitForPort(capNode, eng_.pathCapturePort(spec.handle), false, 800);
    applyMonitorOutputGain(index);
    return true;
}

// Every monitor loopback torn down and built again. Only for the cases where
// the whole set changes at once (a profile being applied, the graph being
// built): a rebuild costs every output its level for as long as it takes the
// new nodes to appear, which is why adding, removing or re-pointing a single
// output does not come through here.
bool MixerGraph::rebuildMonitorOutputs(std::string &error) {
    for (int slot = 0; slot < static_cast<int>(kMaxMonitorOutputs); ++slot)
        eng_.removePath(monitorOutputHandle(slot));
    monitorDriverSlot_ = -1;
    assignMonitorSlots();

    // Keep every assignment even when the sink is gone — never fall back to
    // the default device (headphones unplug must not dump mic-monitor onto
    // speakers). Paths are only created for sinks that are currently online.
    for (size_t i = 0; i < monitorOutputs_.size(); ++i) {
        MonitorOutputEntry &e = monitorOutputs_[i];
        if (e.sink.empty()) continue;
        if (const std::string desc = lookupSinkDescription(e.sink); !desc.empty())
            e.description = desc;
        // No loopback at all for an output that is muted: a module loaded for
        // one would run at unity until its level arrived, which is a muted
        // output making a noise -- at every daemon start.
        if (e.muted || monitorMasterMuted_) continue;
        if (!createMonitorOutputPath(i, error)) return false;
    }
    return true;
}

// True when some *other* output already plays to this sink. Two loopbacks on
// one device fight over the same clock driver and the device ends up carrying
// the sum of two faders, so an output device belongs to exactly one Monitor
// mix at a time.
bool MixerGraph::monitorSinkTaken(const std::string &sinkName,
                                  size_t exceptIndex) const {
    if (sinkName.empty()) return false;
    for (size_t i = 0; i < monitorOutputs_.size(); ++i) {
        if (i == exceptIndex) continue;
        if (monitorOutputs_[i].sink == sinkName) return true;
    }
    return false;
}

bool MixerGraph::setMonitorOutputs(const std::vector<MonitorOutputEntry> &outputs,
                                   std::string &error) {
    if (outputs.empty() || outputs.size() > kMaxMonitorOutputs) return false;
    // A config written before the one-device-per-output rule (or hand-edited)
    // can name the same sink twice. Keep the first entry that claims it and
    // drop the rest rather than refusing to start. Built aside and swapped in:
    // callers may hand us monitorOutputs_ itself.
    std::vector<MonitorOutputEntry> deduped;
    deduped.reserve(outputs.size());
    for (const MonitorOutputEntry &e : outputs) {
        const bool dup =
            !e.sink.empty() &&
            std::any_of(deduped.begin(), deduped.end(),
                        [&e](const MonitorOutputEntry &k) { return k.sink == e.sink; });
        if (!dup) deduped.push_back(e);
    }
    monitorOutputs_ = std::move(deduped);
    for (MonitorOutputEntry &e : monitorOutputs_) {
        if (e.description.empty()) {
            e.description = lookupSinkDescription(e.sink);
            if (e.description.empty()) e.description = e.sink;
        }
    }
    return rebuildMonitorOutputs(error);
}

// Re-pointing one output at a different device. A loopback's target is fixed
// when its module loads, so this one path really does have to be replaced --
// but only this one. Every other output keeps its module and the level already
// on it.
bool MixerGraph::setMonitorOutputAt(size_t index, const std::string &sinkName,
                                    std::string &error) {
    if (index >= monitorOutputs_.size() || sinkName.empty()) return false;
    if (monitorSinkTaken(sinkName, index)) {
        error = "output device is already assigned to another Monitor mix";
        return false;
    }
    MonitorOutputEntry &e = monitorOutputs_[index];
    const bool sameSink = e.sink == sinkName;
    e.sink = sinkName;
    e.description = lookupSinkDescription(sinkName);
    if (e.description.empty()) e.description = sinkName;

    const std::string handle = monitorOutputHandleAt(index);
    const bool live = !handle.empty() && eng_.pathExists(handle);
    if (sameSink && live && eng_.pathTarget(handle) == sinkName) return true;

    dropMonitorOutputPath(index);
    // Move to a spare slot when there is one. The replacement loopback is
    // loaded while the old one's nodes are still in the registry, and the level
    // push resolves nodes by name: reusing the name would land it on the node
    // that is going away and leave the one now playing at unity.
    if (const int spare = freeMonitorSlot(); spare >= 0) e.slot = spare;
    if (e.muted || monitorMasterMuted_) return true;
    return createMonitorOutputPath(index, error);
}

// Adding an output touches nothing but the output being added.
//
// This used to go through rebuildMonitorOutputs(), and that is how every other
// monitor output ended up stuck at full scale: each one's loopback was
// destroyed and reloaded under the same node name, and a loopback plays at
// unity until its level is pushed onto it. The push finds the node by name,
// the name still resolved to the node being destroyed, and the module now
// playing never heard the fader at all.
bool MixerGraph::addMonitorOutput(const std::string &sinkName, std::string &error) {
    if (monitorOutputs_.size() >= kMaxMonitorOutputs || sinkName.empty()) return false;
    if (monitorSinkTaken(sinkName, monitorOutputs_.size())) {
        error = "output device is already assigned to another Monitor mix";
        return false;
    }
    const int slot = freeMonitorSlot();
    if (slot < 0) {
        error = "no free monitor output slot";
        return false;
    }
    MonitorOutputEntry e;
    e.sink = sinkName;
    e.description = lookupSinkDescription(sinkName);
    if (e.description.empty()) e.description = sinkName;
    e.slot = slot;
    monitorOutputs_.push_back(std::move(e));

    const size_t index = monitorOutputs_.size() - 1;
    if (monitorOutputs_[index].muted || monitorMasterMuted_) return true;
    if (!createMonitorOutputPath(index, error)) {
        dropMonitorOutputPath(index);
        monitorOutputs_.pop_back();
        return false;
    }
    return true;
}

// Removing an output takes its loopback with it and leaves the rest alone --
// same reasoning as addMonitorOutput(). Slots are per entry, so the outputs
// below the removed one keep their handles even though their list positions
// shift up.
bool MixerGraph::removeMonitorOutput(size_t index, std::string &error) {
    if (monitorOutputs_.size() <= 1 || index >= monitorOutputs_.size()) return false;
    (void)error;
    dropMonitorOutputPath(index);
    monitorOutputs_.erase(monitorOutputs_.begin() + static_cast<ptrdiff_t>(index));
    return true;
}

bool MixerGraph::setMonitorOutput(const std::string &sinkName, std::string &error) {
    if (monitorOutputs_.empty()) {
        MonitorOutputEntry e;
        e.sink = sinkName;
        monitorOutputs_.push_back(std::move(e));
    } else {
        monitorOutputs_[0].sink = sinkName;
    }
    return setMonitorOutputAt(0, sinkName, error);
}

bool MixerGraph::setMonitorOutputVolume(size_t index, float volume) {
    if (index >= monitorOutputs_.size()) return false;
    const bool wasMuted = monitorOutputs_[index].muted;
    monitorOutputs_[index].volume = volume;
    monitorOutputs_[index].muted = false;
    // Moving the fader on a muted output unmutes it, and a muted output may
    // have no loopback to turn up -- one that was already muted when the graph
    // was built never got one.
    if (wasMuted && !monitorOutputPathLive(index)) {
        std::string err;
        return createMonitorOutputPath(index, err);
    }
    applyMonitorOutputGain(index);
    return true;
}

// Muting used to tear down every monitor loopback and build back the ones still
// wanted. A freshly loaded loopback runs at unity until its node appears and
// the stored level is applied, so muting one output slammed every *other*
// output to full scale for that window -- on headphones, briefly and loudly.
//
// Now a mute is a gain of zero on a path that stays exactly where it is: no
// other output is touched, the clock driver does not move between them, and
// there is no window to be loud in. Unmuting an output that has no path yet --
// one that was already muted when the graph was built -- creates that one
// path, and only that one.
bool MixerGraph::setMonitorOutputMuted(size_t index, bool muted) {
    if (index >= monitorOutputs_.size()) return false;
    if (monitorOutputs_[index].muted == muted) return true;
    monitorOutputs_[index].muted = muted;

    if (!muted && !monitorOutputPathLive(index)) {
        std::string err;
        return createMonitorOutputPath(index, err);
    }
    applyMonitorOutputGain(index);
    return true;
}

bool MixerGraph::setMonitorMasterVolume(float volume) {
    monitorMaster_ = volume;
    monitorMasterMuted_ = false;
    applyAllMonitorOutputGains();
    return true;
}

bool MixerGraph::setMonitorMasterMuted(bool muted) {
    if (monitorMasterMuted_ == muted) return true;
    monitorMasterMuted_ = muted;
    // Gain on the live paths, not a rebuild. Any output with no path yet gets
    // one, at the level it is meant to be at.
    if (!muted) {
        for (size_t i = 0; i < monitorOutputs_.size(); ++i) {
            const MonitorOutputEntry &e = monitorOutputs_[i];
            if (e.muted || e.sink.empty() || !sinkPresent(e.sink)) continue;
            if (monitorOutputPathLive(i)) continue;
            std::string err;
            createMonitorOutputPath(i, err);
        }
    }
    applyAllMonitorOutputGains();
    return true;
}

bool MixerGraph::setStreamMixVolume(float volume) {
    streamMixVolume_ = volume;
    streamMixMuted_ = false;
    applyStreamMixGain();
    return true;
}

bool MixerGraph::setStreamMixMuted(bool muted) {
    streamMixMuted_ = muted;
    applyStreamMixGain();
    return true;
}

bool MixerGraph::isUsableMicNode(const PwNode &n) const {
    if (n.isOurs || n.mediaClass != "Audio/Source") return false;
    if (n.name.size() > 8 && n.name.compare(n.name.size() - 8, 8, ".monitor") == 0)
        return false;
    if (n.name.rfind("waveline-", 0) == 0) return false;
    return true;
}

std::string MixerGraph::findCaptureNode(const std::string &captureMatch) const {
    auto hasCapturePort = [this](const std::string &name) {
        return eng_.hasPort(name, "capture_MONO", true) ||
               eng_.hasPort(name, "capture_FL", true);
    };
    if (!captureMatch.empty()) {
        for (const auto &n : eng_.nodes()) {
            if (n.name.rfind(captureMatch, 0) != 0) continue;
            if (!isUsableMicNode(n)) continue;
            if (hasCapturePort(n.name)) return n.name;
        }
        return {};
    }

    const std::vector<PwNode> nodes = eng_.nodes();
    const std::string dflt = eng_.defaultSourceName();
    if (!dflt.empty()) {
        for (const auto &n : nodes) {
            if (n.name == dflt && isUsableMicNode(n) && hasCapturePort(n.name))
                return n.name;
        }
    }
    for (const auto &n : nodes) {
        if (n.name.rfind("alsa_input.", 0) == 0 && isUsableMicNode(n) &&
            hasCapturePort(n.name))
            return n.name;
    }
    return {};
}

std::string MixerGraph::resolveMasterCaptureNode(const MasterBusRuntime &bus) const {
    if (!bus.captureMatch.empty()) return findCaptureNode(bus.captureMatch);
    if (isPrimaryMaster(bus.id)) return findCaptureNode({});
    return {};
}

void MixerGraph::setMicInputGain(float linear) {
    setMasterInputGain(kPrimaryMasterId, linear);
}

void MixerGraph::setMasterInputGain(const std::string &id, float linear) {
    MasterBusRuntime *bus = masterBus(id);
    if (bus && bus->chain.gain) bus->chain.gain->setGain(linear);
}

bool MixerGraph::reconsiderMasterNode(MasterBusRuntime &bus) {
    if (!bus.captureMatch.empty()) return false;
    if (!isPrimaryMaster(bus.id)) return false;

    if (!bus.captureNode.empty() &&
        !eng_.hasPort(bus.captureNode, masterCapturePort(bus), true)) {
        const std::string want = findCaptureNode({});
        if (want == bus.captureNode) return false;
        bus.captureNode = want;
        return true;
    }

    const std::string dflt = eng_.defaultSourceName();
    if (dflt.empty() || dflt == bus.captureNode) return false;
    for (const auto &n : eng_.nodes()) {
        if (n.name != dflt) continue;
        if (!isUsableMicNode(n)) return false;
        bus.captureNode = dflt;
        return true;
    }
    return false;
}

bool MixerGraph::reconsiderMicNode() {
    bool changed = false;
    for (auto &bus : masterBuses_) {
        if (reconsiderMasterNode(bus)) changed = true;
    }
    return changed;
}

std::string MixerGraph::masterCapturePort(const MasterBusRuntime &bus) const {
    if (bus.captureNode.empty()) return "capture_MONO";
    if (eng_.hasPort(bus.captureNode, "capture_MONO", true)) return "capture_MONO";
    if (eng_.hasPort(bus.captureNode, "capture_FL", true)) return "capture_FL";
    return "capture_MONO";
}

bool MixerGraph::isMasterMidi(const MasterBusRuntime &bus) const {
    return isMidiBusType(bus.busType);
}

std::string MixerGraph::findMidiNode(const std::string &midiMatch) const {
    if (midiMatch.empty()) return {};
    const MidiMatchParts parts = splitMidiMatch(midiMatch);
    for (const auto &n : eng_.nodes()) {
        if (n.isOurs || n.name.rfind("waveline-", 0) == 0) continue;
        if (n.mediaClass.find("Midi") == std::string::npos &&
            n.mediaClass.find("midi") == std::string::npos)
            continue;
        if (n.name != parts.node && n.name.rfind(parts.node, 0) != 0) continue;
        if (!masterMidiOutputPort(n.name, parts.port).empty()) return n.name;
    }
    return {};
}

std::string MixerGraph::masterMidiOutputPort(const std::string &node,
                                             const std::string &portHint) const {
    return eng_.resolveMidiOutputPort(node, portHint);
}

bool MixerGraph::waitForMidiNode(MasterBusRuntime &bus, int timeoutMs, std::string &error) {
    if (bus.midiPortMatch.empty()) {
        bus.midiNode.clear();
        bus.midiSourcePortId = 0;
        return true;
    }
    const MidiMatchParts parts = splitMidiMatch(bus.midiPortMatch);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        eng_.sync();
        const std::string found = findMidiNode(bus.midiPortMatch);
        if (!found.empty()) {
            bus.midiNode = found;
            const std::string outPort = masterMidiOutputPort(found, parts.port);
            if (!outPort.empty() &&
                (eng_.hasPort(found, outPort, true) ||
                 eng_.waitForPort(found, outPort, true, 500)))
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = "MIDI device not ready: " + bus.midiPortMatch;
    bus.midiNode.clear();
    bus.midiSourcePortId = 0;
    return false;
}

bool MixerGraph::linkMasterMidiInput(MasterBusRuntime &bus, std::string &error) {
    if (bus.midiNode.empty() || !bus.chain.synthReady) return true;
    const MidiMatchParts parts = splitMidiMatch(bus.midiPortMatch);
    const std::string outPort = masterMidiOutputPort(bus.midiNode, parts.port);
    if (outPort.empty()) {
        error = "no MIDI output port on " + bus.midiNode;
        return false;
    }
    const std::string synthNode = masterSynthNode(bus.id);
    if (!eng_.waitForPort(synthNode, "midi_in", false, 500)) {
        error = "MIDI synth input not ready";
        return false;
    }
    if (!eng_.linkPorts(bus.midiNode, outPort, synthNode, "midi_in", error, true))
        return false;
    bus.midiLinkedMatch = bus.midiPortMatch;
    bus.midiSourcePortId = eng_.portId(bus.midiNode, outPort, true);
    return true;
}

bool MixerGraph::linkPendingMidiInputs(std::string &error) {
    error.clear();
    for (auto &bus : masterBuses_) {
        if (!isMasterMidi(bus) || bus.midiPortMatch.empty() || !bus.chain.synthReady)
            continue;
        const std::string synthNode = masterSynthNode(bus.id);
        const MidiMatchParts parts = splitMidiMatch(bus.midiPortMatch);
        const std::string liveNode = findMidiNode(bus.midiPortMatch);
        const std::string livePort =
            liveNode.empty() ? std::string{}
                             : masterMidiOutputPort(liveNode, parts.port);
        const uint32_t livePortId =
            livePort.empty() ? 0 : eng_.portId(liveNode, livePort, true);
        if (eng_.hasManualLinkTo(synthNode, "midi_in") &&
            bus.midiLinkedMatch == bus.midiPortMatch &&
            bus.midiNode == liveNode && livePortId != 0 &&
            bus.midiSourcePortId == livePortId)
            continue;

        eng_.forgetLinksTo(synthNode, "midi_in");
        bus.midiNode.clear();
        bus.midiLinkedMatch.clear();
        bus.midiSourcePortId = 0;
        std::string waitErr;
        if (!waitForMidiNode(bus, 500, waitErr)) continue;

        std::string linkErr;
        if (!linkMasterMidiInput(bus, linkErr) && error.empty()) error = linkErr;
    }
    return error.empty();
}

bool MixerGraph::linkPendingMidiAudioPaths(std::string &error) {
    error.clear();
    for (auto &bus : masterBuses_) {
        if (!isMasterMidi(bus) || !bus.chain.synthReady) continue;

        auto linkedTo = [this](const std::string &node,
                               const std::string &port) {
            return eng_.hasManualLinkTo(node, port);
        };
        bool ready = true;
        if (bus.chain.gainReady)
            ready = ready && linkedTo(masterGainNode(bus.id), "input");
        if (bus.chain.ncReady)
            ready = ready && linkedTo(masterNcNode(bus.id), "input");
        if (bus.chain.fxReady)
            ready = ready && linkedTo(masterFxNode(bus.id), "input");
        if (bus.chain.creativeReady)
            ready = ready && linkedTo(masterCreativeNode(bus.id), "input");
        if (bus.chain.dynReady)
            ready = ready && linkedTo(masterDynNode(bus.id), "input");
        if (bus.chain.sourceReady)
            ready = ready && linkedTo(masterSourceNode(bus.id), "input_MONO");

        const std::string streamHandle = pathName(bus.id, Mix::Stream);
        if (!eng_.pathExists(streamHandle)) ready = false;

        auto pathReady = [&](const std::string &handle) {
            const std::string in = handle + "-in";
            if (bus.micStereo)
                return linkedTo(in, "input_FL") && linkedTo(in, "input_FR");
            return linkedTo(in, "input_MONO");
        };
        ready = ready && pathReady(pathName(bus.id, Mix::Stream));
        if (bus.softwareMonitor)
            ready = ready && pathReady(masterMonitorPath(bus.id));
        if (ready) continue;

        std::string wireErr;
        if (!wireMasterPaths(bus.id, wireErr) && error.empty())
            error = wireErr;
    }
    return error.empty();
}

bool MixerGraph::rewireMasterMidiInput(const std::string &id, std::string &error) {
    error.clear();
    MasterBusRuntime *bus = masterBus(id);
    if (!bus || !isMasterMidi(*bus)) {
        error = "not a MIDI input device: " + id;
        return false;
    }
    if (!bus->chain.synthReady) {
        error = "MIDI synth not ready for " + id;
        return false;
    }

    const std::string synthNode = masterSynthNode(id);
    eng_.forgetLinksTo(synthNode, "midi_in");
    bus->midiNode.clear();
    bus->midiLinkedMatch.clear();
    bus->midiSourcePortId = 0;
    eng_.sync();

    if (bus->midiPortMatch.empty()) return true;
    if (!waitForMidiNode(*bus, 500, error)) return false;
    return linkMasterMidiInput(*bus, error);
}

void MixerGraph::invalidateMasterMidiInput(const std::string &id) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus || !isMasterMidi(*bus)) return;
    eng_.forgetLinksTo(masterSynthNode(id), "midi_in");
    bus->midiNode.clear();
    bus->midiLinkedMatch.clear();
    bus->midiSourcePortId = 0;
}

bool MixerGraph::setMasterMidiPortMatch(const std::string &id, const std::string &match) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus || !isMasterMidi(*bus)) return false;
    const bool changed = bus->midiPortMatch != match;
    bus->midiPortMatch = match;
    if (changed) {
        bus->midiNode.clear();
        bus->midiLinkedMatch.clear();
        bus->midiSourcePortId = 0;
    }
    return changed;
}

bool MixerGraph::setMasterSoundfontPath(const std::string &id, const std::string &path) {
    MasterBusRuntime *bus = masterBus(id);
    if (!bus || !isMasterMidi(*bus)) return false;
    bus->soundfontPath = path;
    if (bus->chain.synth) bus->chain.synth->setSoundfontPath(path);
    return true;
}

}  // namespace waveline

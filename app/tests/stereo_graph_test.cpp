// SPDX-License-Identifier: GPL-2.0-or-later
#include "engine/filterhost.h"
#include "engine/mixergraph.h"
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace waveline;
using namespace std::chrono_literals;

static void require(bool ok, const std::string &why) {
    if (!ok) { std::cerr << why << '\n'; std::exit(1); }
}

// Real PipeWire nodes, running through the production MixerGraph and filters.
// The Python launcher supplies a private server with no hardware monitors.
struct Signal {
    pw_filter *filter = nullptr;
    spa_hook listener{};
    void *ports[2]{};
    bool source;
    std::atomic<int> lane{0}; // 0 left, 1 right, 2 both
    std::atomic<float> energy[2]{};
    std::atomic<unsigned> cycles{0};
    uint64_t offset = 0;

    static void process(void *data, spa_io_position *position) {
        auto &s = *static_cast<Signal *>(data);
        const auto n = position->clock.duration;
        const int lane = s.lane.load();
        for (int ch = 0; ch < 2; ++ch) {
            auto *buffer = s.ports[ch] ? static_cast<float *>(pw_filter_get_dsp_buffer(s.ports[ch], n)) : nullptr;
            float sum = 0;
            for (uint32_t i = 0; buffer && i < n; ++i) {
                if (s.source)
                    buffer[i] = (lane == ch || lane == 2)
                        ? 0.2f * std::sin(6.28318530718 * (ch ? 660 : 440) * (s.offset + i) / 48000)
                        : 0;
                sum += buffer[i] * buffer[i];
            }
            s.energy[ch].store(n ? sum / n : 0);
        }
        s.offset += n;
        s.cycles.fetch_add(1);
    }

    Signal(const char *name, bool produce, bool capture = false, int channels = 2) : source(produce) {
        std::string error;
        require(FilterHost::start(error), error);
        auto *loop = FilterHost::loop();
        pw_thread_loop_lock(loop);
        filter = pw_filter_new(FilterHost::core(), name, pw_properties_new(
            "node.name", name, "media.type", "Audio", "media.class",
            capture ? "Audio/Source" : "Stream/Filter/Audio",
            "node.autoconnect", "false", "node.want-driver", "true",
            "audio.rate", "48000", nullptr));
        require(filter, "create signal node");
        static const pw_filter_events events = [] {
            pw_filter_events e{}; e.version = PW_VERSION_FILTER_EVENTS; e.process = process; return e;
        }();
        pw_filter_add_listener(filter, &listener, &events, this);
        for (int ch = 0; ch < channels; ++ch) {
            const char *port = source ? (ch ? "output_FR" : "output_FL") : (ch ? "input_FR" : "input_FL");
            if (capture) port = channels == 1 ? "capture_MONO" : (ch ? "capture_FR" : "capture_FL");
            ports[ch] = pw_filter_add_port(filter, source ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT,
                PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
                pw_properties_new("format.dsp", "32 bit float mono audio", "port.name", port, nullptr), nullptr, 0);
            require(ports[ch], "create signal port");
        }
        require(pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) >= 0, "connect signal");
        pw_thread_loop_unlock(loop);
    }
    ~Signal() {
        pw_thread_loop_lock(FilterHost::loop());
        pw_filter_destroy(filter);
        pw_thread_loop_unlock(FilterHost::loop());
    }
};

int main() {
    require(std::getenv("WAVELINE_TEST_SESSION"), "Run using isolated_pipewire_test.py, never a live session");
    {
        PwEngine engine;
        std::string error;
        require(engine.start(error), error);
        Signal source("alsa_input.issue9-default-mic", true, true, 1);
        require(engine.waitForPort("alsa_input.issue9-default-mic", "capture_MONO", true, 5000),
                "default capture source missing");
        MixerGraph graph(engine);
        require(graph.build(error, false), error);
        auto *bus = graph.masterBus("mic");
        require(bus && bus->captureMatch.empty(), "test must follow default input");
        // Match the recovery quiet phase: the runtime node is deliberately
        // cleared, but an empty saved match must still recover the default.
        graph.silenceMasterCapture("mic");
        engine.forgetLinksForNode(bus->captureNode);
        bus->captureNode.clear();
        require(graph.primeMasterHwCapture("mic", error), error);
        require(bus->captureNode == "alsa_input.issue9-default-mic",
                "default input was skipped during capture warm-up");
        const auto *oldSelector = bus->chain.selector.get();
        const auto instance = oldSelector->health().instance;
        bus->captureNode.clear();
        require(graph.rebuildMasterHwCapture("mic", error), error);
        require(bus->chain.selector && bus->chain.selector->health().instance != instance,
                "default input rebuild reported success without rebuilding");
        require(bus->captureMatch.empty(), "recovery pinned the default microphone");
        require(bus->captureNode == "alsa_input.issue9-default-mic",
                "default microphone was not reconnected");
        require(graph.verifyMasterMixWiring("mic", error), error);
        Signal tap("test-default-capture-tap", false);
        require(engine.waitForPort("waveline-mic", "capture_MONO", true, 5000),
                "rebuilt microphone output missing");
        require(engine.waitForPort("test-default-capture-tap", "input_FL", false, 5000),
                "capture test tap missing");
        require(engine.linkPorts("waveline-mic", "capture_MONO",
                                 "test-default-capture-tap", "input_FL", error), error);
        std::this_thread::sleep_for(500ms);
        const auto cycles = tap.cycles.load();
        std::this_thread::sleep_for(150ms);
        require(tap.cycles.load() > cycles && tap.energy[0].load() > 0.001f,
                "rebuilt default microphone has links but carries no signal");
    }
    {
        PwEngine engine;
        std::string error;
        require(engine.start(error), error);
        MixerGraph graph(engine);
        graph.setMicNodeMatch("absent-test-mic");
        require(graph.build(error, false), error);
        Signal source("test-source", true), stream("test-stream", false), monitor("test-monitor", false);
        auto link = [&](const std::string &from, const std::string &out, const std::string &to, const std::string &in) {
            require(engine.waitForPort(from, out, true, 5000), "missing " + from + ':' + out);
            require(engine.waitForPort(to, in, false, 5000), "missing " + to + ':' + in);
            require(engine.linkPorts(from, out, to, in, error), error);
        };
        graph.setChannelMonitorFx("system", true);
        bool wired = false;
        for (int attempt = 0; attempt < 30 && !wired; ++attempt) {
            wired = graph.wireAllChannelFx(error);
            if (!wired) std::this_thread::sleep_for(100ms);
        }
        require(wired, error);
        for (const auto &channel : {std::string("FL"), std::string("FR")}) {
            link("test-source", "output_" + channel, "waveline-ch-system", "playback_" + channel);
            link(MixerGraph::kStreamMix, "monitor_" + channel, "test-stream", "input_" + channel);
            link(MixerGraph::kMonitorMix, "monitor_" + channel, "test-monitor", "input_" + channel);
        }
        auto check = [&](int lane) {
            source.lane.store(lane);
            std::this_thread::sleep_for(600ms);
            for (Signal *tap : {&stream, &monitor}) {
                const auto before = tap->cycles.load();
                std::this_thread::sleep_for(150ms);
                require(tap->cycles.load() > before, "audio stopped processing");
                for (int ch = 0; ch < 2; ++ch) {
                    float e = tap->energy[ch].load();
                    const bool wanted = lane == ch || lane == 2;
                    require(std::isfinite(e) && (wanted ? e > 0.001f : e < 1e-8f),
                            std::string(tap == &stream ? "Stream" : "Monitor") +
                            " lane " + std::to_string(lane) + " channel " + std::to_string(ch) +
                            " energy " + std::to_string(e));
                }
            }
        };
        check(0); check(1); check(2);
        // Bypassed denoising must preserve both channels.
        graph.setChannelNoiseSuppression("system", FxStage::Output, false);
        graph.setChannelMonitorFx("system", false);
        require(graph.rewireChannelMonitor("system", error), error);
        check(0); check(1);
        graph.setChannelMonitorFx("system", true);
        require(graph.rewireChannelMonitor("system", error), error);
        check(0); check(1);
    }
    {
        PwEngine engine;
        std::string error;
        require(engine.start(error), error);
        Signal source("alsa_input.test-stereo-mic", true, true);
        require(engine.waitForPort("alsa_input.test-stereo-mic", "capture_FR", true, 5000), "stereo hardware missing");
        MixerGraph graph(engine);
        graph.setMicNodeMatch("alsa_input.test-stereo-mic");
        graph.setSoftwareMicGain(true);
        require(graph.build(error, true), error);
        auto *bus = graph.masterBus("mic");
        require(bus && bus->captureChannels == 2, "stereo hardware must build stereo DSP");
        require(graph.noiseFilter(), "stereo denoiser missing");
        graph.noiseFilter()->setEnabled(false);
        graph.setSoftwareMonitor(true);
        require(graph.wireMasterPaths("mic", error), error);
        require(graph.verifyMasterMixWiring("mic", error), error);
        Signal recording("test-mic-recording", false), monitor("test-mic-monitor", false),
               stream("test-mic-stream", false), channel("test-channel-recording", false);
        auto link = [&](const std::string &from, const std::string &out, const std::string &to, const std::string &in) {
            require(engine.waitForPort(from, out, true, 5000), "missing " + from + ':' + out);
            require(engine.waitForPort(to, in, false, 5000), "missing " + to + ':' + in);
            require(engine.linkPorts(from, out, to, in, error), error);
        };
        for (const std::string side : {"FL", "FR"}) {
            link("waveline-mic", "capture_" + side, "test-mic-recording", "input_" + side);
            link(MixerGraph::kMonitorMix, "monitor_" + side, "test-mic-monitor", "input_" + side);
            link(MixerGraph::kStreamMix, "monitor_" + side, "test-mic-stream", "input_" + side);
        }
        auto check = [&](Signal &tap, int lane, const std::string &label, float minimum = 0.001f) {
            const auto cycles = tap.cycles.load();
            std::this_thread::sleep_for(150ms);
            require(tap.cycles.load() > cycles, label + " stalled");
            for (int ch = 0; ch < 2; ++ch) {
                const float e = tap.energy[ch].load();
                const bool active = lane == ch || lane == 2;
                require(std::isfinite(e) && (active ? e > minimum : e < 1e-8f),
                        label + " lane " + std::to_string(ch) + " energy " + std::to_string(e));
            }
        };
        for (bool effects : {false, true}) {
            graph.setMicMonitorFx(effects);
            require(graph.rewireMicMonitor(error), error);
            for (int lane : {0, 1, 2}) {
                source.lane.store(lane);
                std::this_thread::sleep_for(400ms);
                check(recording, lane, "published stereo microphone");
                check(monitor, lane, "stereo monitoring");
                check(stream, lane, "stereo Stream mix");
            }
        }
        // Left/right monitoring controls must not mute or attenuate recordings.
        source.lane.store(2);
        bus->monitorLeft = 0;
        graph.applyMasterPathLevels("mic");
        std::this_thread::sleep_for(400ms);
        check(monitor, 1, "left monitor muted");
        check(recording, 2, "recording unaffected by monitor mute");
        check(stream, 2, "stream unaffected by monitor mute");
        bus->monitorLeft = 1; bus->monitorRight = 0;
        graph.applyMasterPathLevels("mic");
        std::this_thread::sleep_for(400ms);
        check(monitor, 0, "right monitor muted");
        bus->monitorRight = 1;
        graph.applyMasterPathLevels("mic");

        graph.setChannelMicSource("system", true);
        graph.ensureChannelMicSource("system");
        require(graph.rewireChannelMicSource("system", error), error);
        for (const std::string side : {"FL", "FR"})
            link("waveline-system-mic", "capture_" + side, "test-channel-recording", "input_" + side);
        for (bool deviceFx : {false, true}) {
            graph.setChannelMicUseDeviceFx("system", deviceFx);
            require(graph.rewireChannelMicSource("system", error), error);
            for (const std::string side : {"FL", "FR"})
                link("waveline-system-mic", "capture_" + side, "test-channel-recording", "input_" + side);
            for (int lane : {0, 1}) {
                source.lane.store(lane);
                std::this_thread::sleep_for(400ms);
                check(channel, lane, "per-channel stereo microphone");
            }
        }
        // The exact routing helper used by Audio Sharing and the soundboard.
        Signal shared("test-shared-audio", true);
        source.lane.store(3); // silence
        require(engine.waitForPort("test-shared-audio", "output_FR", true, 5000), "sharing source missing");
        require(engine.linkToVirtualSource("test-shared-audio", "output_FL", "output_FR", "waveline-mic", error), error);
        for (int lane : {0, 1}) {
            shared.lane.store(lane);
            std::this_thread::sleep_for(400ms);
            check(recording, lane, "audio sharing into stereo microphone");
        }
        engine.forgetLinksForNode("test-shared-audio");
        // Independent models: activity on one side must never appear on the other.
        graph.noiseFilter()->setEnabled(true);
        for (int lane : {0, 1}) {
            source.lane.store(lane);
            std::this_thread::sleep_for(1200ms);
            check(recording, lane, "stereo noise suppression", 1e-12f);
        }
        // Auto-pause: the filter goes to passthrough without touching the
        // user's switch, and resumes denoising when woken.
        source.lane.store(2);
        graph.setMicNoiseIdle(true);
        std::this_thread::sleep_for(400ms);
        {
            auto *nc = graph.noiseFilter();
            require(nc->enabled() && nc->idle(), "pause leaves the NC switch on");
            const float in = nc->inputRms(), out = nc->outputRms();
            require(in > 0.01f && std::fabs(out - in) < in * 0.01f, "paused NC passes audio through");
            require(nc->speechProbability() == 0.0f, "paused NC runs no model");
        }
        graph.setMicNoiseIdle(false);
        std::this_thread::sleep_for(1200ms);
        require(graph.noiseFilter()->speechProbability() > 0.0f, "resumed NC runs the model again");
        graph.noiseFilter()->setEnabled(false);
        graph.setMicNoiseIdle(true);
        require(graph.rebuildMasterHwCapture("mic", error), error);
        require(bus->captureChannels == 2 && !graph.noiseFilter()->enabled(), "rebuild preserves stereo and bypass");
        require(graph.noiseFilter()->idle(), "rebuilt NC inherits the pause");
        graph.setMicNoiseIdle(false);
        for (const std::string side : {"FL", "FR"})
            link("waveline-mic", "capture_" + side, "test-mic-recording", "input_" + side);
        source.lane.store(1);
        std::this_thread::sleep_for(400ms);
        check(recording, 1, "stereo after capture rebuild");
    }
    FilterHost::stop();
    std::cout << "Production graph preserves left/right through both mixes and FX toggles\n";
}

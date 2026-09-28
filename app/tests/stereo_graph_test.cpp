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
            auto *buffer = static_cast<float *>(pw_filter_get_dsp_buffer(s.ports[ch], n));
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

    Signal(const char *name, bool produce, bool capture = false) : source(produce) {
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
        for (int ch = 0; ch < 2; ++ch) {
            const char *port = source ? (ch ? "output_FR" : "output_FL") : (ch ? "input_FR" : "input_FL");
            if (capture) port = ch ? "capture_FR" : "capture_FL";
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
        Signal source("alsa_input.issue9-default-mic", true, true);
        require(engine.waitForPort("alsa_input.issue9-default-mic", "capture_FL", true, 5000),
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
        // Enabled denoising with a fully dry mix still exercises separate model
        // histories/frame adapters; bypass must preserve both channels too.
        graph.setChannelNoiseSuppression("system", FxStage::Output, true, 0.0f);
        check(0); check(1); check(2);
        graph.setChannelNoiseSuppression("system", FxStage::Output, false, 1.0f);
        graph.setChannelMonitorFx("system", false);
        require(graph.rewireChannelMonitor("system", error), error);
        check(0); check(1);
        graph.setChannelMonitorFx("system", true);
        require(graph.rewireChannelMonitor("system", error), error);
        check(0); check(1);
    }
    FilterHost::stop();
    std::cout << "Production graph preserves left/right through both mixes and FX toggles\n";
}

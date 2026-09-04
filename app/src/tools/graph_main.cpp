// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Brings the mixer topology up and holds it, so the graph can be inspected with
// pactl / pw-link before any daemon or GUI exists. Everything disappears on
// exit: nothing here sets object.linger.
//
//   waveline-graph [--seconds N] [--monitor-output SINK] [--route]
//
// --route moves applications that are ALREADY PLAYING onto channels. It is off
// by default because that is a disruptive thing to do to someone's audio.

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "engine/approuter.h"
#include "engine/mixergraph.h"
#include "engine/pwengine.h"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char **argv) {
    int seconds = 0;  // 0 = until interrupted
    bool route = false;
    std::string monitorOut;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--route"))
            route = true;
        else if (!std::strcmp(argv[i], "--monitor-output") && i + 1 < argc)
            monitorOut = argv[++i];
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    waveline::PwEngine engine;
    std::string error;
    if (!engine.start(error)) {
        std::fprintf(stderr, "engine: %s\n", error.c_str());
        return 1;
    }

    // Let the registry populate so the microphone can be found by name.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    waveline::MixerGraph graph(engine);
    if (!graph.build(error)) {
        std::fprintf(stderr, "graph: %s\n", error.c_str());
        return 1;
    }

    std::printf("graph up:\n");
    std::printf("  sinks apps can play into:\n");
    for (const auto &n : graph.channelSinkNames()) std::printf("    %s\n", n.c_str());
    std::printf("  mixes:\n    %s\n    %s\n", waveline::MixerGraph::kStreamMix,
                waveline::MixerGraph::kMonitorMix);

    if (!monitorOut.empty()) {
        if (!graph.setMonitorOutput(monitorOut, error))
            std::fprintf(stderr, "monitor output: %s\n", error.c_str());
        else
            std::printf("  monitor mix -> %s\n", monitorOut.c_str());
    }

    // A visible volume difference proves the per-path volumes are real and
    // independent, which is the whole point of the dual-mix design.
    graph.setVolume("music", waveline::Mix::Stream, 0.25f);
    graph.setVolume("music", waveline::Mix::Monitor, 1.0f);
    std::printf("  music: stream 25%%, monitor 100%% (independent, as a demo)\n");

    if (graph.noiseFilter()) {
        // Ports appear a moment after the modules load, so the explicit links
        // cannot be made inside build().
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        std::string wireErr;
        if (graph.wireNoiseFilter(wireErr))
            std::printf("  noise suppression: ON  (mic -> waveline-mic-nc -> both mixes)\n");
        else
            std::printf("  noise suppression: NOT wired: %s\n", wireErr.c_str());
    } else {
        std::printf("  noise suppression: unavailable\n");
    }
    std::fflush(stdout);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds ? seconds : 0);
    // Stage 4: send applications to the channel their name implies.
    //
    // OFF unless asked for. Routing moves audio that is already playing, and if
    // the monitor mix has no output the application goes silent and may not
    // recover by itself -- so this must never happen merely because the graph
    // was started.
    waveline::AppRouter router(engine);
    if (route) {
        std::string routeErr;
        if (router.start(routeErr))
            std::printf("  app routing: ON (%zu rules) -- live streams will move\n",
                        router.rules().size());
        else
            std::printf("  app routing: %s\n", routeErr.c_str());
    } else {
        std::printf("  app routing: off (pass --route to enable)\n");
    }
    std::fflush(stdout);

    int tick = 0;
    int lastRouted = -1;
    while (!g_stop) {
        if (const int n = static_cast<int>(router.routed().size()); n != lastRouted) {
            lastRouted = n;
            for (const auto &a : router.routed())
                if (!a.appName.empty())
                    std::printf("  routed: %-28s -> %s\n", a.appName.c_str(),
                                a.channelId.c_str());
            std::fflush(stdout);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Show the filter actually working, so "is it running?" needs no
        // separate tooling.
        if (auto *nc = graph.noiseFilter(); nc && ++tick % 5 == 0) {
            std::printf("  mic in %.5f -> out %.5f   speech %.2f\n",
                        nc->inputRms(), nc->outputRms(), nc->speechProbability());
            std::fflush(stdout);
        }
        if (seconds && std::chrono::steady_clock::now() >= deadline) break;
    }

    std::printf("tearing down\n");
    engine.stop();
    return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
//   waveline-nc --selftest    check the DSP offline, no PipeWire involved
//   waveline-nc [--seconds N] run the denoised-microphone node
//
// --selftest exists because the scaling bug RNNoise invites (feeding it -1..1
// instead of int16 scale) does not crash or warn: it just produces something
// very quiet. A numeric check catches it immediately.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <rnnoise.h>

#include "engine/noisefilter.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

double rms(const std::vector<float> &v) {
    if (v.empty()) return 0.0;
    double acc = 0.0;
    for (float s : v) acc += double(s) * s;
    return std::sqrt(acc / v.size());
}

// Runs a buffer through RNNoise exactly the way the filter does, including the
// int16 scaling and the dry/wet blend, so the two cannot drift apart.
std::vector<float> denoise(const std::vector<float> &in, bool int16Scale,
                           float wet = 1.0f) {
    DenoiseState *st = rnnoise_create(nullptr);
    const int frame = rnnoise_get_frame_size();
    std::vector<float> out;
    out.reserve(in.size());
    std::vector<float> a(frame), b(frame);
    for (size_t i = 0; i + frame <= in.size(); i += frame) {
        for (int j = 0; j < frame; ++j)
            a[j] = int16Scale ? in[i + j] * 32768.0f : in[i + j];
        rnnoise_process_frame(st, b.data(), a.data());
        for (int j = 0; j < frame; ++j) {
            const float denoised = int16Scale ? b[j] / 32768.0f : b[j];
            out.push_back(denoised * wet + in[i + j] * (1.0f - wet));
        }
    }
    rnnoise_destroy(st);
    return out;
}

int selftest() {
    std::printf("RNNoise frame size: %d samples (%.1f ms at 48 kHz)\n",
                rnnoise_get_frame_size(), rnnoise_get_frame_size() / 48.0);

    // Three seconds of white noise: no speech, so a working denoiser should
    // remove most of it.
    std::mt19937 rng(1234);
    std::normal_distribution<float> noise(0.0f, 0.05f);
    std::vector<float> in;
    in.reserve(48000 * 3);
    for (int i = 0; i < 48000 * 3; ++i) in.push_back(noise(rng));

    const auto correct = denoise(in, true);
    const auto wrong = denoise(in, false);

    // Skip the first second: RNNoise needs to settle on the noise profile.
    auto tail = [](const std::vector<float> &v) {
        return std::vector<float>(v.begin() + std::min<size_t>(48000, v.size()),
                                  v.end());
    };
    const double rIn = rms(tail(in));
    const double rOk = rms(tail(correct));
    const double rBad = rms(tail(wrong));

    std::printf("\nwhite noise input rms      : %.6f\n", rIn);
    std::printf("denoised, int16 scale      : %.6f  (%.1f dB reduction)\n", rOk,
                rOk > 0 ? 20 * std::log10(rIn / rOk) : 99.0);
    std::printf("denoised, WRONG -1..1 scale: %.6f  (%.1f dB)\n", rBad,
                rBad > 0 ? 20 * std::log10(rIn / rBad) : 99.0);

    // The intensity control is a dry/wet blend, so 0.0 must be bit-identical
    // to the input and 1.0 must be the fully denoised signal. Checked because
    // "it sounds different" is not something a live level reading can settle
    // when the input is speech and therefore never stationary.
    std::printf("\nintensity blend:\n");
    bool blendOk = true;
    for (float wet : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const auto mixed = denoise(in, true, wet);
        const double r = rms(tail(mixed));
        std::printf("  wet %.2f -> rms %.6f  (%.1f dB vs input)\n", wet, r,
                    r > 0 ? 20 * std::log10(r / rIn) : -99.0);
        if (wet == 0.0f && std::abs(r - rIn) > 1e-6) blendOk = false;
    }
    std::printf("  %s\n", blendOk
                              ? "0.00 reproduces the input exactly, as it must"
                              : "FAIL: 0.00 is not a clean passthrough");

    const bool ok = rOk < rIn * 0.5 && blendOk;
    std::printf("\n%s\n", ok ? "PASS: noise is suppressed with int16 scaling"
                             : "FAIL: no meaningful suppression -- check scaling");
    if (rBad > 0 && rOk > 0)
        std::printf("      (the wrong scaling differs by %.1f dB, which is why "
                    "it must be tested)\n",
                    20 * std::log10(rBad / rOk));
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    int seconds = 0;
    bool toggle = false;   // flip bypass every 4 s, to A/B it on live audio
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--selftest")) return selftest();
        if (!std::strcmp(argv[i], "--toggle")) toggle = true;
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    waveline::NoiseFilter nc;
    std::string error;
    if (!nc.start("waveline-mic-nc", "Wave:3 Microphone (Noise Suppressed)", error)) {
        std::fprintf(stderr, "noise filter: %s\n", error.c_str());
        return 1;
    }
    std::printf("noise suppression node up: waveline-mic-nc\n");
    std::printf("  link the Wave:3 mic into it, then select it as your source\n");
    std::fflush(stdout);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    int tick = 0;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (toggle && ++tick % 8 == 0) nc.setEnabled(!nc.enabled());
        std::printf("  in %.5f  out %.5f  speech %.2f  %s\n", nc.inputRms(),
                    nc.outputRms(), nc.speechProbability(),
                    nc.enabled() ? "" : "(bypassed)");
        std::fflush(stdout);
        if (seconds && std::chrono::steady_clock::now() >= deadline) break;
    }
    nc.stop();
    return 0;
}

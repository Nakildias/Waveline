// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Optional FluidSynth backend via dlopen. Bundled libfluidsynth.so is searched
// next to the daemon binary so users do not need a system install.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace waveline {

class FluidSynthBackend {
public:
    FluidSynthBackend();
    ~FluidSynthBackend();
    FluidSynthBackend(const FluidSynthBackend &) = delete;
    FluidSynthBackend &operator=(const FluidSynthBackend &) = delete;

    static bool available();
    static std::string libraryPath();

    bool init(int sampleRate, std::string &error);
    void shutdown();

    int loadSoundfont(const std::string &path, std::string &error);
    void unloadSoundfont(int id);
    void selectPreset(int sfontId, int bank, int preset);

    void handleMidiBytes(const uint8_t *data, std::size_t len);
    // Mono sum of FluidSynth's stereo render. The synth pans instruments and
    // spreads reverb/chorus across both channels, so taking one channel and
    // calling it mono drops half the mix -- quiet or missing notes wherever the
    // soundfont pans hard.
    void render(float *out, int frames);

    bool ready() const { return synth_ != nullptr; }

private:
    void *synth_ = nullptr;
    void *settings_ = nullptr;
    int activeSfont_ = -1;
    // Scratch for the stereo render, summed into the mono port by render().
    // Sized in init() so the audio thread does not allocate.
    std::vector<float> left_;
    std::vector<float> right_;
};

}  // namespace waveline

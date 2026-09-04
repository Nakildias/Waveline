// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The noise suppression back end, behind one interface so the filter node does
// not care which model is running.
//
// Two are available:
//
//   * RNNoise -- BSD-3-Clause, linked at build time. Tiny, ~0.1 ms per frame,
//     trained on speech. The default, and the only one that is always present.
//   * DeepFilterNet -- MIT OR Apache-2.0, loaded at *runtime* with dlopen.
//     Much better on steady broadband noise (fans, keyboards, street), at
//     perhaps twenty times the CPU. See THIRD-PARTY.md for the licence
//     reasoning.
//
// DeepFilterNet is deliberately not a build dependency. It ships no pkg-config
// file, it is packaged under a different name on every distribution when it is
// packaged at all, and requiring it would mean nobody could build this without
// first installing a Rust toolchain. dlopen keeps the binary buildable and
// runnable everywhere and simply reports the engine as unavailable when the
// library is missing.
//
// Both back ends take and return samples in -1..1 at 48 kHz, mono, in fixed
// frames of frameSize(). The frame size differs between them (480 for both in
// practice, but DeepFilterNet reports its own hop size), so callers must ask
// rather than assume.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace waveline {

enum class NoiseEngine {
    RnNoise,
    DeepFilterNet,
};

// Round-trips through the config file and D-Bus, so these strings are API.
const char *noiseEngineId(NoiseEngine engine);
NoiseEngine noiseEngineFromId(const std::string &id);  // unknown -> RnNoise

class Denoiser {
public:
    virtual ~Denoiser() = default;

    // Samples per call to processFrame(). Constant for the lifetime of the
    // object.
    virtual int frameSize() const = 0;

    // Denoises exactly frameSize() samples, -1..1. `in` and `out` may not
    // overlap. Returns a 0..1 "there is speech here" indicator for the UI.
    virtual float processFrame(const float *in, float *out) = 0;

    virtual NoiseEngine engine() const = 0;
};

// Builds a back end, or returns null and sets `error` to something a user can
// act on ("install deepfilternet", "model not found at ..."). Never throws and
// never aborts: an engine that cannot start must leave the caller free to fall
// back to RNNoise.
std::unique_ptr<Denoiser> makeDenoiser(NoiseEngine engine, std::string &error);

// Whether makeDenoiser() would succeed, without building anything. `reason` is
// filled in when it would not. Cached after the first call, because the UI asks
// on every refresh and the answer involves hitting the filesystem.
bool noiseEngineAvailable(NoiseEngine engine, std::string &reason);

// Where DeepFilterNet was found, for diagnostics. Empty when it was not.
std::string deepFilterNetLibraryPath();
std::string deepFilterNetModelPath();

}  // namespace waveline

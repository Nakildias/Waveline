// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Noise suppression, built on RNNoise.
//
//   RNNoise is Copyright (c) 2007-2017, 2024 Jean-Marc Valin, (c) 2023 Amazon,
//   (c) 2017 Mozilla, (c) 2005-2017 Xiph.Org Foundation, (c) 2003-2004 Mark
//   Borgerding, and is licensed BSD-3-Clause. See LICENSES/ and THIRD-PARTY.md.
//
// BSD-3-Clause is GPL-compatible, so linking it into this GPL program is fine;
// its notice has to survive into anything built and shipped from here, which is
// what that licence file is for. No RNNoise code is vendored -- the build links
// the system librnnoise.
//
// Noise suppression as a real PipeWire filter node.
//
// This is not a wrapper around the noise-suppression-for-voice LADSPA plugin:
// the model is driven directly and the DSP runs in our own pw_filter process
// callback. That removes a distro package dependency that is named differently
// (or missing) on every distribution, and it lets the filter be toggled and
// tuned at runtime rather than at graph-load time.
//
// The model itself lives behind Denoiser (see denoiser.h), so RNNoise and
// DeepFilterNet are interchangeable. Switching between them does not touch the
// node or its links -- important, because rewiring this graph is the part that
// has historically gone wrong.
//
// The node exposes raw DSP ports and is linked inside our graph (mic -> filter
// -> mixes). It is NOT yet a device other applications can pick from a list:
// that needs an adapter wrapper around the filter, the way module-filter-chain
// does it. Until then the denoised microphone reaches consumers through the
// stream and monitor mixes, which is what matters for streaming.
//
// Both engines work on fixed frames at 48 kHz -- 480 samples in practice, but
// each reports its own -- while PipeWire's quantum is whatever the graph
// negotiated, so samples are buffered in both directions. (The int16 scaling
// RNNoise wants is Denoiser's problem now, not this file's.)

#pragma once

#include "denoiser.h"

#include <memory>
#include <string>
#include <vector>

struct pw_thread_loop;

namespace waveline {

class NoiseFilter {
public:
    NoiseFilter();
    ~NoiseFilter();
    NoiseFilter(const NoiseFilter &) = delete;
    NoiseFilter &operator=(const NoiseFilter &) = delete;

    // `sourceName` is the node the filter takes audio from, normally the
    // microphone capture node. Runs on its own thread loop.
    // asSource publishes the node as an Audio/Source so applications can pick
    // it as a recording device. Off by default: the per-channel filters are
    // internal plumbing and listing all twenty of them buries the one source a
    // user actually wants to choose.
    // `engine` falls back to RNNoise if the requested one is unavailable, so a
    // saved DeepFilterNet setting cannot stop the graph from coming up on a
    // machine where it is not installed.
    bool start(const std::string &nodeName, const std::string &description,
               std::string &error, bool asSource = false,
               NoiseEngine engine = NoiseEngine::RnNoise);
    void stop();

    // Bypass keeps the node and its links in place and just stops denoising,
    // so an A/B comparison does not tear the graph down and back up.
    void setEnabled(bool on);
    bool enabled() const;

    // 0.0 .. 1.0. Neither engine has a natural strength control -- RNNoise
    // either denoises a frame or it does not -- so this blends the denoised
    // signal back with the original: 1.0 is the full effect, 0.5 keeps half the
    // original room tone, 0.0 is untouched. Partial settings sound
    // considerably more natural than full suppression, which can make speech
    // pump and sound gated. Applied identically for both engines, so the
    // slider means the same thing whichever is selected.
    void setIntensity(float intensity);
    float intensity() const;

    // Swaps the model without disturbing the node, its ports or its links.
    // The new back end is built on the calling thread and published
    // atomically; the process callback passes audio through untouched for the
    // one quantum the swap takes.
    //
    // Returns false and leaves the current engine running if the requested one
    // cannot start -- DeepFilterNet is loaded at runtime and may simply not be
    // installed. `error` then says why, in terms a user can act on.
    bool setEngine(NoiseEngine engine, std::string &error);
    NoiseEngine engine() const;

    // Last frame's speech probability from RNNoise, 0..1. Useful as a
    // "is it working" indicator in the UI.
    float speechProbability() const;

    // RMS seen at the filter's input and output. Measured inside the process
    // callback, so it proves audio is actually traversing the node without
    // depending on capturing it from elsewhere.
    float inputRms() const;
    float outputRms() const;

    static int frameSize();  // 480 at 48 kHz

    // Public only so the realtime process callback can name it.
    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

}  // namespace waveline

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Third-party models driven from here. Neither is vendored; both are the
// user's own system libraries. See THIRD-PARTY.md and LICENSES/.
//
//   RNNoise is Copyright (c) 2007-2017, 2024 Jean-Marc Valin, (c) 2023 Amazon,
//   (c) 2017 Mozilla, (c) 2005-2017 Xiph.Org Foundation, (c) 2003-2004 Mark
//   Borgerding, and is licensed BSD-3-Clause.
//
//   DeepFilterNet is Copyright (c) 2021 Hendrik Schröter and is dual-licensed
//   MIT OR Apache-2.0. This project takes it under the **MIT** option: the
//   Apache-2.0 option's patent and indemnity terms are "further restrictions"
//   under GPLv2 section 6 and so are incompatible with GPL-2.0-only, while MIT
//   is compatible with GPLv2 and GPLv3 alike. Nothing here links it -- it is
//   dlopen()ed if the user installed it -- so a checkout of this repository
//   carries no DeepFilterNet notice obligation.

#include "denoiser.h"

#include <rnnoise.h>

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace waveline {
namespace {

bool isFile(const std::string &path) {
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string envOr(const char *name) {
    const char *v = ::getenv(name);
    return v ? std::string(v) : std::string();
}

// The WAVELINE_* name, falling back to the WAVE3_* one this project used before
// it was renamed. Someone who put an override in a systemd drop-in months ago
// should not find it silently ignored after an upgrade.
std::string envOverride(const char *name, const char *legacy) {
    const std::string v = envOr(name);
    return v.empty() ? envOr(legacy) : v;
}

std::string homeDir() {
    const std::string h = envOr("HOME");
    return h;
}

// ------------------------------------------------------------------ RNNoise

class RnNoiseDenoiser final : public Denoiser {
public:
    ~RnNoiseDenoiser() override {
        if (state_) rnnoise_destroy(state_);
    }

    bool init(std::string &error) {
        frame_ = rnnoise_get_frame_size();
        state_ = rnnoise_create(nullptr);
        if (!state_) { error = "rnnoise_create failed"; return false; }
        scratchIn_.assign(frame_, 0.0f);
        scratchOut_.assign(frame_, 0.0f);
        return true;
    }

    int frameSize() const override { return frame_; }
    NoiseEngine engine() const override { return NoiseEngine::RnNoise; }

    float processFrame(const float *in, float *out) override {
        // RNNoise wants int16 scale, not the -1..1 PipeWire deals in. Feeding
        // it -1..1 produces near-silence rather than an obvious failure, which
        // is exactly the kind of bug that costs an evening.
        for (int i = 0; i < frame_; ++i) scratchIn_[i] = in[i] * 32768.0f;
        const float vad = rnnoise_process_frame(state_, scratchOut_.data(),
                                                scratchIn_.data());
        for (int i = 0; i < frame_; ++i) out[i] = scratchOut_[i] / 32768.0f;
        return vad;
    }

private:
    DenoiseState *state_ = nullptr;
    int frame_ = 480;
    std::vector<float> scratchIn_;
    std::vector<float> scratchOut_;
};

// ------------------------------------------------------------ DeepFilterNet

// The handful of libdf entry points we need. Names are unmangled `df_*`;
// libDF declares them #[no_mangle] extern "C".
struct DfApi {
    void *handle = nullptr;
    void *(*create)(const char *path, float attenLim, const char *logLevel) = nullptr;
    void (*destroy)(void *st) = nullptr;
    size_t (*frameLength)(void *st) = nullptr;
    float (*processFrame)(void *st, float *in, float *out) = nullptr;
    void (*setAttenLim)(void *st, float limDb) = nullptr;
    void (*setPostFilterBeta)(void *st, float beta) = nullptr;
    std::string path;
};

// Tried in order. The first two are what a distribution package installs; the
// rest cover a manual `cargo build --release` drop-in.
const char *const kDfLibNames[] = {
    "libdf.so",
    "libdf.so.0",
    "libdeep_filter.so",
    "libdeep_filter_ladspa.so",
};

std::vector<std::string> dfLibCandidates() {
    std::vector<std::string> out;
    const std::string override = envOverride("WAVELINE_DFN_LIB", "WAVE3_DFN_LIB");
    if (!override.empty()) out.push_back(override);
    for (const char *n : kDfLibNames) {
        out.emplace_back(n);  // let the dynamic loader search its own paths
        out.emplace_back(std::string("/usr/lib/") + n);
        out.emplace_back(std::string("/usr/local/lib/") + n);
        out.emplace_back(std::string("/usr/lib/deepfilternet/") + n);
    }
    const std::string home = homeDir();
    if (!home.empty()) {
        for (const char *n : kDfLibNames)
            out.push_back(home + "/.local/lib/" + n);
    }
    return out;
}

// DeepFilterNet3 first: it is the current model and the one upstream
// recommends. `_ll` is the low-latency variant, a reasonable fallback.
const char *const kDfModelNames[] = {
    "DeepFilterNet3_onnx.tar.gz",
    "DeepFilterNet3_ll_onnx.tar.gz",
    "DeepFilterNet2_onnx.tar.gz",
    "DeepFilterNet2_onnx_ll.tar.gz",
};

std::vector<std::string> dfModelCandidates() {
    std::vector<std::string> dirs = {
        "/usr/share/deepfilternet",
        "/usr/share/DeepFilterNet",
        "/usr/share/deep-filter",
        "/usr/lib/deepfilternet",
        "/usr/local/share/deepfilternet",
    };
    const std::string home = homeDir();
    if (!home.empty()) {
        // Where we tell users to drop the tarball; checked first so a hand
        // placed model wins over a stale packaged one.
        dirs.insert(dirs.begin(), home + "/.local/share/waveline/models");
        // The pre-rename location, still searched so an upgrade does not have
        // to re-download a 23 MB model that is already on the disk.
        dirs.insert(dirs.begin() + 1, home + "/.local/share/wave3/models");
        dirs.insert(dirs.begin() + 2, home + "/.local/share/deepfilternet");
    }

    std::vector<std::string> out;
    const std::string override =
        envOverride("WAVELINE_DFN_MODEL", "WAVE3_DFN_MODEL");
    if (!override.empty()) out.push_back(override);
    for (const std::string &d : dirs)
        for (const char *m : kDfModelNames) out.push_back(d + "/" + m);
    return out;
}

std::string findDfModel() {
    for (const std::string &c : dfModelCandidates())
        if (isFile(c)) return c;
    return {};
}

// Loaded once and kept: dlclose-ing a library that spawned worker threads (and
// libdf does, through ONNX Runtime) is a reliable way to crash at exit.
DfApi *loadDfApi(std::string &error) {
    static DfApi api;
    static std::once_flag once;
    static std::string loadError;

    std::call_once(once, [] {
        for (const std::string &cand : dfLibCandidates()) {
            void *h = ::dlopen(cand.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!h) continue;

            auto sym = [h](const char *n) { return ::dlsym(h, n); };
            auto create = reinterpret_cast<void *(*)(const char *, float, const char *)>(
                sym("df_create"));
            auto destroy = reinterpret_cast<void (*)(void *)>(sym("df_free"));
            auto len = reinterpret_cast<size_t (*)(void *)>(sym("df_get_frame_length"));
            auto proc = reinterpret_cast<float (*)(void *, float *, float *)>(
                sym("df_process_frame"));

            if (!create || !destroy || !len || !proc) {
                // Something called libdf.so that is not libDF, or a build with
                // the C API feature turned off.
                ::dlclose(h);
                continue;
            }
            api.handle = h;
            api.create = create;
            api.destroy = destroy;
            api.frameLength = len;
            api.processFrame = proc;
            api.setAttenLim =
                reinterpret_cast<void (*)(void *, float)>(sym("df_set_atten_lim"));
            api.setPostFilterBeta =
                reinterpret_cast<void (*)(void *, float)>(sym("df_set_post_filter_beta"));
            api.path = cand;
            return;
        }
        loadError =
            "libdf.so not found -- install DeepFilterNet (the deep-filter "
            "package, or build libDF with --features capi) or set\n"
            "WAVELINE_DFN_LIB";
    });

    if (!api.handle) { error = loadError; return nullptr; }
    return &api;
}

class DeepFilterNetDenoiser final : public Denoiser {
public:
    ~DeepFilterNetDenoiser() override {
        if (state_ && api_) api_->destroy(state_);
    }

    bool init(std::string &error) {
        api_ = loadDfApi(error);
        if (!api_) return false;

        const std::string model = findDfModel();
        if (model.empty()) {
            error =
                "DeepFilterNet model not found -- put DeepFilterNet3_onnx.tar.gz "
                "in ~/.local/share/waveline/models/ or set WAVELINE_DFN_MODEL";
            return false;
        }

        // df_create unwraps both the path and the archive, and a Rust panic
        // crossing back into C++ aborts the daemon rather than raising. The
        // existence check above is what keeps the common failure (no model)
        // out of that path.
        state_ = api_->create(model.c_str(), kAttenLimDb, "warn");
        if (!state_) { error = "df_create failed for " + model; return false; }

        frame_ = static_cast<int>(api_->frameLength(state_));
        if (frame_ <= 0 || frame_ > 8192) {
            api_->destroy(state_);
            state_ = nullptr;
            error = "df_get_frame_length returned an implausible size";
            return false;
        }

        // The intensity control is a dry/wet blend applied by NoiseFilter, the
        // same for every engine. Leaving attenuation unlimited here means the
        // slider is the only thing shaping the result, so switching engines
        // does not silently change what 50% means.
        if (api_->setAttenLim) api_->setAttenLim(state_, kAttenLimDb);
        if (api_->setPostFilterBeta) api_->setPostFilterBeta(state_, 0.02f);

        scratchIn_.assign(frame_, 0.0f);
        scratchOut_.assign(frame_, 0.0f);
        modelPath_ = model;
        return true;
    }

    int frameSize() const override { return frame_; }
    NoiseEngine engine() const override { return NoiseEngine::DeepFilterNet; }

    float processFrame(const float *in, float *out) override {
        // df_process_frame takes non-const pointers and reads exactly
        // frameSize() samples; the copy is what guarantees that, since `in`
        // comes straight from a PipeWire buffer.
        std::memcpy(scratchIn_.data(), in, size_t(frame_) * sizeof(float));
        const float snrDb = api_->processFrame(state_, scratchIn_.data(),
                                               scratchOut_.data());
        std::memcpy(out, scratchOut_.data(), size_t(frame_) * sizeof(float));

        // DeepFilterNet reports local SNR in dB, not a speech probability.
        // Mapped onto 0..1 so the UI's "is it working" indicator behaves the
        // same for both engines: below -5 dB is noise, above 25 dB is clean
        // speech. This is an indicator, not a measurement.
        const float t = (snrDb + 5.0f) / 30.0f;
        return std::clamp(t, 0.0f, 1.0f);
    }

    const std::string &modelPath() const { return modelPath_; }

private:
    // 100 dB is "do not limit". DeepFilterNet treats a finite limit as a floor
    // on how much noise may be removed.
    static constexpr float kAttenLimDb = 100.0f;

    DfApi *api_ = nullptr;
    void *state_ = nullptr;
    int frame_ = 480;
    std::vector<float> scratchIn_;
    std::vector<float> scratchOut_;
    std::string modelPath_;
};

}  // namespace

// ------------------------------------------------------------------- public

const char *noiseEngineId(NoiseEngine engine) {
    switch (engine) {
        case NoiseEngine::DeepFilterNet: return "deepfilternet";
        case NoiseEngine::RnNoise: break;
    }
    return "rnnoise";
}

NoiseEngine noiseEngineFromId(const std::string &id) {
    if (id == "deepfilternet" || id == "dfn" || id == "deepfilter")
        return NoiseEngine::DeepFilterNet;
    return NoiseEngine::RnNoise;
}

std::unique_ptr<Denoiser> makeDenoiser(NoiseEngine engine, std::string &error) {
    if (engine == NoiseEngine::DeepFilterNet) {
        auto d = std::make_unique<DeepFilterNetDenoiser>();
        if (!d->init(error)) return nullptr;
        return d;
    }
    auto d = std::make_unique<RnNoiseDenoiser>();
    if (!d->init(error)) return nullptr;
    return d;
}

bool noiseEngineAvailable(NoiseEngine engine, std::string &reason) {
    if (engine != NoiseEngine::DeepFilterNet) return true;

    // Answered from the library and model lookups rather than by building a
    // state: constructing a DeepFilterNet costs hundreds of milliseconds and
    // the UI asks this on every refresh.
    std::string libError;
    if (!loadDfApi(libError)) { reason = libError; return false; }
    if (findDfModel().empty()) {
        reason =
            "DeepFilterNet model not found -- put DeepFilterNet3_onnx.tar.gz in "
            "~/.local/share/waveline/models/ or set WAVELINE_DFN_MODEL";
        return false;
    }
    reason.clear();
    return true;
}

std::string deepFilterNetLibraryPath() {
    std::string error;
    DfApi *api = loadDfApi(error);
    return api ? api->path : std::string();
}

std::string deepFilterNetModelPath() { return findDfModel(); }

}  // namespace waveline

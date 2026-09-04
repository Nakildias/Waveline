// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Sidechain ducking: smoothly reduce program audio when sidechain sources
// are active (microphones or another channel's app audio).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace waveline {

inline constexpr int kMaxDuckingSources = 6;

enum class DuckingSourceKind { MasterMic, ChannelMic, ChannelAudio };

struct DuckingSourceRef {
    DuckingSourceKind kind = DuckingSourceKind::MasterMic;
    std::string channelId;

    bool operator==(const DuckingSourceRef &o) const {
        return kind == o.kind && channelId == o.channelId;
    }
    bool operator!=(const DuckingSourceRef &o) const { return !(*this == o); }
};

struct DuckingSettings {
    bool enabled = false;
    float intensity = 0.75f;
    std::vector<DuckingSourceRef> sources;
    float thresholdDb = -32.0f;
    float depthDb = -18.0f;
    float attackSec = 0.10f;
    float releaseSec = 0.45f;
    float sidechainAttackSec = 0.004f;
    float sidechainReleaseSec = 0.12f;
    // How long the sidechain has to stay below the threshold before the duck
    // starts letting go. Pauses between words do not lift the program audio.
    float holdSec = 3.0f;

    bool active() const { return enabled && !sources.empty(); }
};

class DuckingProcessor {
public:
    explicit DuckingProcessor(float sampleRate = 48000.0f);

    void setSampleRate(float rate);
    void setSettings(const DuckingSettings &settings);
    void reset();

    void process(float *left, float *right,
                 const float *sidechains[kMaxDuckingSources],
                 const bool sidechainPresent[kMaxDuckingSources], uint32_t n);

private:
    void rebuild();

    static float dbToLinear(float db);
    static float linearToDb(float linear);
    static float smoothCoeff(float timeSec, float sampleRate);

    float sampleRate_ = 48000.0f;
    DuckingSettings settings_{};

    float sidechainEnv_ = 0.0f;
    float gain_ = 1.0f;
    uint64_t holdCounter_ = 0;

    float scAttack_ = 0.0f;
    float scRelease_ = 0.0f;
    float duckAttack_ = 0.0f;
    float duckRelease_ = 0.0f;
    float minGainLinear_ = 1.0f;
    float thresholdLinear_ = 0.0f;
    uint64_t holdSamples_ = 0;
};

}  // namespace waveline

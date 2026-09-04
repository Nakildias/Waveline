// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// BS.1770-style loudness limiter for app-audio ear protection.

#pragma once

#include <cstdint>

namespace waveline {

struct LufsLimiterSettings {
    bool enabled = false;
    float maxLufs = -18.0f;

    bool active() const { return enabled; }
};

class LufsLimiterProcessor {
public:
    explicit LufsLimiterProcessor(float sampleRate = 48000.0f);

    void setSampleRate(float rate);
    void setSettings(const LufsLimiterSettings &settings);
    void reset();

    void process(float *left, float *right, uint32_t n);

private:
    struct Biquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;

        float process(float x);
        void reset() { z1 = z2 = 0.0f; }
    };

    float kWeight(float x, Biquad &s1, Biquad &s2) const;
    static float dbToLinear(float db);
    static float linearToDb(float linear);
    static float smoothCoeff(float timeSec, float sampleRate);

    float sampleRate_ = 48000.0f;
    LufsLimiterSettings settings_{};
    Biquad kStage1L_{}, kStage1R_{}, kStage2L_{}, kStage2R_{};
    float gainDb_ = 0.0f;
    float blockSum_ = 0.0f;
    uint32_t blockCount_ = 0;
    float attackCoeff_ = 0.0f;
    float releaseCoeff_ = 0.0f;
};

}  // namespace waveline

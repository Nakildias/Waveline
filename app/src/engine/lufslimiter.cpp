// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "lufslimiter.h"

#include <algorithm>
#include <cmath>

namespace waveline {
namespace {

constexpr float kMinDb = -96.0f;
constexpr float kMinLinear = 1.0e-12f;
constexpr float kLufsOffset = -0.691f;
constexpr uint32_t kMeasureBlock = 480;

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

}  // namespace

float LufsLimiterProcessor::Biquad::process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}

LufsLimiterProcessor::LufsLimiterProcessor(float sampleRate) {
    setSampleRate(sampleRate);
    setSettings({});
}

void LufsLimiterProcessor::setSampleRate(float rate) {
    sampleRate_ = std::max(rate, 8000.0f);
    attackCoeff_ = smoothCoeff(0.003f, sampleRate_);
    releaseCoeff_ = smoothCoeff(0.15f, sampleRate_);
    kStage1L_ = {1.53512485958697f, -2.69169618940638f, 1.19839281085285f,
                 -1.69065929318241f, 0.73248077421585f};
    kStage1R_ = kStage1L_;
    kStage2L_ = {1.0f, -2.0f, 1.0f, -1.99004745483398f, 0.99007225036621f};
    kStage2R_ = kStage2L_;
}

void LufsLimiterProcessor::setSettings(const LufsLimiterSettings &settings) {
    settings_ = settings;
    settings_.maxLufs = clampf(settings_.maxLufs, -40.0f, -6.0f);
}

void LufsLimiterProcessor::reset() {
    kStage1L_.reset();
    kStage1R_.reset();
    kStage2L_.reset();
    kStage2R_.reset();
    gainDb_ = 0.0f;
    blockSum_ = 0.0f;
    blockCount_ = 0;
}

float LufsLimiterProcessor::kWeight(float x, Biquad &s1, Biquad &s2) const {
    return s2.process(s1.process(x));
}

float LufsLimiterProcessor::dbToLinear(float db) {
    return std::pow(10.0f, db * 0.05f);
}

float LufsLimiterProcessor::linearToDb(float linear) {
    if (linear <= kMinLinear) return kMinDb;
    return 20.0f * std::log10(linear);
}

float LufsLimiterProcessor::smoothCoeff(float timeSec, float sampleRate) {
    timeSec = std::max(timeSec, 0.001f);
    return std::exp(-1.0f / (timeSec * sampleRate));
}

void LufsLimiterProcessor::process(float *left, float *right, uint32_t n) {
    if (!left || !right || n == 0) return;
    if (!settings_.enabled) {
        gainDb_ = 0.0f;
        blockSum_ = 0.0f;
        blockCount_ = 0;
        return;
    }

    const float maxLufs = settings_.maxLufs;

    for (uint32_t i = 0; i < n; ++i) {
        const float wL = kWeight(left[i], kStage1L_, kStage2L_);
        const float wR = kWeight(right[i], kStage1R_, kStage2R_);
        blockSum_ += wL * wL + wR * wR;
        ++blockCount_;

        if (blockCount_ >= kMeasureBlock) {
            const float meanSquare =
                std::max(blockSum_ / float(blockCount_), kMinLinear);
            const float momentaryLufs = kLufsOffset + 10.0f * std::log10(meanSquare);
            float targetGainDb = 0.0f;
            if (momentaryLufs > maxLufs) targetGainDb = maxLufs - momentaryLufs;

            const float coeff = targetGainDb < gainDb_ ? attackCoeff_ : releaseCoeff_;
            gainDb_ = coeff * gainDb_ + (1.0f - coeff) * targetGainDb;
            blockSum_ = 0.0f;
            blockCount_ = 0;
        }

        const float gain = dbToLinear(gainDb_);
        left[i] *= gain;
        right[i] *= gain;
    }
}

}  // namespace waveline

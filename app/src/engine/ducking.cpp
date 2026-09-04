// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "ducking.h"

#include <algorithm>
#include <cmath>

namespace waveline {
namespace {

constexpr float kMinDb = -96.0f;
constexpr float kMaxDb = 0.0f;
constexpr float kMinTimeSec = 0.001f;
constexpr float kMaxTimeSec = 4.0f;
constexpr float kMaxHoldSec = 10.0f;

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

}  // namespace

DuckingProcessor::DuckingProcessor(float sampleRate) {
    setSampleRate(sampleRate);
    setSettings({});
}

void DuckingProcessor::setSampleRate(float rate) {
    sampleRate_ = std::max(rate, 8000.0f);
    rebuild();
}

void DuckingProcessor::setSettings(const DuckingSettings &settings) {
    settings_ = settings;
    settings_.intensity = clampf(settings_.intensity, 0.0f, 1.0f);
    if (settings_.sources.size() > kMaxDuckingSources) {
        settings_.sources.resize(kMaxDuckingSources);
    }
    rebuild();
}

void DuckingProcessor::reset() {
    sidechainEnv_ = 0.0f;
    gain_ = 1.0f;
    holdCounter_ = 0;
}

float DuckingProcessor::dbToLinear(float db) {
    return std::pow(10.0f, db * 0.05f);
}

float DuckingProcessor::linearToDb(float linear) {
    if (linear <= 1.0e-9f) return kMinDb;
    return 20.0f * std::log10(linear);
}

float DuckingProcessor::smoothCoeff(float timeSec, float sampleRate) {
    timeSec = clampf(timeSec, kMinTimeSec, kMaxTimeSec);
    return std::exp(-1.0f / (timeSec * sampleRate));
}

void DuckingProcessor::rebuild() {
    scAttack_ = smoothCoeff(settings_.sidechainAttackSec, sampleRate_);
    scRelease_ = smoothCoeff(settings_.sidechainReleaseSec, sampleRate_);
    duckAttack_ = smoothCoeff(settings_.attackSec, sampleRate_);
    duckRelease_ = smoothCoeff(settings_.releaseSec, sampleRate_);
    minGainLinear_ = dbToLinear(clampf(settings_.depthDb, kMinDb, 0.0f));
    thresholdLinear_ = dbToLinear(clampf(settings_.thresholdDb, kMinDb, kMaxDb));
    holdSamples_ = static_cast<uint64_t>(clampf(settings_.holdSec, 0.0f, kMaxHoldSec) *
                                         sampleRate_);
    // Shortening the hold mid-count must take effect now, not after the old,
    // longer count runs out.
    if (holdCounter_ > holdSamples_) holdCounter_ = holdSamples_;
}

void DuckingProcessor::process(float *left, float *right,
                               const float *sidechains[kMaxDuckingSources],
                               const bool sidechainPresent[kMaxDuckingSources],
                               uint32_t n) {
    if (!left || !right || n == 0) return;

    if (!settings_.enabled) {
        gain_ = 1.0f;
        return;
    }

    const float thresholdDb = clampf(settings_.thresholdDb, kMinDb, kMaxDb);
    const float intensity = settings_.intensity;

    for (uint32_t i = 0; i < n; ++i) {
        float peak = 0.0f;
        for (int s = 0; s < kMaxDuckingSources; ++s) {
            if (!sidechainPresent[s] || !sidechains[s]) continue;
            peak = std::max(peak, std::fabs(sidechains[s][i]));
        }

        // Any sample above the threshold re-arms the hold. It counts down only
        // through real silence, so pauses between words never lift the duck.
        if (peak > thresholdLinear_)
            holdCounter_ = holdSamples_;
        else if (holdCounter_ > 0)
            --holdCounter_;

        // While the hold is armed the detector may only rise. Freezing its
        // decay is what keeps the gain parked at the current duck depth: the
        // release below cannot start until the envelope is allowed to fall.
        const float scCoeff = peak > sidechainEnv_ ? scAttack_ : scRelease_;
        if (peak > sidechainEnv_ || holdCounter_ == 0)
            sidechainEnv_ = scCoeff * sidechainEnv_ + (1.0f - scCoeff) * peak;

        float targetGain = 1.0f;
        const float envDb = linearToDb(sidechainEnv_);
        if (envDb > thresholdDb) {
            const float over =
                clampf((envDb - thresholdDb) / 24.0f, 0.0f, 1.0f);
            const float duck = intensity * over;
            targetGain = 1.0f - duck * (1.0f - minGainLinear_);
        }

        const float gainCoeff = targetGain < gain_ ? duckAttack_ : duckRelease_;
        gain_ = gainCoeff * gain_ + (1.0f - gainCoeff) * targetGain;

        left[i] *= gain_;
        right[i] *= gain_;
    }
}

}  // namespace waveline

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "dynamics.h"

#include <algorithm>
#include <cmath>

namespace waveline {
namespace {

constexpr float kMinDb = -96.0f;
constexpr float kMaxDb = 0.0f;
constexpr float kMinTimeSec = 0.0005f;
constexpr float kMaxTimeSec = 4.0f;
constexpr float kMinRatio = 1.0f;
constexpr float kMaxRatio = 60.0f;

// Sibilance sits between about 5 and 9 kHz for most voices. The corner is put
// at the bottom of that: everything above is what the de-esser is allowed to
// hold down, and a corner any higher leaves the "sss" that a close mic makes
// worst untouched.
constexpr float kDeEsserSplitHz = 5500.0f;
// Fast enough to catch the front of an "s", slow enough not to chew the vowel
// after it. Deliberately not user-visible: these two numbers have one sensible
// answer for speech and every value either side of it sounds broken.
constexpr float kDeEsserAttackSec = 0.0015f;
constexpr float kDeEsserReleaseSec = 0.060f;
// How often the shelf is rebuilt from the smoothed gain. Every 16 samples is a
// third of a millisecond at 48 kHz -- far finer than the 1.5 ms attack, so the
// shelf never lags what the detector asked for -- and it keeps three trig
// calls off all but one sample in sixteen.
constexpr int kDeEsserShelfInterval = 16;
// Rebuilding for a change too small to hear is wasted work; a fifth of a dB is
// well under what a listener can pick out on a shelf.
constexpr float kDeEsserShelfEpsilonDb = 0.2f;

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

}  // namespace

DynamicsProcessor::DynamicsProcessor(float sampleRate) {
    setSampleRate(sampleRate);
    setSettings({});
}

void DynamicsProcessor::setSampleRate(float rate) {
    sampleRate_ = std::max(rate, 8000.0f);
    rebuild();
}

void DynamicsProcessor::setSettings(const DynamicsSettings &settings) {
    settings_ = settings;
    rebuild();
}

void DynamicsProcessor::reset() {
    envLinear_ = 0.0f;
    gateGainDb_ = 0.0f;
    compGainDb_ = 0.0f;
    limitGainDb_ = 0.0f;
    makeupSmoothDb_ = makeupDb_;
    deEsserHp1_.reset();
    deEsserHp2_.reset();
    deEsserShelf_.reset();
    deEsserShelfGainDb_ = 0.0f;
    deEsserShelfAge_ = 0;
    deEsserEnv_ = 0.0f;
    deEsserGainDb_ = 0.0f;
    deEsserThresholdDb_ = deEsserThresholdTargetDb_;
    deEsserRatioInv_ = deEsserRatioInvTarget_;
    deEsserFloorDb_ = deEsserFloorTargetDb_;
}

float DynamicsProcessor::dbToLinear(float db) {
    return std::pow(10.0f, db * 0.05f);
}

float DynamicsProcessor::linearToDb(float linear) {
    if (linear <= 1.0e-9f) return kMinDb;
    return 20.0f * std::log10(linear);
}

float DynamicsProcessor::smoothCoeff(float timeSec, float sampleRate) {
    timeSec = clampf(timeSec, kMinTimeSec, kMaxTimeSec);
    return std::exp(-1.0f / (timeSec * sampleRate));
}

void DynamicsProcessor::rebuild() {
    gateOpenDb_ = clampf(settings_.gateThresholdDb, kMinDb, kMaxDb);
    gateCloseDb_ = gateOpenDb_ - clampf(settings_.gateHysteresisDb, 0.0f, 12.0f);
    gateAttack_ = smoothCoeff(settings_.gateAttackSec, sampleRate_);
    gateRelease_ = smoothCoeff(settings_.gateReleaseSec, sampleRate_);

    compThresholdDb_ = clampf(settings_.compThresholdDb, kMinDb, kMaxDb);
    const float ratio = clampf(settings_.compRatio, kMinRatio, kMaxRatio);
    compRatioInv_ = 1.0f / ratio;
    compAttack_ = smoothCoeff(settings_.compAttackSec, sampleRate_);
    compRelease_ = smoothCoeff(settings_.compReleaseSec, sampleRate_);

    const float knee = clampf(settings_.compKneeDb, 0.0f, 24.0f);
    compKneeHalf_ = knee * 0.5f;
    compKneeScale_ = (knee > 0.0f) ? (compRatioInv_ - 1.0f) / (2.0f * knee) : 0.0f;
    compKneeOffset_ = compThresholdDb_ - compKneeHalf_;

    if (settings_.autoMakeup && settings_.compressor)
        makeupDb_ = -compThresholdDb_ * (1.0f - compRatioInv_);
    else
        makeupDb_ = clampf(settings_.makeupGainDb, -12.0f, 24.0f);

    limitThresholdDb_ = clampf(settings_.limitThresholdDb, kMinDb, kMaxDb);
    limitAttack_ = smoothCoeff(settings_.limitAttackSec, sampleRate_);
    limitRelease_ = smoothCoeff(settings_.limitReleaseSec, sampleRate_);

    // One knob, three parameters. At 0 the de-esser only answers sibilance that
    // is already loud, gently, and can take off 6 dB; at 1 it starts 24 dB
    // lower, squeezes at 8:1 and may take off 18. Everything in between is a
    // straight line, so turning it up does what turning it up should.
    {
        const float k = clampf(settings_.deEsserIntensity, 0.0f, 1.0f);
        deEsserThresholdTargetDb_ = -18.0f - 24.0f * k;
        const float ratio = 2.0f + 6.0f * k;
        deEsserRatioInvTarget_ = 1.0f / ratio;
        deEsserFloorTargetDb_ = -(6.0f + 12.0f * k);
        // Coefficients only. rebuild() runs on every dynamics change -- moving
        // the gate threshold, say -- and assigning a fresh Biquad here would
        // wipe the sidechain's and the shelf's state each time, which is a
        // click on a control that has nothing to do with the de-esser.
        deEsserHp1_.setCoeffs(Biquad::highPass(sampleRate_, kDeEsserSplitHz));
        deEsserHp2_.setCoeffs(deEsserHp1_);
        deEsserShelf_.setCoeffs(
            Biquad::highShelf(sampleRate_, kDeEsserSplitHz, deEsserShelfGainDb_));
        deEsserAttack_ = smoothCoeff(kDeEsserAttackSec, sampleRate_);
        deEsserRelease_ = smoothCoeff(kDeEsserReleaseSec, sampleRate_);
        // 30 ms for the mapped values to travel, so dragging Amount is a slide.
        deEsserGlide_ = 1.0f - smoothCoeff(0.030f, sampleRate_);
    }

    envAttack_ = smoothCoeff(0.001f, sampleRate_);
    envRelease_ = smoothCoeff(0.050f, sampleRate_);
    makeupSmooth_ = smoothCoeff(0.05f, sampleRate_);
}

// A compressor whose detector hears only sibilance and whose output is a shelf
// on the same band: listen high, act high, leave the voice alone.
//
// Detection on the band rather than on the whole signal is what keeps a
// de-esser from lisping -- a wideband detector fires on any loud vowel and
// pulls the top off the whole word.
float DynamicsProcessor::deEss(float sample) {
    deEsserThresholdDb_ +=
        (deEsserThresholdTargetDb_ - deEsserThresholdDb_) * deEsserGlide_;
    deEsserRatioInv_ += (deEsserRatioInvTarget_ - deEsserRatioInv_) * deEsserGlide_;
    deEsserFloorDb_ += (deEsserFloorTargetDb_ - deEsserFloorDb_) * deEsserGlide_;

    const float side = deEsserHp2_.process(deEsserHp1_.process(sample));

    const float absHigh = std::fabs(side);
    const float coeff = (absHigh > deEsserEnv_) ? deEsserAttack_ : deEsserRelease_;
    deEsserEnv_ = coeff * deEsserEnv_ + (1.0f - coeff) * absHigh;

    const float bandDb = linearToDb(deEsserEnv_);
    float targetDb = 0.0f;
    if (bandDb > deEsserThresholdDb_) {
        targetDb = (deEsserThresholdDb_ +
                    (bandDb - deEsserThresholdDb_) * deEsserRatioInv_) - bandDb;
        // A floor on the reduction: past it the band is gone rather than
        // controlled, and a voice with no top at all reads as a fault.
        targetDb = std::max(targetDb, deEsserFloorDb_);
    }

    // Same ballistics either way, on the gain rather than the detector: the
    // envelope above is what decides how fast this reacts.
    const float gCoeff = (targetDb < deEsserGainDb_) ? deEsserAttack_ : deEsserRelease_;
    deEsserGainDb_ = gCoeff * deEsserGainDb_ + (1.0f - gCoeff) * targetDb;

    if (++deEsserShelfAge_ >= kDeEsserShelfInterval) {
        deEsserShelfAge_ = 0;
        if (std::fabs(deEsserGainDb_ - deEsserShelfGainDb_) > kDeEsserShelfEpsilonDb) {
            deEsserShelfGainDb_ = deEsserGainDb_;
            // Coefficients only: the filter's state carries on, so the shelf
            // moves without a click.
            const Biquad next = Biquad::highShelf(sampleRate_, kDeEsserSplitHz,
                                                  deEsserShelfGainDb_);
            deEsserShelf_.b0 = next.b0;
            deEsserShelf_.b1 = next.b1;
            deEsserShelf_.b2 = next.b2;
            deEsserShelf_.a1 = next.a1;
            deEsserShelf_.a2 = next.a2;
        }
    }
    return deEsserShelf_.process(sample);
}

float DynamicsProcessor::process(float sample) {
    if (!settings_.active()) return sample;

    // First, so everything downstream sees the tamed signal: a compressor fed
    // raw sibilance ducks the whole voice every time an "s" arrives, which is
    // the very thing a de-esser exists to stop.
    if (settings_.deEsser) sample = deEss(sample);

    const float absIn = std::fabs(sample);
    if (absIn > envLinear_)
        envLinear_ = envAttack_ * envLinear_ + (1.0f - envAttack_) * absIn;
    else
        envLinear_ = envRelease_ * envLinear_ + (1.0f - envRelease_) * absIn;

    const float inputDb = linearToDb(envLinear_);

    if (settings_.gate) {
        float targetDb = gateGainDb_;
        if (inputDb >= gateOpenDb_)
            targetDb = kMaxDb;
        else if (inputDb <= gateCloseDb_)
            targetDb = kMinDb;

        const float coeff =
            (targetDb > gateGainDb_) ? gateAttack_ : gateRelease_;
        gateGainDb_ = coeff * gateGainDb_ + (1.0f - coeff) * targetDb;
    } else {
        gateGainDb_ = kMaxDb;
    }

    if (settings_.compressor) {
        float targetGrDb = 0.0f;
        const float over = inputDb - compKneeOffset_;
        if (over <= 0.0f) {
            targetGrDb = 0.0f;
        } else if (over < 2.0f * compKneeHalf_) {
            const float x = over - compKneeHalf_;
            targetGrDb = compKneeScale_ * x * x;
        } else {
            targetGrDb = compThresholdDb_ + (inputDb - compThresholdDb_) * compRatioInv_ -
                         inputDb;
        }

        const float coeff =
            (targetGrDb < compGainDb_) ? compAttack_ : compRelease_;
        compGainDb_ = coeff * compGainDb_ + (1.0f - coeff) * targetGrDb;
    } else {
        compGainDb_ = 0.0f;
    }

    makeupSmoothDb_ =
        makeupSmooth_ * makeupSmoothDb_ + (1.0f - makeupSmooth_) * makeupDb_;

    if (settings_.limiter) {
        const float outDb = inputDb + gateGainDb_ + compGainDb_ + makeupSmoothDb_;
        float targetDb = 0.0f;
        if (outDb >= limitThresholdDb_)
            targetDb = limitThresholdDb_ - outDb;

        const float coeff =
            (targetDb < limitGainDb_) ? limitAttack_ : limitRelease_;
        limitGainDb_ = coeff * limitGainDb_ + (1.0f - coeff) * targetDb;
    } else {
        limitGainDb_ = 0.0f;
    }

    const float totalDb = gateGainDb_ + compGainDb_ + makeupSmoothDb_ + limitGainDb_;
    return sample * dbToLinear(totalDb);
}

}  // namespace waveline

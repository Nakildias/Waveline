// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Gate, compressor and limiter for voice/stream processing.

#pragma once

#include "biquad.h"

namespace waveline {

struct DynamicsSettings {
    bool gate = false;
    float gateThresholdDb = -50.0f;
    float gateAttackSec = 0.003f;
    float gateReleaseSec = 0.30f;
    float gateHysteresisDb = 6.0f;

    bool compressor = false;
    float compThresholdDb = -24.0f;
    float compRatio = 4.0f;
    float compAttackSec = 0.005f;
    float compReleaseSec = 0.150f;
    float compKneeDb = 6.0f;
    float makeupGainDb = 0.0f;
    bool autoMakeup = true;

    bool limiter = false;
    float limitThresholdDb = -3.0f;
    float limitAttackSec = 0.001f;
    float limitReleaseSec = 0.050f;

    // De-esser. One control on screen, because the useful range of a de-esser
    // is a single axis -- how hard to lean on sibilance -- and a threshold, a
    // ratio and a crossover asked separately is three ways to get it wrong.
    // intensity 0..1 moves threshold, ratio and the reduction ceiling together;
    // see DynamicsProcessor::rebuild() for the mapping.
    bool deEsser = false;
    float deEsserIntensity = 0.5f;

    bool active() const { return gate || compressor || limiter || deEsser; }
};

class DynamicsProcessor {
public:
    explicit DynamicsProcessor(float sampleRate = 48000.0f);

    void setSampleRate(float rate);
    void setSettings(const DynamicsSettings &settings);
    void reset();

    float process(float sample);

private:
    void rebuild();
    float deEss(float sample);

    static float dbToLinear(float db);
    static float linearToDb(float linear);
    static float smoothCoeff(float timeSec, float sampleRate);

    float sampleRate_ = 48000.0f;
    DynamicsSettings settings_{};

    float envLinear_ = 0.0f;
    float gateGainDb_ = 0.0f;
    float compGainDb_ = 0.0f;
    float limitGainDb_ = 0.0f;

    float envAttack_ = 0.0f;
    float envRelease_ = 0.0f;
    float gateOpenDb_ = -50.0f;
    float gateCloseDb_ = -56.0f;
    float gateAttack_ = 0.0f;
    float gateRelease_ = 0.0f;

    float compThresholdDb_ = -24.0f;
    float compRatioInv_ = 0.25f;
    float compAttack_ = 0.0f;
    float compRelease_ = 0.0f;
    float compKneeHalf_ = 3.0f;
    float compKneeScale_ = 0.0f;
    float compKneeOffset_ = 0.0f;
    float makeupDb_ = 0.0f;
    float makeupSmoothDb_ = 0.0f;
    float makeupSmooth_ = 0.0f;

    float limitThresholdDb_ = -3.0f;
    float limitAttack_ = 0.0f;
    float limitRelease_ = 0.0f;

    // Sidechain: a 4th-order high-pass, so the detector hears sibilance and not
    // the vowel under it. It never reaches the output -- splitting the signal
    // and turning one half down leaves whatever the split leaked through
    // untouched, which caps the reduction at about 7 dB however hard the
    // de-esser is asked to pull.
    Biquad deEsserHp1_{};
    Biquad deEsserHp2_{};
    // What the output actually goes through: one high shelf whose gain is
    // driven by the sidechain. A shelf attenuates everything above its corner
    // by its gain, so asking for 15 dB gives 15 dB, and at 0 dB it is a
    // pass-through -- an idle de-esser is inaudible by construction.
    Biquad deEsserShelf_{};
    float deEsserShelfGainDb_ = 0.0f;   // what the shelf is currently built for
    int deEsserShelfAge_ = 0;           // samples since it was last rebuilt
    float deEsserEnv_ = 0.0f;
    float deEsserGainDb_ = 0.0f;
    // Target (what the amount slider maps to) and current (what the maths is
    // using). Moving a threshold in one step moves the gain reduction with it,
    // at the de-esser's own 1.5 ms attack -- fast enough to be heard as a tick
    // while the slider is being dragged.
    float deEsserThresholdTargetDb_ = -32.0f;
    float deEsserRatioInvTarget_ = 0.25f;
    float deEsserFloorTargetDb_ = -12.0f;
    float deEsserThresholdDb_ = -32.0f;
    float deEsserRatioInv_ = 0.25f;
    float deEsserFloorDb_ = -12.0f;
    float deEsserAttack_ = 0.0f;
    float deEsserRelease_ = 0.0f;
    float deEsserGlide_ = 0.0f;
};

}  // namespace waveline

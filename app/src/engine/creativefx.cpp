// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "creativefx.h"

#include <algorithm>
#include <cmath>

namespace waveline {
namespace {

constexpr float kPi = 3.14159265358979323846f;

constexpr float kMinDelayMs = 10.0f;
constexpr float kMaxDelayMs = 2000.0f;
constexpr float kMaxPredelayMs = 100.0f;

constexpr uint32_t kCombTaps[kCreativeCombCount] = {1557, 1617, 1491, 1422};
constexpr uint32_t kApTaps[kCreativeApCount] = {225, 556};
constexpr float kApGain = 0.5f;

// Fixed base delays for the shared modulated-delay core: long and gentle for
// Chorus (thickening, not an obvious sweep), short and resonant for Flanger
// (the metallic jet). Not user-exposed -- the knob is the modulation depth
// around these anchors, not the anchors themselves.
constexpr float kChorusBaseMs = 20.0f;
constexpr float kChorusMaxDepthMs = 8.0f;
constexpr float kFlangerBaseMs = 0.8f;
constexpr float kFlangerMaxDepthMs = 3.0f;

// Phaser sweep range: a fixed 200 Hz floor, depth widens how high it reaches.
constexpr float kPhaserMinHz = 200.0f;
constexpr float kPhaserSweepHz = 3800.0f;

// Creative EQ tone-stack corners. Fixed, not user-exposed -- the six knobs
// are how much of each band, not where the bands sit, the same trade this
// codebase already made for the phaser's sweep range and the chorus/flanger
// base delays.
constexpr float kEqBassHz = 150.0f;
constexpr float kEqMidLowHz = 300.0f;
constexpr float kEqMidHighHz = 2000.0f;
constexpr float kEqTrebleHz = 2500.0f;
constexpr float kEqPresenceHz = 5000.0f;

// Envelope Filter sweep range: fixed floor/ceiling, sensitivity sets how far
// up the envelope reaches -- the same trade this file already makes for the
// phaser's sweep range.
constexpr float kEnvFilterMinHz = 200.0f;
constexpr float kEnvFilterMaxHz = 2800.0f;

// Pitch Shifter grain window. Fixed, not user-exposed -- the two knobs are
// how much to shift, not the internal grain size, the same trade the
// chorus/flanger base delays already make.
constexpr float kPitchGrainMs = 60.0f;

// Reverse Delay's own max block length matches Delay's kMaxDelayMs; the
// buffer itself is sized to twice that so the block currently being read
// (reversed) never collides with the block currently being recorded.
constexpr float kMaxReverseMs = kMaxDelayMs;

// Tape Saturator wow/flutter: a slow "wow" LFO plus a faster "flutter" LFO
// summed together, modulating a short delay -- not user-exposed, the same
// way Chorus/Flanger's LFO rate anchors are fixed and only depth is a knob.
constexpr float kTapeWowHz = 0.8f;
constexpr float kTapeFlutterHz = 6.5f;
constexpr float kTapeBaseWobbleMs = 3.0f;
constexpr float kTapeMaxWobbleMs = 2.5f;
// Age's bandwidth-narrowing range: a low-cut that rises from 0 Hz and a
// high-cut that falls from a bright ceiling, both driven by the same knob.
constexpr float kTapeMaxLowCutHz = 150.0f;
constexpr float kTapeMinHighCutHz = 2000.0f;
constexpr float kTapeMaxHighCutHz = 9000.0f;

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(v, hi));
}

// A spec with a garbled or missing order block should behave like one that
// never mentioned ordering at all -- the identity sequence -- rather than
// crash on an out-of-range stage index or silently drop a stage by repeating
// another one.
bool isValidOrder(const std::array<int, kFxStageCount> &order) {
    bool seen[kFxStageCount] = {};
    for (int idx : order) {
        if (idx < 0 || idx >= kFxStageCount || seen[idx]) return false;
        seen[idx] = true;
    }
    return true;
}

float dbToLin(float db) {
    return std::pow(10.0f, db / 20.0f);
}

}  // namespace

CreativeFxProcessor::CreativeFxProcessor(float sampleRate) {
    setSampleRate(sampleRate);
    setSettings({});
}

void CreativeFxProcessor::setSampleRate(float rate) {
    sampleRate_ = std::max(rate, 8000.0f);
    glide_ = 1.0f - std::exp(-1.0f / (0.025f * sampleRate_));

    const uint32_t maxDelay = static_cast<uint32_t>(sampleRate_ * kMaxDelayMs * 0.001f) + 4;
    delayBuf_.assign(maxDelay, 0.0f);
    delayWrite_ = 0;

    for (int i = 0; i < kCreativeCombCount; ++i) {
        combBuf_[i].assign(kCombTaps[i] + 1, 0.0f);
        combWrite_[i] = 0;
    }
    for (int i = 0; i < kCreativeApCount; ++i) {
        apBuf_[i].assign(kApTaps[i] + 1, 0.0f);
        apWrite_[i] = 0;
    }

    const uint32_t maxPredelay =
        static_cast<uint32_t>(sampleRate_ * kMaxPredelayMs * 0.001f) + 4;
    predelayBuf_.assign(maxPredelay, 0.0f);
    predelayWrite_ = 0;

    const uint32_t chorusLen = static_cast<uint32_t>(
        sampleRate_ * (kChorusBaseMs + kChorusMaxDepthMs) * 0.001f) + 4;
    chorusState_.buf.assign(chorusLen, 0.0f);
    chorusState_.writePos = 0;

    const uint32_t flangerLen = static_cast<uint32_t>(
        sampleRate_ * (kFlangerBaseMs + kFlangerMaxDepthMs) * 0.001f) + 4;
    flangerState_.buf.assign(flangerLen, 0.0f);
    flangerState_.writePos = 0;

    const uint32_t pitchLen =
        static_cast<uint32_t>(sampleRate_ * kPitchGrainMs * 0.001f) + 4;
    pitchBuf_.assign(pitchLen, 0.0f);
    pitchWrite_ = 0;

    const uint32_t reverseLen =
        static_cast<uint32_t>(sampleRate_ * (kMaxReverseMs * 2.0f) * 0.001f) + 4;
    reverseBuf_.assign(reverseLen, 0.0f);
    reverseWrite_ = 0;
    reverseBlockLen_ = 0;
    reverseBlockPos_ = 0;
    reverseBlockStart_ = 0;

    const uint32_t tapeLen = static_cast<uint32_t>(
        sampleRate_ * (kTapeBaseWobbleMs + kTapeMaxWobbleMs) * 0.001f) + 4;
    tapeBuf_.assign(tapeLen, 0.0f);
    tapeWrite_ = 0;

    // Exact one-pole corner: coeff = 1 - exp(-2*pi*fc/fs).
    const auto onePoleCoeff = [this](float fc) {
        return 1.0f - std::exp(-2.0f * kPi * fc / sampleRate_);
    };
    eqBassCoeff_ = onePoleCoeff(kEqBassHz);
    eqMidLowCoeff_ = onePoleCoeff(kEqMidLowHz);
    eqMidHighCoeff_ = onePoleCoeff(kEqMidHighHz);
    eqTrebleCoeff_ = onePoleCoeff(kEqTrebleHz);
    eqPresenceCoeff_ = onePoleCoeff(kEqPresenceHz);
}

void CreativeFxProcessor::setChannelIndex(int index) {
    channelIndex_ = index;
    // A quarter-turn head start on the odd channel of a stereo pair, so two
    // independent processors given the same settings still drift apart --
    // free width on every modulated effect, no cross-channel coupling.
    const float offset = (index % 2 == 1) ? 0.25f : 0.0f;
    chorusState_.lfoPhase = offset;
    flangerState_.lfoPhase = offset;
    phaserLfoPhase_ = offset;
    tremoloPhase_ = offset;
}

void CreativeFxProcessor::setSettings(const CreativeFxSettings &settings) {
    settings_ = settings;

    BitcrusherSettings &b = settings_.bitcrusher;
    b.bitDepth = clampf(b.bitDepth, 1.0f, 16.0f);
    b.sampleRateReduction = clampf(b.sampleRateReduction, 0.0f, 1.0f);
    b.mix = clampf(b.mix, 0.0f, 1.0f);

    OverdriveSettings &o = settings_.overdrive;
    o.drive = clampf(o.drive, 0.0f, 1.0f);
    o.tone = clampf(o.tone, 0.0f, 1.0f);
    o.outputDb = clampf(o.outputDb, -24.0f, 24.0f);
    o.mix = clampf(o.mix, 0.0f, 1.0f);

    ChorusSettings &c = settings_.chorus;
    c.rateHz = clampf(c.rateHz, 0.02f, 5.0f);
    c.depthMs = clampf(c.depthMs, 0.5f, kChorusMaxDepthMs);
    c.feedback = clampf(c.feedback, 0.0f, 0.9f);
    c.mix = clampf(c.mix, 0.0f, 1.0f);

    FlangerSettings &f = settings_.flanger;
    f.rateHz = clampf(f.rateHz, 0.02f, 2.0f);
    f.depthMs = clampf(f.depthMs, 0.1f, kFlangerMaxDepthMs);
    f.feedback = clampf(f.feedback, 0.0f, 0.95f);
    f.mix = clampf(f.mix, 0.0f, 1.0f);

    PhaserSettings &p = settings_.phaser;
    p.rateHz = clampf(p.rateHz, 0.02f, 5.0f);
    p.depth = clampf(p.depth, 0.0f, 1.0f);
    p.feedback = clampf(p.feedback, 0.0f, 0.95f);
    p.mix = clampf(p.mix, 0.0f, 1.0f);

    TremoloSettings &t = settings_.tremolo;
    t.rateHz = clampf(t.rateHz, 0.1f, 20.0f);
    t.depth = clampf(t.depth, 0.0f, 1.0f);
    t.shape = std::clamp(t.shape, 0, 2);
    t.mix = clampf(t.mix, 0.0f, 1.0f);

    DelaySettings &d = settings_.delay;
    d.timeMs = clampf(d.timeMs, kMinDelayMs, kMaxDelayMs);
    d.feedback = clampf(d.feedback, 0.0f, 0.95f);
    d.mix = clampf(d.mix, 0.0f, 1.0f);
    d.damping = clampf(d.damping, 0.0f, 1.0f);

    ReverbSettings &r = settings_.reverb;
    r.size = clampf(r.size, 0.0f, 1.0f);
    r.damping = clampf(r.damping, 0.0f, 1.0f);
    r.predelayMs = clampf(r.predelayMs, 0.0f, kMaxPredelayMs);
    r.mix = clampf(r.mix, 0.0f, 1.0f);

    CreativeEqSettings &eq = settings_.eq;
    eq.gain = clampf(eq.gain, -24.0f, 24.0f);
    eq.bass = clampf(eq.bass, -12.0f, 12.0f);
    eq.mid = clampf(eq.mid, -12.0f, 12.0f);
    eq.treble = clampf(eq.treble, -12.0f, 12.0f);
    eq.presence = clampf(eq.presence, -12.0f, 12.0f);
    eq.master = clampf(eq.master, -24.0f, 24.0f);

    RingModulatorSettings &rm = settings_.ringMod;
    rm.frequencyHz = clampf(rm.frequencyHz, 10.0f, 2000.0f);
    rm.fineTuneHz = clampf(rm.fineTuneHz, -50.0f, 50.0f);
    rm.mix = clampf(rm.mix, 0.0f, 1.0f);

    EnvelopeFilterSettings &ef = settings_.envFilter;
    ef.sensitivity = clampf(ef.sensitivity, 0.0f, 1.0f);
    ef.attackMs = clampf(ef.attackMs, 1.0f, 100.0f);
    ef.releaseMs = clampf(ef.releaseMs, 10.0f, 500.0f);
    ef.resonance = clampf(ef.resonance, 0.0f, 0.95f);
    ef.mix = clampf(ef.mix, 0.0f, 1.0f);

    PitchShifterSettings &ps = settings_.pitch;
    ps.semitones = clampf(ps.semitones, -12.0f, 12.0f);
    ps.detuneCents = clampf(ps.detuneCents, -50.0f, 50.0f);
    ps.mix = clampf(ps.mix, 0.0f, 1.0f);

    ReverseDelaySettings &rv = settings_.reverseDelay;
    rv.timeMs = clampf(rv.timeMs, 50.0f, kMaxReverseMs);
    rv.feedback = clampf(rv.feedback, 0.0f, 0.95f);
    rv.smoothing = clampf(rv.smoothing, 0.0f, 1.0f);
    rv.mix = clampf(rv.mix, 0.0f, 1.0f);

    TapeSaturatorSettings &ts = settings_.tapeSat;
    ts.drive = clampf(ts.drive, 0.0f, 1.0f);
    ts.flutter = clampf(ts.flutter, 0.0f, 1.0f);
    ts.age = clampf(ts.age, 0.0f, 1.0f);
    ts.mix = clampf(ts.mix, 0.0f, 1.0f);

    if (!isValidOrder(settings_.order)) {
        for (int i = 0; i < kFxStageCount; ++i) settings_.order[i] = i;
    }

    // Predelay is a coarse, rarely-touched buffer offset, not a hot knob --
    // it jumps on change rather than gliding, same as this class always
    // treated buffer sizing (setSampleRate) versus continuous controls.
    predelaySamples_ = static_cast<uint32_t>(r.predelayMs * 0.001f * sampleRate_);

    // Reverse Delay's block length is the same kind of coarse, structural
    // setting predelaySamples_ is above -- it jumps rather than glides, and
    // restarts the block cycle cleanly whenever it actually changes so a
    // stale block boundary never gets read against a new geometry.
    const uint32_t wantReverseBlockLen =
        std::max<uint32_t>(2, static_cast<uint32_t>(rv.timeMs * 0.001f * sampleRate_));
    if (wantReverseBlockLen != reverseBlockLen_) {
        reverseBlockLen_ = wantReverseBlockLen;
        reverseBlockPos_ = 0;
        reverseBlockStart_ = reverseWrite_;
    }

    glide_ = 1.0f - std::exp(-1.0f / (0.025f * sampleRate_));
}

void CreativeFxProcessor::reset() {
    std::fill(delayBuf_.begin(), delayBuf_.end(), 0.0f);
    delayWrite_ = 0;
    delayDampState_ = 0.0f;

    for (int i = 0; i < kCreativeCombCount; ++i) {
        std::fill(combBuf_[i].begin(), combBuf_[i].end(), 0.0f);
        combWrite_[i] = 0;
        combDampState_[i] = 0.0f;
    }
    for (int i = 0; i < kCreativeApCount; ++i) {
        std::fill(apBuf_[i].begin(), apBuf_[i].end(), 0.0f);
        apWrite_[i] = 0;
    }
    std::fill(predelayBuf_.begin(), predelayBuf_.end(), 0.0f);
    predelayWrite_ = 0;

    std::fill(chorusState_.buf.begin(), chorusState_.buf.end(), 0.0f);
    chorusState_.writePos = 0;
    std::fill(flangerState_.buf.begin(), flangerState_.buf.end(), 0.0f);
    flangerState_.writePos = 0;

    toneLpState_ = 0.0f;
    for (AllpassStage &s : phaserStages_) s = AllpassStage{};
    phaserFeedbackState_ = 0.0f;
    bitHeld_ = 0.0f;
    bitHold_ = 0;

    eqBassLp_ = 0.0f;
    eqMidLowLp_ = 0.0f;
    eqMidHighLp_ = 0.0f;
    eqTrebleLp_ = 0.0f;
    eqPresenceLp_ = 0.0f;

    ringPhase_ = 0.0f;

    envFilterLevel_ = 0.0f;
    envFilterSvfLow_ = 0.0f;
    envFilterSvfBand_ = 0.0f;

    std::fill(pitchBuf_.begin(), pitchBuf_.end(), 0.0f);
    pitchWrite_ = 0;
    pitchGrainPhase_ = 0.0f;

    std::fill(reverseBuf_.begin(), reverseBuf_.end(), 0.0f);
    reverseWrite_ = 0;
    reverseBlockPos_ = 0;
    reverseBlockStart_ = 0;

    std::fill(tapeBuf_.begin(), tapeBuf_.end(), 0.0f);
    tapeWrite_ = 0;
    tapeWowPhase_ = 0.0f;
    tapeFlutterPhase_ = 0.0f;
    tapeLowLp_ = 0.0f;
    tapeHighLp_ = 0.0f;
}

// One step of every smoothed control. Called once per sample, before the
// stages read them -- process() calls this via processPreDelay(), so it
// still runs exactly once per sample whether or not the ping-pong split path
// is in use.
void CreativeFxProcessor::advance() {
    bitDepth_ += (settings_.bitcrusher.bitDepth - bitDepth_) * glide_;
    bitSrReduction_ += (settings_.bitcrusher.sampleRateReduction - bitSrReduction_) * glide_;
    bitMix_ += (settings_.bitcrusher.mix - bitMix_) * glide_;
    bitOn_ += ((settings_.bitcrusher.enabled ? 1.0f : 0.0f) - bitOn_) * glide_;

    const float wantDrive = 1.0f + settings_.overdrive.drive * 30.0f;
    drive_ += (wantDrive - drive_) * glide_;
    tone_ += (settings_.overdrive.tone - tone_) * glide_;
    const float wantOutGain = dbToLin(settings_.overdrive.outputDb);
    outputGain_ += (wantOutGain - outputGain_) * glide_;
    overdriveMix_ += (settings_.overdrive.mix - overdriveMix_) * glide_;
    overdriveOn_ += ((settings_.overdrive.enabled ? 1.0f : 0.0f) - overdriveOn_) * glide_;

    chorusRate_ += (settings_.chorus.rateHz - chorusRate_) * glide_;
    chorusDepthMs_ += (settings_.chorus.depthMs - chorusDepthMs_) * glide_;
    chorusFeedback_ += (settings_.chorus.feedback - chorusFeedback_) * glide_;
    chorusMix_ += (settings_.chorus.mix - chorusMix_) * glide_;
    chorusOn_ += ((settings_.chorus.enabled ? 1.0f : 0.0f) - chorusOn_) * glide_;

    flangerRate_ += (settings_.flanger.rateHz - flangerRate_) * glide_;
    flangerDepthMs_ += (settings_.flanger.depthMs - flangerDepthMs_) * glide_;
    flangerFeedback_ += (settings_.flanger.feedback - flangerFeedback_) * glide_;
    flangerMix_ += (settings_.flanger.mix - flangerMix_) * glide_;
    flangerOn_ += ((settings_.flanger.enabled ? 1.0f : 0.0f) - flangerOn_) * glide_;

    phaserRate_ += (settings_.phaser.rateHz - phaserRate_) * glide_;
    phaserDepth_ += (settings_.phaser.depth - phaserDepth_) * glide_;
    phaserFeedback_ += (settings_.phaser.feedback - phaserFeedback_) * glide_;
    phaserMix_ += (settings_.phaser.mix - phaserMix_) * glide_;
    phaserOn_ += ((settings_.phaser.enabled ? 1.0f : 0.0f) - phaserOn_) * glide_;

    tremoloRate_ += (settings_.tremolo.rateHz - tremoloRate_) * glide_;
    tremoloDepth_ += (settings_.tremolo.depth - tremoloDepth_) * glide_;
    tremoloMix_ += (settings_.tremolo.mix - tremoloMix_) * glide_;
    tremoloShape_ = settings_.tremolo.shape;
    tremoloOn_ += ((settings_.tremolo.enabled ? 1.0f : 0.0f) - tremoloOn_) * glide_;

    // A delay time that slides drags its tail with it, the way a tape delay
    // does. Jumping the read head instead splices two unrelated points of
    // the buffer together, which is a click every time.
    const float wantDelaySamples = settings_.delay.timeMs * 0.001f * sampleRate_;
    delaySamples_ += (wantDelaySamples - delaySamples_) * glide_;
    delayFeedback_ += (settings_.delay.feedback - delayFeedback_) * glide_;
    delayMix_ += (settings_.delay.mix - delayMix_) * glide_;
    delayDamping_ += (settings_.delay.damping - delayDamping_) * glide_;
    delayOn_ += ((settings_.delay.enabled ? 1.0f : 0.0f) - delayOn_) * glide_;

    const float wantFeedback = 0.55f + settings_.reverb.size * 0.4f;
    reverbFeedback_ += (wantFeedback - reverbFeedback_) * glide_;
    reverbDamping_ += (settings_.reverb.damping - reverbDamping_) * glide_;
    reverbMix_ += (settings_.reverb.mix - reverbMix_) * glide_;
    reverbOn_ += ((settings_.reverb.enabled ? 1.0f : 0.0f) - reverbOn_) * glide_;

    eqGainLin_ += (dbToLin(settings_.eq.gain) - eqGainLin_) * glide_;
    eqBassLin_ += (dbToLin(settings_.eq.bass) - eqBassLin_) * glide_;
    eqMidLin_ += (dbToLin(settings_.eq.mid) - eqMidLin_) * glide_;
    eqTrebleLin_ += (dbToLin(settings_.eq.treble) - eqTrebleLin_) * glide_;
    eqPresenceLin_ += (dbToLin(settings_.eq.presence) - eqPresenceLin_) * glide_;
    eqMasterLin_ += (dbToLin(settings_.eq.master) - eqMasterLin_) * glide_;
    eqOn_ += ((settings_.eq.enabled ? 1.0f : 0.0f) - eqOn_) * glide_;

    const float wantRingFreq = settings_.ringMod.frequencyHz + settings_.ringMod.fineTuneHz;
    ringFreqHz_ += (wantRingFreq - ringFreqHz_) * glide_;
    ringMix_ += (settings_.ringMod.mix - ringMix_) * glide_;
    ringModOn_ += ((settings_.ringMod.enabled ? 1.0f : 0.0f) - ringModOn_) * glide_;

    envSensitivity_ += (settings_.envFilter.sensitivity - envSensitivity_) * glide_;
    envAttackMs_ += (settings_.envFilter.attackMs - envAttackMs_) * glide_;
    envReleaseMs_ += (settings_.envFilter.releaseMs - envReleaseMs_) * glide_;
    envResonance_ += (settings_.envFilter.resonance - envResonance_) * glide_;
    envFilterMix_ += (settings_.envFilter.mix - envFilterMix_) * glide_;
    envFilterOn_ += ((settings_.envFilter.enabled ? 1.0f : 0.0f) - envFilterOn_) * glide_;

    const float wantSemis = settings_.pitch.semitones + settings_.pitch.detuneCents * 0.01f;
    const float wantRatio = std::pow(2.0f, wantSemis / 12.0f);
    pitchRatio_ += (wantRatio - pitchRatio_) * glide_;
    pitchMix_ += (settings_.pitch.mix - pitchMix_) * glide_;
    pitchOn_ += ((settings_.pitch.enabled ? 1.0f : 0.0f) - pitchOn_) * glide_;

    reverseFeedback_ += (settings_.reverseDelay.feedback - reverseFeedback_) * glide_;
    reverseSmoothing_ += (settings_.reverseDelay.smoothing - reverseSmoothing_) * glide_;
    reverseMix_ += (settings_.reverseDelay.mix - reverseMix_) * glide_;
    reverseDelayOn_ += ((settings_.reverseDelay.enabled ? 1.0f : 0.0f) - reverseDelayOn_) * glide_;

    tapeDrive_ += (settings_.tapeSat.drive - tapeDrive_) * glide_;
    tapeFlutter_ += (settings_.tapeSat.flutter - tapeFlutter_) * glide_;
    tapeAge_ += (settings_.tapeSat.age - tapeAge_) * glide_;
    tapeMix_ += (settings_.tapeSat.mix - tapeMix_) * glide_;
    tapeSatOn_ += ((settings_.tapeSat.enabled ? 1.0f : 0.0f) - tapeSatOn_) * glide_;
}

// ----------------------------------------------------------------- stages

float CreativeFxProcessor::processBitcrusher(float x) {
    const int holdSamples = 1 + static_cast<int>(std::lround(bitSrReduction_ * 39.0f));
    if (bitHold_ <= 0) {
        bitHeld_ = x;
        bitHold_ = holdSamples;
    }
    --bitHold_;
    const float levels = std::pow(2.0f, bitDepth_) - 1.0f;
    const float q = levels > 0.0f ? std::round(bitHeld_ * levels) / levels : bitHeld_;
    return x * (1.0f - bitMix_) + q * bitMix_;
}

float CreativeFxProcessor::processOverdrive(float x) {
    const float norm = std::tanh(drive_);
    float y = (norm <= 1.0e-6f) ? x : std::tanh(x * drive_) / norm;

    // Tone: a fixed-corner one-pole splits y into low/high complements, and
    // the knob crossfades toward whichever side "tone" leans -- 0.5 leaves y
    // untouched, matching today's neutral behavior at the default.
    constexpr float kToneCoeff = 0.15f;
    toneLpState_ += (y - toneLpState_) * kToneCoeff;
    const float lp = toneLpState_;
    const float hp = y - lp;
    const float tilt = (tone_ - 0.5f) * 2.0f;
    const float mag = std::fabs(tilt);
    const float target = tilt < 0.0f ? lp : hp;
    y = y * (1.0f - mag) + target * mag;

    y *= outputGain_;
    return x * (1.0f - overdriveMix_) + y * overdriveMix_;
}

float CreativeFxProcessor::processModDelay(ModDelayState &st, float x, float rateHz,
                                           float baseMs, float depthMs, float feedback,
                                           float mix) {
    if (st.buf.empty()) return x;
    const uint32_t len = static_cast<uint32_t>(st.buf.size());
    if (len < 4) return x;

    st.lfoPhase += rateHz / sampleRate_;
    if (st.lfoPhase >= 1.0f) st.lfoPhase -= 1.0f;
    const float lfo = std::sin(2.0f * kPi * st.lfoPhase);
    const float delayMs = std::max(0.1f, baseMs + depthMs * lfo);
    const float delaySamples = std::min(delayMs * 0.001f * sampleRate_, float(len - 2));

    const uint32_t d0 = static_cast<uint32_t>(delaySamples);
    const uint32_t d1 = d0 + 1;
    const float frac = delaySamples - float(d0);
    const uint32_t idx0 = (st.writePos + len - d0) % len;
    const uint32_t idx1 = (st.writePos + len - d1) % len;
    const float delayed = st.buf[idx0] * (1.0f - frac) + st.buf[idx1] * frac;

    st.buf[st.writePos] = x + delayed * feedback;
    st.writePos = (st.writePos + 1) % len;

    return x * (1.0f - mix) + delayed * mix;
}

float CreativeFxProcessor::processPhaser(float x) {
    phaserLfoPhase_ += phaserRate_ / sampleRate_;
    if (phaserLfoPhase_ >= 1.0f) phaserLfoPhase_ -= 1.0f;
    const float lfo = 0.5f + 0.5f * std::sin(2.0f * kPi * phaserLfoPhase_);
    const float fc = kPhaserMinHz + kPhaserSweepHz * phaserDepth_ * lfo;
    const float t = std::tan(kPi * fc / sampleRate_);
    const float a = (t - 1.0f) / (t + 1.0f);

    float y = x + phaserFeedbackState_ * phaserFeedback_;
    for (AllpassStage &s : phaserStages_) {
        const float out = -a * y + s.za + a * s.zy;
        s.za = y;
        s.zy = out;
        y = out;
    }
    phaserFeedbackState_ = y;

    return x * (1.0f - phaserMix_) + y * phaserMix_;
}

float CreativeFxProcessor::processTremolo(float x) {
    tremoloPhase_ += tremoloRate_ / sampleRate_;
    if (tremoloPhase_ >= 1.0f) tremoloPhase_ -= 1.0f;

    float lfo;
    switch (tremoloShape_) {
        case 1: {  // triangle
            const float p = tremoloPhase_;
            lfo = p < 0.5f ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
            break;
        }
        case 2:  // soft square -- tanh'd rather than a true step, so a shape
                 // change never introduces a discontinuity mid-cycle.
            lfo = std::tanh(std::sin(2.0f * kPi * tremoloPhase_) * 6.0f);
            break;
        default:  // sine
            lfo = std::sin(2.0f * kPi * tremoloPhase_);
            break;
    }

    const float gain = 1.0f - tremoloDepth_ * 0.5f * (1.0f - lfo);
    const float wet = x * gain;
    return x * (1.0f - tremoloMix_) + wet * tremoloMix_;
}

float CreativeFxProcessor::processDelaySelf(float x) {
    if (delayBuf_.empty()) return x;
    const uint32_t delaySamples = static_cast<uint32_t>(delaySamples_ + 0.5f);
    const uint32_t len = static_cast<uint32_t>(delayBuf_.size());
    if (len < 2 || delaySamples == 0) return x;

    const uint32_t readPos = (delayWrite_ + len - (delaySamples % len)) % len;
    const float delayed = delayBuf_[readPos];
    const float out = x * (1.0f - delayMix_) + delayed * delayMix_;

    // Damping only darkens what comes back around the feedback loop, the
    // way a worn tape head does -- the tap just read stays full-bandwidth.
    delayDampState_ += (delayed - delayDampState_) * (1.0f - delayDamping_ * 0.95f);
    delayBuf_[delayWrite_] = x + delayDampState_ * delayFeedback_;
    delayWrite_ = (delayWrite_ + 1) % len;
    return out;
}

float CreativeFxProcessor::peekDelayTap() const {
    if (delayBuf_.empty()) return 0.0f;
    const uint32_t delaySamples = static_cast<uint32_t>(delaySamples_ + 0.5f);
    const uint32_t len = static_cast<uint32_t>(delayBuf_.size());
    if (len < 2 || delaySamples == 0) return 0.0f;
    const uint32_t readPos = (delayWrite_ + len - (delaySamples % len)) % len;
    return delayBuf_[readPos];
}

void CreativeFxProcessor::writeDelayLine(float value) {
    if (delayBuf_.empty()) return;
    const uint32_t len = static_cast<uint32_t>(delayBuf_.size());
    if (len < 2) return;
    // Ping-pong hands this the other channel's dry-plus-feedback sum
    // already combined, so damping is applied to the whole written value
    // rather than isolated to the feedback component as processDelaySelf
    // does -- a small deviation that still reads as a darkening repeat.
    delayDampState_ += (value - delayDampState_) * (1.0f - delayDamping_ * 0.95f);
    delayBuf_[delayWrite_] = delayDampState_;
    delayWrite_ = (delayWrite_ + 1) % len;
}

float CreativeFxProcessor::processReverb(float x) {
    float in = x;
    if (!predelayBuf_.empty() && predelaySamples_ > 0) {
        const uint32_t len = static_cast<uint32_t>(predelayBuf_.size());
        const uint32_t readPos =
            (predelayWrite_ + len - (predelaySamples_ % len)) % len;
        const float delayed = predelayBuf_[readPos];
        predelayBuf_[predelayWrite_] = x;
        predelayWrite_ = (predelayWrite_ + 1) % len;
        in = delayed;
    }

    const float feedback = reverbFeedback_;
    const float dampCoeff = 1.0f - reverbDamping_ * 0.95f;
    float combSum = 0.0f;

    for (int i = 0; i < kCreativeCombCount; ++i) {
        const uint32_t len = static_cast<uint32_t>(combBuf_[i].size());
        if (len < 2) continue;
        const float delayed = combBuf_[i][combWrite_[i]];
        combDampState_[i] += (delayed - combDampState_[i]) * dampCoeff;
        const float filtered = in + combDampState_[i] * feedback;
        combBuf_[i][combWrite_[i]] = filtered;
        combWrite_[i] = (combWrite_[i] + 1) % len;
        combSum += delayed;
    }
    combSum *= 0.25f;

    float apOut = combSum;
    for (int i = 0; i < kCreativeApCount; ++i) {
        const uint32_t len = static_cast<uint32_t>(apBuf_[i].size());
        if (len < 2) continue;
        const float bufOut = apBuf_[i][apWrite_[i]];
        const float apIn = apOut + bufOut * kApGain;
        apBuf_[i][apWrite_[i]] = apIn;
        apWrite_[i] = (apWrite_[i] + 1) % len;
        apOut = bufOut - apIn * kApGain;
    }

    return x * (1.0f - reverbMix_) + apOut * reverbMix_;
}

// Amp-style tone stack: input trim -> bass shelf -> mid peak -> treble shelf
// -> presence shelf -> master trim. Each band is added back into the signal
// scaled by (gain-1) rather than swapped in, the same additive-shelf move
// Overdrive's tone control already uses -- cheap, always stable (no
// feedback), and doesn't need a real biquad for a coarse tone-stack
// approximation. No user-facing mix knob: the shared on/off glide in
// processStage() is what makes toggling this module a fade, not a knob of
// its own.
float CreativeFxProcessor::processCreativeEq(float x) {
    x *= eqGainLin_;

    eqBassLp_ += (x - eqBassLp_) * eqBassCoeff_;
    x += eqBassLp_ * (eqBassLin_ - 1.0f);

    // Mid: the difference of two lowpasses gives a band limited between the
    // two corners, centred roughly on the classic ~800 Hz mid control.
    eqMidLowLp_ += (x - eqMidLowLp_) * eqMidLowCoeff_;
    eqMidHighLp_ += (x - eqMidHighLp_) * eqMidHighCoeff_;
    const float midBand = eqMidHighLp_ - eqMidLowLp_;
    x += midBand * (eqMidLin_ - 1.0f);

    eqTrebleLp_ += (x - eqTrebleLp_) * eqTrebleCoeff_;
    x += (x - eqTrebleLp_) * (eqTrebleLin_ - 1.0f);

    eqPresenceLp_ += (x - eqPresenceLp_) * eqPresenceCoeff_;
    x += (x - eqPresenceLp_) * (eqPresenceLin_ - 1.0f);

    return x * eqMasterLin_;
}

// Multiplies by an internal sine carrier -- the simplest possible ring
// modulator, and (per the header) deliberately so: no filtering, no
// feedback, just x * sin(2*pi*f*t).
float CreativeFxProcessor::processRingMod(float x) {
    ringPhase_ += ringFreqHz_ / sampleRate_;
    if (ringPhase_ >= 1.0f) ringPhase_ -= 1.0f;
    const float osc = std::sin(2.0f * kPi * ringPhase_);
    const float wet = x * osc;
    return x * (1.0f - ringMix_) + wet * ringMix_;
}

// Peak/envelope follower driving a resonant state-variable bandpass filter
// (the Chamberlin topology: two integrators, one feedback tap) -- the
// classic auto-wah circuit. Attack/release use their own one-pole
// coefficients, recomputed from the (smoothed) ms settings each sample the
// same way the EQ's fixed corners are computed once from Hz in
// setSampleRate(), just re-derived per sample here since these corners are
// user-adjustable rather than fixed.
float CreativeFxProcessor::processEnvFilter(float x) {
    const float level = std::fabs(x);
    const float attackCoeff = 1.0f - std::exp(-1.0f / (envAttackMs_ * 0.001f * sampleRate_));
    const float releaseCoeff = 1.0f - std::exp(-1.0f / (envReleaseMs_ * 0.001f * sampleRate_));
    const float coeff = (level > envFilterLevel_) ? attackCoeff : releaseCoeff;
    envFilterLevel_ += (level - envFilterLevel_) * coeff;

    const float env = std::min(1.0f, envFilterLevel_ * (1.0f + envSensitivity_ * 8.0f));
    const float fc = std::min(kEnvFilterMaxHz, kEnvFilterMinHz +
                                                    (kEnvFilterMaxHz - kEnvFilterMinHz) * env);

    const float f = 2.0f * std::sin(kPi * std::min(fc, sampleRate_ * 0.45f) / sampleRate_);
    const float q = std::max(0.05f, 1.0f - envResonance_ * 0.95f);
    envFilterSvfLow_ += f * envFilterSvfBand_;
    const float high = x - envFilterSvfLow_ - q * envFilterSvfBand_;
    envFilterSvfBand_ += f * high;

    const float wet = envFilterSvfBand_;
    return x * (1.0f - envFilterMix_) + wet * envFilterMix_;
}

// A fixed-length grain window with two read taps 180 degrees out of phase,
// each sweeping a variable delay linearly from full to zero (pitch up) or
// zero to full (pitch down) and resetting when it wraps -- the reset click
// is masked by the complementary tap's triangular window, which is at its
// peak exactly when the other tap is at its reset edge. Time-domain and
// grain-based rather than a phase vocoder, per the header: dead simple in
// C, no FFT, no added latency beyond the grain window itself.
float CreativeFxProcessor::processPitch(float x) {
    if (pitchBuf_.empty()) return x;
    const uint32_t len = static_cast<uint32_t>(pitchBuf_.size());
    if (len < 4) return x;

    pitchBuf_[pitchWrite_] = x;

    const float grainSamples = std::min(kPitchGrainMs * 0.001f * sampleRate_, float(len - 2));
    const float rate = std::fabs(pitchRatio_ - 1.0f);
    const float phaseInc = (grainSamples > 1.0f) ? (rate / grainSamples) : 0.0f;
    pitchGrainPhase_ += phaseInc;
    if (pitchGrainPhase_ >= 1.0f) pitchGrainPhase_ -= 1.0f;

    const bool pitchUp = pitchRatio_ >= 1.0f;
    auto readTap = [&](float phase) {
        const float d = pitchUp ? grainSamples * (1.0f - phase) : grainSamples * phase;
        const float dClamped = std::min(std::max(d, 0.0f), float(len - 2));
        const uint32_t d0 = static_cast<uint32_t>(dClamped);
        const uint32_t d1 = d0 + 1;
        const float frac = dClamped - float(d0);
        const uint32_t idx0 = (pitchWrite_ + len - d0) % len;
        const uint32_t idx1 = (pitchWrite_ + len - d1) % len;
        return pitchBuf_[idx0] * (1.0f - frac) + pitchBuf_[idx1] * frac;
    };
    auto windowFor = [](float phase) { return 1.0f - std::fabs(2.0f * phase - 1.0f); };

    const float phase2 = pitchGrainPhase_ < 0.5f ? pitchGrainPhase_ + 0.5f : pitchGrainPhase_ - 0.5f;
    const float wet = readTap(pitchGrainPhase_) * windowFor(pitchGrainPhase_) +
                      readTap(phase2) * windowFor(phase2);

    pitchWrite_ = (pitchWrite_ + 1) % len;
    return x * (1.0f - pitchMix_) + wet * pitchMix_;
}

// Records into reverseBuf_ while replaying the previously-completed block
// backwards from reverseBlockStart_ -- one block of latency, same as any
// buffer-reverse looper. The buffer is sized to twice the max block length
// (see kMaxReverseMs) so the block being written never catches up with the
// block still being read. Feedback re-records the reversed output, so a
// second repeat is reversed again -- back to forward -- which is the
// authentic behaviour of a real reverse-delay pedal's feedback loop, not a
// simplification of it.
float CreativeFxProcessor::processReverseDelay(float x) {
    if (reverseBuf_.empty() || reverseBlockLen_ < 2) return x;
    const uint32_t maxLen = static_cast<uint32_t>(reverseBuf_.size());

    const uint32_t back = reverseBlockPos_ + 1;
    const uint32_t readIdx = (reverseBlockStart_ + maxLen - (back % maxLen)) % maxLen;
    const float reversed = reverseBuf_[readIdx];

    // Triangular fade in/out across the block, width set by smoothing, so
    // the splice back to the start of the next reversed block is inaudible.
    const float halfLen = float(reverseBlockLen_) * 0.5f;
    const float distFromEdge =
        std::min(float(reverseBlockPos_), float(reverseBlockLen_ - 1 - reverseBlockPos_));
    const float fadeSamples = std::max(1.0f, reverseSmoothing_ * halfLen);
    const float fade = std::min(1.0f, distFromEdge / fadeSamples);
    const float wet = reversed * fade;

    reverseBuf_[reverseWrite_] = x + wet * reverseFeedback_;
    reverseWrite_ = (reverseWrite_ + 1) % maxLen;

    ++reverseBlockPos_;
    if (reverseBlockPos_ >= reverseBlockLen_) {
        reverseBlockPos_ = 0;
        reverseBlockStart_ = reverseWrite_;
    }

    return x * (1.0f - reverseMix_) + wet * reverseMix_;
}

// Wow/flutter (a short modulated delay, slow + fast LFOs summed) -> tanh
// saturation, normalized the same way Overdrive's drive stage is -> a
// rising low-cut plus a falling high-cut narrowing the bandwidth from both
// ends as Age increases, the way a worn tape or vinyl playback rolls off.
float CreativeFxProcessor::processTapeSat(float x) {
    tapeWowPhase_ += kTapeWowHz / sampleRate_;
    if (tapeWowPhase_ >= 1.0f) tapeWowPhase_ -= 1.0f;
    tapeFlutterPhase_ += kTapeFlutterHz / sampleRate_;
    if (tapeFlutterPhase_ >= 1.0f) tapeFlutterPhase_ -= 1.0f;
    const float wobbleLfo =
        std::sin(2.0f * kPi * tapeWowPhase_) * 0.7f + std::sin(2.0f * kPi * tapeFlutterPhase_) * 0.3f;

    float wobbled = x;
    if (!tapeBuf_.empty()) {
        const uint32_t len = static_cast<uint32_t>(tapeBuf_.size());
        const float depthMs = tapeFlutter_ * kTapeMaxWobbleMs;
        const float delayMs = std::max(0.1f, kTapeBaseWobbleMs + depthMs * wobbleLfo);
        const float delaySamples = std::min(delayMs * 0.001f * sampleRate_, float(len - 2));
        const uint32_t d0 = static_cast<uint32_t>(delaySamples);
        const uint32_t d1 = d0 + 1;
        const float frac = delaySamples - float(d0);
        const uint32_t idx0 = (tapeWrite_ + len - d0) % len;
        const uint32_t idx1 = (tapeWrite_ + len - d1) % len;
        wobbled = tapeBuf_[idx0] * (1.0f - frac) + tapeBuf_[idx1] * frac;
        tapeBuf_[tapeWrite_] = x;
        tapeWrite_ = (tapeWrite_ + 1) % len;
    }

    const float amount = 1.0f + tapeDrive_ * 9.0f;
    const float norm = std::tanh(amount);
    float y = (norm <= 1.0e-6f) ? wobbled : std::tanh(wobbled * amount) / norm;

    const float lowCutHz = tapeAge_ * kTapeMaxLowCutHz;
    const float highCutHz =
        kTapeMinHighCutHz + (1.0f - tapeAge_) * (kTapeMaxHighCutHz - kTapeMinHighCutHz);
    const float lowCoeff = 1.0f - std::exp(-2.0f * kPi * lowCutHz / sampleRate_);
    const float highCoeff = 1.0f - std::exp(-2.0f * kPi * highCutHz / sampleRate_);
    tapeLowLp_ += (y - tapeLowLp_) * lowCoeff;
    y -= tapeLowLp_;  // high-pass: strip everything below lowCutHz
    tapeHighLp_ += (y - tapeHighLp_) * highCoeff;
    y = tapeHighLp_;  // low-pass: keep only up to highCutHz

    return x * (1.0f - tapeMix_) + y * tapeMix_;
}

// -------------------------------------------------------------- top level

float CreativeFxProcessor::processStage(int stageIndex, float x) {
    switch (stageIndex) {
        case kFxBitcrusher:
            if (bitOn_ > 1.0e-4f) {
                const float wet = processBitcrusher(x);
                x = wet * bitOn_ + x * (1.0f - bitOn_);
            }
            break;
        case kFxOverdrive:
            if (overdriveOn_ > 1.0e-4f) {
                const float wet = processOverdrive(x);
                x = wet * overdriveOn_ + x * (1.0f - overdriveOn_);
            }
            break;
        case kFxChorus:
            if (chorusOn_ > 1.0e-4f) {
                const float wet = processModDelay(chorusState_, x, chorusRate_, kChorusBaseMs,
                                                  chorusDepthMs_, chorusFeedback_, chorusMix_);
                x = wet * chorusOn_ + x * (1.0f - chorusOn_);
            }
            break;
        case kFxFlanger:
            if (flangerOn_ > 1.0e-4f) {
                const float wet = processModDelay(flangerState_, x, flangerRate_, kFlangerBaseMs,
                                                  flangerDepthMs_, flangerFeedback_, flangerMix_);
                x = wet * flangerOn_ + x * (1.0f - flangerOn_);
            }
            break;
        case kFxPhaser:
            if (phaserOn_ > 1.0e-4f) {
                const float wet = processPhaser(x);
                x = wet * phaserOn_ + x * (1.0f - phaserOn_);
            }
            break;
        case kFxTremolo:
            if (tremoloOn_ > 1.0e-4f) {
                const float wet = processTremolo(x);
                x = wet * tremoloOn_ + x * (1.0f - tremoloOn_);
            }
            break;
        case kFxDelay:
            if (delayOn_ > 1.0e-4f) {
                const float wet = processDelaySelf(x);
                x = wet * delayOn_ + x * (1.0f - delayOn_);
            }
            break;
        case kFxReverb:
            if (reverbOn_ > 1.0e-4f) {
                const float wet = processReverb(x);
                x = wet * reverbOn_ + x * (1.0f - reverbOn_);
            }
            break;
        case kFxEq:
            if (eqOn_ > 1.0e-4f) {
                const float wet = processCreativeEq(x);
                x = wet * eqOn_ + x * (1.0f - eqOn_);
            }
            break;
        case kFxRingMod:
            if (ringModOn_ > 1.0e-4f) {
                const float wet = processRingMod(x);
                x = wet * ringModOn_ + x * (1.0f - ringModOn_);
            }
            break;
        case kFxEnvFilter:
            if (envFilterOn_ > 1.0e-4f) {
                const float wet = processEnvFilter(x);
                x = wet * envFilterOn_ + x * (1.0f - envFilterOn_);
            }
            break;
        case kFxPitch:
            if (pitchOn_ > 1.0e-4f) {
                const float wet = processPitch(x);
                x = wet * pitchOn_ + x * (1.0f - pitchOn_);
            }
            break;
        case kFxReverseDelay:
            if (reverseDelayOn_ > 1.0e-4f) {
                const float wet = processReverseDelay(x);
                x = wet * reverseDelayOn_ + x * (1.0f - reverseDelayOn_);
            }
            break;
        case kFxTapeSat:
            if (tapeSatOn_ > 1.0e-4f) {
                const float wet = processTapeSat(x);
                x = wet * tapeSatOn_ + x * (1.0f - tapeSatOn_);
            }
            break;
        default:
            break;
    }
    return x;
}

// Everything ahead of wherever kFxDelay currently sits in settings_.order --
// the ping-pong split point. When Delay is not in the chain's usual middle
// position this is simply "the stages the user put before it," whatever
// they are.
float CreativeFxProcessor::processPreDelay(float sample) {
    advance();
    float x = sample;
    for (int stage : settings_.order) {
        if (stage == kFxDelay) break;
        x = processStage(stage, x);
    }
    return x;
}

// Everything after kFxDelay's position -- the counterpart to
// processPreDelay(). Only meaningful paired with it on the same sample.
float CreativeFxProcessor::processReverbStage(float sample) {
    float x = sample;
    bool pastDelay = false;
    for (int stage : settings_.order) {
        if (stage == kFxDelay) {
            pastDelay = true;
            continue;
        }
        if (pastDelay) x = processStage(stage, x);
    }
    return x;
}

float CreativeFxProcessor::process(float sample) {
    advance();
    float x = sample;
    for (int stage : settings_.order) x = processStage(stage, x);
    return x;
}

}  // namespace waveline

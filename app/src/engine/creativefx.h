// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The Creative FX pedalboard: bitcrusher, overdrive, chorus, flanger,
// phaser, tremolo, delay, reverb, EQ, ring modulator, envelope filter,
// pitch shifter, reverse delay and tape saturator, in that order by
// default, for microphone and app-audio chains.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace waveline {

// Indices into CreativeFxSettings::order, matching declaration order below.
// A disabled effect is a no-op wherever it sits in the chain (its wet/dry
// blend is 0%, so it passes its input through unchanged) -- that's what
// makes an arbitrary, user-chosen order safe to add: nothing downstream of
// this file has to reason about which effects are actually active.
enum CreativeFxStageIndex {
    kFxBitcrusher = 0,
    kFxOverdrive = 1,
    kFxChorus = 2,
    kFxFlanger = 3,
    kFxPhaser = 4,
    kFxTremolo = 5,
    kFxDelay = 6,
    kFxReverb = 7,
    kFxEq = 8,
    kFxRingMod = 9,
    kFxEnvFilter = 10,
    kFxPitch = 11,
    kFxReverseDelay = 12,
    kFxTapeSat = 13,
    kFxStageCount = 14,
};

struct BitcrusherSettings {
    bool enabled = false;
    float bitDepth = 12.0f;           // 1..16
    float sampleRateReduction = 0.0f; // 0..1, 0 = no decimation
    float mix = 1.0f;                 // 0..1
};

struct OverdriveSettings {
    bool enabled = false;
    float drive = 0.5f;     // 0..1 (was overdriveIntensity)
    float tone = 0.5f;      // 0..1, 0=dark .. 1=bright, 0.5=neutral
    float outputDb = 0.0f;  // -24..+24
    float mix = 1.0f;       // 0..1, 1.0 matches today's fully-wet behavior
};

struct ChorusSettings {
    bool enabled = false;
    float rateHz = 0.8f;    // 0.02..5
    float depthMs = 4.0f;   // 0.5..8
    float feedback = 0.0f;  // 0..0.9
    float mix = 0.5f;       // 0..1
};

struct FlangerSettings {
    bool enabled = false;
    float rateHz = 0.25f;   // 0.02..2
    float depthMs = 2.0f;   // 0.1..3
    float feedback = 0.5f;  // 0..0.95
    float mix = 0.5f;       // 0..1
};

struct PhaserSettings {
    bool enabled = false;
    float rateHz = 0.5f;    // 0.02..5
    float depth = 0.8f;     // 0..1, widens the swept-frequency range
    float feedback = 0.3f;  // 0..0.95, resonance
    float mix = 0.5f;       // 0..1
};

struct TremoloSettings {
    bool enabled = false;
    float rateHz = 5.0f;    // 0.1..20
    float depth = 0.5f;     // 0..1
    int shape = 0;          // 0=sine, 1=triangle, 2=soft-square
    float mix = 1.0f;       // 0..1
};

struct DelaySettings {
    bool enabled = false;
    float timeMs = 250.0f;  // 10..2000
    float feedback = 0.35f; // 0..0.95, matches today's kDelayFeedback
    float mix = 0.4f;       // 0..1, matches today's kDelayWet
    float damping = 0.0f;   // 0..1, 0 = today's undamped repeats
    bool pingPong = false;  // stereo bounce; inert on 1-channel instances
};

struct ReverbSettings {
    bool enabled = false;
    float size = 0.5f;        // 0..1 (was reverbDecay)
    float damping = 0.0f;     // 0..1, 0 = today's raw/undamped combs
    float predelayMs = 0.0f;  // 0..100, 0 = today's instant onset
    float mix = 0.35f;        // 0..1, matches today's kReverbWet
};

// An amp-style tone stack, not a parametric EQ -- that's what the standard
// Processing-tab EQ already is. Coarse shelf/peak bands by name rather than
// by frequency, the way a guitar amp's front panel is labelled.
struct CreativeEqSettings {
    bool enabled = false;
    float gain = 0.0f;      // dB, -24..24 -- input trim before the stack
    float bass = 0.0f;      // dB, -12..12 -- low shelf, ~150 Hz corner
    float mid = 0.0f;       // dB, -12..12 -- peak, ~800 Hz centre
    float treble = 0.0f;    // dB, -12..12 -- high shelf, ~2.5 kHz corner
    float presence = 0.0f;  // dB, -12..12 -- higher shelf, ~5 kHz corner
    float master = 0.0f;    // dB, -24..24 -- output trim after the stack
};

// Multiplies the signal by an internal sine oscillator -- metallic, robotic,
// bell-like sum/difference tones rather than a filter or delay effect.
struct RingModulatorSettings {
    bool enabled = false;
    float frequencyHz = 200.0f;  // 10..2000 -- carrier oscillator frequency
    float fineTuneHz = 0.0f;     // -50..50 -- added to frequencyHz
    float mix = 0.5f;            // 0..1
};

// A peak/envelope follower sweeping a resonant bandpass filter -- funk and
// synth-like auto-wah, driven by how hard the signal is picked rather than
// an LFO.
struct EnvelopeFilterSettings {
    bool enabled = false;
    float sensitivity = 0.5f;  // 0..1 -- how far the envelope opens the sweep
    float attackMs = 5.0f;     // 1..100
    float releaseMs = 120.0f;  // 10..500
    float resonance = 0.3f;    // 0..0.95 -- filter Q
    float mix = 1.0f;          // 0..1
};

// Time-domain grain-delay pitch shifter: a fixed semitone shift plus a
// small cents-level detune, for octave lines or wide unison/shimmer tones.
struct PitchShifterSettings {
    bool enabled = false;
    float semitones = 0.0f;    // -12..12
    float detuneCents = 0.0f;  // -50..50 -- fine offset on top of semitones
    float mix = 0.5f;          // 0..1
};

// Records into a rolling buffer and plays each completed block back
// backwards, crossfading across block boundaries -- the shoegaze/ambient
// reverse-swell staple.
struct ReverseDelaySettings {
    bool enabled = false;
    float timeMs = 300.0f;   // 50..2000 -- reversed block length
    float feedback = 0.3f;   // 0..0.95 -- re-feeds the reversed output
    float smoothing = 0.5f;  // 0..1 -- crossfade width at each block edge
    float mix = 0.4f;        // 0..1
};

// Soft tanh saturation plus a short wow/flutter wobble delay and a
// bandwidth-narrowing "age" filter -- instant vintage tape/vinyl texture.
struct TapeSaturatorSettings {
    bool enabled = false;
    float drive = 0.3f;    // 0..1 -- saturation amount
    float flutter = 0.2f;  // 0..1 -- wow/flutter wobble depth
    float age = 0.3f;      // 0..1 -- low-cut/high-cut bandwidth narrowing
    float mix = 1.0f;      // 0..1
};

struct CreativeFxSettings {
    BitcrusherSettings bitcrusher;
    OverdriveSettings overdrive;
    ChorusSettings chorus;
    FlangerSettings flanger;
    PhaserSettings phaser;
    TremoloSettings tremolo;
    DelaySettings delay;
    ReverbSettings reverb;
    CreativeEqSettings eq;
    RingModulatorSettings ringMod;
    EnvelopeFilterSettings envFilter;
    PitchShifterSettings pitch;
    ReverseDelaySettings reverseDelay;
    TapeSaturatorSettings tapeSat;
    // A permutation of the CreativeFxStageIndex values: the processing
    // sequence, front (processed first) to back. Lets the Virtual Rack's
    // drag-to-reorder genuinely change the signal path rather than just
    // rearranging icons -- see the no-op note above for why a disabled
    // effect's position is harmless. Envelope Filter and Pitch default to
    // the very front (both track a clean signal best), then Bitcrusher,
    // Overdrive and EQ (a tone stack shapes the driven signal, matching real
    // amp signal flow), Ring Mod and the modulation effects, Delay and
    // Reverse Delay together, Reverb, and Tape Saturator last as an
    // end-of-chain colour -- still just the default, freely rearrangeable
    // via the Rack.
    std::array<int, kFxStageCount> order = {10, 11, 0, 1, 8, 9, 2, 3, 4, 5, 6, 12, 7, 13};
    // How many of the front entries of `order` the Virtual Rack currently
    // shows a module for -- a stage can be "in the rack" while powered off
    // (its own enabled flag false), which the DSP already treats as a
    // no-op, so this is pure UI bookkeeping with no effect on processing.
    // It exists at all because that distinction has to survive an app
    // restart: -1 means "not recorded" (an older spec, or the tab's own
    // blob, which never had a rack UI at all), and callers that care fall
    // back to inferring presence from which stages are enabled.
    int presentCount = -1;

    bool active() const {
        return bitcrusher.enabled || overdrive.enabled || chorus.enabled ||
               flanger.enabled || phaser.enabled || tremolo.enabled ||
               delay.enabled || reverb.enabled || eq.enabled || ringMod.enabled ||
               envFilter.enabled || pitch.enabled || reverseDelay.enabled ||
               tapeSat.enabled;
    }
};

inline constexpr int kCreativeCombCount = 4;
inline constexpr int kCreativeApCount = 2;
inline constexpr int kPhaserStages = 4;

// One modulated fractional delay line, the shared core behind Chorus and
// Flanger: they are the same circuit at different base-delay/feedback
// settings, not two different algorithms.
struct ModDelayState {
    std::vector<float> buf;
    uint32_t writePos = 0;
    float lfoPhase = 0.0f;
};

struct AllpassStage {
    float za = 0.0f;
    float zy = 0.0f;
};

class CreativeFxProcessor {
public:
    explicit CreativeFxProcessor(float sampleRate = 48000.0f);

    void setSampleRate(float rate);
    void setSettings(const CreativeFxSettings &settings);
    // Offsets each modulation LFO's starting phase (~90 degrees) so a stereo
    // pair run through independent processors get free width instead of
    // moving in lockstep. Set once, right after construction; must not be a
    // constructor argument because CreativeFxFilter builds its processor
    // vector with std::vector::assign(count, prototype), which copies one
    // prototype rather than constructing each element separately.
    void setChannelIndex(int index);
    void reset();

    // Full chain: bitcrusher -> overdrive -> chorus -> flanger -> phaser ->
    // tremolo -> delay -> reverb. What every non-ping-pong instance uses.
    float process(float sample);

    // The chain split in two so CreativeFxFilter can splice its own
    // cross-channel exchange between the delay and reverb stages when ping
    // pong is on. Not used unless the filter is actually doing that -- see
    // creativefxfilter.cpp.
    float processPreDelay(float sample);
    float processReverbStage(float sample);

    // Delay-line primitives for the ping-pong cross-write. Only meaningful
    // between a processPreDelay() and the matching processReverbStage() on
    // the same sample tick.
    float peekDelayTap() const;
    void writeDelayLine(float value);
    float delayFeedback() const { return delayFeedback_; }
    float delayMix() const { return delayMix_; }

private:
    float processBitcrusher(float x);
    float processOverdrive(float x);
    float processModDelay(ModDelayState &st, float x, float rateHz, float baseMs,
                          float depthMs, float feedback, float mix);
    float processPhaser(float x);
    float processTremolo(float x);
    float processDelaySelf(float x);
    float processReverb(float x);
    float processCreativeEq(float x);
    float processRingMod(float x);
    float processEnvFilter(float x);
    float processPitch(float x);
    float processReverseDelay(float x);
    float processTapeSat(float x);
    // Dispatches to one of the fourteen stages above by CreativeFxStageIndex,
    // gated by that stage's own on/off glide -- the single place process(),
    // processPreDelay() and processReverbStage() all funnel through so the
    // chain order only has to be walked once, in settings_.order.
    float processStage(int stageIndex, float x);
    void advance();

    float sampleRate_ = 48000.0f;
    int channelIndex_ = 0;
    CreativeFxSettings settings_{};
    float glide_ = 0.0f;

    // Each effect glides two independent things: waitOn_ is the on/off fade
    // (0 when fully bypassed, 1 when fully patched in) and the *Mix_ below is
    // the effect's own wet/dry knob, applied inside the stage before the
    // on/off fade blends the result back against the dry input. Conflating
    // the two would mean an effect left at 0% mix still pops when toggled.
    float bitOn_ = 0.0f, overdriveOn_ = 0.0f, chorusOn_ = 0.0f, flangerOn_ = 0.0f,
          phaserOn_ = 0.0f, tremoloOn_ = 0.0f, delayOn_ = 0.0f, reverbOn_ = 0.0f,
          eqOn_ = 0.0f, ringModOn_ = 0.0f, envFilterOn_ = 0.0f, pitchOn_ = 0.0f,
          reverseDelayOn_ = 0.0f, tapeSatOn_ = 0.0f;

    // ---- Bitcrusher ----
    float bitDepth_ = 12.0f;
    float bitSrReduction_ = 0.0f;
    float bitMix_ = 0.0f;
    float bitHeld_ = 0.0f;
    int bitHold_ = 0;

    // ---- Overdrive ----
    float drive_ = 1.0f;
    float tone_ = 0.5f;
    float outputGain_ = 1.0f;
    float overdriveMix_ = 0.0f;
    float toneLpState_ = 0.0f;

    // ---- Chorus / Flanger (shared ModDelay core) ----
    ModDelayState chorusState_;
    float chorusRate_ = 0.8f, chorusDepthMs_ = 4.0f, chorusFeedback_ = 0.0f, chorusMix_ = 0.0f;
    ModDelayState flangerState_;
    float flangerRate_ = 0.25f, flangerDepthMs_ = 2.0f, flangerFeedback_ = 0.5f, flangerMix_ = 0.0f;

    // ---- Phaser ----
    AllpassStage phaserStages_[kPhaserStages];
    float phaserLfoPhase_ = 0.0f;
    float phaserFeedbackState_ = 0.0f;
    float phaserRate_ = 0.5f, phaserDepth_ = 0.8f, phaserFeedback_ = 0.3f, phaserMix_ = 0.0f;

    // ---- Tremolo ----
    float tremoloPhase_ = 0.0f;
    float tremoloRate_ = 5.0f, tremoloDepth_ = 0.5f, tremoloMix_ = 0.0f;
    int tremoloShape_ = 0;

    // ---- Delay ----
    std::vector<float> delayBuf_;
    uint32_t delayWrite_ = 0;
    float delaySamples_ = 0.0f;
    float delayFeedback_ = 0.35f, delayMix_ = 0.0f, delayDamping_ = 0.0f;
    float delayDampState_ = 0.0f;

    // ---- Reverb ----
    std::vector<float> combBuf_[kCreativeCombCount];
    uint32_t combWrite_[kCreativeCombCount]{};
    float combDampState_[kCreativeCombCount]{};
    std::vector<float> apBuf_[kCreativeApCount];
    uint32_t apWrite_[kCreativeApCount]{};
    std::vector<float> predelayBuf_;
    uint32_t predelayWrite_ = 0;
    uint32_t predelaySamples_ = 0;
    float reverbFeedback_ = 0.55f, reverbDamping_ = 0.0f, reverbMix_ = 0.0f;

    // ---- Creative EQ (amp-style tone stack) ----
    float eqGainLin_ = 1.0f, eqBassLin_ = 1.0f, eqMidLin_ = 1.0f, eqTrebleLin_ = 1.0f,
          eqPresenceLin_ = 1.0f, eqMasterLin_ = 1.0f;
    float eqBassLp_ = 0.0f, eqMidLowLp_ = 0.0f, eqMidHighLp_ = 0.0f, eqTrebleLp_ = 0.0f,
          eqPresenceLp_ = 0.0f;
    // One-pole coefficients for the fixed band corners above, sized once in
    // setSampleRate() from a real Hz value rather than a magic constant.
    float eqBassCoeff_ = 0.0f, eqMidLowCoeff_ = 0.0f, eqMidHighCoeff_ = 0.0f,
          eqTrebleCoeff_ = 0.0f, eqPresenceCoeff_ = 0.0f;

    // ---- Ring Modulator ----
    float ringPhase_ = 0.0f;
    float ringFreqHz_ = 200.0f, ringMix_ = 0.0f;

    // ---- Envelope Filter (auto-wah) ----
    float envFilterLevel_ = 0.0f;
    float envFilterSvfLow_ = 0.0f, envFilterSvfBand_ = 0.0f;
    float envSensitivity_ = 0.5f, envAttackMs_ = 5.0f, envReleaseMs_ = 120.0f,
          envResonance_ = 0.3f, envFilterMix_ = 0.0f;

    // ---- Pitch Shifter (dual-tap grain delay) ----
    std::vector<float> pitchBuf_;
    uint32_t pitchWrite_ = 0;
    float pitchGrainPhase_ = 0.0f;
    float pitchRatio_ = 1.0f, pitchMix_ = 0.0f;

    // ---- Reverse Delay ----
    std::vector<float> reverseBuf_;
    uint32_t reverseWrite_ = 0;
    uint32_t reverseBlockLen_ = 0;    // current block length in samples; jumps on change, like predelaySamples_
    uint32_t reverseBlockPos_ = 0;    // 0..reverseBlockLen_-1, position within the block being recorded
    uint32_t reverseBlockStart_ = 0;  // reverseWrite_ index at the moment the block being read finished
    float reverseFeedback_ = 0.3f, reverseSmoothing_ = 0.5f, reverseMix_ = 0.0f;

    // ---- Tape Saturator / Vinyl Lo-Fi ----
    std::vector<float> tapeBuf_;
    uint32_t tapeWrite_ = 0;
    float tapeWowPhase_ = 0.0f, tapeFlutterPhase_ = 0.0f;
    float tapeLowLp_ = 0.0f, tapeHighLp_ = 0.0f;
    float tapeDrive_ = 0.3f, tapeFlutter_ = 0.2f, tapeAge_ = 0.3f, tapeMix_ = 0.0f;
};

}  // namespace waveline

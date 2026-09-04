// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "effectsfilter.h"

#include "dspprobe.h"
#include "rtsched.h"

#include "biquad.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/pod/builder.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace waveline {
namespace {

constexpr uint32_t kRate = 48000;
constexpr int kMaxChannels = 2;

struct PortData {
    uint32_t channel = 0;
};

struct ChannelChain {
    Biquad hpf;
    Biquad low;
    Biquad mid;
    Biquad high;
    std::array<Biquad, kProEqBands> pro;

    void reset() {
        hpf.reset();
        low.reset();
        mid.reset();
        high.reset();
        for (auto &b : pro) b.reset();
    }
};

// How long a parameter takes to travel to a new value, near enough. Long
// enough that dragging a slider is a slide rather than a series of steps,
// short enough that letting go feels immediate.
constexpr float kGlideSec = 0.030f;
// Below this, a rebuild would not change the sound and is skipped.
constexpr float kDbEpsilon = 0.01f;
constexpr float kHzEpsilon = 0.5f;
constexpr float kMixEpsilon = 0.0005f;
// Frequency is glided in octaves, not in hertz: a linear slide from 12 kHz to
// 200 Hz spends most of its time in the top octave and then falls off a cliff.
constexpr float kOctEpsilon = 0.002f;
constexpr float kQEpsilon = 0.005f;

float glide(float current, float target, float alpha) {
    return current + (target - current) * alpha;
}

float octaves(float hz) { return std::log2(std::max(hz, 1.0f)); }

// One parametric band as the audio thread sees it: where it is now, not where
// the user has asked for it to be.
struct BandRun {
    EqBandType type = EqBandType::Peak;
    float oct = 0.0f;
    float gainDb = 0.0f;
    float q = 1.0f;
    // 0..1, so switching a band in or out is a crossfade against the signal
    // that skipped it rather than a step in the middle of a word.
    float mix = 0.0f;
    bool dirty = true;
};

struct FxState {
    // What the user asked for, and what the filters are actually set to. The
    // second walks towards the first a block at a time: changing an EQ gain or
    // a corner frequency in one step steps the output with it, which is the
    // click people hear when they touch a slider while talking.
    ChannelFxSettings settings;
    float lowDb = 0.0f;
    float midDb = 0.0f;
    float highDb = 0.0f;
    float lowCutHz = 80.0f;
    // 0..1 blend for the low-cut stage, so switching it on or off is a fade
    // rather than a jump. The EQ needs no equivalent: its target gains go to
    // 0 dB when it is switched off, and a 0 dB shelf is a pass-through.
    float hpMix = 0.0f;
    std::array<BandRun, kProEqBands> pro{};
    // Which of the ten are audible enough to be worth running this block, in
    // order. Recomputed once per quantum so the sample loop is a straight walk
    // over the live ones instead of ten branches per sample.
    std::array<int, kProEqBands> liveBands{};
    int liveBandCount = 0;
    std::vector<ChannelChain> chains;
    bool coeffsDirty = true;

    void ensureChains(int channels) {
        if (static_cast<int>(chains.size()) != channels)
            chains.assign(channels, ChannelChain{});
    }

    // True while anything is still moving, or is set to something audible.
    bool active() const {
        return hpMix > kMixEpsilon || std::fabs(lowDb) > kDbEpsilon ||
               std::fabs(midDb) > kDbEpsilon || std::fabs(highDb) > kDbEpsilon ||
               liveBandCount > 0 || settings.active();
    }

    // The ten parametric bands, one step each. Same idea as the three fixed
    // ones, with two differences: a band can change *type* under the user, and
    // the pass filters and the notch have no gain to glide to zero -- so every
    // band carries a wet/dry mix of its own and is switched in and out with
    // that instead.
    void advanceProBands(float alpha) {
        const bool advOn = settings.eq && settings.eqAdvanced;
        liveBandCount = 0;
        for (int i = 0; i < kProEqBands; ++i) {
            const EqBand &want = settings.bands[i];
            BandRun &run = pro[i];

            const float wantMix = (advOn && want.on) ? 1.0f : 0.0f;
            const float wantOct = octaves(clampEqf(want.freq, kEqMinHz, kEqMaxHz));
            const float wantGain = eqTypeUsesGain(want.type)
                                       ? clampEqf(want.gainDb, kEqMinDb, kEqMaxDb)
                                       : 0.0f;
            const float wantQ = clampEqf(want.q, kEqMinQ, kEqMaxQ);

            bool changed = run.dirty || coeffsDirty;
            if (run.type != want.type) {
                run.type = want.type;
                changed = true;
            }

            // A band nobody can hear is moved straight to where it has been
            // put. Gliding a silent filter towards a new corner would have it
            // arrive somewhere in between if it were switched on halfway.
            const bool silent = run.mix <= kMixEpsilon;
            if (silent) {
                changed = changed || std::fabs(run.oct - wantOct) > kOctEpsilon ||
                          std::fabs(run.gainDb - wantGain) > kDbEpsilon ||
                          std::fabs(run.q - wantQ) > kQEpsilon;
                run.oct = wantOct;
                run.gainDb = wantGain;
                run.q = wantQ;
            } else {
                const float prevOct = run.oct, prevGain = run.gainDb, prevQ = run.q;
                run.oct = glide(run.oct, wantOct, alpha);
                run.gainDb = glide(run.gainDb, wantGain, alpha);
                run.q = glide(run.q, wantQ, alpha);
                changed = changed || std::fabs(run.oct - prevOct) > kOctEpsilon ||
                          std::fabs(run.gainDb - prevGain) > kDbEpsilon ||
                          std::fabs(run.q - prevQ) > kQEpsilon;
            }

            run.mix = glide(run.mix, wantMix, alpha);
            if (std::fabs(run.mix - wantMix) < kMixEpsilon) run.mix = wantMix;

            // Coming back from silence with two blocks of stale history in the
            // filter is a click the crossfade cannot hide, so the state starts
            // clean and the fade covers the settling instead.
            if (silent && run.mix > kMixEpsilon) {
                for (auto &c : chains) c.pro[i].reset();
                changed = true;
            }

            if (changed) {
                const float hz = std::exp2(run.oct);
                for (auto &c : chains)
                    c.pro[i].setCoeffs(
                        Biquad::forBand(kRate, run.type, hz, run.gainDb, run.q));
                run.dirty = false;
            }

            if (run.mix > kMixEpsilon) liveBands[liveBandCount++] = i;
        }
    }

    // One step towards the settings, and new coefficients if that moved
    // anything. Called once per quantum, so the glide is the same length
    // whatever the buffer size is.
    void advance(int channels, uint32_t frames) {
        ensureChains(channels);
        const float blockSec = float(frames) / float(kRate);
        const float alpha = 1.0f - std::exp(-blockSec / kGlideSec);

        advanceProBands(alpha);

        // The easy EQ glides to 0 dB whenever it is not the one selected, and
        // a 0 dB shelf is a pass-through -- so switching to Advanced fades the
        // three bands out while the ten fade in, with no gap between them.
        const bool easy = settings.eq && !settings.eqAdvanced;
        const float wantLow = easy ? settings.lowDb : 0.0f;
        const float wantMid = easy ? settings.midDb : 0.0f;
        const float wantHigh = easy ? settings.highDb : 0.0f;
        const float wantHz = float(settings.lowCutHz);
        const float wantMix = settings.lowCut ? 1.0f : 0.0f;

        const float prevLow = lowDb, prevMid = midDb, prevHigh = highDb;
        const float prevHz = lowCutHz;
        lowDb = glide(lowDb, wantLow, alpha);
        midDb = glide(midDb, wantMid, alpha);
        highDb = glide(highDb, wantHigh, alpha);
        // Only while the stage is audible; sliding the corner of a filter
        // nobody is listening to would arrive at the wrong place if it were
        // switched on mid-glide.
        lowCutHz = (hpMix > kMixEpsilon) ? glide(lowCutHz, wantHz, alpha) : wantHz;
        hpMix = glide(hpMix, wantMix, alpha);
        if (std::fabs(hpMix - wantMix) < kMixEpsilon) hpMix = wantMix;

        const bool moved = coeffsDirty ||
                           std::fabs(lowDb - prevLow) > kDbEpsilon ||
                           std::fabs(midDb - prevMid) > kDbEpsilon ||
                           std::fabs(highDb - prevHigh) > kDbEpsilon ||
                           std::fabs(lowCutHz - prevHz) > kHzEpsilon;
        if (!moved) return;

        for (auto &c : chains) {
            // setCoeffs, never assignment: the filters keep their state and
            // carry the signal through the change.
            c.hpf.setCoeffs(Biquad::highPass(kRate, lowCutHz));
            c.low.setCoeffs(Biquad::lowShelf(kRate, 200.0f, lowDb));
            c.mid.setCoeffs(Biquad::peaking(kRate, 1000.0f, midDb));
            c.high.setCoeffs(Biquad::highShelf(kRate, 4000.0f, highDb));
        }
        coeffsDirty = false;
    }
};

}  // namespace

struct EffectsFilter::Impl {
    pw_thread_loop *loop = nullptr;
    pw_filter *filter = nullptr;
    PortData *inPorts[kMaxChannels]{};
    PortData *outPorts[kMaxChannels]{};
    int channels = 2;

    std::mutex settingsMutex;
    FxState fx;
    std::atomic<bool> settingsDirty{true};
    DspMeter meter;
};

namespace {

void onProcess(void *userdata, spa_io_position *position) {
    auto *d = static_cast<EffectsFilter::Impl *>(userdata);
    DspScope probe(d->meter, position);
    const uint32_t n = position->clock.duration;

    float hpMix = 0.0f;
    bool bypass = false;
    // Lifted out of the state under the lock, so the sample loop below reads
    // nothing the control thread can be writing.
    int liveCount = 0;
    int liveIdx[kProEqBands]{};
    float liveMix[kProEqBands]{};
    {
        std::lock_guard<std::mutex> lock(d->settingsMutex);
        d->fx.advance(d->channels, n);
        d->settingsDirty.store(false, std::memory_order_relaxed);
        hpMix = d->fx.hpMix;
        liveCount = d->fx.liveBandCount;
        for (int i = 0; i < liveCount; ++i) {
            liveIdx[i] = d->fx.liveBands[i];
            liveMix[i] = d->fx.pro[liveIdx[i]].mix;
        }
        // Only once everything has finished moving: bypassing the moment the
        // last switch went off would cut the fade short, which is the click
        // the fade is there to avoid.
        bypass = !d->fx.active();
    }

    for (int ch = 0; ch < d->channels; ++ch) {
        auto *in = static_cast<float *>(pw_filter_get_dsp_buffer(d->inPorts[ch], n));
        auto *out = static_cast<float *>(pw_filter_get_dsp_buffer(d->outPorts[ch], n));
        if (!out) continue;
        if (!in) {
            std::memset(out, 0, n * sizeof(float));
            continue;
        }
        if (bypass) {
            if (out != in) std::memcpy(out, in, n * sizeof(float));
            continue;
        }

        ChannelChain &chain = d->fx.chains[ch];
        for (uint32_t i = 0; i < n; ++i) {
            const float dry = in[i];
            // The high-pass runs whether or not it is switched on, so its
            // state stays warm and switching it in is a fade, not a restart.
            const float cut = chain.hpf.process(dry);
            float s = cut * hpMix + dry * (1.0f - hpMix);
            // At 0 dB these are pass-throughs, which is what "EQ off" glides
            // to -- no branch, and nothing to click on the way.
            s = chain.low.process(s);
            s = chain.mid.process(s);
            s = chain.high.process(s);
            // The parametric bands. Each carries its own mix so that a
            // band-pass or a notch -- neither of which has a neutral gain --
            // fades in and out like everything else here.
            for (int k = 0; k < liveCount; ++k) {
                const float m = liveMix[k];
                const float wet = chain.pro[liveIdx[k]].process(s);
                s = wet * m + s * (1.0f - m);
            }
            out[i] = s;
        }
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_filter_events kFilterEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .process = onProcess,
};
#pragma GCC diagnostic pop

const char *positionForChannels(int channels) {
    return channels == 1 ? "[ MONO ]" : "[ FL FR ]";
}

}  // namespace

EffectsFilter::EffectsFilter() : d_(std::make_unique<Impl>()) {}
EffectsFilter::~EffectsFilter() { stop(); }

void EffectsFilter::setSettings(const ChannelFxSettings &s) {
    std::lock_guard<std::mutex> lock(d_->settingsMutex);
    d_->fx.settings = s;
    if (d_->fx.settings.lowCutHz != 80 && d_->fx.settings.lowCutHz != 120)
        d_->fx.settings.lowCutHz = 80;
    for (EqBand &b : d_->fx.settings.bands) clampEqBand(b);
    d_->fx.coeffsDirty = true;
    d_->settingsDirty.store(true, std::memory_order_relaxed);
}

ChannelFxSettings EffectsFilter::settings() const {
    std::lock_guard<std::mutex> lock(d_->settingsMutex);
    return d_->fx.settings;
}

bool EffectsFilter::start(const std::string &nodeName, const std::string &description,
                          int channels, std::string &error, bool asSource) {
    if (channels < 1 || channels > kMaxChannels) {
        error = "effects filter supports 1 or 2 channels";
        return false;
    }

    pw_init(nullptr, nullptr);
    d_->channels = channels;
    // Before the filter is connected, so the audio thread never sees a
    // half-registered meter. See dspprobe.h.
    d_->meter.attach(nodeName, "EQ");

    d_->loop = pw_thread_loop_new("waveline-fx", nullptr);
    if (!d_->loop) {
        error = "pw_thread_loop_new failed";
        return false;
    }

    pw_thread_loop_lock(d_->loop);

    auto *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_MEDIA_CLASS, asSource ? "Audio/Source" : "Stream/Filter/Audio",
        PW_KEY_NODE_NAME, nodeName.c_str(),
        PW_KEY_NODE_DESCRIPTION, description.c_str(),
        PW_KEY_NODE_AUTOCONNECT, "false",
        "audio.rate", "48000",
        "audio.channels", channels == 1 ? "1" : "2",
        "audio.position", positionForChannels(channels),
        "node.want-driver", "true",
        nullptr);

    d_->filter = pw_filter_new_simple(pw_thread_loop_get_loop(d_->loop),
                                      nodeName.c_str(), applyRealtimeProps(props), &kFilterEvents,
                                      d_.get());
    if (!d_->filter) {
        error = "pw_filter_new_simple failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    for (int ch = 0; ch < channels; ++ch) {
        const char *inName = channels == 1 ? "input" : (ch == 0 ? "input_FL" : "input_FR");
        const char *outName =
            channels == 1 ? "output" : (ch == 0 ? "output_FL" : "output_FR");

        d_->inPorts[ch] = static_cast<PortData *>(pw_filter_add_port(
            d_->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(PortData),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, inName, nullptr),
            nullptr, 0));
        d_->outPorts[ch] = static_cast<PortData *>(pw_filter_add_port(
            d_->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(PortData),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, outName, nullptr),
            nullptr, 0));
        if (d_->inPorts[ch]) d_->inPorts[ch]->channel = ch;
        if (d_->outPorts[ch]) d_->outPorts[ch]->channel = ch;
    }

    if (!d_->inPorts[0] || !d_->outPorts[0]) {
        error = "pw_filter_add_port failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    if (pw_filter_connect(d_->filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        error = "pw_filter_connect failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    pw_thread_loop_unlock(d_->loop);

    if (pw_thread_loop_start(d_->loop) < 0) {
        error = "pw_thread_loop_start failed";
        return false;
    }
    return true;
}

void EffectsFilter::stop() {
    if (!d_) return;
    if (!d_->loop) {
        // start() can fail after attach() and before the loop exists. The
        // registration still has to go, or a stage nothing is running shows up
        // in the panel for as long as the process lives.
        d_->meter.detach();
        return;
    }
    pw_thread_loop_lock(d_->loop);
    if (d_->filter) {
        pw_filter_destroy(d_->filter);
        d_->filter = nullptr;
    }
    pw_thread_loop_unlock(d_->loop);
    pw_thread_loop_stop(d_->loop);
    pw_thread_loop_destroy(d_->loop);
    d_->loop = nullptr;
    // After the loop is gone, so nothing can still be inside the counters.
    d_->meter.detach();
}

}  // namespace waveline

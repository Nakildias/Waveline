// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The Creative FX pedalboard's wire format -- the same job eqspec.h does for
// Pro EQ bands, for the same reason: too many parameters to hand over as
// positional D-Bus arguments, and a text field that a comma-decimal locale
// could corrupt if it carried real decimals. Integers only, one block per
// effect, blocks joined by ';', fields within a block joined by ','.
//
// Header-only and free of Qt, PipeWire and the rest of the engine, because
// the daemon and the GUI client both need this and link neither of the other.

#pragma once

#include "creativefx.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace waveline {

namespace creativefx_detail {

inline long scaled(float v, float factor) {
    return static_cast<long>(std::lround(v * factor));
}

inline float unscaled(long v, float factor) {
    return static_cast<float>(v) / factor;
}

// Splits "a,b,c" into up to maxFields longs. Returns how many it actually
// found -- a short block is not an error, it just means "use the defaults
// for what's missing," same as decodeEqBands does per band.
inline int splitFields(const std::string &part, long *field, int maxFields) {
    int n = 0;
    size_t fp = 0;
    while (n < maxFields && fp <= part.size()) {
        const size_t fe = part.find(',', fp);
        const std::string tok =
            part.substr(fp, fe == std::string::npos ? std::string::npos : fe - fp);
        field[n++] = std::strtol(tok.c_str(), nullptr, 10);
        if (fe == std::string::npos) break;
        fp = fe + 1;
    }
    return n;
}

// Walks the ';'-separated blocks of `spec`, calling decodeBlock(part) for
// each of the `count` slots present. Missing trailing blocks simply never
// call decodeBlock, leaving that slot's caller-supplied default untouched.
template <typename Fn>
inline void forEachBlock(const std::string &spec, int count, Fn &&decodeBlock) {
    size_t pos = 0;
    for (int i = 0; i < count && pos <= spec.size(); ++i) {
        const size_t end = spec.find(';', pos);
        const std::string part =
            spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? spec.size() + 1 : end + 1;
        if (part.empty()) continue;
        decodeBlock(i, part);
    }
}

// A chain order is only valid if it is actually a permutation of the eight
// stage indices -- garbage here would mean either an out-of-range stage
// index or two stages claiming the same slot (and a third never running).
inline bool isValidOrderSpec(const long field[kFxStageCount]) {
    bool seen[kFxStageCount] = {};
    for (int i = 0; i < kFxStageCount; ++i) {
        const long v = field[i];
        if (v < 0 || v >= kFxStageCount || seen[v]) return false;
        seen[v] = true;
    }
    return true;
}

}  // namespace creativefx_detail

// "on,bitDepthTenths,srReductionHundredths,mixHundredths"
inline std::string encodeCreativeFx(const CreativeFxSettings &s) {
    using namespace creativefx_detail;
    std::string out;

    auto block = [&](const std::vector<long> &fields) {
        if (!out.empty()) out += ';';
        bool first = true;
        for (long f : fields) {
            if (!first) out += ',';
            first = false;
            out += std::to_string(f);
        }
    };

    const BitcrusherSettings &b = s.bitcrusher;
    block({b.enabled ? 1 : 0, scaled(b.bitDepth, 10.0f), scaled(b.sampleRateReduction, 100.0f),
          scaled(b.mix, 100.0f)});

    const OverdriveSettings &o = s.overdrive;
    block({o.enabled ? 1 : 0, scaled(o.drive, 100.0f), scaled(o.tone, 100.0f),
          scaled(o.outputDb, 10.0f), scaled(o.mix, 100.0f)});

    const ChorusSettings &c = s.chorus;
    block({c.enabled ? 1 : 0, scaled(c.rateHz, 100.0f), scaled(c.depthMs, 100.0f),
          scaled(c.feedback, 100.0f), scaled(c.mix, 100.0f)});

    const FlangerSettings &f = s.flanger;
    block({f.enabled ? 1 : 0, scaled(f.rateHz, 100.0f), scaled(f.depthMs, 100.0f),
          scaled(f.feedback, 100.0f), scaled(f.mix, 100.0f)});

    const PhaserSettings &p = s.phaser;
    block({p.enabled ? 1 : 0, scaled(p.rateHz, 100.0f), scaled(p.depth, 100.0f),
          scaled(p.feedback, 100.0f), scaled(p.mix, 100.0f)});

    const TremoloSettings &t = s.tremolo;
    block({t.enabled ? 1 : 0, scaled(t.rateHz, 100.0f), scaled(t.depth, 100.0f),
          static_cast<long>(t.shape), scaled(t.mix, 100.0f)});

    const DelaySettings &d = s.delay;
    block({d.enabled ? 1 : 0, scaled(d.timeMs, 10.0f), scaled(d.feedback, 100.0f),
          scaled(d.mix, 100.0f), scaled(d.damping, 100.0f), d.pingPong ? 1 : 0});

    const ReverbSettings &r = s.reverb;
    block({r.enabled ? 1 : 0, scaled(r.size, 100.0f), scaled(r.damping, 100.0f),
          scaled(r.predelayMs, 10.0f), scaled(r.mix, 100.0f)});

    const CreativeEqSettings &eq = s.eq;
    block({eq.enabled ? 1 : 0, scaled(eq.gain, 10.0f), scaled(eq.bass, 10.0f),
          scaled(eq.mid, 10.0f), scaled(eq.treble, 10.0f), scaled(eq.presence, 10.0f),
          scaled(eq.master, 10.0f)});

    // "stage0,stage1,...,stageN,presentCount" -- the processing sequence,
    // front to back, plus how many of those front entries the Rack UI
    // currently shows a module for. A loop rather than a hardcoded tuple so
    // a future stage addition only has to bump kFxStageCount, not hand-edit
    // this list too.
    std::vector<long> orderFields(kFxStageCount + 1);
    for (int i = 0; i < kFxStageCount; ++i) orderFields[i] = static_cast<long>(s.order[i]);
    orderFields[kFxStageCount] = static_cast<long>(s.presentCount);
    block(orderFields);

    // The five effects added after the order block became part of the wire
    // format, deliberately appended here rather than inserted earlier in the
    // sequence -- an older decoder's forEachBlock(spec, 10, ...) simply never
    // sees these trailing blocks and ignores them, and this encoder's own
    // order block above stays at the same position an older spec already
    // has it at, so specs keep round-tripping across the boundary either way.
    const RingModulatorSettings &rm = s.ringMod;
    block({rm.enabled ? 1 : 0, scaled(rm.frequencyHz, 10.0f), scaled(rm.fineTuneHz, 10.0f),
          scaled(rm.mix, 100.0f)});

    const EnvelopeFilterSettings &ef = s.envFilter;
    block({ef.enabled ? 1 : 0, scaled(ef.sensitivity, 100.0f), scaled(ef.attackMs, 10.0f),
          scaled(ef.releaseMs, 10.0f), scaled(ef.resonance, 100.0f), scaled(ef.mix, 100.0f)});

    const PitchShifterSettings &ps = s.pitch;
    block({ps.enabled ? 1 : 0, scaled(ps.semitones, 10.0f), scaled(ps.detuneCents, 10.0f),
          scaled(ps.mix, 100.0f)});

    const ReverseDelaySettings &rv = s.reverseDelay;
    block({rv.enabled ? 1 : 0, scaled(rv.timeMs, 10.0f), scaled(rv.feedback, 100.0f),
          scaled(rv.smoothing, 100.0f), scaled(rv.mix, 100.0f)});

    const TapeSaturatorSettings &ts = s.tapeSat;
    block({ts.enabled ? 1 : 0, scaled(ts.drive, 100.0f), scaled(ts.flutter, 100.0f),
          scaled(ts.age, 100.0f), scaled(ts.mix, 100.0f)});

    return out;
}

// Anything unparseable falls back to the default settings, block by block --
// a truncated or older string leaves the effects it did not mention exactly
// as CreativeFxSettings{} would construct them.
inline CreativeFxSettings decodeCreativeFx(const std::string &spec) {
    using namespace creativefx_detail;
    CreativeFxSettings s{};

    forEachBlock(spec, 15, [&](int i, const std::string &part) {
        long f[kFxStageCount + 1] = {};
        switch (i) {
            case 0:
                if (splitFields(part, f, 4) < 4) return;
                s.bitcrusher = {f[0] != 0, unscaled(f[1], 10.0f), unscaled(f[2], 100.0f),
                               unscaled(f[3], 100.0f)};
                break;
            case 1:
                if (splitFields(part, f, 5) < 5) return;
                s.overdrive = {f[0] != 0, unscaled(f[1], 100.0f), unscaled(f[2], 100.0f),
                              unscaled(f[3], 10.0f), unscaled(f[4], 100.0f)};
                break;
            case 2:
                if (splitFields(part, f, 5) < 5) return;
                s.chorus = {f[0] != 0, unscaled(f[1], 100.0f), unscaled(f[2], 100.0f),
                           unscaled(f[3], 100.0f), unscaled(f[4], 100.0f)};
                break;
            case 3:
                if (splitFields(part, f, 5) < 5) return;
                s.flanger = {f[0] != 0, unscaled(f[1], 100.0f), unscaled(f[2], 100.0f),
                            unscaled(f[3], 100.0f), unscaled(f[4], 100.0f)};
                break;
            case 4:
                if (splitFields(part, f, 5) < 5) return;
                s.phaser = {f[0] != 0, unscaled(f[1], 100.0f), unscaled(f[2], 100.0f),
                           unscaled(f[3], 100.0f), unscaled(f[4], 100.0f)};
                break;
            case 5:
                if (splitFields(part, f, 5) < 5) return;
                s.tremolo = {f[0] != 0, unscaled(f[1], 100.0f), unscaled(f[2], 100.0f),
                            static_cast<int>(f[3]), unscaled(f[4], 100.0f)};
                break;
            case 6:
                if (splitFields(part, f, 6) < 6) return;
                s.delay = {f[0] != 0, unscaled(f[1], 10.0f), unscaled(f[2], 100.0f),
                          unscaled(f[3], 100.0f), unscaled(f[4], 100.0f), f[5] != 0};
                break;
            case 7:
                if (splitFields(part, f, 5) < 5) return;
                s.reverb = {f[0] != 0, unscaled(f[1], 100.0f), unscaled(f[2], 100.0f),
                           unscaled(f[3], 10.0f), unscaled(f[4], 100.0f)};
                break;
            case 8:
                if (splitFields(part, f, 7) < 7) return;
                s.eq = {f[0] != 0,          unscaled(f[1], 10.0f), unscaled(f[2], 10.0f),
                       unscaled(f[3], 10.0f), unscaled(f[4], 10.0f), unscaled(f[5], 10.0f),
                       unscaled(f[6], 10.0f)};
                break;
            case 9: {
                // The order permutation is required; presentCount is a
                // later addition and optional -- a spec written before it
                // existed still restores a valid order, just with
                // presentCount left at its "unknown" sentinel so the caller
                // falls back to inferring presence from which stages are
                // enabled, same as before this field existed.
                const int got = splitFields(part, f, kFxStageCount + 1);
                if (got < kFxStageCount) return;
                if (!isValidOrderSpec(f)) return;
                for (int k = 0; k < kFxStageCount; ++k) s.order[k] = static_cast<int>(f[k]);
                if (got >= kFxStageCount + 1)
                    s.presentCount = static_cast<int>(f[kFxStageCount]);
                break;
            }
            case 10:
                if (splitFields(part, f, 4) < 4) return;
                s.ringMod = {f[0] != 0, unscaled(f[1], 10.0f), unscaled(f[2], 10.0f),
                            unscaled(f[3], 100.0f)};
                break;
            case 11:
                if (splitFields(part, f, 6) < 6) return;
                s.envFilter = {f[0] != 0,          unscaled(f[1], 100.0f), unscaled(f[2], 10.0f),
                              unscaled(f[3], 10.0f), unscaled(f[4], 100.0f), unscaled(f[5], 100.0f)};
                break;
            case 12:
                if (splitFields(part, f, 4) < 4) return;
                s.pitch = {f[0] != 0, unscaled(f[1], 10.0f), unscaled(f[2], 10.0f),
                          unscaled(f[3], 100.0f)};
                break;
            case 13:
                if (splitFields(part, f, 5) < 5) return;
                s.reverseDelay = {f[0] != 0, unscaled(f[1], 10.0f), unscaled(f[2], 100.0f),
                                 unscaled(f[3], 100.0f), unscaled(f[4], 100.0f)};
                break;
            case 14:
                if (splitFields(part, f, 5) < 5) return;
                s.tapeSat = {f[0] != 0, unscaled(f[1], 100.0f), unscaled(f[2], 100.0f),
                            unscaled(f[3], 100.0f), unscaled(f[4], 100.0f)};
                break;
            default:
                break;
        }
    });

    return s;
}

}  // namespace waveline

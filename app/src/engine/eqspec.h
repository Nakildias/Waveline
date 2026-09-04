// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The parametric ("Pro") EQ's band model, and the wire format it travels in.
//
// Header-only and free of Qt, PipeWire and the rest of the engine, because
// three very different places need the same definition of a band: the filter
// that runs it, the daemon that stores it, and the GUI -- which links neither
// of the other two and draws the curve from these numbers alone.
//
// The wire format is integers only. Bands cross D-Bus and land in a config
// file as text, and a user in a comma-decimal locale writing "1,5" into a
// comma-separated field is a corrupted profile, not a rounding error. Tenths
// of a hertz and of a decibel, hundredths of a Q, are finer than anything the
// UI can express.

#pragma once

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

namespace waveline {

enum class EqBandType {
    Peak = 0,
    LowShelf = 1,
    HighShelf = 2,
    HighPass = 3,
    LowPass = 4,
    Notch = 5,
    BandPass = 6,
};

constexpr int kEqBandTypeCount = 7;

// Ten is what a channel strip on a desk gives you and what people expect from
// "the pro one". More would be free in CPU terms and unreadable on the curve.
constexpr int kProEqBands = 10;

constexpr float kEqMinHz = 20.0f;
constexpr float kEqMaxHz = 20000.0f;
// Matches the vertical range the curve is drawn over: a band that can be set
// past the top of its own graph is a band whose node cannot be dragged back.
constexpr float kEqMinDb = -18.0f;
constexpr float kEqMaxDb = 18.0f;
constexpr float kEqMinQ = 0.1f;
constexpr float kEqMaxQ = 18.0f;

struct EqBand {
    bool on = false;
    EqBandType type = EqBandType::Peak;
    float freq = 1000.0f;
    float gainDb = 0.0f;
    float q = 1.0f;
};

using EqBands = std::array<EqBand, kProEqBands>;

// Shelves and bells are set by gain; the pass filters and the notch are not,
// and their gain control is hidden rather than left there doing nothing.
inline bool eqTypeUsesGain(EqBandType t) {
    return t == EqBandType::Peak || t == EqBandType::LowShelf ||
           t == EqBandType::HighShelf;
}

inline float clampEqf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline void clampEqBand(EqBand &b) {
    if (static_cast<int>(b.type) < 0 || static_cast<int>(b.type) >= kEqBandTypeCount)
        b.type = EqBandType::Peak;
    b.freq = clampEqf(b.freq, kEqMinHz, kEqMaxHz);
    b.gainDb = clampEqf(b.gainDb, kEqMinDb, kEqMaxDb);
    b.q = clampEqf(b.q, kEqMinQ, kEqMaxQ);
    if (!eqTypeUsesGain(b.type)) b.gainDb = 0.0f;
}

// The layout a fresh EQ opens with: every band switched off (so the default is
// exactly flat), but already spread across the spectrum in the roles people
// reach for -- a high-pass at the bottom, a shelf at each end, bells between.
// Switching one on is then a single click rather than a hunt for a frequency.
inline EqBands defaultEqBands() {
    EqBands b{};
    struct Slot {
        EqBandType type;
        float freq;
        float q;
    };
    static constexpr Slot kSlots[kProEqBands] = {
        {EqBandType::HighPass, 30.0f, 0.707f},   {EqBandType::LowShelf, 100.0f, 0.707f},
        {EqBandType::Peak, 200.0f, 1.0f},        {EqBandType::Peak, 450.0f, 1.0f},
        {EqBandType::Peak, 1000.0f, 1.0f},       {EqBandType::Peak, 2200.0f, 1.0f},
        {EqBandType::Peak, 4500.0f, 1.0f},       {EqBandType::Peak, 8000.0f, 1.0f},
        {EqBandType::HighShelf, 12000.0f, 0.707f},
        {EqBandType::LowPass, 18000.0f, 0.707f},
    };
    for (int i = 0; i < kProEqBands; ++i) {
        b[i].on = false;
        b[i].type = kSlots[i].type;
        b[i].freq = kSlots[i].freq;
        b[i].gainDb = 0.0f;
        b[i].q = kSlots[i].q;
    }
    return b;
}

inline bool anyEqBandOn(const EqBands &bands) {
    for (const EqBand &b : bands)
        if (b.on) return true;
    return false;
}

// "on,type,freqTenthHz,gainTenthDb,qHundredths" per band, bands joined by ';'.
inline std::string encodeEqBands(const EqBands &bands) {
    std::string out;
    for (int i = 0; i < kProEqBands; ++i) {
        EqBand b = bands[i];
        clampEqBand(b);
        if (i) out += ';';
        out += std::to_string(b.on ? 1 : 0);
        out += ',';
        out += std::to_string(static_cast<int>(b.type));
        out += ',';
        out += std::to_string(static_cast<long>(std::lround(b.freq * 10.0f)));
        out += ',';
        out += std::to_string(static_cast<long>(std::lround(b.gainDb * 10.0f)));
        out += ',';
        out += std::to_string(static_cast<long>(std::lround(b.q * 100.0f)));
    }
    return out;
}

// Anything unparseable falls back to the default layout, band by band: a
// truncated or older string leaves the bands it did not mention at their
// defaults rather than at zero hertz.
inline EqBands decodeEqBands(const std::string &spec) {
    EqBands bands = defaultEqBands();
    size_t pos = 0;
    for (int i = 0; i < kProEqBands && pos <= spec.size(); ++i) {
        const size_t end = spec.find(';', pos);
        const std::string part =
            spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = (end == std::string::npos) ? spec.size() + 1 : end + 1;
        if (part.empty()) continue;

        long field[5] = {0, 0, 0, 0, 0};
        int n = 0;
        size_t fp = 0;
        while (n < 5 && fp <= part.size()) {
            const size_t fe = part.find(',', fp);
            const std::string tok =
                part.substr(fp, fe == std::string::npos ? std::string::npos : fe - fp);
            field[n++] = std::strtol(tok.c_str(), nullptr, 10);
            if (fe == std::string::npos) break;
            fp = fe + 1;
        }
        if (n < 5) continue;

        EqBand b;
        b.on = field[0] != 0;
        b.type = static_cast<EqBandType>(field[1]);
        b.freq = static_cast<float>(field[2]) / 10.0f;
        b.gainDb = static_cast<float>(field[3]) / 10.0f;
        b.q = static_cast<float>(field[4]) / 100.0f;
        clampEqBand(b);
        bands[i] = b;
    }
    return bands;
}

}  // namespace waveline

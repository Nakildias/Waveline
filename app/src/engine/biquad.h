// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Biquad filters for the per-channel effects chain (EQ and low-cut).

#pragma once

#include <algorithm>
#include <cmath>

#include "eqspec.h"

namespace waveline {

struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() { z1 = z2 = 0.0f; }

    // Retune in place. Assigning a whole Biquad -- `f = Biquad::lowPass(...)` --
    // copies the factory's zeroed z1/z2 over the running state, which restarts
    // the filter mid-signal and is heard as a click. Every parameter change on
    // a live filter goes through here instead.
    void setCoeffs(const Biquad &src) {
        b0 = src.b0;
        b1 = src.b1;
        b2 = src.b2;
        a1 = src.a1;
        a2 = src.a2;
    }

    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    static Biquad highPass(float sampleRate, float freq, float q = 0.707f) {
        const float w0 = 2.0f * float(M_PI) * freq / sampleRate;
        const float cosw = std::cos(w0);
        const float sinw = std::sin(w0);
        const float alpha = sinw / (2.0f * q);
        const float a0 = 1.0f + alpha;
        Biquad f;
        f.b0 = (1.0f + cosw) / 2.0f / a0;
        f.b1 = -(1.0f + cosw) / a0;
        f.b2 = f.b0;
        f.a1 = -2.0f * cosw / a0;
        f.a2 = (1.0f - alpha) / a0;
        return f;
    }

    static Biquad lowPass(float sampleRate, float freq, float q = 0.707f) {
        const float w0 = 2.0f * float(M_PI) * freq / sampleRate;
        const float cosw = std::cos(w0);
        const float sinw = std::sin(w0);
        const float alpha = sinw / (2.0f * q);
        const float a0 = 1.0f + alpha;
        Biquad f;
        f.b0 = (1.0f - cosw) / 2.0f / a0;
        f.b1 = (1.0f - cosw) / a0;
        f.b2 = f.b0;
        f.a1 = -2.0f * cosw / a0;
        f.a2 = (1.0f - alpha) / a0;
        return f;
    }

    static Biquad lowShelf(float sampleRate, float freq, float gainDb, float q = 0.707f) {
        const float a = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = 2.0f * float(M_PI) * freq / sampleRate;
        const float cosw = std::cos(w0);
        const float sinw = std::sin(w0);
        const float alpha = sinw / (2.0f * q);
        const float sqrtA = std::sqrt(a);
        const float ap1 = a + 1.0f;
        const float am1 = a - 1.0f;
        const float twoSqrtAalpha = 2.0f * sqrtA * alpha;
        const float a0 = ap1 + am1 * cosw + twoSqrtAalpha;
        Biquad f;
        f.b0 = a * (ap1 - am1 * cosw + twoSqrtAalpha) / a0;
        f.b1 = 2.0f * a * (am1 - ap1 * cosw) / a0;
        f.b2 = a * (ap1 - am1 * cosw - twoSqrtAalpha) / a0;
        f.a1 = -2.0f * (am1 + ap1 * cosw) / a0;
        f.a2 = (ap1 + am1 * cosw - twoSqrtAalpha) / a0;
        return f;
    }

    static Biquad peaking(float sampleRate, float freq, float gainDb, float q = 1.0f) {
        const float a = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = 2.0f * float(M_PI) * freq / sampleRate;
        const float cosw = std::cos(w0);
        const float sinw = std::sin(w0);
        const float alpha = sinw / (2.0f * q);
        const float a0 = 1.0f + alpha / a;
        Biquad f;
        f.b0 = (1.0f + alpha * a) / a0;
        f.b1 = (-2.0f * cosw) / a0;
        f.b2 = (1.0f - alpha * a) / a0;
        f.a1 = f.b1;
        f.a2 = (1.0f - alpha / a) / a0;
        return f;
    }

    static Biquad notch(float sampleRate, float freq, float q = 1.0f) {
        const float w0 = 2.0f * float(M_PI) * freq / sampleRate;
        const float cosw = std::cos(w0);
        const float sinw = std::sin(w0);
        const float alpha = sinw / (2.0f * q);
        const float a0 = 1.0f + alpha;
        Biquad f;
        f.b0 = 1.0f / a0;
        f.b1 = -2.0f * cosw / a0;
        f.b2 = f.b0;
        f.a1 = f.b1;
        f.a2 = (1.0f - alpha) / a0;
        return f;
    }

    // Constant 0 dB peak gain, so a band-pass swept across the spectrum keeps
    // the level of whatever it is letting through.
    static Biquad bandPass(float sampleRate, float freq, float q = 1.0f) {
        const float w0 = 2.0f * float(M_PI) * freq / sampleRate;
        const float cosw = std::cos(w0);
        const float sinw = std::sin(w0);
        const float alpha = sinw / (2.0f * q);
        const float a0 = 1.0f + alpha;
        Biquad f;
        f.b0 = alpha / a0;
        f.b1 = 0.0f;
        f.b2 = -alpha / a0;
        f.a1 = -2.0f * cosw / a0;
        f.a2 = (1.0f - alpha) / a0;
        return f;
    }

    static Biquad highShelf(float sampleRate, float freq, float gainDb, float q = 0.707f) {
        const float a = std::pow(10.0f, gainDb / 40.0f);
        const float w0 = 2.0f * float(M_PI) * freq / sampleRate;
        const float cosw = std::cos(w0);
        const float sinw = std::sin(w0);
        const float alpha = sinw / (2.0f * q);
        const float sqrtA = std::sqrt(a);
        const float ap1 = a + 1.0f;
        const float am1 = a - 1.0f;
        const float twoSqrtAalpha = 2.0f * sqrtA * alpha;
        const float a0 = ap1 - am1 * cosw + twoSqrtAalpha;
        Biquad f;
        f.b0 = a * (ap1 + am1 * cosw + twoSqrtAalpha) / a0;
        f.b1 = -2.0f * a * (am1 + ap1 * cosw) / a0;
        f.b2 = a * (ap1 + am1 * cosw - twoSqrtAalpha) / a0;
        f.a1 = 2.0f * (am1 - ap1 * cosw) / a0;
        f.a2 = (ap1 - am1 * cosw - twoSqrtAalpha) / a0;
        return f;
    }

    // |H(e^jw)| in dB. Not used by the audio path -- this is what draws the
    // EQ curve, which has to be the response of the filters that actually run
    // rather than an idealised redraw of the same formulas.
    float magnitudeDb(float sampleRate, float freq) const {
        const float w = 2.0f * float(M_PI) * freq / sampleRate;
        const float c1 = std::cos(w), s1 = std::sin(w);
        const float c2 = std::cos(2.0f * w), s2 = std::sin(2.0f * w);
        const float nr = b0 + b1 * c1 + b2 * c2;
        const float ni = -(b1 * s1 + b2 * s2);
        const float dr = 1.0f + a1 * c1 + a2 * c2;
        const float di = -(a1 * s1 + a2 * s2);
        const float num = nr * nr + ni * ni;
        const float den = dr * dr + di * di;
        if (den <= 1e-20f) return 0.0f;
        const float mag2 = num / den;
        if (mag2 <= 1e-12f) return -120.0f;
        return 10.0f * std::log10(mag2);
    }

    // The filter a band asks for. One place, so the curve the GUI draws and
    // the coefficients the audio thread loads can never come from two
    // different readings of the same band.
    static Biquad forBand(float sampleRate, EqBandType type, float freq, float gainDb,
                          float q) {
        const float f = clampEqf(freq, kEqMinHz, std::min(kEqMaxHz, sampleRate * 0.49f));
        const float qq = clampEqf(q, kEqMinQ, kEqMaxQ);
        switch (type) {
            case EqBandType::LowShelf:
                return lowShelf(sampleRate, f, gainDb, qq);
            case EqBandType::HighShelf:
                return highShelf(sampleRate, f, gainDb, qq);
            case EqBandType::HighPass:
                return highPass(sampleRate, f, qq);
            case EqBandType::LowPass:
                return lowPass(sampleRate, f, qq);
            case EqBandType::Notch:
                return notch(sampleRate, f, qq);
            case EqBandType::BandPass:
                return bandPass(sampleRate, f, qq);
            case EqBandType::Peak:
                break;
        }
        return peaking(sampleRate, f, gainDb, qq);
    }
};

}  // namespace waveline

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// What a capture device's latency actually is, read from the kernel.
//
// This replaces an arithmetic answer with a measured one. The version before
// it took PipeWire's SPA_PARAM_Latency, resolved its quantum term against the
// graph clock from the settings metadata, and added a per-node node.latency
// request on top. Every input of that calculation was real; the result was
// not, because node.latency is a *quantum request* and quantum is negotiated
// per driver group rather than per node. A device that asked for a short cycle
// got charged its own request and a device that asked for nothing got charged
// the graph default, so two microphones measuring identically on the hardware
// were reported 4x apart -- ranked entirely by which of them happened to ask.
//
// ALSA already knows. Every running PCM publishes `delay` in
// /proc/asound/card<N>/pcm<D>c/sub<S>/status: how many frames sit between the
// hardware pointer and the application pointer, which is exactly how far
// behind real time that capture is. It costs two small file reads to sample,
// it needs no PipeWire concepts to interpret, and it is the number the device
// itself is living with.
//
// ---------------------------------------------------------------- the caveats
//
// It is noisy. The raw value swings across a cycle as the buffer fills and
// drains, so a single reading is not a latency -- it is a phase. This keeps a
// rolling window per device and reports the median.
//
// It is sometimes wrong. The sc0710 capture card reports `delay: 0` while
// claiming state RUNNING; zero is not a latency, it is a status file that does
// not work, and averaging it in would quietly halve the figure. Readings
// outside a plausible band are dropped rather than smoothed.
//
// It is capture-side only, and it does not include anything the device did
// before handing audio to the host -- a camera running its own noise
// suppression can spend a hundred milliseconds that is invisible here by
// construction. See scripts/latency-offset-test.sh, and say so in the UI
// rather than implying the figure is end-to-end.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace waveline {

// One ALSA capture PCM, identified the way PipeWire describes it.
struct AlsaPcmRef {
    int card = -1;
    int device = 0;
    int subdevice = 0;

    bool valid() const { return card >= 0; }
    bool operator<(const AlsaPcmRef &o) const {
        if (card != o.card) return card < o.card;
        if (device != o.device) return device < o.device;
        return subdevice < o.subdevice;
    }
};

// Which capture PCM belongs to a card, found by looking. Returns an invalid
// ref when the card has none.
//
// Deliberately not parsed out of api.alsa.path. That property is not always
// "hw:N" -- measured on one machine, two of four microphones reported
// "front:5" and "front:7" instead, because PipeWire names the device with
// whichever ALSA plugin it opened. A parser that accepted only "hw:" silently
// dropped both devices from every latency figure and every diagnostic, and the
// ones it did accept were the ones that happened not to need a plugin. So the
// card index (api.alsa.pcm.card, always a plain integer) is the identity, and
// the PCM under it is discovered rather than assumed -- capture is normally
// pcm0c but nothing guarantees it.
AlsaPcmRef findCapturePcm(int card);

class AlsaDelayProbe {
public:
    // Microseconds. Same convention as PwNode::latencyUs: -1 means "we have
    // not been able to measure this", which is a different claim from zero and
    // must not be rendered as one.
    static constexpr int64_t kUnknown = -1;

    // Samples every device handed to track() once. Cheap enough to call from a
    // UI-rate timer: two reads of a small procfs file per device, and the rate
    // is only re-read when the PCM restarts.
    void sample();

    // Starts (or keeps) watching a PCM. Devices not re-registered between
    // sweeps are forgotten by forgetUntracked(), so unplugging a microphone
    // does not leave its last reading to be reported forever.
    void track(const AlsaPcmRef &ref);
    void beginSweep();
    void forgetUntracked();

    // Median of the recent window, or kUnknown when there are not enough
    // usable readings yet. Deliberately requires several: reporting a latency
    // off one sample means reporting a phase of the buffer cycle.
    int64_t medianUs(const AlsaPcmRef &ref) const;

    // Diagnostics: what the kernel says about this PCM right now, for the
    // view that has to explain an unexpected number rather than just show it.
    struct Detail {
        bool running = false;
        int rate = 0;
        int periodSize = 0;
        int bufferSize = 0;
        int64_t medianUs = kUnknown;
        int64_t minUs = kUnknown;
        int64_t maxUs = kUnknown;
        int samples = 0;
        int rejected = 0;   // readings dropped as implausible
    };
    Detail detail(const AlsaPcmRef &ref) const;

private:
    struct Track {
        bool seen = false;          // registered during the current sweep
        int rate = 0;
        int periodSize = 0;
        int bufferSize = 0;
        std::string statusPath;
        std::string hwParamsPath;
        std::deque<int64_t> window;  // recent usable delays, in frames
        int rejected = 0;
        bool running = false;
        // hw_params only changes when the stream is reopened, which is rare
        // and always accompanied by the stream leaving RUNNING. Re-reading it
        // every tick would double the syscalls for a value that is constant
        // between those moments.
        bool haveParams = false;
    };

    void refreshParams(Track &t) const;

    std::map<AlsaPcmRef, Track> tracks_;
};

}  // namespace waveline

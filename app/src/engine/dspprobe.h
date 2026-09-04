// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// What each DSP stage costs, and which of them missed a deadline.
//
// The latency panel could already say how long the kernel holds a microphone's
// audio before this process sees it. It could not say what happens next: the
// noise suppressor, the EQ, the compressor and the creative chain all run
// between that reading and the mix, and their cost was invisible. "Is the
// crackling my effects or my buffer size" had no answer here, and neither did
// "which of these six effects is the expensive one".
//
// Not answerable from outside the process either. `pw-top` reports time per
// *client*, and every filter in this graph is its own client with a name like
// waveline-voice-nc -- so the answer arrives as twenty rows that have to be
// mapped back onto input devices by hand, with the channel chains mixed in
// among them. Measured here, each stage already knows which bus it belongs to.
//
// Two separate things are recorded, and conflating them is the mistake this
// header exists to prevent:
//
//   * How long a stage spends inside its process callback. This is CPU cost,
//     not delay. A filter that takes 0.3 ms does not add 0.3 ms of latency --
//     everything in a driver group runs inside one cycle, and as long as the
//     cycle is met the audio comes out on time regardless.
//   * How much delay a stage genuinely adds, which is a property of its
//     algorithm rather than of the machine: noise suppression buffers a whole
//     frame before it can emit anything, and that 10 ms is paid on the fastest
//     CPU in the world. Declared at attach() time, not measured.
//
// The first turns into the second only when it overruns, which is what
// SPA_IO_CLOCK_FLAG_XRUN_RECOVER reports: PipeWire sets it on a node that did
// not finish the previous cycle in time. That flag is the missed-cycle count,
// and it is per node -- so a graph that is glitching names the stage that did
// it instead of leaving the user to guess between six.
//
// Off by default, and the check is one relaxed atomic load inlined into the
// caller: a disabled probe costs a branch that every branch predictor on the
// planet gets right, and nothing else. Enabled it costs two clock_gettime()
// calls per stage per cycle -- tens of nanoseconds against a cycle budget of
// 2700 us at the shortest quantum this mixer offers. Toggle-able anyway,
// because a measurement running when nobody is reading it is a cost with no
// buyer, and because "does it still glitch with the probe off" has to be a
// question the user can ask.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct spa_io_position;

namespace waveline {

namespace detail {
// Read on every audio thread once per stage per cycle. Public only so the
// check below can inline into the process callbacks; nothing outside this
// header should touch it.
extern std::atomic<bool> gDspProfiling;
}  // namespace detail

inline bool dspProfiling() {
    return detail::gDspProfiling.load(std::memory_order_relaxed);
}

// Turning it on resets every stage's counters, so the numbers always mean
// "since you asked for them" rather than "since the daemon started, three days
// and two microphone swaps ago". Safe from any thread.
void setDspProfiling(bool on);

// One stage's accounting. Written only by the audio thread that owns it, read
// under the registry lock by whoever is drawing the panel. Every field is
// relaxed: these are counters, nothing downstream of them is ordered against
// anything, and an audio thread must not pay for a fence to report on itself.
struct DspCounters {
    // Bumped by setDspProfiling(true). The audio thread zeroes its own block
    // when it notices the change, which is why a reset needs no writer other
    // than the thread that owns the counters -- and so cannot race with one.
    std::atomic<uint32_t> generation{0};
    std::atomic<uint64_t> cycles{0};
    std::atomic<uint64_t> nsecTotal{0};
    // Wall time, so it includes anything that preempted the audio thread --
    // an interrupt, a higher-priority thread, a page fault. That is on purpose
    // for the question being asked ("did this fit in its cycle" does not care
    // why it did not), but it does mean a single peak can be a scheduling
    // event rather than expensive DSP. The average is the figure to optimise
    // against; the peak is the figure to check for headroom.
    std::atomic<uint64_t> nsecMax{0};
    // Cycles PipeWire told this node it had overrun. The missed-cycle count.
    std::atomic<uint64_t> xruns{0};
    // Cycles where this stage alone spent longer than the whole cycle budget.
    // Distinct from xruns above: this one says the stage cannot keep up on its
    // own, that one says the graph noticed. Usually the first predicts the
    // second, and seeing which came first is the point.
    std::atomic<uint64_t> overruns{0};
    // Budget of the most recent cycle, so a percentage can be worked out
    // against the quantum actually in force rather than the one configured.
    std::atomic<uint64_t> cycleNsec{0};
    // The driver's own running total of dropped audio, in nanoseconds, less
    // whatever it already read when profiling was switched on.
    std::atomic<uint64_t> driverXrunNsec{0};
    std::atomic<uint64_t> driverXrunBase{0};
    // Cycles still to be thrown away before anything is recorded. The first
    // cycles after a filter starts, or after the counters are reset, are cold
    // caches, first-touch page faults and a denoiser that has not yet been
    // through its own code once -- routinely an order of magnitude slower than
    // the steady state. Kept out entirely rather than averaged in: they would
    // own the peak figure permanently and make it useless for the one thing it
    // is for, which is knowing how close a chain runs to its cycle.
    std::atomic<uint32_t> warmup{0};
};

// About 16 cycles: 43 ms at the shortest quantum this mixer offers, 340 ms at
// the longest. Short enough that a panel opened straight after a reset is not
// waiting on it, long enough to be past the first-touch costs.
inline constexpr uint32_t kDspWarmupCycles = 16;

// Held by a filter, for its lifetime. Registers the counters under the node's
// name so the graph can find them again without every filter class having to
// grow a reporting API of its own.
class DspMeter {
public:
    DspMeter() = default;
    ~DspMeter();
    DspMeter(const DspMeter &) = delete;
    DspMeter &operator=(const DspMeter &) = delete;

    // Call from start(), *before* pw_filter_connect(): after that the audio
    // thread reads these counters with no synchronisation, which is only safe
    // because nothing writes the registration while that thread exists.
    //
    // latencyFrames is the delay the stage's algorithm adds at 48 kHz, and is
    // 0 for the great majority of them -- a biquad, a gain and a compressor
    // all produce their output from the sample they were handed.
    void attach(std::string node, std::string kind, uint32_t latencyFrames = 0);
    // Call from stop(), *after* pw_filter_destroy(): when this returns the
    // counters are out of the registry, so no reader can still be inside them.
    void detach();

    // For a stage whose algorithm can be swapped under it while it runs. Noise
    // suppression is the one: RNNoise and DeepFilterNet buffer different
    // amounts, and a panel still quoting the old figure would be reporting a
    // delay nothing in the graph is paying. No-op while detached.
    void setLatencyFrames(uint32_t frames);

    DspCounters &counters() { return c_; }

private:
    DspCounters c_;
    std::string name_;
    bool live_ = false;
};

// Wraps a process callback. Does nothing at all while profiling is off.
class DspScope {
public:
    DspScope(DspMeter &m, const spa_io_position *pos) {
        if (dspProfiling()) begin(m.counters(), pos);
    }
    ~DspScope() {
        if (c_) end();
    }
    DspScope(const DspScope &) = delete;
    DspScope &operator=(const DspScope &) = delete;

private:
    void begin(DspCounters &c, const spa_io_position *pos);
    void end();

    DspCounters *c_ = nullptr;
    uint64_t t0_ = 0;
    bool warming_ = false;
};

// A stage as the panel sees it. Plain numbers: no atomics escape the registry.
struct DspStageStats {
    std::string node;
    std::string kind;
    uint32_t latencyFrames = 0;
    uint64_t cycles = 0;
    uint64_t xruns = 0;
    uint64_t overruns = 0;
    double avgUs = 0.0;
    double maxUs = 0.0;
    // The cycle this stage was last given, so "% of cycle" is against the
    // quantum in force rather than the one the settings file asked for.
    double cycleUs = 0.0;
    double driverXrunMs = 0.0;

    // A stage nothing has scheduled since the counters were reset. Its zeroes
    // are an absence of measurement, not a measurement of zero, and the two
    // must never be drawn the same way.
    bool idle() const { return cycles == 0; }
};

// Every attached stage, in no particular order.
std::vector<DspStageStats> dspStages();
// Just these, in the order given, skipping any name nothing is attached under.
// This is how a chain is reported in signal order rather than in whatever
// order the filters happened to register.
std::vector<DspStageStats> dspStages(const std::vector<std::string> &nodes);

}  // namespace waveline

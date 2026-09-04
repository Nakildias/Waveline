// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "dspprobe.h"

#include <spa/node/io.h>

#include <ctime>
#include <mutex>
#include <unordered_map>

namespace waveline {

namespace detail {
std::atomic<bool> gDspProfiling{false};
}  // namespace detail

namespace {

// What a stage registered under, beside the counters it writes into. The
// counters live in the filter, not here: a stage that stops must be able to
// take its storage with it, and a registry that owned the blocks would have to
// outlive every filter to make that safe.
struct Entry {
    DspCounters *counters = nullptr;
    std::string kind;
    uint32_t latencyFrames = 0;
};

// Registration is rare -- a handful of stages per input device, only when the
// graph is built or rebuilt -- and reads happen on a 2 s panel timer. The lock
// is never taken by an audio thread: those touch only their own counters,
// through the atomics, which is the whole reason this is a registry of
// pointers rather than of values.
std::mutex &registryLock() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, Entry> &registry() {
    static std::unordered_map<std::string, Entry> r;
    return r;
}

// Bumped on every enable. The audio threads compare it against their own copy
// and zero themselves when it moves, so a reset touches no counter from the
// outside and cannot tear one in half.
std::atomic<uint32_t> gGeneration{1};

uint64_t nowNsec() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

// The cycle's budget, from the driver's own clock rather than from our idea of
// the quantum. clock.rate is a fraction (1/48000 in every graph this mixer has
// seen, but it is not ours to assume) and duration is counted in it.
uint64_t cycleNsec(const spa_io_clock &c) {
    if (c.rate.num == 0 || c.rate.denom == 0) return 0;
    return c.duration * 1000000000ull * c.rate.num / c.rate.denom;
}

// clock.xrun is the driver's accumulated dropped audio, counted in the same
// units as duration.
uint64_t driverXrunNsec(const spa_io_clock &c) {
    if (c.rate.num == 0 || c.rate.denom == 0) return 0;
    return c.xrun * 1000000000ull * c.rate.num / c.rate.denom;
}

DspStageStats snapshot(const std::string &node, const Entry &e) {
    DspStageStats s;
    s.node = node;
    s.kind = e.kind;
    s.latencyFrames = e.latencyFrames;
    const DspCounters &c = *e.counters;
    s.cycles = c.cycles.load(std::memory_order_relaxed);
    s.xruns = c.xruns.load(std::memory_order_relaxed);
    s.overruns = c.overruns.load(std::memory_order_relaxed);
    const uint64_t total = c.nsecTotal.load(std::memory_order_relaxed);
    if (s.cycles > 0) s.avgUs = double(total) / double(s.cycles) / 1000.0;
    s.maxUs = double(c.nsecMax.load(std::memory_order_relaxed)) / 1000.0;
    s.cycleUs = double(c.cycleNsec.load(std::memory_order_relaxed)) / 1000.0;

    const uint64_t xrunNow = c.driverXrunNsec.load(std::memory_order_relaxed);
    const uint64_t xrunBase = c.driverXrunBase.load(std::memory_order_relaxed);
    if (xrunNow > xrunBase)
        s.driverXrunMs = double(xrunNow - xrunBase) / 1000000.0;

    // A stage whose counters belong to an earlier run of the probe has not been
    // scheduled since the reset, so it has nothing to say. Reported as idle
    // rather than as a stale total, which would otherwise show a filter that
    // stopped days ago as the most expensive thing in the graph.
    if (c.generation.load(std::memory_order_relaxed) !=
        gGeneration.load(std::memory_order_relaxed)) {
        s.cycles = 0;
        s.xruns = 0;
        s.overruns = 0;
        s.avgUs = 0.0;
        s.maxUs = 0.0;
        s.driverXrunMs = 0.0;
    }
    return s;
}

}  // namespace

void setDspProfiling(bool on) {
    if (on) {
        // Generation first, so no audio thread can start a cycle against the
        // new flag while still holding the old generation and add one more
        // sample to the totals the user is about to be shown as fresh.
        gGeneration.fetch_add(1, std::memory_order_relaxed);
    }
    detail::gDspProfiling.store(on, std::memory_order_relaxed);
}

// ------------------------------------------------------------------- meter

void DspMeter::attach(std::string node, std::string kind, uint32_t latencyFrames) {
    if (node.empty()) return;
    detach();
    Entry e;
    e.counters = &c_;
    e.kind = std::move(kind);
    e.latencyFrames = latencyFrames;
    {
        std::lock_guard<std::mutex> lock(registryLock());
        registry()[node] = std::move(e);
    }
    name_ = std::move(node);
    live_ = true;
}

void DspMeter::detach() {
    if (!live_) return;
    live_ = false;
    std::lock_guard<std::mutex> lock(registryLock());
    auto it = registry().find(name_);
    // Only if it is still ours. A filter that was destroyed and rebuilt under
    // the same node name has already replaced this entry, and erasing it here
    // would unregister the live stage on behalf of the dead one.
    if (it != registry().end() && it->second.counters == &c_) registry().erase(it);
    name_.clear();
}

void DspMeter::setLatencyFrames(uint32_t frames) {
    if (!live_) return;
    std::lock_guard<std::mutex> lock(registryLock());
    auto it = registry().find(name_);
    if (it != registry().end() && it->second.counters == &c_)
        it->second.latencyFrames = frames;
}

DspMeter::~DspMeter() { detach(); }

// ------------------------------------------------------------------- scope

void DspScope::begin(DspCounters &c, const spa_io_position *pos) {
    const uint32_t gen = gGeneration.load(std::memory_order_relaxed);
    if (c.generation.load(std::memory_order_relaxed) != gen) {
        c.cycles.store(0, std::memory_order_relaxed);
        c.nsecTotal.store(0, std::memory_order_relaxed);
        c.nsecMax.store(0, std::memory_order_relaxed);
        c.xruns.store(0, std::memory_order_relaxed);
        c.overruns.store(0, std::memory_order_relaxed);
        c.driverXrunNsec.store(0, std::memory_order_relaxed);
        c.driverXrunBase.store(pos ? driverXrunNsec(pos->clock) : 0,
                               std::memory_order_relaxed);
        c.warmup.store(kDspWarmupCycles, std::memory_order_relaxed);
        c.generation.store(gen, std::memory_order_relaxed);
    }

    warming_ = c.warmup.load(std::memory_order_relaxed) > 0;

    if (pos) {
        c.cycleNsec.store(cycleNsec(pos->clock), std::memory_order_relaxed);
        c.driverXrunNsec.store(driverXrunNsec(pos->clock), std::memory_order_relaxed);
        // Set by the driver at the top of this cycle when *this* node did not
        // finish the last one, and cleared again once the callback returns --
        // so it has to be read here rather than after the DSP has run.
        if (!warming_ && (pos->clock.flags & SPA_IO_CLOCK_FLAG_XRUN_RECOVER))
            c.xruns.fetch_add(1, std::memory_order_relaxed);
    }

    c_ = &c;
    t0_ = nowNsec();
}

void DspScope::end() {
    const uint64_t spent = nowNsec() - t0_;
    DspCounters &c = *c_;
    c_ = nullptr;

    if (warming_) {
        c.warmup.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    c.cycles.fetch_add(1, std::memory_order_relaxed);
    c.nsecTotal.fetch_add(spent, std::memory_order_relaxed);
    if (spent > c.nsecMax.load(std::memory_order_relaxed))
        c.nsecMax.store(spent, std::memory_order_relaxed);

    const uint64_t budget = c.cycleNsec.load(std::memory_order_relaxed);
    if (budget > 0 && spent > budget)
        c.overruns.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------- readback

std::vector<DspStageStats> dspStages() {
    std::vector<DspStageStats> out;
    std::lock_guard<std::mutex> lock(registryLock());
    out.reserve(registry().size());
    for (const auto &[node, entry] : registry()) out.push_back(snapshot(node, entry));
    return out;
}

std::vector<DspStageStats> dspStages(const std::vector<std::string> &nodes) {
    std::vector<DspStageStats> out;
    std::lock_guard<std::mutex> lock(registryLock());
    out.reserve(nodes.size());
    for (const std::string &node : nodes) {
        auto it = registry().find(node);
        if (it != registry().end()) out.push_back(snapshot(node, it->second));
    }
    return out;
}

}  // namespace waveline

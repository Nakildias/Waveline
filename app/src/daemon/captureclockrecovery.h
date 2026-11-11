// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace waveline {

// Control-thread policy for the robotic-microphone bug.
//
// A USB microphone has its own crystal. Unless it drives the graph, its ALSA
// node follows whichever node does and bridges the two clocks with an
// adaptive resampler, which keeps a running estimate of their rate ratio.
// When the group's driver changes underneath a running capture -- a display
// unplugged or replugged reshuffling the GPU's HDMI/DP card, an output sink
// coming or going, a group re-partitioning -- that estimate belongs to a
// clock that is no longer there. The resampler can settle on a wrong ratio
// and stay on it, dropping or repeating a slice of every cycle: the voice
// still comes through, chopped into something robotic, until the capture hop
// is recreated (the manual Rebuild button).
//
// The registry often shows nothing useful when this happens. The GPU card's
// sinks are rarely assigned outputs and the microphone's own nodes never
// leave. So this watches the clock itself: CaptureSelector counts, on the
// data thread, every cycle that arrives from a different driver or at a
// different rate than the one before. Once that has changed and then stayed
// put long enough to be a settled graph, one settled recovery is asked for.
//
// Quantum changes are deliberately not counted. With the quantum negotiated,
// it moves whenever an application asks for a different latency, and a
// rebuild costs the microphone a few seconds of silence.
//
// Feedback guard: a rebuild can itself make the graph pick a driver again.
// Changes seen while recovering, or within kGraceMs after it, are absorbed
// into the baseline rather than answered, and the whole thing runs on a
// bounded budget so a graph that never settles cannot keep the mic in a
// rebuild loop.
class CaptureClockRecovery {
public:
    static constexpr std::int64_t kSettleMs = 2000;
    static constexpr std::int64_t kGraceMs = 5000;
    static constexpr std::int64_t kBudgetWindowMs = 600000;
    static constexpr unsigned kBudget = 6;

    // True when a settled recovery should be scheduled now.
    bool observe(std::int64_t now, std::uint64_t instance, std::uint64_t clockChanges,
                 std::uint64_t cycles, bool recovering = false) {
        if (instance != instance_ || clockChanges < changes_ || recovering) {
            instance_ = instance;
            changes_ = clockChanges;
            cycles_ = cycles;
            pending_ = false;
            quietUntil_ = std::max(quietUntil_, now + kGraceMs);
            return false;
        }
        const bool progressing = cycles != cycles_;
        cycles_ = cycles;
        if (clockChanges != changes_) {
            changes_ = clockChanges;
            lastChange_ = now;
            if (now >= quietUntil_) pending_ = true;
            return false;
        }
        // Wait for a running graph whose clock has stopped moving; a pending
        // change survives a suspended stretch and is answered on resume.
        if (!pending_ || !progressing || now - lastChange_ < kSettleMs) return false;
        pending_ = false;
        if (now - budgetStart_ >= kBudgetWindowMs) { budgetStart_ = now; recoveries_ = 0; }
        if (recoveries_ >= kBudget) return false;
        ++recoveries_;
        return true;
    }

private:
    std::uint64_t instance_ = 0, changes_ = 0, cycles_ = 0;
    std::int64_t lastChange_ = 0, quietUntil_ = 0, budgetStart_ = 0;
    unsigned recoveries_ = 0;
    bool pending_ = false;
};

} // namespace waveline

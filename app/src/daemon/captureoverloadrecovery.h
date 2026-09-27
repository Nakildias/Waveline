// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace waveline {

// Control-thread policy. A few isolated missed deadlines do not justify a
// disruptive rebuild. Wait until an overload burst is over, then allow one
// settled recovery, with a cooldown and a bounded ten-minute budget.
class CaptureOverloadRecovery {
public:
    bool observe(std::int64_t now, std::uint64_t instance, std::uint64_t xruns,
                 std::uint64_t cycles, bool recovering = false) {
        if (instance != instance_ || xruns < xruns_ || recovering) {
            instance_ = instance;
            xruns_ = xruns;
            cycles_ = cycles;
            burst_ = 0;
            nextAllowed_ = std::max(nextAllowed_, now + 15000);
            return false;
        }
        const bool progressing = cycles != cycles_;
        cycles_ = cycles;
        if (xruns != xruns_) {
            if (now - lastXrun_ > 10000) burst_ = 0;
            burst_ += std::min<std::uint64_t>(xruns - xruns_, 3);
            xruns_ = xruns;
            lastXrun_ = now;
            return false;
        }
        if (!progressing || burst_ < 3 || now - lastXrun_ < 5000) return false;
        // Never queue an old burst to interrupt the user minutes later.
        burst_ = 0;
        if (now < nextAllowed_) return false;
        if (now - budgetStart_ >= 600000) { budgetStart_ = now; recoveries_ = 0; }
        if (recoveries_ >= 2) return false;
        ++recoveries_;
        nextAllowed_ = now + 60000;
        return true;
    }
private:
    std::uint64_t instance_ = 0, xruns_ = 0, cycles_ = 0, burst_ = 0;
    std::int64_t lastXrun_ = 0, nextAllowed_ = 0, budgetStart_ = 0;
    unsigned recoveries_ = 0;
};

} // namespace waveline

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "rtsched.h"

#include <dirent.h>
#include <pipewire/pipewire.h>
#include <sched.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace waveline {
namespace {

// Raise one limit's soft value to its hard value. Returns true if it moved.
bool liftToHard(int resource) {
    rlimit lim{};
    if (getrlimit(resource, &lim) != 0) return false;
    if (lim.rlim_cur >= lim.rlim_max) return false;
    rlimit next{lim.rlim_max, lim.rlim_max};
    return setrlimit(resource, &next) == 0;
}

// PipeWire names its data threads "data-loop.N" (and "data-loop" on older
// releases). The prefix is the whole test: comm is capped at 15 characters, so
// matching the tail would be matching something the kernel may have truncated.
bool isDataLoop(const char *comm) {
    return std::strncmp(comm, "data-loop", 9) == 0;
}

// sched_getscheduler() returns the policy with SCHED_RESET_ON_FORK still set in
// it, and rtkit always sets that flag when it grants a request -- so a plain
// `policy == SCHED_RR` test reports zero real-time threads on exactly the
// machines this code exists to diagnose. Measured: ps saw 19 SCHED_RR data
// loops while the unmasked comparison saw none.
#ifndef SCHED_RESET_ON_FORK
#define SCHED_RESET_ON_FORK 0x40000000
#endif
bool isRealtime(int policy) {
    if (policy < 0) return false;  // the thread exited under us
    const int p = policy & ~SCHED_RESET_ON_FORK;
    return p == SCHED_RR || p == SCHED_FIFO;
}

}  // namespace

RtLimits raiseRealtimeLimits() {
    // MEMLOCK first, and its result deliberately ignored: locking pages is a
    // refinement, being real-time at all is not, and a system that grants one
    // without the other should still get the one it granted.
    liftToHard(RLIMIT_MEMLOCK);

    RtLimits out;
    const bool moved = liftToHard(RLIMIT_RTPRIO);

    rlimit lim{};
    if (getrlimit(RLIMIT_RTPRIO, &lim) == 0) {
        out.soft = lim.rlim_cur;
        out.hard = lim.rlim_max;
    }
    out.raised = moved;
    return out;
}

RtThreads scanDataLoops() {
    RtThreads out;

    DIR *d = opendir("/proc/self/task");
    if (!d) return out;

    while (const dirent *e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const pid_t tid = static_cast<pid_t>(std::strtol(e->d_name, nullptr, 10));
        if (tid <= 0) continue;

        char path[64];
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        FILE *f = std::fopen(path, "re");
        if (!f) continue;  // the thread exited between readdir and here
        char comm[64] = {0};
        const bool got = std::fgets(comm, sizeof(comm), f) != nullptr;
        std::fclose(f);
        if (!got) continue;
        if (char *nl = std::strchr(comm, '\n')) *nl = '\0';
        if (!isDataLoop(comm)) continue;

        ++out.dataLoops;

        // sched_getscheduler() and sched_getparam() take a TID on Linux, which
        // is the whole reason this walks /proc rather than asking pthreads:
        // the data loops belong to PipeWire and we never hold their handles.
        if (!isRealtime(sched_getscheduler(tid))) continue;

        sched_param param{};
        if (sched_getparam(tid, &param) != 0) continue;

        ++out.realtime;
        const int prio = param.sched_priority;
        if (out.realtime == 1 || prio < out.minPrio) out.minPrio = prio;
        if (prio > out.maxPrio) out.maxPrio = prio;
    }
    closedir(d);

    return out;
}

namespace {
// Not atomic and deliberately not locked: it is written once, from the daemon's
// own thread, before the first context exists, and only read afterwards.
bool g_realtime = true;
}  // namespace

void setRealtimeEnabled(bool on) { g_realtime = on; }
bool realtimeEnabled() { return g_realtime; }

pw_properties *applyRealtimeProps(pw_properties *props) {
    // Absent, not "true", when real-time is on. See the header: writing the
    // property either way would override a system-wide choice we did not make.
    if (!g_realtime && props) pw_properties_set(props, "module.rt", "false");
    return props;
}

pw_properties *realtimeContextProps() {
    if (g_realtime) return nullptr;
    return pw_properties_new("module.rt", "false", nullptr);
}

}  // namespace waveline

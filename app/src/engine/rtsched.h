// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Real-time scheduling: taking it when it is offered, and telling the truth
// about it when it is not.
//
// Every audio thread in this process has a hard deadline. At the 512-frame
// default that deadline is 10.7 ms and an ordinary SCHED_OTHER thread will
// usually make it even on a loaded machine. At 128 frames it is 2.66 ms and it
// will not: put 32 CPU-bound threads on a 32-core machine and every
// SCHED_OTHER thread waits behind them, which is heard as clicks and pops.
//
// So the audio threads have to be SCHED_RR/SCHED_FIFO. Whether they get to be
// is decided outside this process, and the mechanism is worth writing down
// because its failure mode is silent and its symptom looks like bad hardware.
//
// PipeWire's libpipewire-module-rt is loaded into every context this daemon
// creates, and it makes each data-loop thread real-time by one of two routes:
//
//   rlimits  -- pthread_setschedparam() directly, if RLIMIT_RTPRIO allows the
//               priority it wants. No limit on how many threads, no daemon in
//               the middle. This is the route we want.
//   rtkit    -- ask rtkit-daemon over D-Bus. The fallback when RLIMIT_RTPRIO is
//               zero, which it is by default on every mainstream distribution.
//
// The rtkit route does not scale to a graph this size. Measured on the
// development machine with rtkit's shipped defaults:
//
//   --threads-per-user-max=25      25 real-time threads per user, ever
//   --actions-per-burst-max=25     25 requests per 20 s, then denials
//   --max-realtime-priority=20     and a low ceiling on what it will grant
//
// Waveline builds ~100 filter nodes, each in its own PipeWire context with its
// own data loop. A full graph therefore asks for ~100 real-time threads in one
// burst. The first ~25 are granted and the rest are refused, so the daemon runs
// with a couple of dozen real-time threads and hundreds of ordinary ones -- and
// the refusals land on whichever filters happened to be built last, which is
// why the same machine glitches differently after every rewire. The journal
// says "Reached maximum concurrent threads limit for user, denying request" a
// hundred times and nothing in the audio stack says anything at all.
//
// install.sh grants the rlimits route (see the real-time privileges step). This
// file is the two halves that belong in the daemon: lifting the soft limit to
// whatever the hard limit allows, before any PipeWire context exists, and
// counting afterwards what actually happened so the mixer's diagnostics window
// can say so instead of leaving the user to guess.

#pragma once

#include <sys/resource.h>

#include <string>

// PipeWire's own type, declared here so this header stays free of its
// includes. At global scope on purpose: inside the namespace it would declare
// a waveline::pw_properties that matches nothing PipeWire hands us.
struct pw_properties;

namespace waveline {

// What RLIMIT_RTPRIO looks like after raiseRealtimeLimits() has had its say.
struct RtLimits {
    rlim_t soft = 0;   // highest real-time priority this process may ask for
    rlim_t hard = 0;   // highest it could raise `soft` to
    bool raised = false;  // true if soft was actually lifted

    // The only question that matters: can module-rt skip rtkit entirely?
    // Anything above zero means pthread_setschedparam() is available to it.
    bool realtimeAllowed() const { return soft > 0; }
};

// Lift RLIMIT_RTPRIO -- and RLIMIT_MEMLOCK, so buffers can be locked resident
// and no audio thread takes a page fault on its deadline -- from the soft limit
// to the hard one. Raising soft to hard never needs privilege and never fails
// for want of one, so this is safe to call unconditionally.
//
// Must be called before the first pw_context_new(): module-rt reads the limit
// when a context loads it, and a context that already chose rtkit does not
// reconsider.
RtLimits raiseRealtimeLimits();

// What the audio threads of *this* process ended up with, read live from
// /proc/self/task. Only the PipeWire data loops are counted: they are the
// threads with the deadline, and the control loops beside them (one per filter,
// named waveline-*) are supposed to be ordinary.
struct RtThreads {
    int dataLoops = 0;   // PipeWire data-loop threads found
    int realtime = 0;    // of those, how many are SCHED_RR or SCHED_FIFO
    int minPrio = 0;     // priority range across the real-time ones
    int maxPrio = 0;

    bool complete() const { return dataLoops > 0 && realtime == dataLoops; }
};

RtThreads scanDataLoops();

// ---------------------------------------------------------------------------
// Turning the whole thing off.
//
// Everything above is about *getting* real-time scheduling. This is the switch
// for the machine that does not want it: a host where a hundred SCHED_RR
// threads starve something that matters more, and the machine where the
// question being asked is whether Waveline is the thing stuttering.
//
// The mechanism is PipeWire's own. Both client.conf and pipewire.conf guard
// module-rt with
//
//     condition = [ { module.rt = !false } ]
//
// so a context created with module.rt=false in its properties never loads the
// module and never asks for real-time scheduling by either route. That is a
// real off, as opposed to dropping RLIMIT_RTPRIO to zero, which only demotes us
// to the rtkit path -- still real-time, still capped at 25 threads, and with
// the failures landing on whichever filters were built last.
//
// Per context, and read when the context is created: a running context does not
// reconsider. So this decides the scheduling of contexts made *after* it is
// set, nothing more, and flipping it for a graph that already exists means
// building that graph again.
void setRealtimeEnabled(bool on);
bool realtimeEnabled();

// Stamps module.rt onto a property set on its way into pw_filter_new_simple()
// or pw_context_new(). A no-op while real-time is enabled -- the property is
// left absent rather than set to true, so the shipped configuration's own
// default keeps deciding, and a user who has disabled module-rt system-wide
// does not find us switching it back on.
pw_properties *applyRealtimeProps(pw_properties *props);

// The same thing for the three contexts built with no properties of their own.
// Returns a property set to hand straight to pw_context_new(), which takes
// ownership, or nullptr when there is nothing to say.
pw_properties *realtimeContextProps();

}  // namespace waveline

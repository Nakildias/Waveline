// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "filterhost.h"

#include "rtsched.h"

#include <pipewire/pipewire.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>

namespace waveline {
namespace {

std::mutex g_mutex;
pw_thread_loop *g_loop = nullptr;
pw_context *g_context = nullptr;
pw_core *g_core = nullptr;

// How many data loops to ask PipeWire for on the shared context.
//
// With a connection per filter this was implicit: a hundred filters meant a
// hundred data threads, which is why rtkit refused most of them (see rtsched.h)
// and why the idle daemon was doing thousands of context switches a second. One
// connection with one data loop would be the other extreme -- every DSP stage
// in the graph on a single core, meeting a 512-frame deadline between them.
//
// So: a handful, scaled to the machine. PipeWire places linked nodes on the
// same loop where it can, so a channel's own chain still hands buffers straight
// down without crossing threads, and separate channels can land on separate
// cores. Capped low because these are real-time threads and the ceiling on how
// many of those a session will actually be granted is not ours to set.
int dataLoopCount() {
    const unsigned cores = std::thread::hardware_concurrency();
    if (cores <= 2) return 1;
    return static_cast<int>(std::min(cores / 2, 4u));
}

}  // namespace

bool FilterHost::start(std::string &error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_core) return true;

    pw_init(nullptr, nullptr);

    g_loop = pw_thread_loop_new("waveline-dsp", nullptr);
    if (!g_loop) {
        error = "pw_thread_loop_new failed for the shared filter connection";
        return false;
    }

    // realtimeContextProps() carries the module.rt decision and may be null,
    // which means "the defaults", so the data-loop count has to be added to
    // whatever it returns rather than passed instead of it.
    pw_properties *props = realtimeContextProps();
    if (!props) props = pw_properties_new(nullptr, nullptr);
    if (props)
        pw_properties_setf(props, "context.num-data-loops", "%d", dataLoopCount());

    pw_thread_loop_lock(g_loop);
    g_context = pw_context_new(pw_thread_loop_get_loop(g_loop), props, 0);
    if (!g_context) {
        error = "pw_context_new failed for the shared filter connection";
        pw_thread_loop_unlock(g_loop);
        pw_thread_loop_destroy(g_loop);
        g_loop = nullptr;
        return false;
    }
    g_core = pw_context_connect(g_context, nullptr, 0);
    if (!g_core) {
        error = "pw_context_connect failed for the shared filter connection";
        pw_context_destroy(g_context);
        g_context = nullptr;
        pw_thread_loop_unlock(g_loop);
        pw_thread_loop_destroy(g_loop);
        g_loop = nullptr;
        return false;
    }
    pw_thread_loop_unlock(g_loop);

    if (pw_thread_loop_start(g_loop) < 0) {
        error = "pw_thread_loop_start failed for the shared filter connection";
        pw_thread_loop_lock(g_loop);
        pw_core_disconnect(g_core);
        g_core = nullptr;
        pw_context_destroy(g_context);
        g_context = nullptr;
        pw_thread_loop_unlock(g_loop);
        pw_thread_loop_destroy(g_loop);
        g_loop = nullptr;
        return false;
    }
    return true;
}

void FilterHost::stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_loop) return;

    pw_thread_loop_lock(g_loop);
    if (g_core) {
        pw_core_disconnect(g_core);
        g_core = nullptr;
    }
    if (g_context) {
        pw_context_destroy(g_context);
        g_context = nullptr;
    }
    pw_thread_loop_unlock(g_loop);
    pw_thread_loop_stop(g_loop);
    pw_thread_loop_destroy(g_loop);
    g_loop = nullptr;
}

pw_thread_loop *FilterHost::loop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_loop;
}

pw_core *FilterHost::core() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_core;
}

}  // namespace waveline

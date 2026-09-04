// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "levelprobe.h"

#include "rtsched.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <list>

namespace waveline {
namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 2;
// Decay applied on every read, so a peak stays visible for a few polls rather
// than flickering with the quantum. ~1.5 dB per 100 ms read.
constexpr float kDecay = 0.82f;

struct Watch {
    std::string key;
    pw_stream *stream = nullptr;
    spa_hook listener{};
    std::atomic<float> peak{0.0f};
    struct spa_audio_info_raw format {};
};

void onProcess(void *userdata) {
    auto *w = static_cast<Watch *>(userdata);

    pw_buffer *b = pw_stream_dequeue_buffer(w->stream);
    if (!b) return;

    spa_buffer *buf = b->buffer;
    float peak = 0.0f;
    for (uint32_t c = 0; c < buf->n_datas; ++c) {
        const auto &d = buf->datas[c];
        if (!d.data) continue;
        const auto *samples = static_cast<const float *>(d.data);
        const uint32_t n = d.chunk->size / sizeof(float);
        for (uint32_t i = 0; i < n; ++i) {
            const float a = std::fabs(samples[i]);
            if (a > peak) peak = a;
        }
    }
    // Only ever raised here; the decay happens on the reader's side, so a
    // silent stream cannot hold the meter up.
    float prev = w->peak.load(std::memory_order_relaxed);
    while (peak > prev &&
           !w->peak.compare_exchange_weak(prev, peak, std::memory_order_relaxed)) {
    }

    pw_stream_queue_buffer(w->stream, b);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = onProcess,
};
#pragma GCC diagnostic pop

}  // namespace

struct LevelProbe::Impl {
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    // A list, not a vector: the process callback holds a Watch* and the
    // registered spa_hook is embedded in it, so the elements must never move.
    std::list<Watch> watches;
};

LevelProbe::LevelProbe() : d_(std::make_unique<Impl>()) {}
LevelProbe::~LevelProbe() { stop(); }

bool LevelProbe::start(std::string &error) {
    pw_init(nullptr, nullptr);

    d_->loop = pw_thread_loop_new("waveline-meters", nullptr);
    if (!d_->loop) { error = "pw_thread_loop_new failed"; return false; }

    pw_thread_loop_lock(d_->loop);
    d_->context = pw_context_new(pw_thread_loop_get_loop(d_->loop), realtimeContextProps(), 0);
    if (!d_->context) {
        error = "pw_context_new failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }
    d_->core = pw_context_connect(d_->context, nullptr, 0);
    if (!d_->core) {
        error = "pw_context_connect failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }
    pw_thread_loop_unlock(d_->loop);

    if (pw_thread_loop_start(d_->loop) < 0) {
        error = "pw_thread_loop_start failed";
        return false;
    }
    return true;
}

bool LevelProbe::watch(const std::string &key, const std::string &sinkName,
                       std::string &error, bool sourceIsSink) {
    if (!d_->core) { error = "probe not started"; return false; }

    pw_thread_loop_lock(d_->loop);
    Watch &w = d_->watches.emplace_back();
    w.key = key;

    // stream.capture.sink is the whole trick: without it the capture end
    // attaches to the default *source* -- the microphone -- and every meter
    // silently shows the same thing.
    auto *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, ("waveline-meter-" + key).c_str(),
        PW_KEY_NODE_DESCRIPTION, ("Waveline meter (" + key + ")").c_str(),
        PW_KEY_TARGET_OBJECT, sinkName.c_str(),
        PW_KEY_STREAM_CAPTURE_SINK, sourceIsSink ? "true" : "false",
        // Deliberately not node.passive: a passive consumer does not keep its
        // source scheduled, and the Stream mix has no other consumer until a
        // recorder attaches. Measured as making no difference on this system,
        // but a meter that reads zero because it was too polite to wake the
        // node it is measuring would be worse than no meter.
        //
        // Marks it as a meter tap. Session managers and volume panels hide
        // these, so six of them do not turn up in pavucontrol looking like six
        // applications recording the user.
        PW_KEY_STREAM_MONITOR, "true",
        // Never anywhere but the node we asked for. Without this, a probe
        // created while its target is missing -- the window during a capture
        // rebuild, say -- is autoconnected to whatever source the policy likes
        // instead, and the meter then reads a different device for the rest of
        // the session. An input device's card was showing its raw capture node
        // this way: the bar moved while the microphone was muted, because the
        // mute is downstream of the point the meter had landed on.
        //
        // A probe with nowhere to attach reads zero, and syncMasterMeters()
        // re-arms it when the node it wants exists.
        PW_KEY_NODE_DONT_RECONNECT, "true",
        nullptr);

    w.stream = pw_stream_new(d_->core, ("waveline-meter-" + key).c_str(), props);
    if (!w.stream) {
        d_->watches.pop_back();
        pw_thread_loop_unlock(d_->loop);
        error = "pw_stream_new failed";
        return false;
    }
    pw_stream_add_listener(w.stream, &w.listener, &kStreamEvents, &w);

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kRate;
    info.channels = kChannels;
    const spa_pod *params[1] = {
        spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

    const int rc = pw_stream_connect(
        w.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                     PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    pw_thread_loop_unlock(d_->loop);

    if (rc < 0) { error = "pw_stream_connect failed"; return false; }
    return true;
}

std::vector<std::pair<std::string, float>> LevelProbe::levels() const {
    std::vector<std::pair<std::string, float>> out;
    out.reserve(d_->watches.size());
    for (auto &w : d_->watches) {
        const float v = w.peak.load(std::memory_order_relaxed);
        out.emplace_back(w.key, v);
        // Decayed on read rather than on a timer: the reader's cadence is what
        // decides how long a peak should linger on screen.
        w.peak.store(v * kDecay, std::memory_order_relaxed);
    }
    return out;
}

bool LevelProbe::unwatch(const std::string &key) {
    if (!d_->core) return false;
    pw_thread_loop_lock(d_->loop);
    bool found = false;
    for (auto it = d_->watches.begin(); it != d_->watches.end(); ++it) {
        if (it->key != key) continue;
        if (it->stream) { pw_stream_destroy(it->stream); it->stream = nullptr; }
        d_->watches.erase(it);
        found = true;
        break;
    }
    pw_thread_loop_unlock(d_->loop);
    return found;
}

void LevelProbe::stop() {
    if (!d_ || !d_->loop) return;
    pw_thread_loop_lock(d_->loop);
    for (auto &w : d_->watches)
        if (w.stream) { pw_stream_destroy(w.stream); w.stream = nullptr; }
    d_->watches.clear();
    if (d_->core) { pw_core_disconnect(d_->core); d_->core = nullptr; }
    if (d_->context) { pw_context_destroy(d_->context); d_->context = nullptr; }
    pw_thread_loop_unlock(d_->loop);
    pw_thread_loop_stop(d_->loop);
    pw_thread_loop_destroy(d_->loop);
    d_->loop = nullptr;
}

}  // namespace waveline

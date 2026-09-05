// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "gainfilter.h"

#include "dspprobe.h"
#include "filterhost.h"
#include "rtsched.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/pod/builder.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

namespace waveline {
namespace {

constexpr int kMaxChannels = 2;

struct PortData {
    uint32_t channel = 0;
};

}  // namespace

struct GainFilter::Impl {
    pw_thread_loop *loop = nullptr;
    pw_filter *filter = nullptr;
    spa_hook listener{};
    DspMeter meter;
    PortData *inPorts[kMaxChannels]{};
    PortData *outPorts[kMaxChannels]{};
    int channels = 1;
    std::atomic<float> gain{1.0f};
    // What each channel's multiply is actually at, chasing `gain`.
    float gainRamp[kMaxChannels] = {1.0f, 1.0f};
};

namespace {

// One-pole step per sample, about 20 ms to travel. 1 - exp(-1/(0.02 * 48000)).
constexpr float kGainGlide = 0.00104f;

void onProcess(void *userdata, spa_io_position *position) {
    auto *d = static_cast<GainFilter::Impl *>(userdata);
    DspScope probe(d->meter, position);
    const uint32_t n = position->clock.duration;
    const float g = d->gain.load(std::memory_order_relaxed);

    for (int ch = 0; ch < d->channels; ++ch) {
        auto *in = static_cast<float *>(pw_filter_get_dsp_buffer(d->inPorts[ch], n));
        auto *out = static_cast<float *>(pw_filter_get_dsp_buffer(d->outPorts[ch], n));
        if (!out) continue;
        if (!in) {
            std::memset(out, 0, n * sizeof(float));
            continue;
        }
        // Ramped, not stamped: an input level slider dragged while talking
        // steps the signal once per block otherwise, and every step is a
        // click. Each channel walks its own copy so they stay identical.
        float cur = d->gainRamp[ch];
        if (cur == g && g == 1.0f && out == in) continue;
        for (uint32_t i = 0; i < n; ++i) {
            cur += (g - cur) * kGainGlide;
            out[i] = in[i] * cur;
        }
        // Snap once it is near enough, so the multiply settles exactly on the
        // requested gain rather than approaching it forever.
        d->gainRamp[ch] = (std::fabs(g - cur) < 1.0e-5f) ? g : cur;
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_filter_events kFilterEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .process = onProcess,
};
#pragma GCC diagnostic pop

}  // namespace

GainFilter::GainFilter() : d_(std::make_unique<Impl>()) {}
GainFilter::~GainFilter() { stop(); }

void GainFilter::setGain(float linear) {
    d_->gain.store(linear, std::memory_order_relaxed);
}

float GainFilter::gain() const {
    return d_->gain.load(std::memory_order_relaxed);
}

bool GainFilter::start(const std::string &nodeName, const std::string &description,
                       int channels, std::string &error) {
    if (channels < 1 || channels > kMaxChannels) {
        error = "gain filter supports 1 or 2 channels";
        return false;
    }

    d_->channels = channels;

    d_->meter.attach(nodeName, "Input gain");

    // The shared DSP connection, not one of this filter's own. See filterhost.h.
    if (!FilterHost::start(error)) return false;
    d_->loop = FilterHost::loop();
    if (!d_->loop) {
        error = "shared filter connection unavailable";
        return false;
    }

    pw_thread_loop_lock(d_->loop);

    auto *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_MEDIA_CLASS, "Stream/Filter/Audio",
        PW_KEY_NODE_NAME, nodeName.c_str(),
        PW_KEY_NODE_DESCRIPTION, description.c_str(),
        PW_KEY_NODE_AUTOCONNECT, "false",
        "audio.rate", "48000",
        "audio.channels", channels == 1 ? "1" : "2",
        "audio.position", channels == 1 ? "[ MONO ]" : "[ FL FR ]",
        "node.want-driver", "true",
        nullptr);

    d_->filter = pw_filter_new(FilterHost::core(), nodeName.c_str(), applyRealtimeProps(props));
    if (d_->filter)
        pw_filter_add_listener(d_->filter, &d_->listener, &kFilterEvents, d_.get());
    if (!d_->filter) {
        error = "pw_filter_new_simple failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    for (int ch = 0; ch < channels; ++ch) {
        const char *name = channels == 1 ? "input" : (ch == 0 ? "input_FL" : "input_FR");
        const char *outName =
            channels == 1 ? "output" : (ch == 0 ? "output_FL" : "output_FR");
        d_->inPorts[ch] = static_cast<PortData *>(pw_filter_add_port(
            d_->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(PortData),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, name, nullptr),
            nullptr, 0));
        d_->outPorts[ch] = static_cast<PortData *>(pw_filter_add_port(
            d_->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(PortData),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, outName, nullptr),
            nullptr, 0));
        if (d_->inPorts[ch]) d_->inPorts[ch]->channel = ch;
        if (d_->outPorts[ch]) d_->outPorts[ch]->channel = ch;
    }

    if (!d_->inPorts[0] || !d_->outPorts[0]) {
        error = "pw_filter_add_port failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    if (pw_filter_connect(d_->filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        error = "pw_filter_connect failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    pw_thread_loop_unlock(d_->loop);

    return true;
}

void GainFilter::stop() {
    if (!d_) return;
    if (!d_->loop) {
        // start() can fail after attach() and before the loop exists. The
        // registration still has to go, or a stage nothing is running shows
        // up in the panel for as long as the process lives.
        d_->meter.detach();
        return;
    }
    pw_thread_loop_lock(d_->loop);
    if (d_->filter) {
        pw_filter_destroy(d_->filter);
        d_->filter = nullptr;
    }
    pw_thread_loop_unlock(d_->loop);
    // The loop and the connection are shared and outlive this filter; only the
    // node on them is ours to destroy. See filterhost.h.
    d_->loop = nullptr;
    // After the loop is gone, so nothing can still be inside the counters.
    d_->meter.detach();
}

}  // namespace waveline

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "channelinputmixer.h"

#include "dspprobe.h"
#include "rtsched.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include <cstring>
#include <memory>

namespace waveline {

struct ChannelInputMixer::Impl {
    pw_thread_loop *loop = nullptr;
    pw_filter *filter = nullptr;
    DspMeter meter;
    void *inFl = nullptr;
    void *inFr = nullptr;
    void *inMic = nullptr;
    void *outFl = nullptr;
    void *outFr = nullptr;
};

namespace {

enum Port : uint32_t { InFl = 0, InFr = 1, InMic = 2, OutFl = 3, OutFr = 4 };

void onProcess(void *userdata, spa_io_position *position) {
    auto *d = static_cast<ChannelInputMixer::Impl *>(userdata);
    DspScope probe(d->meter, position);
    const uint32_t n = position->clock.duration;

    auto *appFl = static_cast<float *>(pw_filter_get_dsp_buffer(d->inFl, n));
    auto *appFr = static_cast<float *>(pw_filter_get_dsp_buffer(d->inFr, n));
    auto *mic = static_cast<float *>(pw_filter_get_dsp_buffer(d->inMic, n));
    auto *outL = static_cast<float *>(pw_filter_get_dsp_buffer(d->outFl, n));
    auto *outR = static_cast<float *>(pw_filter_get_dsp_buffer(d->outFr, n));
    // Each side is written independently. Requiring *both* buffers before
    // writing *either* is what silenced every channel the moment the mixer
    // went into the path: the next stage is RNNoise, which is mono and so
    // consumes output_FL only, leaving output_FR with no consumer and no
    // buffer -- and the whole callback returned without writing the left
    // channel it did have. EffectsFilter and GainFilter both already skip
    // per channel; this one abandoned the lot.
    if (!outL && !outR) return;

    for (uint32_t i = 0; i < n; ++i) {
        const float m = mic ? mic[i] : 0.0f;
        if (outL) outL[i] = (appFl ? appFl[i] : 0.0f) + m;
        if (outR) outR[i] = (appFr ? appFr[i] : 0.0f) + m;
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

ChannelInputMixer::ChannelInputMixer() : d_(std::make_unique<Impl>()) {}
ChannelInputMixer::~ChannelInputMixer() { stop(); }

bool ChannelInputMixer::start(const std::string &nodeName,
                              const std::string &description, std::string &error) {
    d_->meter.attach(nodeName, "Channel input mix");

    d_->loop = pw_thread_loop_new("waveline-ch-mix", nullptr);
    if (!d_->loop) {
        error = "pw_thread_loop_new failed";
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
        "audio.channels", "2",
        "audio.position", "[ FL FR ]",
        "node.want-driver", "true",
        nullptr);

    d_->filter = pw_filter_new_simple(pw_thread_loop_get_loop(d_->loop),
                                      nodeName.c_str(), applyRealtimeProps(props), &kFilterEvents,
                                      d_.get());
    if (!d_->filter) {
        error = "pw_filter_new_simple failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    auto addIn = [&](const char *name, void **slot) {
        *slot = pw_filter_add_port(
            d_->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, name, nullptr),
            nullptr, 0);
    };
    auto addOut = [&](const char *name, void **slot) {
        *slot = pw_filter_add_port(
            d_->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, name, nullptr),
            nullptr, 0);
    };

    addIn("input_FL", &d_->inFl);
    addIn("input_FR", &d_->inFr);
    addIn("input_MIC", &d_->inMic);
    addOut("output_FL", &d_->outFl);
    addOut("output_FR", &d_->outFr);

    if (!d_->inFl || !d_->inFr || !d_->inMic || !d_->outFl || !d_->outFr) {
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

    if (pw_thread_loop_start(d_->loop) < 0) {
        error = "pw_thread_loop_start failed";
        return false;
    }
    return true;
}

void ChannelInputMixer::stop() {
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
    pw_thread_loop_stop(d_->loop);
    pw_thread_loop_destroy(d_->loop);
    d_->loop = nullptr;
    // After the loop is gone, so nothing can still be inside the counters.
    d_->meter.detach();
}

}  // namespace waveline

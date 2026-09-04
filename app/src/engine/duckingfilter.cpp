// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "duckingfilter.h"

#include "dspprobe.h"
#include "rtsched.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace waveline {

constexpr int kProgramChannels = 2;

namespace {

struct PortData {
    uint32_t channel = 0;
};

}  // namespace

struct DuckingFilter::Impl {
    pw_thread_loop *loop = nullptr;
    pw_filter *filter = nullptr;
    DspMeter meter;
    PortData *inPorts[kProgramChannels]{};
    PortData *sidechainPorts[kMaxDuckingSources]{};
    PortData *outPorts[kProgramChannels]{};

    std::mutex settingsMutex;
    DuckingSettings settings;
    DuckingProcessor processor{48000.0f};
    std::atomic<bool> settingsDirty{true};
};

void DuckingFilter::filterProcess(void *userdata, spa_io_position *position) {
    auto *d = static_cast<Impl *>(userdata);
    DspScope probe(d->meter, position);
    const uint32_t n = position->clock.duration;

    DuckingSettings settings;
    {
        std::lock_guard<std::mutex> lock(d->settingsMutex);
        if (d->settingsDirty.load(std::memory_order_relaxed)) {
            // No reset(): editing the source list or the intensity slider must
            // not snap the envelope back to unity mid-duck. setSettings() only
            // recomputes coefficients, so the running gain stays valid and
            // glides to the new target instead of stepping to it.
            d->processor.setSettings(d->settings);
            d->settingsDirty.store(false, std::memory_order_relaxed);
        }
        settings = d->settings;
    }

    const float *sidechains[kMaxDuckingSources]{};
    bool sidechainPresent[kMaxDuckingSources]{};
    for (int s = 0; s < kMaxDuckingSources; ++s) {
        sidechains[s] = static_cast<float *>(
            pw_filter_get_dsp_buffer(d->sidechainPorts[s], n));
        sidechainPresent[s] = sidechains[s] != nullptr;
    }

    auto *inL = static_cast<float *>(pw_filter_get_dsp_buffer(d->inPorts[0], n));
    auto *inR = static_cast<float *>(pw_filter_get_dsp_buffer(d->inPorts[1], n));
    auto *outL = static_cast<float *>(pw_filter_get_dsp_buffer(d->outPorts[0], n));
    auto *outR = static_cast<float *>(pw_filter_get_dsp_buffer(d->outPorts[1], n));

    if (!outL || !outR) return;

    if (!inL || !inR) {
        std::memset(outL, 0, n * sizeof(float));
        std::memset(outR, 0, n * sizeof(float));
        return;
    }

    if (outL != inL) std::memcpy(outL, inL, n * sizeof(float));
    if (outR != inR) std::memcpy(outR, inR, n * sizeof(float));

    if (!settings.enabled) return;

    d->processor.process(outL, outR, sidechains, sidechainPresent, n);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_filter_events kDuckFilterEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .process = DuckingFilter::filterProcess,
};
#pragma GCC diagnostic pop

DuckingFilter::DuckingFilter() : d_(std::make_unique<Impl>()) {}
DuckingFilter::~DuckingFilter() { stop(); }

void DuckingFilter::setSettings(const DuckingSettings &s) {
    std::lock_guard<std::mutex> lock(d_->settingsMutex);
    d_->settings = s;
    d_->settingsDirty.store(true, std::memory_order_relaxed);
}

DuckingSettings DuckingFilter::settings() const {
    std::lock_guard<std::mutex> lock(d_->settingsMutex);
    return d_->settings;
}

bool DuckingFilter::start(const std::string &nodeName, const std::string &description,
                          std::string &error) {
    pw_init(nullptr, nullptr);

    d_->meter.attach(nodeName, "Ducking");

    d_->loop = pw_thread_loop_new("waveline-duck", nullptr);
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

    d_->filter = pw_filter_new_simple(pw_thread_loop_get_loop(d_->loop), nodeName.c_str(),
                                      applyRealtimeProps(props), &kDuckFilterEvents, d_.get());
    if (!d_->filter) {
        error = "pw_filter_new_simple failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    for (int ch = 0; ch < kProgramChannels; ++ch) {
        const char *inName = ch == 0 ? "input_FL" : "input_FR";
        const char *outName = ch == 0 ? "output_FL" : "output_FR";
        d_->inPorts[ch] = static_cast<PortData *>(pw_filter_add_port(
            d_->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(PortData),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, inName, nullptr),
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

    for (int s = 0; s < kMaxDuckingSources; ++s) {
        const std::string portName = "sidechain_" + std::to_string(s);
        d_->sidechainPorts[s] = static_cast<PortData *>(pw_filter_add_port(
            d_->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(PortData),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, portName.c_str(), nullptr),
            nullptr, 0));
    }

    if (!d_->inPorts[0] || !d_->inPorts[1] || !d_->outPorts[0] || !d_->outPorts[1] ||
        !d_->sidechainPorts[0]) {
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

void DuckingFilter::stop() {
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

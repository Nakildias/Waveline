// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "captureselector.h"

#include "dspprobe.h"
#include "filterhost.h"
#include "rtsched.h"

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>

namespace waveline {

struct CaptureSelector::Impl {
    pw_thread_loop *loop = nullptr;
    pw_filter *filter = nullptr;
    spa_hook listener{};
    DspMeter meter;
    std::array<std::array<void *, 2>, kMaxInputs> inputs{};
    int channels = 1;
    std::atomic<int> mode{0};
    void *outputs[2]{};
    std::atomic<std::size_t> selected{0};
    uint64_t instance = 0;
    std::atomic<uint64_t> xruns{0}, cycles{0};
    uint64_t previousXrun = 0;
    uint32_t previousDriver = SPA_ID_INVALID;
};

namespace {

void onProcess(void *userdata, spa_io_position *position) {
    auto *d = static_cast<CaptureSelector::Impl *>(userdata);
    DspScope probe(d->meter, position);
    const uint32_t n = position->clock.duration;

    const std::size_t selected = d->selected.load(std::memory_order_relaxed);
    if (selected < d->inputs.size()) {
        // These counters stay available without enabling expensive DSP profiling.
        const auto &clock = position->clock;
        if (d->previousDriver == clock.id &&
            ((clock.flags & SPA_IO_CLOCK_FLAG_XRUN_RECOVER) ||
             clock.xrun > d->previousXrun))
            d->xruns.fetch_add(1, std::memory_order_relaxed);
        d->previousDriver = clock.id;
        d->previousXrun = clock.xrun;
        d->cycles.fetch_add(1, std::memory_order_relaxed);
    } else {
        d->previousDriver = SPA_ID_INVALID;
    }
    const float *in[2]{};
    float *out[2]{};
    for (int ch = 0; ch < d->channels; ++ch) {
        out[ch] = static_cast<float *>(pw_filter_get_dsp_buffer(d->outputs[ch], n));
        if (selected < d->inputs.size())
            in[ch] = static_cast<float *>(pw_filter_get_dsp_buffer(d->inputs[selected][ch], n));
    }
    const int mode = d->channels == 2 ? d->mode.load(std::memory_order_relaxed) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        // Read both before writing: PipeWire may alias an input and output.
        float left = in[0] ? in[0][i] : 0.0f;
        float right = in[1] ? in[1][i] : 0.0f;
        if (mode == 1) right = left;
        else if (mode == 2) left = right;
        else if (mode == 3) left = right = 0.5f * (left + right);
        if (out[0]) out[0][i] = left;
        if (out[1]) out[1][i] = right;
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

CaptureSelector::CaptureSelector() : d_(std::make_unique<Impl>()) {
    static std::atomic<uint64_t> next{0};
    d_->instance = next.fetch_add(1, std::memory_order_relaxed) + 1;
}
CaptureSelector::~CaptureSelector() { stop(); }

CaptureSelector::Health CaptureSelector::health() const {
    return {d_->instance, d_->xruns.load(std::memory_order_relaxed),
            d_->cycles.load(std::memory_order_relaxed)};
}

std::string CaptureSelector::inputPort(std::size_t index, int channel) {
    return "input_" + std::to_string(index) + (channel ? "_FR" : "");
}

void CaptureSelector::select(std::size_t index) {
    if (index < kMaxInputs)
        d_->selected.store(index, std::memory_order_release);
}

void CaptureSelector::setMode(int mode) {
    d_->mode.store(mode >= 0 && mode <= 3 ? mode : 0, std::memory_order_relaxed);
}

void CaptureSelector::selectSilence() {
    d_->selected.store(kMaxInputs, std::memory_order_release);
}

bool CaptureSelector::start(const std::string &nodeName,
                            const std::string &description,
                            std::string &error, int channels) {
    if (channels != 1 && channels != 2) { error = "invalid capture width"; return false; }
    d_->channels = channels;
    d_->meter.attach(nodeName, "Capture");

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
        "audio.channels", channels == 2 ? "2" : "1",
        "audio.position", channels == 2 ? "[ FL FR ]" : "[ MONO ]",
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

    for (std::size_t i = 0; i < kMaxInputs; ++i) {
      for (int ch = 0; ch < channels; ++ch) {
        const std::string port = inputPort(i, ch);
        d_->inputs[i][ch] = pw_filter_add_port(
            d_->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, port.c_str(), nullptr),
            nullptr, 0);
        if (!d_->inputs[i][ch]) {
            error = "pw_filter_add_port failed";
            pw_thread_loop_unlock(d_->loop);
            return false;
        }
    }
    }
    for (int ch = 0; ch < channels; ++ch) {
    d_->outputs[ch] = pw_filter_add_port(
        d_->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, channels == 1 ? "output" : (ch ? "output_FR" : "output_FL"), nullptr),
        nullptr, 0);
    if (!d_->outputs[ch]) {
        error = "pw_filter_add_port failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    }

    if (pw_filter_connect(d_->filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        error = "pw_filter_connect failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }
    pw_thread_loop_unlock(d_->loop);

    return true;
}

void CaptureSelector::stop() {
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

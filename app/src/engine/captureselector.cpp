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
    std::array<void *, kMaxInputs> inputs{};
    void *output = nullptr;
    std::atomic<std::size_t> selected{0};
};

namespace {

void onProcess(void *userdata, spa_io_position *position) {
    auto *d = static_cast<CaptureSelector::Impl *>(userdata);
    DspScope probe(d->meter, position);
    const uint32_t n = position->clock.duration;
    auto *out = static_cast<float *>(pw_filter_get_dsp_buffer(d->output, n));
    if (!out) return;

    const std::size_t selected = d->selected.load(std::memory_order_relaxed);
    auto *in = selected < d->inputs.size()
                   ? static_cast<float *>(
                         pw_filter_get_dsp_buffer(d->inputs[selected], n))
                   : nullptr;
    if (!in) {
        std::memset(out, 0, n * sizeof(float));
        return;
    }
    if (out != in) std::memcpy(out, in, n * sizeof(float));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_filter_events kFilterEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .process = onProcess,
};
#pragma GCC diagnostic pop

}  // namespace

CaptureSelector::CaptureSelector() : d_(std::make_unique<Impl>()) {}
CaptureSelector::~CaptureSelector() { stop(); }

std::string CaptureSelector::inputPort(std::size_t index) {
    return "input_" + std::to_string(index);
}

void CaptureSelector::select(std::size_t index) {
    if (index < kMaxInputs)
        d_->selected.store(index, std::memory_order_release);
}

void CaptureSelector::selectSilence() {
    d_->selected.store(kMaxInputs, std::memory_order_release);
}

bool CaptureSelector::start(const std::string &nodeName,
                            const std::string &description,
                            std::string &error) {
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
        "audio.channels", "1",
        "audio.position", "[ MONO ]",
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
        const std::string port = inputPort(i);
        d_->inputs[i] = pw_filter_add_port(
            d_->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, port.c_str(), nullptr),
            nullptr, 0);
        if (!d_->inputs[i]) {
            error = "pw_filter_add_port failed";
            pw_thread_loop_unlock(d_->loop);
            return false;
        }
    }
    d_->output = pw_filter_add_port(
        d_->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "output", nullptr),
        nullptr, 0);
    if (!d_->output) {
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

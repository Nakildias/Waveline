// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "creativefxfilter.h"

#include "dspprobe.h"
#include "filterhost.h"
#include "rtsched.h"

#include <algorithm>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace waveline {

constexpr int kMaxChannels = 2;

namespace {

constexpr uint32_t kRate = 48000;

struct PortData {
    uint32_t channel = 0;
};

const char *positionForChannels(int channels) {
    return channels == 1 ? "[ MONO ]" : "[ FL FR ]";
}

}  // namespace

struct CreativeFxFilter::Impl {
    pw_thread_loop *loop = nullptr;
    pw_filter *filter = nullptr;
    spa_hook listener{};
    DspMeter meter;
    PortData *inPorts[kMaxChannels]{};
    PortData *outPorts[kMaxChannels]{};
    int channels = 1;

    std::mutex settingsMutex;
    CreativeFxSettings settings;
    std::vector<CreativeFxProcessor> processors;
    std::atomic<bool> settingsDirty{true};
};

void CreativeFxFilter::filterProcess(void *userdata, spa_io_position *position) {
    auto *d = static_cast<Impl *>(userdata);
    DspScope probe(d->meter, position);
    const uint32_t n = position->clock.duration;

    CreativeFxSettings settings;
    {
        std::lock_guard<std::mutex> lock(d->settingsMutex);
        const bool resize = static_cast<int>(d->processors.size()) != d->channels;
        if (resize) {
            d->processors.assign(d->channels, CreativeFxProcessor(float(kRate)));
            for (int i = 0; i < static_cast<int>(d->processors.size()); ++i) {
                CreativeFxProcessor &p = d->processors[i];
                p.setChannelIndex(i);
                p.setSettings(d->settings);
                p.reset();
            }
        } else if (d->settingsDirty.load(std::memory_order_relaxed)) {
            for (auto &p : d->processors) {
                p.setSettings(d->settings);
                p.reset();
            }
        }
        if (resize || d->settingsDirty.load(std::memory_order_relaxed))
            d->settingsDirty.store(false, std::memory_order_relaxed);
        settings = d->settings;
    }

    const bool bypass = !settings.active();

    // Ping-pong needs both channels' delay lines to trade samples, which
    // only makes sense with a real stereo pair and the delay actually on --
    // every other case (mono mic input, delay off, ping-pong off) takes the
    // ordinary per-channel path below, each processor fully independent.
    const bool pingPong =
        !bypass && d->channels == 2 && settings.delay.enabled && settings.delay.pingPong;

    if (pingPong) {
        auto *inL = static_cast<float *>(pw_filter_get_dsp_buffer(d->inPorts[0], n));
        auto *inR = static_cast<float *>(pw_filter_get_dsp_buffer(d->inPorts[1], n));
        auto *outL = static_cast<float *>(pw_filter_get_dsp_buffer(d->outPorts[0], n));
        auto *outR = static_cast<float *>(pw_filter_get_dsp_buffer(d->outPorts[1], n));
        if (outL && outR) {
            if (!inL || !inR) {
                std::memset(outL, 0, n * sizeof(float));
                std::memset(outR, 0, n * sizeof(float));
                return;
            }
            CreativeFxProcessor &procL = d->processors[0];
            CreativeFxProcessor &procR = d->processors[1];
            for (uint32_t i = 0; i < n; ++i) {
                const float preL = procL.processPreDelay(inL[i]);
                const float preR = procR.processPreDelay(inR[i]);
                const float tapL = procL.peekDelayTap();
                const float tapR = procR.peekDelayTap();
                const float feedback = procL.delayFeedback();
                const float mix = procL.delayMix();
                // Cross-write: a hit on L lands in R's line first, then
                // bounces back to L, and so on -- the classic ping-pong.
                procL.writeDelayLine(preR + tapR * feedback);
                procR.writeDelayLine(preL + tapL * feedback);
                const float delayedL = preL * (1.0f - mix) + tapL * mix;
                const float delayedR = preR * (1.0f - mix) + tapR * mix;
                outL[i] = procL.processReverbStage(delayedL);
                outR[i] = procR.processReverbStage(delayedR);
            }
        }
        return;
    }

    for (int ch = 0; ch < d->channels; ++ch) {
        auto *in = static_cast<float *>(pw_filter_get_dsp_buffer(d->inPorts[ch], n));
        auto *out = static_cast<float *>(pw_filter_get_dsp_buffer(d->outPorts[ch], n));
        if (!out) continue;
        if (!in) {
            std::memset(out, 0, n * sizeof(float));
            continue;
        }
        if (bypass) {
            if (out != in) std::memcpy(out, in, n * sizeof(float));
            continue;
        }

        CreativeFxProcessor &proc = d->processors[ch];
        for (uint32_t i = 0; i < n; ++i) out[i] = proc.process(in[i]);
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_filter_events kCreativeFxFilterEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .process = CreativeFxFilter::filterProcess,
};
#pragma GCC diagnostic pop

CreativeFxFilter::CreativeFxFilter() : d_(std::make_unique<Impl>()) {}
CreativeFxFilter::~CreativeFxFilter() { stop(); }

void CreativeFxFilter::setSettings(const CreativeFxSettings &s) {
    std::lock_guard<std::mutex> lock(d_->settingsMutex);
    d_->settings = s;
    d_->settingsDirty.store(true, std::memory_order_relaxed);
}

CreativeFxSettings CreativeFxFilter::settings() const {
    std::lock_guard<std::mutex> lock(d_->settingsMutex);
    return d_->settings;
}

bool CreativeFxFilter::start(const std::string &nodeName, const std::string &description,
                             int channels, std::string &error) {
    pw_init(nullptr, nullptr);
    d_->channels = std::clamp(channels, 1, kMaxChannels);

    d_->meter.attach(nodeName, "Creative FX");

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
        "audio.channels", d_->channels == 1 ? "1" : "2",
        "audio.position", positionForChannels(d_->channels),
        "node.want-driver", "true",
        nullptr);

    d_->filter = pw_filter_new(FilterHost::core(), nodeName.c_str(), applyRealtimeProps(props));
    if (d_->filter)
        pw_filter_add_listener(d_->filter, &d_->listener, &kCreativeFxFilterEvents, d_.get());
    if (!d_->filter) {
        error = "pw_filter_new_simple failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    for (int ch = 0; ch < d_->channels; ++ch) {
        const char *inName = d_->channels == 1 ? "input" : (ch == 0 ? "input_FL" : "input_FR");
        const char *outName =
            d_->channels == 1 ? "output" : (ch == 0 ? "output_FL" : "output_FR");
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

    for (int ch = 0; ch < d_->channels; ++ch) {
        if (!d_->inPorts[ch] || !d_->outPorts[ch]) {
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

void CreativeFxFilter::stop() {
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

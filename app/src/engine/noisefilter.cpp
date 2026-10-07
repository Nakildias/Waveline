// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "noisefilter.h"

#include "dspprobe.h"
#include "filterhost.h"
#include "frameadapter.h"
#include "rtsched.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>

#include <atomic>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace waveline {
namespace {

// Both engines are trained for 48 kHz and this is not configurable.
constexpr uint32_t kRate = 48000;

struct PortData {
    // pw_filter stores per-port user data here; we only need the tag.
    uint32_t unused = 0;
};

}  // namespace

struct NoiseFilter::Impl {
    pw_thread_loop *loop = nullptr;
    pw_filter *filter = nullptr;
    spa_hook listener{};
    DspMeter meter;
    struct Channel {
        PortData *inPort = nullptr;
        PortData *outPort = nullptr;
        std::unique_ptr<Denoiser> denoiser;
        FrameAdapter adapter;
        bool resetPending = true;
    };
    std::array<Channel, 2> channel;
    int channels = 1;

    // Held for the whole of the DSP section of the callback, and taken by
    // setEngine() while it swaps. The callback try_locks and falls back to
    // passthrough rather than blocking the data thread on a load that can take
    // a second (DeepFilterNet unpacks an ONNX archive).
    std::mutex engineLock;
    std::atomic<NoiseEngine> engine{NoiseEngine::RnNoise};
    int frame = 480;


    std::atomic<bool> enabled{true};
    std::atomic<bool> idle{false};

    bool processing() const {
        return enabled.load(std::memory_order_relaxed) &&
               !idle.load(std::memory_order_relaxed);
    }
    void updateLatency() {
        meter.setLatencyFrames(processing() ? static_cast<uint32_t>(frame) : 0);
    }
    std::atomic<float> speechProb{0.0f};
    std::atomic<float> inRms{0.0f};
    std::atomic<float> outRms{0.0f};
};

namespace {

void onProcess(void *userdata, spa_io_position *position) {
    auto *d = static_cast<NoiseFilter::Impl *>(userdata);
    DspScope probe(d->meter, position);
    const uint32_t n = position->clock.duration;
    std::unique_lock<std::mutex> lk(d->engineLock, std::try_to_lock);
    const bool process = lk.owns_lock() && d->processing();
    double inputEnergy = 0, outputEnergy = 0;
    float speech = 0;
    for (int ch = 0; ch < d->channels; ++ch) {
        auto &c = d->channel[ch];
        auto *in = static_cast<float *>(pw_filter_get_dsp_buffer(c.inPort, n));
        auto *out = static_cast<float *>(pw_filter_get_dsp_buffer(c.outPort, n));
        if (!out) { c.resetPending = true; continue; }
        if (!in) {
            std::memset(out, 0, n * sizeof(float));
            c.resetPending = true;
            continue;
        }
        for (uint32_t i = 0; i < n; ++i) inputEnergy += double(in[i]) * in[i];
        if (!process || !c.denoiser) {
            if (out != in) std::memcpy(out, in, n * sizeof(float));
            c.resetPending = true;
        } else {
            if (c.resetPending) { c.adapter.reset(); c.resetPending = false; }
            c.adapter.process(in, out, n, [&](const float *frameIn, float *frameOut) {
                speech = std::max(speech, c.denoiser->processFrame(frameIn, frameOut));
            });
        }
        for (uint32_t i = 0; i < n; ++i) outputEnergy += double(out[i]) * out[i];
    }
    const double samples = double(n) * d->channels;
    d->inRms.store(samples ? float(std::sqrt(inputEnergy / samples)) : 0, std::memory_order_relaxed);
    d->outRms.store(samples ? float(std::sqrt(outputEnergy / samples)) : 0, std::memory_order_relaxed);
    d->speechProb.store(speech, std::memory_order_relaxed);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
// Only .process is needed; the rest of the event struct is optional.
const pw_filter_events kFilterEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .process = onProcess,
};
#pragma GCC diagnostic pop

}  // namespace

NoiseFilter::NoiseFilter() : d_(std::make_unique<Impl>()) {}
NoiseFilter::~NoiseFilter() { stop(); }

int NoiseFilter::frameSize() { return 480; }

NoiseEngine NoiseFilter::engine() const {
    return d_->engine.load(std::memory_order_relaxed);
}

bool NoiseFilter::setEngine(NoiseEngine engine, std::string &error) {
    if (d_->channel[0].denoiser &&
        (d_->channels == 1 || d_->channel[1].denoiser) &&
        d_->engine.load(std::memory_order_relaxed) == engine) return true;

    // Build every channel and allocate its frame buffers on the control thread.
    // A failure leaves the old stereo pair intact; channels never share history.
    std::array<std::unique_ptr<Denoiser>, 2> next;
    std::array<FrameAdapter, 2> adapters;
    int frame = 0;
    for (int ch = 0; ch < d_->channels; ++ch) {
        next[ch] = makeDenoiser(engine, error);
        if (!next[ch]) return false;
        frame = next[ch]->frameSize();
        adapters[ch].configure(static_cast<std::size_t>(frame));
    }
    {
        std::lock_guard<std::mutex> lk(d_->engineLock);
        for (int ch = 0; ch < d_->channels; ++ch) {
            d_->channel[ch].denoiser.swap(next[ch]);
            std::swap(d_->channel[ch].adapter, adapters[ch]);
        }
        d_->frame = frame;
    }
    // Old models/buffers are released outside the lock, on this thread.
    d_->engine.store(engine, std::memory_order_relaxed);
    // The delay this stage adds moved with the engine, so what the diagnostics
    // panel quotes has to move with it too -- unless it is switched off, in
    // which case it is bypassing and adding nothing whatever engine is loaded.
    d_->updateLatency();
    return true;
}

void NoiseFilter::setEnabled(bool on) {
    d_->enabled.store(on, std::memory_order_relaxed);
    // A switched-off denoiser takes the bypass path in onProcess: a memcpy,
    // no frame buffering, and therefore no delay at all. Reporting the frame
    // size regardless would put 10 ms of delay on every channel strip that has
    // ever had a noise filter built for it -- which is all of them -- and none
    // of those channels is paying it. What the panel quotes has to be what the
    // audio is actually going through, not what this stage could cost.
    d_->updateLatency();
}
bool NoiseFilter::enabled() const {
    return d_->enabled.load(std::memory_order_relaxed);
}

void NoiseFilter::setIdle(bool idle) {
    d_->idle.store(idle, std::memory_order_relaxed);
    d_->updateLatency();
}
bool NoiseFilter::idle() const {
    return d_->idle.load(std::memory_order_relaxed);
}

float NoiseFilter::speechProbability() const {
    return d_->speechProb.load(std::memory_order_relaxed);
}
float NoiseFilter::inputRms() const {
    return d_->inRms.load(std::memory_order_relaxed);
}
float NoiseFilter::outputRms() const {
    return d_->outRms.load(std::memory_order_relaxed);
}

bool NoiseFilter::start(const std::string &nodeName, const std::string &description,
                        std::string &error, bool asSource, NoiseEngine engine, int channels) {
    if (channels < 1 || channels > 2) { error = "noise filter supports 1 or 2 channels"; return false; }
    d_->channels = channels;
    pw_init(nullptr, nullptr);

    // A saved DeepFilterNet setting must not be able to stop the graph from
    // coming up on a machine where it is not installed, so failure here is
    // demoted to a fallback rather than an error.
    std::string engineError;
    if (!setEngine(engine, engineError) &&
        !setEngine(NoiseEngine::RnNoise, error))
        return false;

    // After setEngine, because d_->frame is whatever engine actually came up.
    // The only stage in the graph that adds real delay rather than only CPU:
    // it cannot emit anything until it has a whole frame to work on, which is
    // 10 ms on RNNoise and paid on any CPU. See dspprobe.h on why that is kept
    // apart from the time the callback spends.
    d_->meter.attach(nodeName, "Noise suppression",
                     d_->enabled.load(std::memory_order_relaxed)
                         ? static_cast<uint32_t>(d_->frame)
                         : 0);

    // The shared DSP connection, not one of this filter's own. See filterhost.h.
    if (!FilterHost::start(error)) return false;
    d_->loop = FilterHost::loop();
    if (!d_->loop) {
        error = "shared filter connection unavailable";
        return false;
    }

    pw_thread_loop_lock(d_->loop);

    // media.class Audio/Source makes it selectable anywhere a microphone is,
    // so the denoised mic is not locked inside this application.
    auto *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Filter",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_MEDIA_CLASS, asSource ? "Audio/Source" : "Stream/Filter/Audio",
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
        auto &c = d_->channel[ch];
        const char *inName = channels == 1 ? "input" : (ch == 0 ? "input_FL" : "input_FR");
        const char *outName = channels == 1 ? "output" : (ch == 0 ? "output_FL" : "output_FR");
        c.inPort = static_cast<PortData *>(pw_filter_add_port(
            d_->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(PortData),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, inName, nullptr), nullptr, 0));
        c.outPort = static_cast<PortData *>(pw_filter_add_port(
            d_->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(PortData),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, outName, nullptr), nullptr, 0));
        if (!c.inPort || !c.outPort) {
            error = "pw_filter_add_port failed";
            pw_thread_loop_unlock(d_->loop);
            return false;
        }
    }

    // Declare the processing latency so downstream consumers can compensate.
    uint8_t buffer[512];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_process_latency_info latency{};
    latency.ns = static_cast<uint64_t>(d_->frame) * SPA_NSEC_PER_SEC / kRate;
    const spa_pod *params[1] = {spa_process_latency_build(&b, SPA_PARAM_ProcessLatency,
                                                         &latency)};

    if (pw_filter_connect(d_->filter, PW_FILTER_FLAG_RT_PROCESS, params, 1) < 0) {
        error = "pw_filter_connect failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    pw_thread_loop_unlock(d_->loop);

    return true;
}

void NoiseFilter::stop() {
    if (!d_) return;
    if (!d_->loop) {
        // start() can fail after attach() and before the loop exists. The
        // registration still has to go, or a stage nothing is running shows
        // up in the panel for as long as the process lives.
        d_->meter.detach();
        return;
    }
    pw_thread_loop_lock(d_->loop);
    if (d_->filter) { pw_filter_destroy(d_->filter); d_->filter = nullptr; }
    pw_thread_loop_unlock(d_->loop);
    // The loop and the connection are shared and outlive this filter; only the
    // node on them is ours to destroy. See filterhost.h.
    d_->loop = nullptr;
    // After the loop is gone, so nothing can still be inside the counters.
    d_->meter.detach();
    // After the loop is gone, so the process callback cannot be holding it.
    std::lock_guard<std::mutex> lk(d_->engineLock);
    for (auto &c : d_->channel) c.denoiser.reset();
}

}  // namespace waveline

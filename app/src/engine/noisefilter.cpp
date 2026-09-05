// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "noisefilter.h"

#include "dspprobe.h"
#include "filterhost.h"
#include "rtsched.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

namespace waveline {
namespace {

// One-pole step per sample for the wet/dry mix: about 30 ms to travel, which
// is slow enough to be silent and fast enough that releasing the slider feels
// immediate. 1 - exp(-1/(0.03 * 48000)).
constexpr float kWetGlide = 0.000694f;

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
    PortData *inPort = nullptr;
    PortData *outPort = nullptr;

    // Held for the whole of the DSP section of the callback, and taken by
    // setEngine() while it swaps. The callback try_locks and falls back to
    // passthrough rather than blocking the data thread on a load that can take
    // a second (DeepFilterNet unpacks an ONNX archive).
    std::mutex engineLock;
    std::unique_ptr<Denoiser> denoiser;
    std::atomic<NoiseEngine> engine{NoiseEngine::RnNoise};
    int frame = 480;

    // Audio waiting to be denoised, and denoised audio waiting to be handed
    // back to PipeWire. Both are needed because the graph quantum is not a
    // multiple of the engine's frame size.
    std::deque<float> pending;
    std::deque<float> ready;
    // The untouched copy, delayed by exactly the same amount, so the dry/wet
    // blend stays phase-aligned. Mixing a delayed wet signal against an
    // undelayed dry one would comb-filter the result.
    std::deque<float> dry;
    std::vector<float> scratchIn;
    std::vector<float> scratchOut;

    std::atomic<bool> enabled{true};
    std::atomic<float> intensity{1.0f};
    // The mix actually in use, chasing `intensity` a sample at a time.
    float wetMix = 1.0f;
    std::atomic<float> speechProb{0.0f};
    std::atomic<float> inRms{0.0f};
    std::atomic<float> outRms{0.0f};
    // Primed once enough output has accumulated to cover a full frame of
    // latency; before that we emit silence rather than stutter.
    bool primed = false;
};

namespace {

void measureOut(NoiseFilter::Impl *d, const float *out, uint32_t n) {
    double acc = 0.0;
    for (uint32_t i = 0; i < n; ++i) acc += double(out[i]) * out[i];
    d->outRms.store(n ? float(std::sqrt(acc / n)) : 0.0f,
                    std::memory_order_relaxed);
}

void onProcess(void *userdata, spa_io_position *position) {
    auto *d = static_cast<NoiseFilter::Impl *>(userdata);
    DspScope probe(d->meter, position);
    const uint32_t n = position->clock.duration;

    auto *in = static_cast<float *>(
        pw_filter_get_dsp_buffer(d->inPort, n));
    auto *out = static_cast<float *>(
        pw_filter_get_dsp_buffer(d->outPort, n));

    if (!out) return;
    if (!in) {  // no input connected yet
        std::memset(out, 0, n * sizeof(float));
        return;
    }

    {   // Measured before anything else, so it reflects what really arrived.
        double acc = 0.0;
        for (uint32_t i = 0; i < n; ++i) acc += double(in[i]) * in[i];
        d->inRms.store(n ? float(std::sqrt(acc / n)) : 0.0f,
                       std::memory_order_relaxed);
    }

    // Every path below must fall through to the output meter at the end.
    // Returning early from the bypass branch left it reporting a stale value,
    // which made an A/B comparison read as if bypass were quieter than the
    // denoised signal.
    // A swap is in progress, or the engine failed to build. Either way there is
    // nothing to denoise with, and passing audio through beats dropping it:
    // a quantum of untouched microphone is a blip, a quantum of silence is a
    // dropout the user hears.
    std::unique_lock<std::mutex> lk(d->engineLock, std::try_to_lock);
    if (!lk.owns_lock() || !d->denoiser ||
        !d->enabled.load(std::memory_order_relaxed)) {
        // Bypass: copy through, and drop any buffered state so that switching
        // back does not replay stale audio.
        std::memcpy(out, in, n * sizeof(float));
        if (lk.owns_lock()) {
            d->pending.clear();
            d->ready.clear();
            d->dry.clear();
            d->primed = false;
        }
        measureOut(d, out, n);
        return;
    }

    for (uint32_t i = 0; i < n; ++i) d->pending.push_back(in[i]);

    // Where the mix is heading. It is walked towards, sample by sample, in the
    // loop below: applying a new intensity to a whole frame at once steps the
    // output at the frame boundary, which is the click heard when the strength
    // slider is dragged while talking.
    const float wetTarget = d->intensity.load(std::memory_order_relaxed);
    const int frame = d->frame;
    while (static_cast<int>(d->pending.size()) >= frame) {
        for (int i = 0; i < frame; ++i) {
            const float sample = d->pending.front();
            d->scratchIn[i] = sample;
            d->dry.push_back(sample);
            d->pending.pop_front();
        }
        const float vad = d->denoiser->processFrame(d->scratchIn.data(),
                                                    d->scratchOut.data());
        d->speechProb.store(vad, std::memory_order_relaxed);
        for (int i = 0; i < frame; ++i) {
            const float original = d->dry.front();
            d->dry.pop_front();
            d->wetMix += (wetTarget - d->wetMix) * kWetGlide;
            d->ready.push_back(d->scratchOut[i] * d->wetMix +
                               original * (1.0f - d->wetMix));
        }
    }

    // One frame of slack before output starts, so a quantum larger than the
    // frame size cannot underrun mid-callback.
    if (!d->primed && static_cast<int>(d->ready.size()) >= frame) d->primed = true;

    if (!d->primed || d->ready.size() < n) {
        std::memset(out, 0, n * sizeof(float));
        measureOut(d, out, n);
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = d->ready.front();
        d->ready.pop_front();
    }
    measureOut(d, out, n);
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
    if (d_->denoiser && d_->engine.load(std::memory_order_relaxed) == engine)
        return true;

    // Built before the lock is taken: DeepFilterNet unpacks and loads an ONNX
    // model here, which is far too slow to hold the audio thread out for.
    auto next = makeDenoiser(engine, error);
    if (!next) return false;

    const int frame = next->frameSize();
    {
        std::lock_guard<std::mutex> lk(d_->engineLock);
        d_->denoiser = std::move(next);
        d_->frame = frame;
        d_->scratchIn.assign(frame, 0.0f);
        d_->scratchOut.assign(frame, 0.0f);
        // The engines have different latencies, so anything buffered against
        // the old one would land at the wrong offset against its dry copy and
        // comb-filter the blend.
        d_->pending.clear();
        d_->ready.clear();
        d_->dry.clear();
        d_->primed = false;
    }
    d_->engine.store(engine, std::memory_order_relaxed);
    // The delay this stage adds moved with the engine, so what the diagnostics
    // panel quotes has to move with it too -- unless it is switched off, in
    // which case it is bypassing and adding nothing whatever engine is loaded.
    if (d_->enabled.load(std::memory_order_relaxed))
        d_->meter.setLatencyFrames(static_cast<uint32_t>(frame));
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
    d_->meter.setLatencyFrames(on ? static_cast<uint32_t>(d_->frame) : 0);
}
bool NoiseFilter::enabled() const {
    return d_->enabled.load(std::memory_order_relaxed);
}
void NoiseFilter::setIntensity(float intensity) {
    d_->intensity.store(intensity < 0.0f ? 0.0f : (intensity > 1.0f ? 1.0f : intensity),
                        std::memory_order_relaxed);
}
float NoiseFilter::intensity() const {
    return d_->intensity.load(std::memory_order_relaxed);
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
                        std::string &error, bool asSource, NoiseEngine engine) {
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

    d_->inPort = static_cast<PortData *>(pw_filter_add_port(
        d_->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(PortData),
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "input", nullptr),
        nullptr, 0));

    d_->outPort = static_cast<PortData *>(pw_filter_add_port(
        d_->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
        sizeof(PortData),
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "output", nullptr),
        nullptr, 0));

    if (!d_->inPort || !d_->outPort) {
        error = "pw_filter_add_port failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
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
    d_->denoiser.reset();
}

}  // namespace waveline

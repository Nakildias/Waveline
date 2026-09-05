// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "fluidsynthfilter.h"

#include "dspprobe.h"
#include "filterhost.h"
#include "rtsched.h"
#include "umpcompat.h"

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/control/control.h>
#include <spa/pod/event.h>
#include <spa/pod/iter.h>

#include <cstring>
#include <vector>

namespace waveline {

namespace {

constexpr uint32_t kRate = 48000;

struct PortData {
    bool midi = false;
};

}  // namespace

struct FluidSynthFilter::Impl {
    pw_thread_loop *loop = nullptr;
    pw_filter *filter = nullptr;
    spa_hook listener{};
    DspMeter meter;
    PortData *midiIn = nullptr;
    PortData *audioOut = nullptr;

    FluidSynthBackend synth;
};

void FluidSynthFilter::filterProcess(void *userdata, spa_io_position *position) {
    auto *d = static_cast<Impl *>(userdata);
    DspScope probe(d->meter, position);
    const uint32_t n = position->clock.duration;

    if (d->midiIn) {
        struct pw_buffer *b = nullptr;
        while ((b = pw_filter_dequeue_buffer(d->midiIn)) != nullptr) {
            if (b->buffer && b->buffer->n_datas > 0) {
                const spa_data &data = b->buffer->datas[0];
                if (data.data && data.chunk && data.chunk->size > 0) {
                    const uint32_t offset = data.chunk->offset;
                    const uint32_t size = data.chunk->size;
                    if (offset <= data.maxsize && size <= data.maxsize - offset &&
                        size >= sizeof(spa_pod_sequence)) {
                        const auto *sequence = static_cast<const spa_pod_sequence *>(
                            SPA_PTROFF(data.data, offset, const void));
                        if (spa_pod_is_sequence(&sequence->pod) &&
                            SPA_POD_SIZE(&sequence->pod) <= size) {
                            const spa_pod_control *control;
                            SPA_POD_SEQUENCE_FOREACH(sequence, control) {
                                const void *eventData = nullptr;
                                uint32_t eventSize = 0;
                                if (spa_pod_get_bytes(&control->value, &eventData,
                                                      &eventSize) < 0 ||
                                    !eventData || eventSize == 0)
                                    continue;

                                if (control->type == SPA_CONTROL_Midi) {
                                    d->synth.handleMidiBytes(
                                        static_cast<const uint8_t *>(eventData),
                                        eventSize);
                                }
#if WAVELINE_HAVE_SPA_UMP
                                else if (control->type == SPA_CONTROL_UMP) {
                                    const auto *ump =
                                        static_cast<const uint32_t *>(eventData);
                                    size_t remaining = eventSize;
                                    uint64_t state = 0;
                                    while (remaining > 0) {
                                        uint8_t midi[8]{};
                                        const int midiSize = spa_ump_to_midi(
                                            &ump, &remaining, midi, sizeof(midi),
                                            &state);
                                        if (midiSize > 0)
                                            d->synth.handleMidiBytes(
                                                midi,
                                                static_cast<size_t>(midiSize));
                                        else if (midiSize < 0)
                                            break;
                                    }
                                }
#endif
                            }
                        }
                    }
                }
            }
            pw_filter_queue_buffer(d->midiIn, b);
        }
    }

    float *out = nullptr;
    if (d->audioOut)
        out = static_cast<float *>(pw_filter_get_dsp_buffer(d->audioOut, n));
    if (!out) return;

    if (!d->synth.ready()) {
        std::memset(out, 0, n * sizeof(float));
        return;
    }

    d->synth.render(out, static_cast<int>(n));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_filter_events kFluidSynthFilterEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .process = FluidSynthFilter::filterProcess,
};
#pragma GCC diagnostic pop

FluidSynthFilter::FluidSynthFilter() : d_(std::make_unique<Impl>()) {}
FluidSynthFilter::~FluidSynthFilter() { stop(); }

bool FluidSynthFilter::start(const std::string &nodeName, const std::string &description,
                             std::string &error) {
    std::string initErr;
    if (!d_->synth.init(static_cast<int>(kRate), initErr)) {
        error = initErr;
        return false;
    }

    pw_init(nullptr, nullptr);
    d_->meter.attach(nodeName, "MIDI synth");

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
        pw_filter_add_listener(d_->filter, &d_->listener, &kFluidSynthFilterEvents, d_.get());
    if (!d_->filter) {
        error = "pw_filter_new_simple failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    d_->midiIn = static_cast<PortData *>(pw_filter_add_port(
        d_->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(PortData),
        pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME, "midi_in",
                          nullptr),
        nullptr, 0));
    d_->midiIn->midi = true;

    d_->audioOut = static_cast<PortData *>(pw_filter_add_port(
        d_->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(PortData),
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "output", nullptr),
        nullptr, 0));

    if (!d_->midiIn || !d_->audioOut) {
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

void FluidSynthFilter::stop() {
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
    d_->midiIn = nullptr;
    d_->audioOut = nullptr;
    pw_thread_loop_unlock(d_->loop);
    // The loop and the connection are shared and outlive this filter; only the
    // node on them is ours to destroy. See filterhost.h.
    d_->loop = nullptr;
    // After the loop is gone, so nothing can still be inside the counters.
    d_->meter.detach();
    d_->synth.shutdown();
}

bool FluidSynthFilter::loadSoundfont(const std::string &path, std::string &error) {
    return d_->synth.loadSoundfont(path, error);
}

void FluidSynthFilter::setSoundfontPath(const std::string &path) {
    if (path.empty()) return;
    std::string err;
    (void)d_->synth.loadSoundfont(path, err);
}

}  // namespace waveline

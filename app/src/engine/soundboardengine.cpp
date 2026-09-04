// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "soundboardengine.h"

#include "dspprobe.h"
#include "pwengine.h"
#include "rtsched.h"

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// dr_wav / dr_mp3 are vendored, header-only, public-domain-or-MIT-0
// decoders (see app/lib/thirdparty/README.md). The implementation is
// compiled exactly once, here.
#define DR_WAV_IMPLEMENTATION
#include "../../lib/thirdparty/dr_wav.h"

#define DR_MP3_IMPLEMENTATION
#include "../../lib/thirdparty/dr_mp3.h"

namespace waveline {

namespace {

constexpr uint32_t kGraphRate = 48000;

std::string lowerExt(const std::string &path) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

// Downmixes `channels`-interleaved `frames` of `src` to interleaved stereo.
std::vector<float> toStereo(const float *src, uint64_t frames, uint32_t channels) {
    std::vector<float> out(static_cast<size_t>(frames) * 2);
    if (channels == 2) {
        std::memcpy(out.data(), src, out.size() * sizeof(float));
    } else if (channels == 1) {
        for (uint64_t i = 0; i < frames; ++i) out[i * 2] = out[i * 2 + 1] = src[i];
    } else {
        // Anything wider (5.1, etc.): average every source channel into both
        // output channels. Soundboard clips are not where surround content
        // belongs, so this only has to be reasonable, not exact.
        for (uint64_t i = 0; i < frames; ++i) {
            float sum = 0.0f;
            const float *frame = src + i * channels;
            for (uint32_t c = 0; c < channels; ++c) sum += frame[c];
            const float avg = sum / static_cast<float>(channels);
            out[i * 2] = out[i * 2 + 1] = avg;
        }
    }
    return out;
}

// Linear-interpolation resample, interleaved stereo in and out. Adequate for
// one-shot sound effects; this is not the mixer's monitor path.
std::vector<float> resampleStereo(const std::vector<float> &in, uint32_t inRate,
                                  uint32_t outRate) {
    if (inRate == 0 || outRate == 0 || inRate == outRate) return in;
    const uint64_t inFrames = in.size() / 2;
    if (inFrames == 0) return {};
    const double ratio = static_cast<double>(inRate) / static_cast<double>(outRate);
    const uint64_t outFrames =
        static_cast<uint64_t>(std::llround(inFrames / ratio));
    std::vector<float> out(outFrames * 2);
    for (uint64_t i = 0; i < outFrames; ++i) {
        const double srcPos = i * ratio;
        uint64_t i0 = static_cast<uint64_t>(srcPos);
        if (i0 >= inFrames) i0 = inFrames - 1;
        uint64_t i1 = std::min(i0 + 1, inFrames - 1);
        const float t = static_cast<float>(srcPos - static_cast<double>(i0));
        out[i * 2 + 0] = in[i0 * 2 + 0] + (in[i1 * 2 + 0] - in[i0 * 2 + 0]) * t;
        out[i * 2 + 1] = in[i0 * 2 + 1] + (in[i1 * 2 + 1] - in[i0 * 2 + 1]) * t;
    }
    return out;
}

}  // namespace

SoundboardBuffer decodeSoundFile(const std::string &path, std::string &error) {
    SoundboardBuffer result;
    const std::string ext = lowerExt(path);

    std::vector<float> stereo;
    uint32_t nativeRate = kGraphRate;

    if (ext == "wav") {
        unsigned int channels = 0, sampleRate = 0;
        drwav_uint64 frames = 0;
        float *pcm = drwav_open_file_and_read_pcm_frames_f32(
            path.c_str(), &channels, &sampleRate, &frames, nullptr);
        if (!pcm || channels == 0 || frames == 0) {
            if (pcm) drwav_free(pcm, nullptr);
            error = "could not read '" + path + "' as WAV audio";
            return result;
        }
        stereo = toStereo(pcm, frames, channels);
        drwav_free(pcm, nullptr);
        nativeRate = sampleRate;
    } else if (ext == "mp3") {
        drmp3_config config{};
        drmp3_uint64 frames = 0;
        float *pcm = drmp3_open_file_and_read_pcm_frames_f32(path.c_str(), &config,
                                                              &frames, nullptr);
        if (!pcm || config.channels == 0 || frames == 0) {
            if (pcm) drmp3_free(pcm, nullptr);
            error = "could not read '" + path + "' as MP3 audio";
            return result;
        }
        stereo = toStereo(pcm, frames, config.channels);
        drmp3_free(pcm, nullptr);
        nativeRate = config.sampleRate;
    } else {
        error = "unsupported file type '" + ext + "' -- only .wav and .mp3 are supported";
        return result;
    }

    stereo = resampleStereo(stereo, nativeRate, kGraphRate);
    result.samples = std::move(stereo);
    result.frames = static_cast<uint32_t>(result.samples.size() / 2);
    if (result.frames == 0) error = "'" + path + "' decoded to no audio";
    return result;
}

std::vector<float> soundboardPeaks(const SoundboardBuffer &buf, int count, uint32_t startFrame,
                                   uint32_t endFrame) {
    std::vector<float> out;
    if (count <= 0 || buf.frames == 0) return out;
    const uint32_t end = (endFrame == 0 || endFrame > buf.frames) ? buf.frames : endFrame;
    const uint32_t start = std::min(startFrame, end);
    const uint32_t span = end - start;
    if (span == 0) return out;
    out.assign(static_cast<size_t>(count), 0.0f);
    const uint32_t framesPerBucket = std::max<uint32_t>(1, span / static_cast<uint32_t>(count));
    for (int b = 0; b < count; ++b) {
        const uint64_t bucketStart = start + static_cast<uint64_t>(b) * framesPerBucket;
        if (bucketStart >= end) break;
        const uint64_t bucketEnd = std::min<uint64_t>(bucketStart + framesPerBucket, end);
        float peak = 0.0f;
        for (uint64_t i = bucketStart; i < bucketEnd; ++i) {
            peak = std::max(peak, std::fabs(buf.samples[i * 2]));
            peak = std::max(peak, std::fabs(buf.samples[i * 2 + 1]));
        }
        out[static_cast<size_t>(b)] = peak;
    }
    return out;
}

// ============================================================== SoundboardVoice

namespace {

struct PortData {
    bool isShare = false;
};

}  // namespace

struct SoundboardVoice::Impl {
    pw_thread_loop *loop = nullptr;
    pw_filter *filter = nullptr;
    PortData *localL = nullptr;
    PortData *localR = nullptr;
    PortData *shareL = nullptr;
    PortData *shareR = nullptr;
    DspMeter meter;
    std::string nodeName;

    std::shared_ptr<const SoundboardBuffer> buffer;
    std::atomic<uint32_t> cursor{0};
    uint32_t startFrame = 0;
    uint32_t endFrame = 0;
    float localGain = 1.0f;
    float shareGain = 1.0f;
    std::atomic<bool> *finished = nullptr;  // owned by the enclosing SoundboardVoice

    static void filterProcess(void *userdata, spa_io_position *position) {
        auto *d = static_cast<Impl *>(userdata);
        DspScope probe(d->meter, position);
        const uint32_t n = position->clock.duration;

        float *lL = d->localL ? static_cast<float *>(
                                    pw_filter_get_dsp_buffer(d->localL, n))
                              : nullptr;
        float *lR = d->localR ? static_cast<float *>(
                                    pw_filter_get_dsp_buffer(d->localR, n))
                              : nullptr;
        float *sL = d->shareL ? static_cast<float *>(
                                    pw_filter_get_dsp_buffer(d->shareL, n))
                              : nullptr;
        float *sR = d->shareR ? static_cast<float *>(
                                    pw_filter_get_dsp_buffer(d->shareR, n))
                              : nullptr;

        const SoundboardBuffer *buf = d->buffer.get();
        uint32_t cursor = d->cursor.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) {
            float l = 0.0f, r = 0.0f;
            if (buf && cursor < d->endFrame && cursor < buf->frames) {
                l = buf->samples[static_cast<size_t>(cursor) * 2 + 0];
                r = buf->samples[static_cast<size_t>(cursor) * 2 + 1];
                ++cursor;
            }
            if (lL) lL[i] = l * d->localGain;
            if (lR) lR[i] = r * d->localGain;
            if (sL) sL[i] = l * d->shareGain;
            if (sR) sR[i] = r * d->shareGain;
        }
        d->cursor.store(cursor, std::memory_order_relaxed);
        if (cursor >= d->endFrame) d->finished->store(true, std::memory_order_release);
    }
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_filter_events kSoundboardVoiceEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .process = SoundboardVoice::Impl::filterProcess,
};
#pragma GCC diagnostic pop

SoundboardVoice::SoundboardVoice() : d_(std::make_unique<Impl>()) {
    d_->finished = &finished_;
}

SoundboardVoice::~SoundboardVoice() { stop(); }

bool SoundboardVoice::start(PwEngine &engine, std::shared_ptr<const SoundboardBuffer> buffer,
                            const SoundboardPlaySpec &spec, std::string &error) {
    if (!buffer || !buffer->valid()) {
        error = "no audio to play";
        return false;
    }

    d_->buffer = buffer;
    const uint32_t end =
        (spec.trimEndFrames == 0 || spec.trimEndFrames > buffer->frames)
            ? buffer->frames
            : spec.trimEndFrames;
    const uint32_t start = std::min(spec.trimStartFrames, end);
    d_->cursor.store(start, std::memory_order_relaxed);
    d_->startFrame = start;
    d_->endFrame = end;
    d_->localGain = spec.localGain;
    d_->shareGain = spec.shareGain;
    d_->nodeName = spec.nodeName;

    pw_init(nullptr, nullptr);
    d_->meter.attach(spec.nodeName, "Soundboard");

    d_->loop = pw_thread_loop_new("waveline-soundboard", nullptr);
    if (!d_->loop) {
        error = "pw_thread_loop_new failed";
        return false;
    }

    pw_thread_loop_lock(d_->loop);

    auto *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Production",
        PW_KEY_MEDIA_CLASS, "Stream/Filter/Audio",
        PW_KEY_NODE_NAME, spec.nodeName.c_str(),
        PW_KEY_NODE_DESCRIPTION, spec.description.c_str(),
        PW_KEY_NODE_AUTOCONNECT, "false",
        "node.want-driver", "true",
        nullptr);

    d_->filter =
        pw_filter_new_simple(pw_thread_loop_get_loop(d_->loop), spec.nodeName.c_str(),
                             applyRealtimeProps(props), &kSoundboardVoiceEvents, d_.get());
    if (!d_->filter) {
        error = "pw_filter_new_simple failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }

    auto addOut = [&](const char *portName) -> PortData * {
        return static_cast<PortData *>(pw_filter_add_port(
            d_->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS,
            sizeof(PortData),
            pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                              PW_KEY_PORT_NAME, portName, nullptr),
            nullptr, 0));
    };

    d_->localL = addOut("local_FL");
    d_->localR = addOut("local_FR");
    if (!d_->localL || !d_->localR) {
        error = "pw_filter_add_port failed";
        pw_thread_loop_unlock(d_->loop);
        return false;
    }
    d_->localL->isShare = false;
    d_->localR->isShare = false;

    const bool wantShare = !spec.shareTarget.empty();
    if (wantShare) {
        d_->shareL = addOut("share_FL");
        d_->shareR = addOut("share_FR");
        if (!d_->shareL || !d_->shareR) {
            error = "pw_filter_add_port failed";
            pw_thread_loop_unlock(d_->loop);
            return false;
        }
        d_->shareL->isShare = true;
        d_->shareR->isShare = true;
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

    // The filter's own ports register with the registry asynchronously; wait
    // for them before linking, the same ladder every other filter in this
    // graph follows immediately after start().
    //
    // The local target is a channel sink -- support.null-audio-sink, created
    // by PwEngine::addNullSink() -- and that adapter names its input ports
    // "playback_FL"/"playback_FR", not "input_FL"/"input_FR" (that naming
    // belongs to the loopback-module-based virtual sources the share target
    // below points at instead; the two adapter types disagree and both
    // conventions are real, just for different node kinds).
    engine.waitForPort(spec.nodeName, "local_FL", true, 1000);
    bool ok =
        engine.linkPorts(spec.nodeName, "local_FL", spec.localTarget, "playback_FL", error);
    ok = engine.linkPorts(spec.nodeName, "local_FR", spec.localTarget, "playback_FR", error) &&
        ok;

    if (wantShare) {
        engine.waitForPort(spec.nodeName, "share_FL", true, 1000);
        if (spec.shareTargetMono) {
            engine.linkPorts(spec.nodeName, "share_FL", spec.shareTarget, "input_MONO", error);
            engine.linkPorts(spec.nodeName, "share_FR", spec.shareTarget, "input_MONO", error);
        } else {
            engine.linkPorts(spec.nodeName, "share_FL", spec.shareTarget, "input_FL", error);
            engine.linkPorts(spec.nodeName, "share_FR", spec.shareTarget, "input_FR", error);
        }
    }

    if (!ok) return false;
    error.clear();
    return true;
}

void SoundboardVoice::stop() {
    if (!d_) return;
    if (!d_->loop) {
        d_->meter.detach();
        return;
    }
    pw_thread_loop_lock(d_->loop);
    if (d_->filter) {
        pw_filter_destroy(d_->filter);
        d_->filter = nullptr;
    }
    d_->localL = d_->localR = d_->shareL = d_->shareR = nullptr;
    pw_thread_loop_unlock(d_->loop);
    pw_thread_loop_stop(d_->loop);
    pw_thread_loop_destroy(d_->loop);
    d_->loop = nullptr;
    d_->meter.detach();
}

double SoundboardVoice::progress() const {
    if (!d_ || d_->endFrame <= d_->startFrame) return 0.0;
    const uint32_t cur = d_->cursor.load(std::memory_order_relaxed);
    const uint32_t clamped = std::min(std::max(cur, d_->startFrame), d_->endFrame);
    return static_cast<double>(clamped - d_->startFrame) /
           static_cast<double>(d_->endFrame - d_->startFrame);
}

// ============================================================= SoundboardEngine

SoundboardEngine::SoundboardEngine(PwEngine &engine) : engine_(&engine) {}

SoundboardEngine::~SoundboardEngine() { stopAll(); }

bool SoundboardEngine::play(const std::string &soundId,
                            std::shared_ptr<const SoundboardBuffer> buffer,
                            SoundboardPlaySpec spec, std::string &error) {
    if (!engine_) {
        error = "no engine";
        return false;
    }

    // voice->start() below blocks for as long as PipeWire takes to register
    // and link the new voice's ports (~100ms typical, not instant -- see
    // MixerService::PlaySoundboardSound's own comment on why it now runs
    // this off the D-Bus dispatch thread in the first place). Holding
    // mutex_ across that -- what this used to do -- defeated the point of
    // that change: playing()/progress() below take the same lock, and
    // SoundboardWindow polls exactly those on every refresh to draw the
    // progress ring, so every one of those reads would still stall for the
    // same ~100ms, just against this lock instead of the D-Bus dispatch
    // queue. stop()/stopAll() already follow the rule this was missing:
    // only ever hold mutex_ for the brief, non-blocking mutation of
    // voices_/nextSeq_, never across a PipeWire wait.
    uint64_t seq;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seq = nextSeq_++;
    }
    spec.nodeName = "waveline-sb-" + soundId + "-" + std::to_string(seq);
    if (spec.description.empty()) spec.description = "Soundboard";

    auto voice = std::make_unique<SoundboardVoice>();
    if (!voice->start(*engine_, std::move(buffer), spec, error)) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    voices_.emplace(soundId, std::move(voice));
    return true;
}

void SoundboardEngine::stop(const std::string &soundId) {
    std::vector<std::unique_ptr<SoundboardVoice>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto range = voices_.equal_range(soundId);
        for (auto it = range.first; it != range.second; ++it)
            doomed.push_back(std::move(it->second));
        voices_.erase(range.first, range.second);
    }
    // Destroyed outside the lock: SoundboardVoice::stop() blocks on its own
    // PipeWire thread loop, which must never happen while holding a lock
    // reap() or another stop() call might also be waiting on.
    doomed.clear();
}

void SoundboardEngine::stopAll() {
    std::vector<std::unique_ptr<SoundboardVoice>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &[id, voice] : voices_) doomed.push_back(std::move(voice));
        voices_.clear();
    }
    doomed.clear();
}

std::vector<std::string> SoundboardEngine::playing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(voices_.size());
    for (const auto &[id, voice] : voices_) {
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

double SoundboardEngine::progress(const std::string &soundId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = voices_.find(soundId);
    if (it == voices_.end()) return -1.0;
    return it->second->progress();
}

bool SoundboardEngine::reap() {
    std::vector<std::unique_ptr<SoundboardVoice>> doomed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = voices_.begin(); it != voices_.end();) {
            if (it->second->finished()) {
                doomed.push_back(std::move(it->second));
                it = voices_.erase(it);
            } else {
                ++it;
            }
        }
    }
    const bool changed = !doomed.empty();
    doomed.clear();
    return changed;
}

}  // namespace waveline

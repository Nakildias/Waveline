// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "tuner.h"

#include "rtsched.h"

#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/control/control.h>
#include <spa/control/ump-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "biquad.h"

namespace waveline {
namespace {

constexpr uint32_t kRate = 48000;

// Analysis runs on a decimated copy: YIN is O(window * taus), and at 48 kHz a
// window long enough for a low B costs ~40x what it costs at 12 kHz for an
// answer that is then refined at full rate anyway.
constexpr int kDecim = 4;
constexpr int kLowRate = static_cast<int>(kRate) / kDecim;   // 12 kHz

// 25 Hz covers a 5-string bass low B (30.9 Hz) with room underneath; 1400 Hz
// is well above a mandolin's top E (659 Hz). Nothing outside that is a string
// anyone is tuning, and a narrower search is a search with fewer octave errors.
constexpr double kMinHz = 25.0;
constexpr double kMaxHz = 1400.0;

constexpr int kTauMax = kLowRate / static_cast<int>(kMinHz);   // 480
constexpr int kTauMin = kLowRate / static_cast<int>(kMaxHz);   // 8
// The difference function needs `window` samples of overlap at every lag, so
// the buffer is the window plus the longest lag.
constexpr int kWindowLow = 1024;
constexpr int kNeedLow = kWindowLow + kTauMax;                  // 1504

// Refinement window at 48 kHz. At 12 kHz a top-E period is 36 samples, so even
// a tenth of a sample of interpolation error is several cents -- fine for
// finding the note, not fine for the last few cents, which is the entire job.
constexpr int kWindowHi = 2048;
constexpr int kRefineSpan = 8;
// One past the longest lag the refinement can ask for, so the parabola's right
// neighbour is still inside the buffer.
constexpr int kNeedHi = kWindowHi + kTauMax * kDecim + kRefineSpan + 1;  // 3977

constexpr int kRingLow = 4096;     // power of two, ~340 ms
constexpr int kRingHi = 16384;     // power of two, ~340 ms

// Below this the window is room noise and the estimate is meaningless. This is
// the floor for a *quiet* room; a microphone in a normal one sits well above it
// with nobody playing, which is what the tracker below is for.
constexpr double kSilenceRms = 0.0015;

// ------------------------------------------------------- microphone in a room
// A guitar cable carries the guitar and nothing else. A condenser carries the
// guitar, a fan, a desk, a room, and whoever is in it -- and a fan is periodic
// enough to fool a pitch detector into a confident, wrong, wandering answer.
// Three things keep that out, in order of how much work they do:
//
//   1. Room tone is measured rather than assumed. The floor falls quickly to a
//      quiet window and climbs back slowly, so it settles on what the room does
//      when nobody is playing, whatever that happens to be.
constexpr double kFloorFall = 0.30;    // per 30 ms frame, toward a quieter one
constexpr double kFloorRise = 0.0025;  // per frame, back up
//      Anything less than this far above the floor is the room, not a string.
//      12 dB is enough to clear an office and low enough for a string that has
//      been ringing for a few seconds.
constexpr double kSignalOverNoise = 4.0;
//
//   2. The periodicity bar is higher than a needle strictly needs. Noise makes
//      shallow dips all over the difference function; a string makes a deep
//      one. 0.62 passes a plucked string comfortably and rejects most of what a
//      room offers.
constexpr double kAcceptConfidence = 0.62;
//      And when nothing dips below YIN's threshold at all, the shallowest lag
//      is only worth reporting if it is still reasonably deep.
constexpr float kYinFallbackMax = 0.30f;
//
//   3. A *jump* is not published until several frames agree. This is what stops
//      the needle wandering: noise disagrees with itself, a string does not.
//      Three frames is 90 ms.
constexpr int kVoteFrames = 3;
constexpr double kVoteSpreadCents = 45.0;
//      Movement near the note already on the needle is not a jump, and waiting
//      for a vote on it is how a tuner sits still while a peg is being turned.
//      Anything inside this window of the current reading is followed at once;
//      only something further away has to prove itself first.
constexpr double kTrackWindowCents = 200.0;
// A note that has stopped ringing, or a frame the gates rejected, leaves the
// last good reading on the needle rather than blanking it. Long enough to look
// down at, short enough not to lie about a string you have stopped playing.
constexpr int64_t kAudioHoldMs = 1500;
// YIN's absolute threshold. The classic 0.10 is tuned for speech; strings are
// far more periodic than voice, and a slightly looser gate keeps a decaying
// note readable for the couple of seconds it takes to turn a peg.
constexpr float kYinThreshold = 0.15f;

// A MIDI note stays on the display after its note-off, so releasing the key
// does not blank the reading before you have looked at it.
constexpr int64_t kMidiHoldMs = 2000;

constexpr const char *kNodeName = "waveline-tuner";
constexpr const char *kMidiPort = "midi_in";

// pw_filter hands each port a block of user data. Nothing is needed in it, but
// the API has no way to add a port without one.
struct MidiPort {
    int unused = 0;
};

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

struct Tuner::Impl {
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener{};
    // The MIDI tap is a filter rather than a stream, so its input port can be
    // given a fixed name for the caller to link into.
    pw_filter *filter = nullptr;
    MidiPort *midiIn = nullptr;

    std::string kind;
    std::string source;
    bool midi = false;

    // ------------------------------------------------------- audio capture
    // Written by the PipeWire thread, read by the analysis thread. Single
    // producer, single consumer, and the rings hold ~340 ms against a read of
    // ~80 ms, so the reader is never anywhere near the writer.
    std::vector<float> ringLow = std::vector<float>(kRingLow, 0.0f);
    std::vector<float> ringHi = std::vector<float>(kRingHi, 0.0f);
    std::atomic<uint64_t> writeLow{0};
    std::atomic<uint64_t> writeHi{0};

    uint32_t channels = 1;
    Biquad dcBlock = Biquad::highPass(static_cast<float>(kRate), 25.0f);
    // Three cascaded sections ahead of the 4:1 decimation. Anything left above
    // 6 kHz folds back into the band YIN searches.
    Biquad decimLp[3] = {
        Biquad::lowPass(static_cast<float>(kRate), 4000.0f),
        Biquad::lowPass(static_cast<float>(kRate), 4000.0f),
        Biquad::lowPass(static_cast<float>(kRate), 4000.0f),
    };
    int decimPhase = 0;

    // ------------------------------------------------- reference tone output
    // A plucked note, played back into the Monitor mix on request. Written by
    // whoever calls playReference(), read by the stream's own process callback.
    pw_stream *refStream = nullptr;
    spa_hook refListener{};
    std::atomic<double> refHz{0.0};
    std::atomic<int> refSamples{0};   // how much of the note is left to play
    std::atomic<uint32_t> refTrigger{0};
    uint32_t refSeen = 0;
    double refPhase = 0.0;
    double refEnv = 0.0;
    int refLeft = 0;
    int refTotal = 0;
    bool startReference(const std::string &targetSink);

    // ---------------------------------------------------------- MIDI state
    std::atomic<int> midiNote{-1};
    std::atomic<int> midiVelocity{0};
    std::atomic<bool> midiHeld{false};
    std::atomic<double> midiBendCents{0.0};
    std::atomic<int64_t> midiStamp{0};

    // ------------------------------------------------------------- results
    std::thread worker;
    std::atomic<bool> quit{false};
    mutable std::mutex resultMutex;
    TunerReading result;

    // Analysis scratch, allocated once.
    std::vector<float> low = std::vector<float>(kNeedLow, 0.0f);
    std::vector<float> hi = std::vector<float>(kNeedHi, 0.0f);
    std::vector<float> diff = std::vector<float>(kTauMax + 1, 0.0f);
    std::vector<float> cmnd = std::vector<float>(kTauMax + 1, 0.0f);
    double smoothed = 0.0;
    // What the room sounds like with nobody playing, and the last few estimates
    // that got past the gates. See the constants above.
    double noiseFloor = 0.0;
    std::vector<double> votes;
    TunerReading held;
    int64_t heldStamp = 0;

    // Both are called with the thread loop locked and before it is started.
    bool startAudioTap(const std::string &nodeName, std::string &error);
    bool startMidiTap(std::string &error);

    void pushSample(float mono);
    bool snapshot();
    double detect(double &confidence);
    void analyse();
    void handleMidiBytes(const uint8_t *bytes, size_t len);
    void publish(const TunerReading &r) {
        std::lock_guard<std::mutex> lock(resultMutex);
        result = r;
    }
};

void Tuner::Impl::pushSample(float mono) {
    const float hp = dcBlock.process(mono);
    const uint64_t wh = writeHi.load(std::memory_order_relaxed);
    ringHi[wh & (kRingHi - 1)] = hp;
    writeHi.store(wh + 1, std::memory_order_release);

    float lp = hp;
    for (Biquad &b : decimLp) lp = b.process(lp);
    if (++decimPhase < kDecim) return;
    decimPhase = 0;

    const uint64_t wl = writeLow.load(std::memory_order_relaxed);
    ringLow[wl & (kRingLow - 1)] = lp;
    writeLow.store(wl + 1, std::memory_order_release);
}

bool Tuner::Impl::snapshot() {
    const uint64_t wl = writeLow.load(std::memory_order_acquire);
    const uint64_t wh = writeHi.load(std::memory_order_acquire);
    if (wl < static_cast<uint64_t>(kNeedLow) || wh < static_cast<uint64_t>(kNeedHi))
        return false;

    for (int i = 0; i < kNeedLow; ++i)
        low[i] = ringLow[(wl - kNeedLow + i) & (kRingLow - 1)];
    for (int i = 0; i < kNeedHi; ++i)
        hi[i] = ringHi[(wh - kNeedHi + i) & (kRingHi - 1)];
    return true;
}

// YIN over the decimated window, then one pass at full rate around the answer.
// Returns 0 when nothing periodic was found.
double Tuner::Impl::detect(double &confidence) {
    confidence = 0.0;

    // Squared difference function, and the running sum that normalises it.
    // Lags below kTauMin are still computed: the normalisation is cumulative,
    // so skipping them would scale every later lag by the wrong denominator.
    double running = 0.0;
    cmnd[0] = 1.0f;
    for (int tau = 1; tau <= kTauMax; ++tau) {
        double sum = 0.0;
        for (int j = 0; j < kWindowLow; ++j) {
            const double delta = static_cast<double>(low[j]) - low[j + tau];
            sum += delta * delta;
        }
        diff[tau] = static_cast<float>(sum);
        running += sum;
        cmnd[tau] = running > 0.0 ? static_cast<float>(sum * tau / running) : 1.0f;
    }

    // The *first* dip below the threshold, not the deepest one. Taking the
    // global minimum is what makes naive autocorrelation tuners read an octave
    // low on a wound string, where the dip at twice the period is often the
    // deeper of the two.
    int best = -1;
    for (int tau = kTauMin; tau <= kTauMax; ++tau) {
        if (cmnd[tau] >= kYinThreshold) continue;
        // Walk to the bottom of this dip before committing to it.
        while (tau + 1 <= kTauMax && cmnd[tau + 1] < cmnd[tau]) ++tau;
        best = tau;
        break;
    }
    if (best < 0) {
        // Nothing dipped below the threshold. The shallowest lag is worth
        // reporting only if it is still a real dip -- on a microphone in a
        // room there is always *some* minimum, and reporting it is how a tuner
        // ends up confidently displaying the fan.
        float lowest = 1.0f;
        for (int tau = kTauMin; tau <= kTauMax; ++tau) {
            if (cmnd[tau] >= lowest) continue;
            lowest = cmnd[tau];
            best = tau;
        }
        if (lowest >= kYinFallbackMax) return 0.0;
    }
    if (best < 0) return 0.0;

    confidence = std::clamp(1.0 - static_cast<double>(cmnd[best]), 0.0, 1.0);

    // Refine at 48 kHz. The coarse lag maps to kDecim times as many samples;
    // rescan a few either side, then interpolate the parabola through the
    // minimum. That last step is what buys the final few cents of resolution:
    // at 12 kHz a top-E period is 36 samples, and a tenth of a sample there is
    // already five cents of error.
    auto diffAt = [this](int tau) {
        double sum = 0.0;
        for (int j = 0; j < kWindowHi; ++j) {
            const double delta = static_cast<double>(hi[j]) - hi[j + tau];
            sum += delta * delta;
        }
        return sum;
    };

    const int centre = best * kDecim;
    const int from = std::max(2, centre - kRefineSpan);
    const int to = std::min(centre + kRefineSpan, kTauMax * kDecim + kRefineSpan);

    double bestVal = 0.0;
    int bestTau = -1;
    for (int tau = from; tau <= to; ++tau) {
        const double sum = diffAt(tau);
        if (bestTau < 0 || sum < bestVal) {
            bestVal = sum;
            bestTau = tau;
        }
    }
    if (bestTau < 0) return static_cast<double>(kLowRate) / best;

    const double prev = bestTau > 2 ? diffAt(bestTau - 1) : bestVal;
    const double next = diffAt(bestTau + 1);

    double tauRefined = bestTau;
    const double denom = prev + next - 2.0 * bestVal;
    if (denom > 0.0) {
        const double shift = 0.5 * (prev - next) / denom;
        if (shift > -1.0 && shift < 1.0) tauRefined = bestTau + shift;
    }
    if (tauRefined <= 0.0) return 0.0;

    const double freq = static_cast<double>(kRate) / tauRefined;
    if (freq < kMinHz || freq > kMaxHz) return 0.0;
    return freq;
}

void Tuner::Impl::analyse() {
    if (midi) {
        TunerReading r;
        const int64_t age = nowMs() - midiStamp.load(std::memory_order_relaxed);
        const int note = midiNote.load(std::memory_order_relaxed);
        const bool held = midiHeld.load(std::memory_order_relaxed);
        if (note >= 0 && (held || age < kMidiHoldMs)) {
            r.midiNote = note;
            r.bendCents = midiBendCents.load(std::memory_order_relaxed);
            r.frequencyHz =
                440.0 * std::pow(2.0, (note - 69 + r.bendCents / 100.0) / 12.0);
            r.confidence = 1.0;
            r.level = held ? midiVelocity.load(std::memory_order_relaxed) / 127.0 : 0.0;
        }
        publish(r);
        return;
    }

    if (!snapshot()) return;

    double sumSq = 0.0;
    for (int i = kNeedHi - kWindowHi; i < kNeedHi; ++i)
        sumSq += static_cast<double>(hi[i]) * hi[i];
    const double rms = std::sqrt(sumSq / kWindowHi);

    TunerReading r;
    r.level = std::min(1.0, rms);

    // The last good reading stays on the needle for a moment, so a string that
    // has decayed below the gate -- or a frame the gates threw out -- does not
    // blank the display you are looking at.
    const int64_t now = nowMs();
    const auto publishHeld = [&](const TunerReading &bare) {
        if (heldStamp != 0 && now - heldStamp < kAudioHoldMs) {
            TunerReading h = held;
            h.level = bare.level;   // the meter is always live
            publish(h);
        } else {
            publish(bare);
        }
    };

    // Room tone, measured. Falls quickly to a quiet window, climbs slowly back:
    // it ends up tracking what the room does between notes rather than what it
    // does while one is ringing.
    if (noiseFloor <= 0.0)
        noiseFloor = rms;
    else
        noiseFloor += (rms - noiseFloor) *
                      (rms < noiseFloor ? kFloorFall : kFloorRise);

    const double gate = std::max(kSilenceRms, noiseFloor * kSignalOverNoise);
    if (rms < gate) {
        // Below the room, by definition. Nothing here is a string.
        smoothed = 0.0;
        votes.clear();
        publishHeld(r);
        return;
    }

    double confidence = 0.0;
    const double freq = detect(confidence);
    if (freq <= 0.0 || confidence < kAcceptConfidence) {
        // One rejected frame is not a reason to forget the note being tuned;
        // the agreement test below is what decides whether anything changes.
        publishHeld(r);
        return;
    }

    votes.push_back(freq);
    if (static_cast<int>(votes.size()) > kVoteFrames) votes.erase(votes.begin());

    // Is this the note already on the needle, moving? Turning a peg walks the
    // pitch a few cents at a time, and every one of those steps has to reach
    // the display or the tuner is no use for tuning -- which is the whole job.
    const bool tracking =
        smoothed > 0.0 &&
        std::fabs(1200.0 * std::log2(freq / smoothed)) <= kTrackWindowCents;

    double agreed = freq;
    if (!tracking) {
        // A jump: a different string, an octave slip, or noise. It has to hold
        // still for a few frames before it is believed.
        if (static_cast<int>(votes.size()) < kVoteFrames) {
            publishHeld(r);
            return;
        }
        // Agreement, in cents rather than hertz: 45 cents at the top of a
        // mandolin and 45 cents at the bottom of a bass mean the same thing.
        std::vector<double> sorted = votes;
        std::sort(sorted.begin(), sorted.end());
        if (1200.0 * std::log2(sorted.back() / sorted.front()) > kVoteSpreadCents) {
            // The estimates disagree with each other. Noise, or the moment a
            // new string was struck; either way the honest thing is to keep
            // showing the last note until the new one settles.
            publishHeld(r);
            return;
        }
        // The middle one, not the newest: one bad frame inside an otherwise
        // steady group cannot move it, and an octave slip has to survive
        // several frames before it can be believed.
        agreed = sorted[sorted.size() / 2];
    }

    // Smoothed, but only within a note, and lightly: a needle that takes a
    // third of a second to admit the string moved is a needle you cannot tune
    // against.
    if (smoothed <= 0.0 || std::fabs(agreed - smoothed) / smoothed > 0.03)
        smoothed = agreed;
    else
        smoothed += 0.45 * (agreed - smoothed);

    r.frequencyHz = smoothed;
    r.confidence = confidence;
    held = r;
    heldStamp = now;
    publish(r);
}

void Tuner::Impl::handleMidiBytes(const uint8_t *bytes, size_t len) {
    if (len < 2) return;
    const uint8_t status = static_cast<uint8_t>(bytes[0] & 0xf0);

    if (status == 0x90 && len >= 3 && bytes[2] > 0) {
        midiNote.store(bytes[1], std::memory_order_relaxed);
        midiVelocity.store(bytes[2], std::memory_order_relaxed);
        midiHeld.store(true, std::memory_order_relaxed);
        midiStamp.store(nowMs(), std::memory_order_relaxed);
        return;
    }
    // Note-on with velocity 0 is a note-off; plenty of controllers send only
    // that form.
    if (status == 0x80 || (status == 0x90 && len >= 3 && bytes[2] == 0)) {
        if (bytes[1] == midiNote.load(std::memory_order_relaxed)) {
            midiHeld.store(false, std::memory_order_relaxed);
            midiStamp.store(nowMs(), std::memory_order_relaxed);
        }
        return;
    }
    if (status == 0xe0 && len >= 3) {
        const int raw = (static_cast<int>(bytes[2]) << 7) | static_cast<int>(bytes[1]);
        // The bend range is not carried on the wire; ±2 semitones is the
        // default every synth ships with and the only sane assumption here.
        midiBendCents.store((raw - 8192) / 8192.0 * 200.0, std::memory_order_relaxed);
    }
}

namespace {

void onAudioProcess(void *userdata) {
    auto *d = static_cast<Tuner::Impl *>(userdata);
    pw_buffer *b = pw_stream_dequeue_buffer(d->stream);
    if (!b) return;

    spa_buffer *buf = b->buffer;
    if (buf->n_datas > 0 && buf->datas[0].data && buf->datas[0].chunk) {
        const auto *samples = static_cast<const float *>(buf->datas[0].data);
        const uint32_t total = buf->datas[0].chunk->size / sizeof(float);
        const uint32_t ch = d->channels > 0 ? d->channels : 1;
        const uint32_t frames = total / ch;
        for (uint32_t i = 0; i < frames; ++i) {
            float mono = 0.0f;
            for (uint32_t c = 0; c < ch; ++c) mono += samples[i * ch + c];
            d->pushSample(mono / static_cast<float>(ch));
        }
    }

    pw_stream_queue_buffer(d->stream, b);
}

void onAudioParamChanged(void *userdata, uint32_t id, const spa_pod *param) {
    auto *d = static_cast<Tuner::Impl *>(userdata);
    if (!param || id != SPA_PARAM_Format) return;

    uint32_t mediaType = 0, mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0) return;
    if (mediaType != SPA_MEDIA_TYPE_audio || mediaSubtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) < 0) return;
    // Asked for mono, but a server that cannot convert hands back what it has,
    // and reading a stereo buffer as mono halves every period it finds.
    d->channels = info.channels > 0 ? info.channels : 1;
}

void onMidiProcess(void *userdata, spa_io_position *) {
    auto *d = static_cast<Tuner::Impl *>(userdata);
    if (!d->midiIn) return;

    pw_buffer *b = nullptr;
    while ((b = pw_filter_dequeue_buffer(d->midiIn)) != nullptr) {
        if (!b->buffer || b->buffer->n_datas == 0) {
            pw_filter_queue_buffer(d->midiIn, b);
            continue;
        }
        const spa_data &data = b->buffer->datas[0];
        if (!data.data || !data.chunk || data.chunk->size == 0) {
            pw_filter_queue_buffer(d->midiIn, b);
            continue;
        }

        const uint32_t offset = data.chunk->offset;
        const uint32_t size = data.chunk->size;
        if (offset > data.maxsize || size > data.maxsize - offset ||
            size < sizeof(spa_pod_sequence)) {
            pw_filter_queue_buffer(d->midiIn, b);
            continue;
        }

        const auto *sequence =
            static_cast<const spa_pod_sequence *>(SPA_PTROFF(data.data, offset, const void));
        if (spa_pod_is_sequence(&sequence->pod) && SPA_POD_SIZE(&sequence->pod) <= size) {
            const spa_pod_control *control;
            SPA_POD_SEQUENCE_FOREACH(sequence, control) {
                const void *eventData = nullptr;
                uint32_t eventSize = 0;
                if (spa_pod_get_bytes(&control->value, &eventData, &eventSize) < 0 ||
                    !eventData || eventSize == 0)
                    continue;

                if (control->type == SPA_CONTROL_Midi) {
                    d->handleMidiBytes(static_cast<const uint8_t *>(eventData), eventSize);
                } else if (control->type == SPA_CONTROL_UMP) {
                    // Newer servers carry UMP packets; unpack them to MIDI 1.0
                    // the same way the synth filter does.
                    const auto *ump = static_cast<const uint32_t *>(eventData);
                    size_t remaining = eventSize;
                    uint64_t state = 0;
                    while (remaining > 0) {
                        uint8_t midi[8]{};
                        const int midiSize =
                            spa_ump_to_midi(&ump, &remaining, midi, sizeof(midi), &state);
                        if (midiSize > 0)
                            d->handleMidiBytes(midi, static_cast<size_t>(midiSize));
                        else if (midiSize < 0)
                            break;
                    }
                }
            }
        }
        pw_filter_queue_buffer(d->midiIn, b);
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_stream_events kAudioEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .param_changed = onAudioParamChanged,
    .process = onAudioProcess,
};
const pw_filter_events kMidiEvents = {
    .version = PW_VERSION_FILTER_EVENTS,
    .process = onMidiProcess,
};
#pragma GCC diagnostic pop

// A plucked-string-ish reference note: a fundamental with two quiet partials
// above it, a short attack so it does not click, and an exponential decay.
// Deliberately quiet -- it plays into the same mix as the microphone, and a
// reference that drowns the instrument is a reference nobody can tune against.
void onReferenceProcess(void *userdata) {
    auto *d = static_cast<Tuner::Impl *>(userdata);
    pw_buffer *b = pw_stream_dequeue_buffer(d->refStream);
    if (!b) return;
    spa_buffer *buf = b->buffer;
    auto *dst = static_cast<float *>(buf->datas[0].data);
    const uint32_t stride = sizeof(float) * 2;
    uint32_t frames = buf->datas[0].maxsize / stride;
    if (b->requested) frames = std::min<uint32_t>(frames, uint32_t(b->requested));

    // A new note: restart the envelope from the top rather than crossfading,
    // which is what a second pluck sounds like anyway.
    const uint32_t trig = d->refTrigger.load(std::memory_order_acquire);
    if (trig != d->refSeen) {
        d->refSeen = trig;
        d->refLeft = d->refSamples.load(std::memory_order_relaxed);
        d->refTotal = std::max(1, d->refLeft);
        d->refEnv = 0.0;
        d->refPhase = 0.0;
    }

    const double hz = d->refHz.load(std::memory_order_relaxed);
    const double step = 2.0 * M_PI * hz / double(kRate);
    // 8 ms of attack, then a decay that has the note gone as its time runs out.
    const double attack = 1.0 - std::exp(-1.0 / (0.008 * double(kRate)));

    if (dst) {
        for (uint32_t i = 0; i < frames; ++i) {
            float v = 0.0f;
            if (d->refLeft > 0 && hz > 0.0) {
                const double fade =
                    double(d->refLeft) / double(d->refTotal);   // 1 -> 0
                d->refEnv += (1.0 - d->refEnv) * attack;
                // Squared so the tail thins out rather than stopping dead.
                const double amp = 0.22 * d->refEnv * fade * fade;
                v = float(amp * (std::sin(d->refPhase) +
                                 0.25 * std::sin(2.0 * d->refPhase) +
                                 0.12 * std::sin(3.0 * d->refPhase)));
                d->refPhase += step;
                if (d->refPhase > 2.0 * M_PI) d->refPhase -= 2.0 * M_PI;
                --d->refLeft;
            }
            dst[i * 2] = v;
            dst[i * 2 + 1] = v;
        }
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = static_cast<int32_t>(stride);
    buf->datas[0].chunk->size = frames * stride;
    pw_stream_queue_buffer(d->refStream, b);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const pw_stream_events kReferenceEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = onReferenceProcess,
};
#pragma GCC diagnostic pop

}  // namespace

bool Tuner::Impl::startReference(const std::string &targetSink) {
    if (refStream) return true;
    if (!core || targetSink.empty()) return false;

    auto *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, "waveline-tuner-reference",
        PW_KEY_NODE_DESCRIPTION, "Waveline tuner reference",
        PW_KEY_TARGET_OBJECT, targetSink.c_str(),
        // The Monitor mix or nothing: a reference note that lands on the
        // default sink because the mix was busy is a note in the wrong room.
        PW_KEY_NODE_DONT_RECONNECT, "true",
        nullptr);

    refStream = pw_stream_new(core, "waveline-tuner-reference", props);
    if (!refStream) return false;
    pw_stream_add_listener(refStream, &refListener, &kReferenceEvents, this);

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kRate;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *params[1] = {
        spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

    if (pw_stream_connect(refStream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                       PW_STREAM_FLAG_MAP_BUFFERS |
                                                       PW_STREAM_FLAG_RT_PROCESS),
                          params, 1) < 0) {
        pw_stream_destroy(refStream);
        refStream = nullptr;
        return false;
    }
    return true;
}

void Tuner::playReference(double hz, int ms, const std::string &targetSink) {
    if (!d_ || !d_->loop || hz <= 0.0) return;
    const int frames = std::clamp(ms, 100, 5000) * int(kRate) / 1000;

    pw_thread_loop_lock(d_->loop);
    const bool ok = d_->startReference(targetSink);
    pw_thread_loop_unlock(d_->loop);
    if (!ok) return;

    d_->refHz.store(hz, std::memory_order_relaxed);
    d_->refSamples.store(frames, std::memory_order_relaxed);
    // Last, and with release: the process callback reads this to decide that
    // everything above it is ready.
    d_->refTrigger.fetch_add(1, std::memory_order_release);
}

bool Tuner::Impl::startAudioTap(const std::string &nodeName, std::string &error) {
    auto *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, kNodeName,
        PW_KEY_NODE_DESCRIPTION, "Waveline tuner",
        PW_KEY_TARGET_OBJECT, nodeName.c_str(),
        // Same reason LevelProbe sets it: this is a measurement tap, and a
        // volume applet listing it as an application recording the user is
        // noise, not information.
        PW_KEY_STREAM_MONITOR, "true",
        nullptr);

    stream = pw_stream_new(core, kNodeName, props);
    if (!stream) {
        error = "pw_stream_new failed";
        return false;
    }
    pw_stream_add_listener(stream, &listener, &kAudioEvents, this);

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kRate;
    // Mono: a tuner has no use for a stereo image, and asking the server for
    // the downmix is cheaper and more correct than averaging whatever channel
    // layout a two-input interface happens to have.
    info.channels = 1;
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    const spa_pod *params[1] = {
        spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

    if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                       PW_STREAM_FLAG_MAP_BUFFERS |
                                                       PW_STREAM_FLAG_RT_PROCESS),
                          params, 1) < 0) {
        error = "pw_stream_connect failed";
        return false;
    }
    return true;
}

bool Tuner::Impl::startMidiTap(std::string &error) {
    // No target and no autoconnect: see the note on Tuner::midiNodeName().
    // The port is named here and linked by whoever knows which device the user
    // picked.
    auto *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Midi",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_MEDIA_CLASS, "Stream/Filter/Midi",
        PW_KEY_NODE_NAME, kNodeName,
        PW_KEY_NODE_DESCRIPTION, "Waveline tuner",
        PW_KEY_NODE_AUTOCONNECT, "false",
        "node.want-driver", "true",
        nullptr);

    filter = pw_filter_new_simple(pw_thread_loop_get_loop(loop), kNodeName, applyRealtimeProps(props),
                                  &kMidiEvents, this);
    if (!filter) {
        error = "pw_filter_new_simple failed";
        return false;
    }

    midiIn = static_cast<MidiPort *>(pw_filter_add_port(
        filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(MidiPort),
        pw_properties_new(PW_KEY_FORMAT_DSP, "8 bit raw midi", PW_KEY_PORT_NAME,
                          kMidiPort, nullptr),
        nullptr, 0));
    if (!midiIn) {
        error = "pw_filter_add_port failed";
        return false;
    }

    if (pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
        error = "pw_filter_connect failed";
        return false;
    }
    return true;
}

Tuner::Tuner() : d_(std::make_unique<Impl>()) {}
Tuner::~Tuner() { stop(); }

bool Tuner::active() const { return d_ && (d_->stream || d_->filter); }
const char *Tuner::midiNodeName() { return kNodeName; }
const char *Tuner::midiPortName() { return kMidiPort; }
std::string Tuner::sourceKind() const { return d_ ? d_->kind : std::string{}; }
std::string Tuner::source() const { return d_ ? d_->source : std::string{}; }

TunerReading Tuner::reading() const {
    std::lock_guard<std::mutex> lock(d_->resultMutex);
    return d_->result;
}

bool Tuner::start(const std::string &kind, const std::string &source, std::string &error) {
    // Changing source is a restart: one tap at a time, and tearing the old one
    // down first means never briefly metering two instruments at once.
    stop();

    if (source.empty()) {
        error = "no tuner source given";
        return false;
    }
    const bool midi = kind == "midi";

    // A different instrument in a different room: nothing learned about the
    // last one carries over, least of all its noise floor.
    d_->noiseFloor = 0.0;
    d_->votes.clear();
    d_->held = TunerReading{};
    d_->heldStamp = 0;
    d_->smoothed = 0.0;

    pw_init(nullptr, nullptr);
    d_->loop = pw_thread_loop_new("waveline-tuner", nullptr);
    if (!d_->loop) {
        error = "pw_thread_loop_new failed";
        return false;
    }

    pw_thread_loop_lock(d_->loop);
    d_->context = pw_context_new(pw_thread_loop_get_loop(d_->loop), realtimeContextProps(), 0);
    if (!d_->context) {
        error = "pw_context_new failed";
        pw_thread_loop_unlock(d_->loop);
        stop();
        return false;
    }
    d_->core = pw_context_connect(d_->context, nullptr, 0);
    if (!d_->core) {
        error = "pw_context_connect failed";
        pw_thread_loop_unlock(d_->loop);
        stop();
        return false;
    }

    const bool built =
        midi ? d_->startMidiTap(error) : d_->startAudioTap(source, error);
    pw_thread_loop_unlock(d_->loop);
    if (!built) {
        stop();
        return false;
    }

    if (pw_thread_loop_start(d_->loop) < 0) {
        error = "pw_thread_loop_start failed";
        stop();
        return false;
    }

    d_->kind = midi ? "midi" : "audio";
    d_->source = source;
    d_->midi = midi;
    d_->quit.store(false, std::memory_order_relaxed);
    d_->publish(TunerReading{});

    // The analysis runs here rather than in the process callback: a 370k
    // operation autocorrelation has no business on a real-time thread, and it
    // only needs to run about as often as a person can read a needle.
    Impl *impl = d_.get();
    d_->worker = std::thread([impl] {
        while (!impl->quit.load(std::memory_order_relaxed)) {
            impl->analyse();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    });
    return true;
}

void Tuner::stop() {
    if (!d_) return;

    d_->quit.store(true, std::memory_order_relaxed);
    if (d_->worker.joinable()) d_->worker.join();

    if (d_->loop) {
        pw_thread_loop_lock(d_->loop);
        if (d_->stream) {
            pw_stream_destroy(d_->stream);
            d_->stream = nullptr;
        }
        if (d_->refStream) {
            pw_stream_destroy(d_->refStream);
            d_->refStream = nullptr;
        }
        if (d_->filter) {
            pw_filter_destroy(d_->filter);
            d_->filter = nullptr;
        }
        d_->midiIn = nullptr;
        if (d_->core) {
            pw_core_disconnect(d_->core);
            d_->core = nullptr;
        }
        if (d_->context) {
            pw_context_destroy(d_->context);
            d_->context = nullptr;
        }
        pw_thread_loop_unlock(d_->loop);
        pw_thread_loop_stop(d_->loop);
        pw_thread_loop_destroy(d_->loop);
        d_->loop = nullptr;
    }

    d_->kind.clear();
    d_->source.clear();
    d_->midi = false;
    d_->writeLow.store(0, std::memory_order_relaxed);
    d_->writeHi.store(0, std::memory_order_relaxed);
    d_->decimPhase = 0;
    d_->channels = 1;
    d_->smoothed = 0.0;
    d_->midiNote.store(-1, std::memory_order_relaxed);
    d_->midiVelocity.store(0, std::memory_order_relaxed);
    d_->midiHeld.store(false, std::memory_order_relaxed);
    d_->midiBendCents.store(0.0, std::memory_order_relaxed);
    d_->dcBlock.reset();
    for (Biquad &f : d_->decimLp) f.reset();
    d_->publish(TunerReading{});
}

}  // namespace waveline

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "engine/alsadelay.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace waveline {

namespace {

// How many readings the rolling median runs over. At the daemon's 4 Hz poll
// this is about eight seconds of history: long enough that the median is not
// chasing the buffer's fill/drain cycle, short enough that unplugging a device
// or changing its settings shows up promptly rather than being averaged
// against a minute of the old value.
constexpr size_t kWindow = 32;

// Below this the median is a phase of the cycle rather than a latency.
constexpr size_t kMinSamples = 5;

// Reads a whole small procfs file. Returns false for anything unreadable,
// which is the normal case for a device that has just been unplugged and must
// not be logged about.
bool readFile(const std::string &path, std::string &out) {
    std::FILE *f = std::fopen(path.c_str(), "re");
    if (!f) return false;
    char buf[4096];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    out.assign(buf, n);
    return true;
}

// "delay      : 1146" -> 1146. procfs pads these to a column, so the value is
// not a fixed offset from the key.
bool field(const std::string &text, const char *key, long long &out) {
    const size_t klen = std::strlen(key);
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t eol = text.find('\n', pos);
        const size_t end = eol == std::string::npos ? text.size() : eol;
        if (text.compare(pos, klen, key) == 0) {
            size_t i = pos + klen;
            while (i < end && (text[i] == ' ' || text[i] == '\t')) ++i;
            if (i < end && text[i] == ':') {
                ++i;
                while (i < end && (text[i] == ' ' || text[i] == '\t')) ++i;
                char *stop = nullptr;
                const long long v = std::strtoll(text.c_str() + i, &stop, 10);
                if (stop && stop != text.c_str() + i) {
                    out = v;
                    return true;
                }
            }
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return false;
}

bool hasLine(const std::string &text, const char *prefix) {
    const size_t plen = std::strlen(prefix);
    size_t pos = 0;
    while (pos < text.size()) {
        if (text.compare(pos, plen, prefix) == 0) return true;
        const size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return false;
}

}  // namespace

AlsaPcmRef findCapturePcm(int card) {
    AlsaPcmRef ref;
    if (card < 0) return ref;
    // Capture is pcm0c on every device seen so far, but a card is free to put
    // it elsewhere, so the first few are tried rather than assumed. A card with
    // both a running and an idle capture PCM prefers the running one -- that is
    // the stream someone is actually listening to.
    AlsaPcmRef firstExisting;
    for (int dev = 0; dev < 8; ++dev) {
        char path[128];
        std::snprintf(path, sizeof(path),
                      "/proc/asound/card%d/pcm%dc/sub0/status", card, dev);
        std::string text;
        if (!readFile(path, text)) continue;
        AlsaPcmRef candidate;
        candidate.card = card;
        candidate.device = dev;
        if (hasLine(text, "state: RUNNING")) return candidate;
        if (!firstExisting.valid()) firstExisting = candidate;
    }
    return firstExisting;
}

void AlsaDelayProbe::beginSweep() {
    for (auto &[_, t] : tracks_) t.seen = false;
}

void AlsaDelayProbe::track(const AlsaPcmRef &ref) {
    if (!ref.valid()) return;
    auto [it, inserted] = tracks_.try_emplace(ref);
    Track &t = it->second;
    t.seen = true;
    if (inserted) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "/proc/asound/card%d/pcm%dc/sub%d/status", ref.card,
                      ref.device, ref.subdevice);
        t.statusPath = buf;
        std::snprintf(buf, sizeof(buf),
                      "/proc/asound/card%d/pcm%dc/sub%d/hw_params", ref.card,
                      ref.device, ref.subdevice);
        t.hwParamsPath = buf;
    }
}

void AlsaDelayProbe::forgetUntracked() {
    for (auto it = tracks_.begin(); it != tracks_.end();)
        it = it->second.seen ? std::next(it) : tracks_.erase(it);
}

void AlsaDelayProbe::refreshParams(Track &t) const {
    std::string text;
    if (!readFile(t.hwParamsPath, text)) return;
    // A closed stream's hw_params reads "closed"; leaving the previous values
    // in place means a device that is briefly reopened does not lose its rate
    // and start reporting nothing.
    long long v = 0;
    if (field(text, "rate", v) && v > 0) t.rate = static_cast<int>(v);
    if (field(text, "period_size", v) && v > 0) t.periodSize = static_cast<int>(v);
    if (field(text, "buffer_size", v) && v > 0) t.bufferSize = static_cast<int>(v);
    t.haveParams = t.rate > 0;
}

void AlsaDelayProbe::sample() {
    for (auto &[ref, t] : tracks_) {
        std::string text;
        if (!readFile(t.statusPath, text)) {
            // Unplugged, or a card that renumbered under us. Drop the history
            // rather than keep answering with it.
            t.running = false;
            t.window.clear();
            t.haveParams = false;
            continue;
        }

        const bool running = hasLine(text, "state: RUNNING");
        if (!running) {
            // Between RUNNING states the buffer is not a latency. Clearing
            // also means a stream that comes back with different settings is
            // not averaged against the settings it had before.
            if (t.running) {
                t.window.clear();
                t.haveParams = false;
            }
            t.running = false;
            continue;
        }
        t.running = true;

        if (!t.haveParams) refreshParams(t);
        if (t.rate <= 0) continue;

        long long delay = 0;
        if (!field(text, "delay", delay)) continue;

        // Plausibility. Two real failures, both observed:
        //
        //   delay <= 0        the sc0710 reports 0 while claiming RUNNING.
        //   delay > buffer    a stale or garbage read; nothing can be further
        //                     behind than the buffer it lives in.
        //
        // Rejected rather than clamped: a clamped garbage reading still moves
        // the median, and the count is worth showing in diagnostics because a
        // device rejecting everything is a device we cannot measure, which is
        // a different thing from a device with no latency.
        const long long limit = t.bufferSize > 0 ? t.bufferSize
                                                 : static_cast<long long>(t.rate);
        if (delay <= 0 || delay > limit) {
            if (t.rejected < 1000000) ++t.rejected;
            continue;
        }

        t.window.push_back(delay);
        if (t.window.size() > kWindow) t.window.pop_front();
    }
}

int64_t AlsaDelayProbe::medianUs(const AlsaPcmRef &ref) const {
    const auto it = tracks_.find(ref);
    if (it == tracks_.end()) return kUnknown;
    const Track &t = it->second;
    if (t.rate <= 0 || t.window.size() < kMinSamples) return kUnknown;
    std::vector<int64_t> v(t.window.begin(), t.window.end());
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    const int64_t frames = v[mid];
    return frames * 1000000 / t.rate;
}

AlsaDelayProbe::Detail AlsaDelayProbe::detail(const AlsaPcmRef &ref) const {
    Detail d;
    const auto it = tracks_.find(ref);
    if (it == tracks_.end()) return d;
    const Track &t = it->second;
    d.running = t.running;
    d.rate = t.rate;
    d.periodSize = t.periodSize;
    d.bufferSize = t.bufferSize;
    d.samples = static_cast<int>(t.window.size());
    d.rejected = t.rejected;
    if (t.rate > 0 && !t.window.empty()) {
        const auto [lo, hi] = std::minmax_element(t.window.begin(), t.window.end());
        d.minUs = *lo * 1000000 / t.rate;
        d.maxUs = *hi * 1000000 / t.rate;
    }
    d.medianUs = medianUs(ref);
    return d;
}

}  // namespace waveline

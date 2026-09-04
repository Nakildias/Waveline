// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The Soundboard's playback engine: decodes a .wav/.mp3 file once, then plays
// as many overlapping instances of it as get triggered, each its own ephemeral
// PipeWire filter node with up to two independent stereo outputs -- one for
// what the channel (and you) hear, one for what joins a shared microphone --
// so a sound's "how loud for me" and "how loud in the mic" sliders are as
// independent as the same two controls already are for applications in
// mixerservice.cpp's linkSoundShareNode(). Neither output is auto-connected;
// callers link both ends with PwEngine::linkPorts() once the node's ports
// exist, the same way every other filter in this graph is wired.
//
// A voice is deliberately not folded into the generic app-routing machinery
// (AppRouter/SoundShareRouter/AppGainStage): those exist for long-lived,
// unpredictable third-party streams identified by name, and reusing them
// would mean every soundboard press wrote into the general per-application
// profile state. A voice is instead a self-contained, explicitly configured
// thing that lives for the seconds it takes to play, named with the
// "waveline-" prefix so it is automatically excluded from the Apps and Sound
// Sharing tabs the same way every other node this daemon creates for itself
// already is (see mergedPlaybackApps() and SoundShareRouter::routeNode()).

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace waveline {

class PwEngine;

// Decoded audio, always resampled to 48 kHz interleaved stereo float --
// the graph's own rate, so a voice never has to resample live.
struct SoundboardBuffer {
    std::vector<float> samples;  // L,R,L,R... length == frames*2
    uint32_t frames = 0;

    bool valid() const { return frames > 0; }
    double durationMs() const { return frames * 1000.0 / 48000.0; }
};

// Decodes a .wav or .mp3 file (by extension) into 48 kHz stereo. Empty
// result and a filled `error` on anything that goes wrong -- a missing file,
// an unsupported format, a corrupt one.
SoundboardBuffer decodeSoundFile(const std::string &path, std::string &error);

// `count` peak magnitudes (0..1), evenly spanning [startFrame, endFrame) --
// the whole buffer by default (endFrame 0 means "to the end", the same
// convention SoundboardPlaySpec::trimEndFrames uses) -- for a waveform
// display. Cheap enough to hand to a client as a CSV string.
//
// Callers that want the whole file regardless of any trim (the trim editor
// itself, which has to show the parts being cut as well as the parts kept)
// pass no range at all; MixerService::soundboardPeaksFor(), which is what
// the Soundboard panel's mini waveform is drawn from, passes the sound's
// current trim -- showing the parts that were cut there would just be
// showing an audience of one thing the button no longer plays.
std::vector<float> soundboardPeaks(const SoundboardBuffer &buf, int count,
                                   uint32_t startFrame = 0, uint32_t endFrame = 0);

struct SoundboardPlaySpec {
    // A channel's sink, e.g. "waveline-ch-voice" or the default
    // "waveline-ch-system" -- always stereo, always linked.
    std::string localTarget;
    float localGain = 1.0f;  // sound volume x "what I hear" volume

    // Where the shared copy goes; empty means the sound is not shared into a
    // microphone at all, and no share ports are created.
    std::string shareTarget;
    // True when shareTarget is a mono microphone bus (masterSourceNode(),
    // one "input_MONO" port); false for a channel's own published mic
    // (stereo, "input_FL"/"input_FR") -- see linkSoundShareNode() for the
    // same distinction on the application-sharing side.
    bool shareTargetMono = false;
    float shareGain = 1.0f;  // sound volume x "audio sharing" volume

    uint32_t trimStartFrames = 0;
    uint32_t trimEndFrames = 0;  // exclusive; 0 or >frames means "to the end"

    std::string nodeName;    // must be unique per voice
    std::string description;
};

// One playing instance: its own pw_filter and thread loop, reading from a
// shared decoded buffer that outlives it. Created by SoundboardEngine::play()
// and destroyed once playback runs past the trimmed end or stop() is called.
class SoundboardVoice {
public:
    SoundboardVoice();
    ~SoundboardVoice();
    SoundboardVoice(const SoundboardVoice &) = delete;
    SoundboardVoice &operator=(const SoundboardVoice &) = delete;

    bool start(PwEngine &engine, std::shared_ptr<const SoundboardBuffer> buffer,
              const SoundboardPlaySpec &spec, std::string &error);
    void stop();

    // Past the end of its trimmed range. Set by the audio thread on its way
    // out; safe to poll from the Qt thread that owns the reap timer. A plain
    // member rather than something inside Impl: stop() always halts the
    // PipeWire thread before Impl is torn down, so nothing outlives it, and
    // finished() must keep answering even after stop() has run.
    bool finished() const { return finished_.load(std::memory_order_acquire); }
    // How far through the trimmed range playback has got, 0..1. Reads the
    // same cursor the audio thread advances every cycle, so this is a live
    // position, not a snapshot from when playback started.
    double progress() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
    std::atomic<bool> finished_{false};
};

// Owns every currently-playing voice, for every sound, for the life of the
// daemon. One instance; MixerService polls reap() on the same kind of timer
// it already uses for hardware, since a filter has no "I am done" signal
// that crosses back to the Qt thread on its own.
class SoundboardEngine {
public:
    explicit SoundboardEngine(PwEngine &engine);
    ~SoundboardEngine();
    SoundboardEngine(const SoundboardEngine &) = delete;
    SoundboardEngine &operator=(const SoundboardEngine &) = delete;

    // Starts one more voice playing `buffer` under `soundId`. Sounds can
    // overlap themselves and each other; each call gets its own voice.
    bool play(const std::string &soundId, std::shared_ptr<const SoundboardBuffer> buffer,
              SoundboardPlaySpec spec, std::string &error);
    // Stops every voice currently playing this sound id.
    void stop(const std::string &soundId);
    void stopAll();
    // Sound ids with at least one voice still playing right now.
    std::vector<std::string> playing() const;
    // How far through its trimmed range the given sound's (first found)
    // voice has got, 0..1, or -1 when it is not currently playing. When a
    // sound overlaps itself this reports whichever instance the map happens
    // to hand back first -- good enough for a glance-sized progress bar,
    // which was never going to track more than one instance at a time.
    double progress(const std::string &soundId) const;
    // Drops voices that finished on their own. Returns true when the playing
    // set changed, so the caller knows whether to tell its own clients.
    bool reap();

private:
    PwEngine *engine_;
    mutable std::mutex mutex_;
    std::multimap<std::string, std::unique_ptr<SoundboardVoice>> voices_;
    uint64_t nextSeq_ = 0;
};

}  // namespace waveline

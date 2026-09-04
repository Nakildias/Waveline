// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Anti-Pop: ramp application playback streams instead of switching them on and
// off at full scale.
//
// A stream that starts mid-waveform steps the mix from silence to whatever
// sample it happened to begin on. That step is a click, and it is loudest for
// exactly the material people notice -- a game or a soundboard hit starting on
// a peak. The same step happens in reverse when Waveline moves a stream from
// one channel to another: the old link is torn down and the new one comes up
// somewhere else in the waveform.
//
// Nothing here touches the audio thread. An application stream's level is a
// node property, so the ramp is a series of channelVolumes writes on a 10 ms
// timer, replacing one full-scale step with a handful of small ones.
//
// What this cannot do, and no setting will change: fade out when the
// *application* stops. The app writes its last sample and drops the stream in
// the same breath, and nothing here gets to run in between -- there is no
// moment at which a fade could start. Only a look-ahead delay on every channel
// could hide that edge, at the cost of latency on all app audio. What is
// covered is every edge Waveline itself is responsible for: a stream starting,
// and a stream being moved from one channel to another.

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <cstdint>
#include <functional>

namespace waveline {
class PwEngine;
}

class AntiPopFader : public QObject {
    Q_OBJECT

public:
    explicit AntiPopFader(waveline::PwEngine &engine, QObject *parent = nullptr);
    ~AntiPopFader() override;

    bool enabled() const { return enabled_; }
    // Turning this off finishes every ramp in flight at its target level and
    // runs any move that was waiting on one, so a stream can never be stranded
    // at zero by the switch itself.
    void setEnabled(bool on);

    int fadeMs() const { return fadeMs_; }
    void setFadeMs(int ms);

    // A stream that has just appeared. PwEngine has already stamped it to
    // silence, so this only has to ramp it up -- and must always be called for
    // a new stream while enabled, or it stays silent.
    //
    // Fires more than once per stream (PipeWire re-announces a node once its
    // process metadata resolves); only the first call ramps, the rest simply
    // apply the level.
    void fadeIn(uint32_t nodeId, float target);

    // Ramp `nodeIds` down, run `move` once they are all silent, then ramp them
    // back to the level `target` reports for each. With anti-pop off, `move`
    // runs immediately. `move` runs on this object's thread.
    void fadeThroughMove(const QList<uint32_t> &nodeIds,
                         std::function<float(uint32_t)> target,
                         std::function<void()> move);

    // The volume write for an application stream. Retargets a ramp in flight
    // rather than stamping over it -- a fader move during a fade-in must
    // change where the fade is going, not cancel it.
    void applyVolume(uint32_t nodeId, float target);

    // The stream is gone. Drops its ramp and releases any move waiting on it.
    void forget(uint32_t nodeId);

    // Land every ramp on its target now, and let any move waiting on one
    // through. Call before the daemon exits: WirePlumber remembers the last
    // volume it saw on an application's stream and restores it the next time
    // that application plays, so a daemon that stops half way down a fade would
    // leave the app quiet -- or silent -- with nothing on screen to explain it.
    void finish();

private:
    struct Ramp {
        float from = 0.0f;
        float to = 0.0f;
        int elapsedMs = 0;
        int durationMs = 0;
        int group = 0;  // 0 = not part of a move
    };

    struct Group {
        int remaining = 0;
        std::function<void()> onSilent;
    };

    // Fine enough to put a volume in front of every quantum, coarse enough
    // that a handful of streams cost nothing. PipeWire consumes at most one
    // per quantum, so ticking faster than this buys nothing.
    static constexpr int kTickMs = 10;

    // How long to stay silent after moving a stream, before ramping it back up.
    // A move is a metadata write the session manager acts on; the stream is not
    // attached to its new channel for a few tens of milliseconds afterwards, and
    // ramping through that window means the first part of the fade plays into
    // nothing and the channel hears the stream arrive part-way up -- which is a
    // smaller version of the step being removed. Measured at ~65 ms here, so
    // this has room over it.
    static constexpr int kMoveSettleMs = 120;

    void tick();
    void runSettle(int token);
    void start(uint32_t nodeId, float from, float to, int group);
    void write(uint32_t nodeId, float volume);
    // Runs a group's move and starts the ramps back up. Also the path taken
    // when the last stream a group was waiting on disappears.
    void releaseGroup(int group);
    void leaveGroup(int group);

    waveline::PwEngine &engine_;
    QTimer timer_;
    QHash<uint32_t, Ramp> ramps_;
    QHash<int, Group> groups_;
    // The second half of a move, waiting out kMoveSettleMs. Held here as well
    // as on its timer so finish() can run it early.
    QHash<int, std::function<void()>> settling_;
    int nextSettle_ = 1;
    // Streams already faded in, so a re-announce does not restart the ramp and
    // dip audio that is already playing.
    QSet<uint32_t> faded_;
    int nextGroup_ = 1;
    int fadeMs_ = 150;
    bool enabled_ = false;
};

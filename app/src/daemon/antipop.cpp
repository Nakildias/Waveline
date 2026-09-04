// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "antipop.h"

#include "engine/pwengine.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace {

// Straight line, deliberately, and not the raised cosine an audio fade would
// normally use.
//
// PipeWire applies a node volume once per graph quantum -- 1024 samples on a
// stock desktop, 21 ms -- with no interpolation inside the buffer. A fade is
// therefore not a curve but a staircase of fadeMs/quantum steps however often
// this ticks, and what is left of the click is the largest single step. A
// raised cosine peaks at pi/2 times the average slope, so it makes that step
// 57% larger than it has to be; a straight line spreads the travel evenly and
// is the shape with the smallest possible worst step for a given number of
// them.
//
// Consequence worth knowing: the artefact shrinks with fade length, because a
// longer fade spans more quanta. scripts/antipop-test.sh measures it.
float curve(float t) { return t; }

}  // namespace

AntiPopFader::AntiPopFader(waveline::PwEngine &engine, QObject *parent)
    : QObject(parent), engine_(engine) {
    timer_.setInterval(kTickMs);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &AntiPopFader::tick);
}

AntiPopFader::~AntiPopFader() = default;

void AntiPopFader::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    // The engine flag moves with enabled_, before anything else either way: a
    // stream must never be silenced by the registry callback while this object
    // is in a state that would not ramp it back up.
    engine_.setSilenceNewStreams(on);
    if (on) return;
    finish();
    faded_.clear();
}

void AntiPopFader::finish() {
    // Land every ramp on its target, let every pending move through, and bring
    // forward the settle timers that carry a move's second half -- otherwise a
    // stream is left at whatever level the fade had reached. Each of those
    // starts fresh ramps, so this repeats until nothing is left in flight.
    while (!ramps_.isEmpty() || !settling_.isEmpty()) {
        for (uint32_t nodeId : ramps_.keys()) {
            auto it = ramps_.find(nodeId);
            if (it == ramps_.end()) continue;
            const Ramp r = *it;
            ramps_.erase(it);
            write(nodeId, r.to);
            if (r.group) leaveGroup(r.group);
        }
        for (int token : settling_.keys()) runSettle(token);
    }
    groups_.clear();
    timer_.stop();
}

void AntiPopFader::runSettle(int token) {
    auto it = settling_.find(token);
    if (it == settling_.end()) return;
    const std::function<void()> raise = *it;
    settling_.erase(it);
    if (raise) raise();
}

void AntiPopFader::setFadeMs(int ms) { fadeMs_ = std::clamp(ms, 10, 2000); }

void AntiPopFader::fadeIn(uint32_t nodeId, float target) {
    if (!nodeId) return;
    if (!enabled_) {
        write(nodeId, target);
        return;
    }
    // Already up, or already on its way up. Re-announces of the same node must
    // not restart the ramp and dip audio that is playing.
    if (faded_.contains(nodeId)) {
        applyVolume(nodeId, target);
        return;
    }
    faded_.insert(nodeId);
    start(nodeId, 0.0f, target, 0);
}

void AntiPopFader::fadeThroughMove(const QList<uint32_t> &nodeIds,
                                   std::function<float(uint32_t)> target,
                                   std::function<void()> move) {
    QList<uint32_t> ids;
    for (uint32_t nodeId : nodeIds)
        if (nodeId) ids.append(nodeId);

    if (!enabled_ || ids.isEmpty()) {
        if (move) move();
        return;
    }

    const int group = nextGroup_++;
    Group g;
    g.remaining = 0;
    // `target` is called again after the move rather than captured now: the
    // level may have been changed while the fade-out was running, and the level
    // belongs to the application, not to the channel it is being moved to.
    g.onSilent = [this, ids, target, move = std::move(move)] {
        if (move) move();
        // Parked in settling_ as well as on the timer, so that a shutdown or a
        // switch-off during the settle can run it early rather than leave the
        // streams sitting at zero.
        const int token = nextSettle_++;
        settling_.insert(token, [this, ids, target] {
            for (uint32_t nodeId : ids)
                start(nodeId, 0.0f, target ? target(nodeId) : 1.0f, 0);
        });
        QTimer::singleShot(kMoveSettleMs, this, [this, token] { runSettle(token); });
    };
    groups_.insert(group, std::move(g));

    for (uint32_t nodeId : ids) {
        // From wherever the stream currently is, so a move landing during a
        // fade-in does not jump to full scale before fading out.
        const float from = ramps_.contains(nodeId)
                               ? ramps_.value(nodeId).to
                               : (target ? target(nodeId) : 1.0f);
        start(nodeId, from, 0.0f, group);
    }
}

void AntiPopFader::applyVolume(uint32_t nodeId, float target) {
    if (!nodeId) return;
    auto it = ramps_.find(nodeId);
    if (it == ramps_.end()) {
        write(nodeId, target);
        return;
    }
    // A ramp on its way to silence is clearing the way for a move; retargeting
    // it would defeat the move. The new level is picked up on the way back up,
    // which re-reads it.
    if (it->group) return;
    // Retarget in place: reset the clock but start from where the ramp actually
    // is, so changing the level mid-fade does not step it.
    const float t = it->durationMs > 0
                        ? std::clamp(float(it->elapsedMs) / float(it->durationMs),
                                     0.0f, 1.0f)
                        : 1.0f;
    it->from = it->from + (it->to - it->from) * curve(t);
    it->to = target;
    it->elapsedMs = 0;
}

void AntiPopFader::forget(uint32_t nodeId) {
    faded_.remove(nodeId);
    auto it = ramps_.find(nodeId);
    if (it == ramps_.end()) return;
    const int group = it->group;
    ramps_.erase(it);
    if (ramps_.isEmpty()) timer_.stop();
    // The stream this group was waiting on is gone -- silent in the only sense
    // that matters -- so let the move through rather than stalling it forever.
    if (group) leaveGroup(group);
}

void AntiPopFader::start(uint32_t nodeId, float from, float to, int group) {
    Ramp r;
    r.from = from;
    r.to = to;
    r.elapsedMs = 0;
    r.durationMs = fadeMs_;
    r.group = group;

    // Replacing a ramp that belonged to another group would leave that group
    // waiting on a stream nobody is ramping any more.
    int oldGroup = 0;
    if (auto old = ramps_.constFind(nodeId); old != ramps_.constEnd())
        oldGroup = old->group;
    ramps_.insert(nodeId, r);

    if (group) {
        if (auto g = groups_.find(group); g != groups_.end()) g->remaining++;
    }
    if (oldGroup && oldGroup != group) leaveGroup(oldGroup);

    write(nodeId, from);
    if (!timer_.isActive()) timer_.start();
}

void AntiPopFader::write(uint32_t nodeId, float volume) {
    engine_.setNodeVolumeById(nodeId, std::max(0.0f, volume), false, 2);
}

void AntiPopFader::leaveGroup(int group) {
    auto it = groups_.find(group);
    if (it == groups_.end()) return;
    if (--it->remaining > 0) return;
    releaseGroup(group);
}

void AntiPopFader::releaseGroup(int group) {
    auto it = groups_.find(group);
    if (it == groups_.end()) return;
    // Taken out of the map before running: onSilent starts the ramps back up,
    // and those must not be able to re-enter a group that is finishing.
    const Group g = *it;
    groups_.erase(it);
    if (g.onSilent) g.onSilent();
}

void AntiPopFader::tick() {
    QList<int> released;

    for (auto it = ramps_.begin(); it != ramps_.end();) {
        Ramp &r = *it;
        r.elapsedMs += kTickMs;
        const float t = r.durationMs > 0
                            ? std::clamp(float(r.elapsedMs) / float(r.durationMs),
                                         0.0f, 1.0f)
                            : 1.0f;
        write(it.key(), r.from + (r.to - r.from) * curve(t));
        if (t < 1.0f) {
            ++it;
            continue;
        }
        const int group = r.group;
        it = ramps_.erase(it);
        if (group) {
            auto g = groups_.find(group);
            if (g != groups_.end() && --g->remaining <= 0) released.append(group);
        }
    }

    // After the loop: a released group starts fresh ramps, and mutating ramps_
    // while iterating it would invalidate the iterator.
    for (int group : released) releaseGroup(group);
    if (ramps_.isEmpty()) timer_.stop();
}

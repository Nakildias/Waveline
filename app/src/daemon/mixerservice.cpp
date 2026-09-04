// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "mixerservice.h"

#include "desktopnames.h"

#include "alsaaliases.h"
#include "wpheadroom.h"
#include "engine/appidentity.h"
#include "engine/creativefxspec.h"
#include "engine/dspprobe.h"
#include "engine/masterbus.h"
#include "engine/rtsched.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

#include <map>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include <QDBusConnectionInterface>
#include <QThread>

#include <cmath>
#include <memory>
#include <unistd.h>

MixerService::MixerService(QObject *parent) : QObject(parent) {
    // A desktop shell renamed a device. Nothing in the graph moved, but every
    // client showing that device is showing the old name, so this is a Changed()
    // like any other. Costs nothing on a machine with no such shell: the file
    // never appears and this never fires.
    connect(&waveline::DesktopNames::instance(), &waveline::DesktopNames::changed, this,
            [this] { emit Changed(); });

    // The hardware is polled because these devices report nothing on their own:
    // tapping the mute pad or turning a dial sends no USB status message. See
    // docs/protocol.md. 4 Hz is enough to feel responsive and stays far below
    // the endpoint-0 traffic a busier poller would generate.
    hwTimer_.setInterval(250);
    connect(&hwTimer_, &QTimer::timeout, this, &MixerService::pollHardware);
    // A second is well inside "nobody noticed" for an unplug and cheap enough
    // to leave running: the sweep is a stat() per hardware node.
    deadCardTimer_.setInterval(1000);
    connect(&deadCardTimer_, &QTimer::timeout, this,
            &MixerService::sweepDeadHardwareNodes);

    // Waking from sleep. See onPrepareForSleep() for why this is needed at all
    // -- in short, every capture device is reopened underneath a graph that is
    // never told, and until now the only thing that fixed it was the user
    // pressing Rebuild.
    //
    // Not fatal if it does not connect: a machine with no logind, or a
    // container without the system bus, simply keeps the old behaviour.
    if (!QDBusConnection::systemBus().connect(
            QStringLiteral("org.freedesktop.login1"),
            QStringLiteral("/org/freedesktop/login1"),
            QStringLiteral("org.freedesktop.login1.Manager"),
            QStringLiteral("PrepareForSleep"), this,
            SLOT(onPrepareForSleep(bool)))) {
        qWarning("waveline: no logind PrepareForSleep signal; capture devices "
                 "will not be rebuilt automatically after resume");
    }

    // Debounced: dragging a fader emits Changed() continuously, and rewriting
    // the config on every pixel would be pointless disk churn.
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(1500);
    connect(&saveTimer_, &QTimer::timeout, this, [this] {
        captureToProfile();
        if (!config_.save()) lastError_ = config_.lastError();
    });

    rewireTimer_.setSingleShot(true);
    rewireTimer_.setInterval(800);
    connect(&rewireTimer_, &QTimer::timeout, this, &MixerService::rewireGraph);

    // Recording streams appear in bursts -- a browser joining a call creates
    // several inside a few hundred milliseconds -- and the set often nets out
    // unchanged, because a stream that restarts is a remove and an add. Both
    // are handled here: coalesce the burst, then compare, and stay silent when
    // nothing a client would draw differently actually moved.
    micConsumerTimer_.setSingleShot(true);
    micConsumerTimer_.setInterval(250);
    connect(&micConsumerTimer_, &QTimer::timeout, this, [this] {
        QSet<uint> now;
        for (const QString &row : MicrophoneConsumers())
            now.insert(row.section(QLatin1Char('\t'), 0, 0).toUInt());
        if (now == lastMicConsumers_) return;
        lastMicConsumers_ = now;
        emit MicrophoneConsumersChanged();
    });

    // Which desktop shell is on the bus changes nothing about how the mixer
    // behaves; it is reported so a client can say so, and so a shell that
    // starts after the daemon is noticed without either side polling.
    shellWatcher_ = std::make_unique<QDBusServiceWatcher>(
        QStringLiteral("org.glassbar.Settings"), QDBusConnection::sessionBus(),
        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(shellWatcher_.get(), &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &, const QString &newOwner) {
                const bool present = !newOwner.isEmpty();
                if (shellPresent_ == present) return;
                shellPresent_ = present;
                emit Changed();
            });

    // Monitor output levels, re-pushed after their loopbacks were rebuilt.
    //
    // A loopback plays at unity from the moment it loads until a level is
    // pushed onto it, and the push finds nodes by name -- so a path recreated
    // under a name that was destroyed moments earlier writes to the outgoing
    // node and leaves the one now playing at full scale. Restarting WirePlumber
    // takes every ALSA sink away and brings it back, which does that to every
    // Monitor output at once: headphones at 17% become headphones at 100%.
    //
    // Several passes because there is no event for "the replacement node is the
    // one the registry now resolves"; the ladder outlasts it. Cheap enough to
    // be worth repeating -- one Props write per output, no core sync.
    monitorLevelTimer_.setSingleShot(true);
    connect(&monitorLevelTimer_, &QTimer::timeout, this, [this] {
        if (!graph_) return;
        graph_->applyAllMonitorOutputGains();
        if (++monitorLevelPasses_ < 3) {
            monitorLevelTimer_.start(700 * monitorLevelPasses_);
        }
    });

    midiWireTimer_.setInterval(1000);
    connect(&midiWireTimer_, &QTimer::timeout, this, [this] {
        if (!graph_) return;
        std::string audioErr;
        const bool audioReady = graph_->linkPendingMidiAudioPaths(audioErr);

        // Order matters, and it is the whole reason a MIDI device survives a
        // warm restart but not a cold boot. PipeWire exposes ALSA sequencer
        // hardware through a bridge node that is itself a driver
        // (node.driver = true, priority.driver = 1). Attaching it to the synth
        // pulls the synth -- and every filter directly downstream that is not
        // already anchored elsewhere -- into the *bridge's* scheduling group.
        // The chain then has to hop from that group back into the mixer's, and
        // PipeWire cannot schedule a link across driver groups, so it sits in
        // "init" forever: `waveline-master-1-fx:output ->
        // waveline-master-1-creative:input` never activates.
        //
        // On a warm restart the mixer graph is already established, so the
        // chain is anchored before the bridge ever attaches and nothing moves.
        // On a cold boot the bridge is there first and wins. So: never attach
        // the bridge until this bus's audio chain is wired and active.
        if (!audioReady) {
            // Detach once, on the tick that first notices, so the chain is free
            // to settle into the mixer's group. Repeating it every second only
            // churns links, and this timer runs for the life of the daemon.
            //
            // Deliberately no chain rebuild here. An earlier version tore the
            // chain down after a few failed ticks on the theory that a captured
            // node never migrates back. That was the wrong read: the failures
            // this timer actually sees come from the PipeWire server running
            // out of file descriptors, and destroying and recreating six filter
            // nodes -- each its own client connection -- spends more of exactly
            // the resource that has run out. Retry, log, and leave the graph
            // alone; a chain that never comes up is reported below, not
            // hammered.
            if (midiWireStalls_ == 0) {
                for (const MasterBusState &m : config_.live().masterBuses) {
                    if (m.busType != QLatin1String("midi")) continue;
                    graph_->invalidateMasterMidiInput(m.id.toStdString());
                }
            }
            // Once when it starts, then every 30s, rather than a line a second
            // for as long as the condition lasts.
            if (!audioErr.empty() && midiWireStalls_ % 30 == 0) {
                qWarning("waveline: MIDI audio not wired (%ds): %s",
                         midiWireStalls_, audioErr.c_str());
                if (midiWireStalls_ > 0)
                    warnIfDescriptorsExhausted();
            }
            ++midiWireStalls_;
            return;
        }

        if (midiWireStalls_ > 0)
            qInfo("waveline: MIDI audio wired after %ds", midiWireStalls_);
        midiWireStalls_ = 0;
        healDisconnectedMidiInputs();
        std::string err;
        if (!graph_->linkPendingMidiInputs(err) && !err.empty())
            qInfo("waveline: MIDI input pending: %s", err.c_str());
    });

    // After cold wire / USB appear: disconnect that master's ALSA hop, wait
    // quiet, then one full DSP recreate (second recreate was poisoning good hops).
    captureSettleTimer_.setSingleShot(true);
    captureSettleTimer_.setInterval(1500);
    connect(&captureSettleTimer_, &QTimer::timeout, this,
            &MixerService::rebuildCaptureHops);

    routeRetryTimer_.setSingleShot(true);
    routeRetryTimer_.setInterval(400);
    connect(&routeRetryTimer_, &QTimer::timeout, this, [this] {
        if (!routing_ || !router_ || routeRetryLeft_ <= 0) return;
        router_->routeAll();
        if (--routeRetryLeft_ > 0) routeRetryTimer_.start();
    });

    // Watches where streams ended up, and never moves one. Kept separate from
    // the retry ladder above for exactly that reason: the moment a detector
    // starts re-asserting routing, two programs are writing the same
    // target.object key in a loop and the user hears every move. The router
    // still gives up after its four tries; this only makes it say why.
    routeVerifyTimer_.setSingleShot(true);
    routeVerifyTimer_.setInterval(500);
    connect(&routeVerifyTimer_, &QTimer::timeout, this, [this] {
        if (!routing_ || !router_ || routeVerifyLeft_ <= 0) return;
        recordRouteBounces();
        if (--routeVerifyLeft_ > 0)
            routeVerifyTimer_.start();
        else
            settleRouteConflicts();
    });

    // A soundboard voice has no signal that crosses back to this thread when
    // it runs out of audio -- filterProcess() only sets an atomic flag from
    // the PipeWire thread it runs on. This is what actually destroys the
    // voice once that flag is up, the same role hwTimer_ plays for hardware
    // that cannot push its own state either.
    soundboardReapTimer_.setInterval(150);
    connect(&soundboardReapTimer_, &QTimer::timeout, this, [this] {
        if (soundboard_ && soundboard_->reap()) emit Changed();
    });
}

MixerService::~MixerService() = default;

void MixerService::muteOutputsForShutdown() {
    Profile &p = config_.live();
    QStringList muted;
    for (const MonitorOutputState &out : p.monitorOutputs) {
        if (out.sink.isEmpty()) continue;
        // An output already muted in the mixer is feeding its device nothing,
        // so there is nothing to protect anyone from and no reason to touch a
        // device the user may have left as they want it.
        if (out.muted) continue;
        if (!engine_.setNodeMuted(out.sink.toStdString(), true)) continue;
        muted << out.sink;
        qInfo("waveline: muted %s for shutdown", qUtf8Printable(out.sink));
    }
    p.sinksMutedAtStop = muted;
    // Straight to disk, not through the debounce: there may be no next tick.
    flushPendingSave();
}

void MixerService::restoreOutputsMutedAtShutdown() {
    Profile &p = config_.live();
    if (p.sinksMutedAtStop.isEmpty()) return;
    for (const QString &sink : p.sinksMutedAtStop) {
        if (engine_.setNodeMuted(sink.toStdString(), false))
            qInfo("waveline: unmuted %s after startup", qUtf8Printable(sink));
        else
            qWarning("waveline: could not unmute %s -- device not present",
                     qUtf8Printable(sink));
    }
    // Cleared either way. A device that is not here now is one this daemon can
    // do nothing about, and keeping the entry would unmute it at some later
    // start for no reason anybody could follow.
    p.sinksMutedAtStop.clear();
    scheduleSave();
}


waveline::Mix MixerService::parseMix(const QString &mix) {
    return mix.compare(QLatin1String("monitor"), Qt::CaseInsensitive) == 0
               ? waveline::Mix::Monitor
               : waveline::Mix::Stream;
}

waveline::FxStage MixerService::parseFxStage(const QString &stage) {
    return stage.compare(QLatin1String("output"), Qt::CaseInsensitive) == 0
               ? waveline::FxStage::Output
               : waveline::FxStage::Input;
}

namespace {

const MasterBusState *findMasterBus(const Profile &p, const QString &id) {
    for (const MasterBusState &m : p.masterBuses)
        if (m.id == id) return &m;
    return nullptr;
}

QString resolveMasterBusId(const Profile &p, const QString &id) {
    if (id.isEmpty()) return {};
    return findMasterBus(p, id) ? id : QStringLiteral("mic");
}

// Drops ids no bus answers to and duplicates, keeping the user's order. Never
// empty: a channel microphone with no input device would publish silence.
QStringList resolveMasterMicIds(const Profile &p, const QStringList &ids) {
    QStringList out;
    for (const QString &id : ids) {
        if (id.isEmpty() || out.contains(id) || !findMasterBus(p, id)) continue;
        out << id;
    }
    if (out.isEmpty()) out << QStringLiteral("mic");
    return out;
}

std::vector<std::string> toStdIds(const QStringList &ids) {
    std::vector<std::string> out;
    out.reserve(ids.size());
    for (const QString &id : ids) out.push_back(id.toStdString());
    return out;
}

ChannelFxStageState masterMicStage(const MasterBusState &m) {
    ChannelFxStageState s;
    s.fx = m.micFx;
    s.dynamics = m.micDynamics;
    s.creativeFx = m.micCreativeFx;
    s.noiseSuppression = m.noiseSuppression;
    s.noiseIntensity = m.noiseIntensity;
    s.deEsser = m.deEsser;
    s.deEsserIntensity = m.deEsserIntensity;
    return s;
}

// The de-esser rides in DynamicsSettings -- it is a dynamics processor, and
// the filter node that runs the compressor is exactly where it belongs -- but
// it is *stored* outside DynamicsState, so the two are joined here rather than
// in the plain state->settings conversion.
waveline::DynamicsSettings withDeEsser(waveline::DynamicsSettings d, bool on,
                                       double intensity) {
    d.deEsser = on;
    d.deEsserIntensity = static_cast<float>(std::clamp(intensity, 0.0, 1.0));
    return d;
}

void scrubDuckingMasterRef(DuckingState &d, const QString &removedId) {
    for (DuckingSourceState &src : d.sources) {
        if (src.kind != QLatin1String("master_mic")) continue;
        if (src.channelId.isEmpty() || src.channelId == removedId)
            src.channelId = QStringLiteral("mic");
    }
}

void scrubMasterReferences(Profile &p, const QString &removedId) {
    scrubDuckingMasterRef(p.masterOutputDucking, removedId);
    for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
        it->masterMicIds.removeAll(removedId);
        if (it->masterMicIds.isEmpty()) it->masterMicIds << QStringLiteral("mic");
        if (it->inputEffectSourceMasterId == removedId)
            it->inputEffectSourceMasterId = QStringLiteral("mic");
        if (it->outputEffectSourceMasterId == removedId)
            it->outputEffectSourceMasterId = QStringLiteral("mic");
        scrubDuckingMasterRef(it->ducking, removedId);
        it->inputUseMasterEffects = !it->inputEffectSourceMasterId.isEmpty();
        it->outputUseMasterEffects = !it->outputEffectSourceMasterId.isEmpty();
    }
}

ChannelFxStageState effectiveInputStage(const Profile &p, const ChannelEffectsState &ch) {
    const QString masterId = resolveMasterBusId(p, ch.inputEffectSourceMasterId);
    if (masterId.isEmpty()) return ch.input;
    const MasterBusState *m = findMasterBus(p, masterId);
    if (!m) return ch.input;
    return masterMicStage(*m);
}

ChannelFxStageState effectiveOutputStage(const Profile &p, const ChannelEffectsState &ch) {
    const QString masterId = resolveMasterBusId(p, ch.outputEffectSourceMasterId);
    if (masterId.isEmpty()) return ch.output;
    if (masterId == QLatin1String("mic")) {
        ChannelFxStageState s;
        s.fx = p.masterOutput.fx;
        s.dynamics = p.masterOutput.dynamics;
        s.creativeFx = p.masterOutput.creativeFx;
        s.noiseSuppression = p.masterOutput.noiseSuppression;
        s.noiseIntensity = p.masterOutput.noiseIntensity;
        s.deEsser = p.masterOutput.deEsser;
        s.deEsserIntensity = p.masterOutput.deEsserIntensity;
        return s;
    }
    const MasterBusState *m = findMasterBus(p, masterId);
    if (!m) return ch.output;
    return masterMicStage(*m);
}

DuckingState effectiveDucking(const Profile &p, const ChannelEffectsState &ch) {
    return ch.outputEffectSourceMasterId.isEmpty() ? ch.ducking : p.masterOutputDucking;
}

LufsLimiterState effectiveLufsLimiter(const Profile &p, const ChannelEffectsState &ch) {
    return ch.outputEffectSourceMasterId.isEmpty() ? ch.lufsLimiter
                                                   : p.masterOutputLufsLimiter;
}

bool wantsInputNoiseFilter(const Profile &p, const ChannelEffectsState &ch) {
    if (!ch.effectsEnabled) return false;
    const QString masterId = resolveMasterBusId(p, ch.inputEffectSourceMasterId);
    if (!masterId.isEmpty()) {
        const MasterBusState *m = findMasterBus(p, masterId);
        return m && m->noiseSuppression && m->micEffectsEnabled;
    }
    return ch.input.noiseSuppression;
}

bool wantsOutputNoiseFilter(const Profile &p, const ChannelEffectsState &ch) {
    if (!ch.effectsEnabled) return false;
    if (!ch.outputEffectSourceMasterId.isEmpty()) return p.masterOutput.noiseSuppression;
    return ch.output.noiseSuppression;
}

void syncLegacyFromPrimaryBus(Profile &p) {
    if (p.masterBuses.isEmpty()) return;
    const MasterBusState &m = p.masterBuses.first();
    p.mic = m.mix;
    p.micFx = m.micFx;
    p.micDynamics = m.micDynamics;
    p.micCreativeFx = m.micCreativeFx;
    p.micEffectsEnabled = m.micEffectsEnabled;
    p.micMonitorFx = m.micMonitorFx;
    p.noiseSuppression = m.noiseSuppression;
    p.noiseIntensity = m.noiseIntensity;
    p.softwareMonitor = m.softwareMonitor;
    p.micStereo = m.micStereo;
    p.micInputVolume = m.micInputVolume;
    p.micInputMuted = m.micInputMuted;
    p.hardwareMonitor = m.hardwareMonitor;
    p.micGainDb = m.micGainDb;
    p.hwClipguard = m.hwClipguard;
    p.hwMicMuted = m.hwMicMuted;
    p.hwHpVolumeDb = m.hwHpVolumeDb;
    p.hwHpMuted = m.hwHpMuted;
}

constexpr const char *kGenericCaptureBrand = "Waveline";

// Short default for unrecognized capture hardware (no profile / unknown ALSA node).
QString genericInputName(int slotOneBased) {
    return QStringLiteral("Input #%1").arg(qMax(1, slotOneBased));
}

QString masterNameForCapture(const QString &captureMatch,
                             const waveline::DeviceProfile & /*profile*/,
                             const waveline::MixerGraph *graph) {
    QString match = captureMatch;
    if (match.isEmpty() && graph)
        match = QString::fromStdString(graph->findCaptureNode({}));
    return QString::fromStdString(
        waveline::masterCaptureBrand(match.toStdString()));
}

bool isGenericCaptureBrand(const QString &base) {
    return base.isEmpty() || base == QLatin1String(kGenericCaptureBrand);
}

// Auto-name non-custom strips: profile brands (Wave:3, C922, 4K60 MK.2, …) or
// Input #1 / #2 for generic capture. Duplicate brands get #1, #2 suffixes.
void assignMasterNames(Profile &p, const waveline::DeviceProfile &profile,
                       const waveline::MixerGraph *graph) {
    if (p.masterBuses.isEmpty()) return;

    clearLegacyAutoInputNames(p);
    p.masterBuses.first().id = QStringLiteral("mic");

    QMap<QString, int> baseCounts;
    int midiSlot = 0;
    for (const MasterBusState &m : p.masterBuses) {
        if (m.nameCustom) continue;
        if (m.busType == QLatin1String("midi")) continue;
        const QString base = masterNameForCapture(m.captureMatch, profile, graph);
        if (!isGenericCaptureBrand(base)) baseCounts[base]++;
    }

    QMap<QString, int> seen;
    for (int i = 0; i < p.masterBuses.size(); ++i) {
        MasterBusState &m = p.masterBuses[i];
        if (m.nameCustom) continue;
        if (m.busType == QLatin1String("midi")) {
            m.name = QStringLiteral("MIDI #%1").arg(++midiSlot);
            continue;
        }
        const QString base = masterNameForCapture(m.captureMatch, profile, graph);
        if (isGenericCaptureBrand(base)) {
            m.name = genericInputName(i + 1);
        } else if (baseCounts[base] <= 1) {
            m.name = base;
        } else {
            const int n = ++seen[base];
            m.name = QStringLiteral("%1 #%2").arg(base).arg(n);
        }
    }
}

void syncPrimaryBusFromLegacy(Profile &p) {
    if (p.masterBuses.isEmpty()) {
        MasterBusState m;
        m.id = QStringLiteral("mic");
        m.name = genericInputName(1);
        p.masterBuses.append(m);
    }
    MasterBusState &m = p.masterBuses.first();
    m.id = QStringLiteral("mic");
    if (m.name.isEmpty()) m.name = genericInputName(1);
    m.mix = p.mic;
    m.micFx = p.micFx;
    m.micDynamics = p.micDynamics;
    m.micCreativeFx = p.micCreativeFx;
    m.micEffectsEnabled = p.micEffectsEnabled;
    m.micMonitorFx = p.micMonitorFx;
    m.noiseSuppression = p.noiseSuppression;
    m.noiseIntensity = p.noiseIntensity;
    m.softwareMonitor = p.softwareMonitor;
    m.micStereo = p.micStereo;
    m.micInputVolume = p.micInputVolume;
    m.micInputMuted = p.micInputMuted;
    m.hardwareMonitor = p.hardwareMonitor;
    m.micGainDb = p.micGainDb;
    m.hwClipguard = p.hwClipguard;
    m.hwMicMuted = p.hwMicMuted;
    m.hwHpVolumeDb = p.hwHpVolumeDb;
    m.hwHpMuted = p.hwHpMuted;
}

// The reply both effects getters send. The parametric fields are appended
// rather than woven in, so a client written against the six-field version --
// including every gdbus one-liner in the troubleshooting notes -- still reads
// the fields it knows at the indices it knows them by.
QString fxToTabString(const waveline::ChannelFxSettings &s) {
    return QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8")
        .arg(s.lowCut ? 1 : 0)
        .arg(s.lowCutHz)
        .arg(s.eq ? 1 : 0)
        .arg(s.lowDb)
        .arg(s.midDb)
        .arg(s.highDb)
        .arg(s.eqAdvanced ? 1 : 0)
        .arg(QString::fromStdString(waveline::encodeEqBands(s.bands)));
}

// SetChannelEffects and friends carry the three-band EQ only. Writing a whole
// ChannelFxState back from them would take the parametric curve with it --
// silently flattening someone's advanced EQ every time they nudged a Low
// slider -- so they overwrite exactly the fields they were given.
void applyEasyFxFields(ChannelFxState &dst, const waveline::ChannelFxSettings &s) {
    dst.lowCut = s.lowCut;
    dst.lowCutHz = s.lowCutHz;
    dst.eq = s.eq;
    dst.lowDb = s.lowDb;
    dst.midDb = s.midDb;
    dst.highDb = s.highDb;
}

void applyProEqFields(ChannelFxState &dst, bool advanced, const QString &bands) {
    dst.eqAdvanced = advanced;
    // Round-tripped through the codec rather than stored as handed over: what
    // lands in the config file is then always well-formed and in range,
    // whoever called the method.
    dst.proEqBands = QString::fromStdString(
        waveline::encodeEqBands(waveline::decodeEqBands(bands.toStdString())));
}

}  // namespace

waveline::ChannelFxSettings MixerService::toFxSettings(const ChannelFxState &s) {
    waveline::ChannelFxSettings fx;
    fx.lowCut = s.lowCut;
    fx.lowCutHz = s.lowCutHz;
    fx.eq = s.eq;
    fx.lowDb = static_cast<float>(s.lowDb);
    fx.midDb = static_cast<float>(s.midDb);
    fx.highDb = static_cast<float>(s.highDb);
    fx.eqAdvanced = s.eqAdvanced;
    fx.bands = s.proEqBands.isEmpty()
                   ? waveline::defaultEqBands()
                   : waveline::decodeEqBands(s.proEqBands.toStdString());
    return fx;
}

ChannelFxState MixerService::fromFxSettings(const waveline::ChannelFxSettings &s) {
    ChannelFxState fx;
    fx.lowCut = s.lowCut;
    fx.lowCutHz = s.lowCutHz;
    fx.eq = s.eq;
    fx.lowDb = s.lowDb;
    fx.midDb = s.midDb;
    fx.highDb = s.highDb;
    fx.eqAdvanced = s.eqAdvanced;
    fx.proEqBands = QString::fromStdString(waveline::encodeEqBands(s.bands));
    return fx;
}

waveline::DynamicsSettings MixerService::toDynamicsSettings(const DynamicsState &s) {
    waveline::DynamicsSettings d;
    d.gate = s.gate;
    d.gateThresholdDb = static_cast<float>(s.gateThresholdDb);
    d.gateAttackSec = static_cast<float>(s.gateAttackMs * 0.001);
    d.gateReleaseSec = static_cast<float>(s.gateReleaseMs * 0.001);
    d.compressor = s.compressor;
    d.compThresholdDb = static_cast<float>(s.compThresholdDb);
    d.compRatio = static_cast<float>(s.compRatio);
    d.compAttackSec = static_cast<float>(s.compAttackMs * 0.001);
    d.compReleaseSec = static_cast<float>(s.compReleaseMs * 0.001);
    d.compKneeDb = static_cast<float>(s.compKneeDb);
    d.makeupGainDb = static_cast<float>(s.makeupGainDb);
    d.autoMakeup = s.autoMakeup;
    d.limiter = s.limiter;
    d.limitThresholdDb = static_cast<float>(s.limitThresholdDb);
    d.limitAttackSec = static_cast<float>(s.limitAttackMs * 0.001);
    d.limitReleaseSec = static_cast<float>(s.limitReleaseMs * 0.001);
    return d;
}

DynamicsState MixerService::fromDynamicsSettings(const waveline::DynamicsSettings &s) {
    DynamicsState d;
    d.gate = s.gate;
    d.gateThresholdDb = s.gateThresholdDb;
    d.gateAttackMs = s.gateAttackSec * 1000.0;
    d.gateReleaseMs = s.gateReleaseSec * 1000.0;
    d.compressor = s.compressor;
    d.compThresholdDb = s.compThresholdDb;
    d.compRatio = s.compRatio;
    d.compAttackMs = s.compAttackSec * 1000.0;
    d.compReleaseMs = s.compReleaseSec * 1000.0;
    d.compKneeDb = s.compKneeDb;
    d.makeupGainDb = s.makeupGainDb;
    d.autoMakeup = s.autoMakeup;
    d.limiter = s.limiter;
    d.limitThresholdDb = s.limitThresholdDb;
    d.limitAttackMs = s.limitAttackSec * 1000.0;
    d.limitReleaseMs = s.limitReleaseSec * 1000.0;
    return d;
}

waveline::LufsLimiterSettings MixerService::toLufsLimiterSettings(const LufsLimiterState &s) {
    waveline::LufsLimiterSettings out;
    out.enabled = s.enabled;
    out.maxLufs = static_cast<float>(s.maxLufs);
    return out;
}

waveline::CreativeFxSettings MixerService::toCreativeFxSettings(const CreativeFxState &s) {
    return waveline::decodeCreativeFx(s.spec.toStdString());
}

waveline::DuckingSettings MixerService::toDuckingSettings(const DuckingState &s) {
    waveline::DuckingSettings d;
    d.enabled = s.enabled;
    d.intensity = static_cast<float>(s.intensity);
    d.thresholdDb = static_cast<float>(s.thresholdDb);
    d.depthDb = static_cast<float>(s.depthDb);
    d.attackSec = static_cast<float>(s.attackMs * 0.001);
    d.releaseSec = static_cast<float>(s.releaseMs * 0.001);
    d.holdSec = static_cast<float>(qBound(0.0, s.holdSec, 10.0));
    d.sources.clear();
    for (const DuckingSourceState &src : s.sources) {
        if (d.sources.size() >= waveline::kMaxDuckingSources) break;
        waveline::DuckingSourceRef ref;
        if (src.kind == QLatin1String("channel_mic")) {
            ref.kind = waveline::DuckingSourceKind::ChannelMic;
            ref.channelId = src.channelId.toStdString();
        } else if (src.kind == QLatin1String("channel_audio")) {
            ref.kind = waveline::DuckingSourceKind::ChannelAudio;
            ref.channelId = src.channelId.toStdString();
        } else {
            ref.kind = waveline::DuckingSourceKind::MasterMic;
            ref.channelId = src.channelId.toStdString();
        }
        d.sources.push_back(ref);
    }
    if (d.sources.empty()) {
        d.sources.push_back({waveline::DuckingSourceKind::MasterMic, "mic"});
    }
    return d;
}

DuckingState MixerService::fromDuckingSettings(const waveline::DuckingSettings &s) {
    DuckingState d;
    d.enabled = s.enabled;
    d.intensity = s.intensity;
    d.thresholdDb = s.thresholdDb;
    d.depthDb = s.depthDb;
    d.attackMs = s.attackSec * 1000.0;
    d.releaseMs = s.releaseSec * 1000.0;
    d.holdSec = s.holdSec;
    d.sources.clear();
    for (const waveline::DuckingSourceRef &ref : s.sources) {
        DuckingSourceState src;
        switch (ref.kind) {
        case waveline::DuckingSourceKind::ChannelMic:
            src.kind = QStringLiteral("channel_mic");
            src.channelId = QString::fromStdString(ref.channelId);
            break;
        case waveline::DuckingSourceKind::ChannelAudio:
            src.kind = QStringLiteral("channel_audio");
            src.channelId = QString::fromStdString(ref.channelId);
            break;
        default:
            src.kind = QStringLiteral("master_mic");
            src.channelId = QString::fromStdString(ref.channelId);
            if (src.channelId.isEmpty()) src.channelId = QStringLiteral("mic");
            break;
        }
        d.sources.append(src);
    }
    if (d.sources.isEmpty())
        d.sources.append(DuckingSourceState{QStringLiteral("master_mic"),
                                            QStringLiteral("mic")});
    return d;
}

namespace {

DynamicsState clampDynamics(DynamicsState d) {
    d.gateThresholdDb = qBound(-80.0, d.gateThresholdDb, 0.0);
    d.gateAttackMs = qBound(0.5, d.gateAttackMs, 500.0);
    d.gateReleaseMs = qBound(10.0, d.gateReleaseMs, 2000.0);
    d.compThresholdDb = qBound(-60.0, d.compThresholdDb, 0.0);
    d.compRatio = qBound(1.0, d.compRatio, 20.0);
    d.compAttackMs = qBound(0.5, d.compAttackMs, 500.0);
    d.compReleaseMs = qBound(10.0, d.compReleaseMs, 2000.0);
    d.compKneeDb = qBound(0.0, d.compKneeDb, 24.0);
    d.makeupGainDb = qBound(-12.0, d.makeupGainDb, 24.0);
    d.limitThresholdDb = qBound(-12.0, d.limitThresholdDb, 0.0);
    d.limitAttackMs = qBound(0.1, d.limitAttackMs, 100.0);
    d.limitReleaseMs = qBound(5.0, d.limitReleaseMs, 500.0);
    return d;
}

QString dynamicsToTabString(const DynamicsState &d) {
    return QStringLiteral(
               "%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\t%11\t%12\t%13\t%14\t%15\t%16")
        .arg(d.gate ? 1 : 0)
        .arg(d.gateThresholdDb)
        .arg(d.gateAttackMs)
        .arg(d.gateReleaseMs)
        .arg(d.compressor ? 1 : 0)
        .arg(d.compThresholdDb)
        .arg(d.compRatio)
        .arg(d.compAttackMs)
        .arg(d.compReleaseMs)
        .arg(d.compKneeDb)
        .arg(d.makeupGainDb)
        .arg(d.autoMakeup ? 1 : 0)
        .arg(d.limiter ? 1 : 0)
        .arg(d.limitThresholdDb)
        .arg(d.limitAttackMs)
        .arg(d.limitReleaseMs);
}

QString encodeDuckingSources(const QVector<DuckingSourceState> &sources) {
    QStringList parts;
    for (const DuckingSourceState &src : sources) {
        if (src.kind == QLatin1String("master_mic")) {
            if (!src.channelId.isEmpty())
                parts << QStringLiteral("master_mic:") + src.channelId;
            else
                parts << QStringLiteral("master_mic");
        } else if (!src.channelId.isEmpty()) {
            parts << src.kind + QLatin1Char(':') + src.channelId;
        }
    }
    if (parts.isEmpty()) parts << QStringLiteral("master_mic");
    return parts.join(QLatin1Char('|'));
}

QVector<DuckingSourceState> decodeDuckingSources(const QString &encoded) {
    QVector<DuckingSourceState> out;
    const QStringList parts =
        encoded.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (out.size() >= waveline::kMaxDuckingSources) break;
        DuckingSourceState src;
        const int colon = part.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            if (part == QLatin1String("master") || part == QLatin1String("master_mic"))
                src.kind = QStringLiteral("master_mic");
            else {
                src.kind = QStringLiteral("channel_mic");
                src.channelId = part;
            }
        } else {
            const QString kind = part.left(colon);
            src.kind = kind;
            src.channelId = part.mid(colon + 1);
            if (src.kind == QLatin1String("master"))
                src.kind = QStringLiteral("master_mic");
        }
        if (src.kind == QLatin1String("master_mic") ||
            src.kind == QLatin1String("channel_mic") ||
            src.kind == QLatin1String("channel_audio")) {
            if (src.kind == QLatin1String("master_mic") && src.channelId.isEmpty())
                src.channelId = QStringLiteral("mic");
            out.append(src);
        }
    }
    if (out.isEmpty())
        out.append(DuckingSourceState{QStringLiteral("master_mic"),
                                      QStringLiteral("mic")});
    return out;
}

// Hold goes on the end: a client built before it existed reads the first seven
// fields exactly as it always did.
QString duckingToTabString(const DuckingState &d) {
    return QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8")
        .arg(d.enabled ? 1 : 0)
        .arg(d.intensity)
        .arg(d.thresholdDb)
        .arg(d.depthDb)
        .arg(d.attackMs)
        .arg(d.releaseMs)
        .arg(encodeDuckingSources(d.sources))
        .arg(d.holdSec);
}

DuckingState duckingFromArgs(bool enabled, const QString &sourcesEncoded, double intensity,
                             double holdSec) {
    DuckingState d;
    d.enabled = enabled;
    d.intensity = qBound(0.0, intensity, 1.0);
    d.holdSec = qBound(0.0, holdSec, 10.0);
    d.sources = decodeDuckingSources(sourcesEncoded);
    return d;
}

DynamicsState dynamicsFromArgs(bool gate, double gateThresholdDb, double gateAttackMs,
                               double gateReleaseMs, bool compressor,
                               double compThresholdDb, double compRatio,
                               double compAttackMs, double compReleaseMs,
                               double compKneeDb, double makeupGainDb, bool autoMakeup,
                               bool limiter, double limitThresholdDb,
                               double limitAttackMs, double limitReleaseMs) {
    DynamicsState d;
    d.gate = gate;
    d.gateThresholdDb = gateThresholdDb;
    d.gateAttackMs = gateAttackMs;
    d.gateReleaseMs = gateReleaseMs;
    d.compressor = compressor;
    d.compThresholdDb = compThresholdDb;
    d.compRatio = compRatio;
    d.compAttackMs = compAttackMs;
    d.compReleaseMs = compReleaseMs;
    d.compKneeDb = compKneeDb;
    d.makeupGainDb = makeupGainDb;
    d.autoMakeup = autoMakeup;
    d.limiter = limiter;
    d.limitThresholdDb = limitThresholdDb;
    d.limitAttackMs = limitAttackMs;
    d.limitReleaseMs = limitReleaseMs;
    return clampDynamics(d);
}

}  // namespace

MasterBusState *MixerService::masterBusState(Profile &p, const QString &id) {
    for (MasterBusState &m : p.masterBuses) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

const MasterBusState *MixerService::masterBusState(const Profile &p,
                                                   const QString &id) const {
    for (const MasterBusState &m : p.masterBuses) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

MixerService::MasterHwSlot *MixerService::masterHwSlot(const QString &masterId) {
    return &masterHw_[masterId];
}

const MixerService::MasterHwSlot *MixerService::masterHwSlot(const QString &masterId) const {
    const auto it = masterHw_.find(masterId);
    return it == masterHw_.end() ? nullptr : &it->second;
}

QString MixerService::nextMasterId() const {
    const Profile &p = config_.live();
    for (int n = 1; n < 1000; ++n) {
        const QString id = QStringLiteral("master-%1").arg(n);
        if (masterBusState(p, id)) continue;
        if (graph_ && graph_->masterBus(id.toStdString())) continue;
        return id;
    }
    return {};
}

QString MixerService::effectiveMasterCaptureMatch(const QString & /*masterId*/,
                                                  const QString &configMatch) const {
    return configMatch;
}

bool MixerService::masterHasWave3Hw(const QString &masterId) const {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m || m->busType == QLatin1String("midi")) return false;
    const QString match = effectiveMasterCaptureMatch(masterId, m->captureMatch);
    return match.startsWith(QLatin1String(waveline::kWave3CapturePrefix));
}

bool MixerService::isCaptureDeviceNode(const std::string &name) const {
    if (name.rfind("waveline-", 0) == 0) return false;
    if (name.size() > 8 && name.compare(name.size() - 8, 8, ".monitor") == 0)
        return false;
    return true;
}

void MixerService::applyMasterInputVolume(const QString &masterId) {
    if (!graph_) return;
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    graph_->setMasterInputGain(masterId.toStdString(),
                               m->micInputMuted ? 0.0f
                                                : static_cast<float>(m->micInputVolume));
}

void MixerService::applyMasterMixLevels(const QString &masterId) {
    if (!graph_) return;
    const Profile &p = config_.live();
    const MasterBusState *m = masterBusState(p, masterId);
    if (!m) return;
    const std::string id = masterId.toStdString();

    if (masterId == QLatin1String("mic")) {
        micStreamLevel_ = m->mix.streamVolume;
        micMonitorLevel_ = m->mix.monitorVolume;
        micStreamMuted_ = m->mix.streamMuted;
        micMonitorMuted_ = m->mix.monitorMuted;
        // Volumes before software-monitor: setPathMuted reapplies the path's
        // recorded volume, which is still unity on a freshly created loopback.
        graph_->setMicVolume(waveline::Mix::Stream,
                             static_cast<float>(micStreamLevel_));
        graph_->setMicVolume(waveline::Mix::Monitor,
                             static_cast<float>(micMonitorLevel_));
        graph_->setSoftwareMonitor(p.softwareMonitor);
        graph_->setMicMuted(waveline::Mix::Stream, micStreamMuted_);
        if (p.softwareMonitor)
            graph_->setMicMuted(waveline::Mix::Monitor, micMonitorMuted_);
        graph_->applyMasterPathLevels(id);
        return;
    }

    if (!graph_->masterBus(id)) return;
    graph_->setMasterVolume(id, waveline::Mix::Stream,
                            static_cast<float>(m->mix.streamVolume));
    graph_->setMasterMuted(id, waveline::Mix::Stream, m->mix.streamMuted);
    graph_->setMasterSoftwareMonitor(id, m->softwareMonitor);
    if (m->softwareMonitor) {
        graph_->setMasterVolume(id, waveline::Mix::Monitor,
                                static_cast<float>(m->mix.monitorVolume));
        graph_->setMasterMuted(id, waveline::Mix::Monitor, m->mix.monitorMuted);
    }
    graph_->applyMasterPathLevels(id);
}

void MixerService::applyMasterFx(const QString &masterId) {
    if (!graph_) return;
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    const std::string id = masterId.toStdString();
    const bool on = m->micEffectsEnabled;
    if (auto *nc = graph_->masterNoiseFilter(id)) {
        const bool ncOn = on && m->noiseSuppression;
        nc->setEnabled(ncOn);
        nc->setIntensity(static_cast<float>(m->noiseIntensity));
    }
    // Rack Mode swaps which Creative FX blob actually drives this device's
    // mic input chain; the Microphone > Creative tab's own blob is kept
    // untouched underneath so turning Rack Mode back off restores exactly
    // what it had.
    const CreativeFxState &effectiveMicCreative =
        m->rackMode ? m->rackCreativeFx : m->micCreativeFx;
    const waveline::CreativeFxSettings creative =
        on ? toCreativeFxSettings(effectiveMicCreative) : waveline::CreativeFxSettings{};
    waveline::ChannelFxSettings fx = on ? toFxSettings(m->micFx) : waveline::ChannelFxSettings{};
    // The Creative EQ module is a second equalizer on the same signal; while
    // it's active the standard one is bypassed rather than stacked, without
    // touching the stored m->micFx.eq value the tab still shows.
    if (on && creative.eq.enabled) fx.eq = false;
    graph_->setMasterEffects(id, fx);
    graph_->setMasterDynamics(
        id, on ? withDeEsser(toDynamicsSettings(m->micDynamics), m->deEsser,
                             m->deEsserIntensity)
               : waveline::DynamicsSettings{});
    graph_->setMasterCreativeFx(id, creative);
    Profile &live = config_.live();
    if (masterId == QLatin1String("mic")) syncLegacyFromPrimaryBus(live);
    for (auto it = live.channelEffects.begin(); it != live.channelEffects.end(); ++it) {
        if (resolveMasterBusId(live, it->inputEffectSourceMasterId) == masterId)
            applyChannelFx(it.key());
    }
}

void MixerService::applyMasterBuses() {
    if (!graph_) return;
    Profile &p = config_.live();
    if (p.masterBuses.isEmpty()) syncPrimaryBusFromLegacy(p);
    assignMasterNames(p, profile_, graph_.get());
    rememberMasterDeviceLabels();

    std::set<std::string> want;
    for (const MasterBusState &m : p.masterBuses) want.insert(m.id.toStdString());

    for (const auto &bus : graph_->masterBuses()) {
        if (!want.count(bus.id) && !waveline::isPrimaryMaster(bus.id)) {
            std::string err;
            graph_->removeMasterBus(bus.id, err);
        }
    }

    for (const MasterBusState &m : p.masterBuses) {
        const std::string id = m.id.toStdString();
        if (!graph_->masterBus(id)) {
            if (waveline::isPrimaryMaster(id)) continue;
            const bool midi = m.busType == QLatin1String("midi");
            if (midi) {
                if (m.midiPortMatch.isEmpty()) continue;
            } else if (effectiveMasterCaptureMatch(m.id, m.captureMatch).isEmpty()) {
                continue;
            }
            std::string err;
            if (!graph_->addMasterBus(id, m.name.toStdString(), m.busType.toStdString(),
                                      err))
                qWarning("waveline: add master bus %s: %s", qUtf8Printable(m.id),
                         err.c_str());
        }
    }

    for (const MasterBusState &m : p.masterBuses) {
        const std::string id = m.id.toStdString();
        graph_->setMasterName(id, m.name.toStdString());
        graph_->setMasterCaptureMatch(
            id, effectiveMasterCaptureMatch(m.id, m.captureMatch).toStdString());
        if (m.busType == QLatin1String("midi")) {
            graph_->setMasterMidiPortMatch(id, m.midiPortMatch.toStdString());
            if (waveline::MasterBusRuntime *rt = graph_->masterBus(id))
                rt->busType = "midi";
            if (!m.soundfontPath.isEmpty())
                graph_->setMasterSoundfontPath(id, m.soundfontPath.toStdString());
            std::string pathErr;
            graph_->ensureMidiStreamPath(id, pathErr);
        } else if (waveline::MasterBusRuntime *rt = graph_->masterBus(id)) {
            rt->busType = "capture";
        }

        if (waveline::MasterBusRuntime *rt = graph_->masterBus(id)) {
            rt->micMonitorFx = m.micMonitorFx;
            rt->wantSoftwareGain = true;
            if (!waveline::isPrimaryMaster(id)) {
                rt->softwareMonitor = m.softwareMonitor;
                if (rt->micStereo != m.micStereo) {
                    std::string stereoErr;
                    graph_->setMasterMicStereo(id, m.micStereo, stereoErr);
                }
            }
        }

        if (m.id == QLatin1String("mic")) {
            micStreamLevel_ = m.mix.streamVolume;
            micMonitorLevel_ = m.mix.monitorVolume;
            micStreamMuted_ = m.mix.streamMuted;
            micMonitorMuted_ = m.mix.monitorMuted;
            if (m.micStereo != graph_->micStereo()) {
                std::string err;
                graph_->setMicStereo(m.micStereo, err);
            }
            graph_->setSoftwareMonitor(p.softwareMonitor);
            graph_->setMicVolume(waveline::Mix::Monitor,
                                 static_cast<float>(micMonitorLevel_));
            graph_->setMicVolume(waveline::Mix::Stream,
                                 static_cast<float>(micStreamLevel_));
            graph_->setMicMuted(waveline::Mix::Stream, micStreamMuted_);
            if (p.softwareMonitor)
                graph_->setMicMuted(waveline::Mix::Monitor, micMonitorMuted_);
            graph_->setMicMonitorFx(m.micMonitorFx);
        } else if (graph_->masterBus(id)) {
            graph_->setMasterVolume(id, waveline::Mix::Stream,
                                    static_cast<float>(m.mix.streamVolume));
            graph_->setMasterMuted(id, waveline::Mix::Stream, m.mix.streamMuted);
            // Monitor fader only applies while software monitor is on. Applying
            // unmute here while softwareMonitor is false was reopening ghost
            // monitoring on every master (C922, MK.2, Meet 4K, …).
            if (m.softwareMonitor) {
                graph_->setMasterVolume(id, waveline::Mix::Monitor,
                                        static_cast<float>(m.mix.monitorVolume));
                graph_->setMasterMuted(id, waveline::Mix::Monitor, m.mix.monitorMuted);
            } else {
                graph_->setMasterSoftwareMonitor(id, false);
            }
        }

        applyMasterFx(m.id);
        applyMasterInputVolume(m.id);
    }

    for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it)
        graph_->setChannelMasterMics(it.key().toStdString(),
                                     toStdIds(it->masterMicIds));

    for (const MasterBusState &m : p.masterBuses) {
        if (m.id == QLatin1String("mic")) continue;
        if (!graph_->masterBus(m.id.toStdString())) continue;
        graph_->setMasterSoftwareMonitor(m.id.toStdString(), m.softwareMonitor);
    }

    // Persist auto-renamed strips (brand names, Input #N, legacy title migration).
    scheduleSave();
    syncMasterMeters();
}

QString MixerService::pickUnusedCaptureDevice() const {
    QSet<QString> used;
    const Profile &p = config_.live();
    for (const MasterBusState &m : p.masterBuses) {
        const QString match = effectiveMasterCaptureMatch(m.id, m.captureMatch);
        if (!match.isEmpty()) used.insert(match);
    }
    if (graph_) {
        for (const auto &bus : graph_->masterBuses()) {
            if (!bus.captureNode.empty())
                used.insert(QString::fromStdString(bus.captureNode));
        }
    }
    for (const auto &n : engine_.nodes()) {
        if (n.isOurs || n.mediaClass != "Audio/Source") continue;
        if (!isCaptureDeviceNode(n.name)) continue;
        const QString name = QString::fromStdString(n.name);
        bool taken = used.contains(name);
        for (const QString &u : used) {
            if (!u.isEmpty() && name.startsWith(u)) {
                taken = true;
                break;
            }
        }
        if (!taken) return name;
    }
    return {};
}

bool MixerService::start(QString &error) {
    // Read first: it decides what everything below is called and whether there
    // is a vendor USB device to open at all.
    profile_ = waveline::DeviceProfile::load();
    qInfo("waveline: device profile '%s' (%s), brand '%s', hardware controls %s",
          profile_.id.c_str(), profile_.label.c_str(), profile_.brand.c_str(),
          profile_.hardwareControls ? "yes" : "no");

    // Before engine_.start(), which creates the first PipeWire context, because
    // whether that context loads module-rt is read out of the config and cannot
    // be changed afterwards -- see engine/rtsched.h. Nothing between here and
    // the original load site touches config_, so this only moves the read
    // earlier; applyProfile() still runs where it did, once the graph exists.
    if (!config_.load()) lastError_ = config_.lastError();
    waveline::setRealtimeEnabled(config_.audio().realtime);
    if (!config_.audio().realtime)
        qInfo("waveline: real-time scheduling disabled by setting; audio threads "
              "will run SCHED_OTHER");
    // Also before the graph exists, so the first stage to run is already
    // counting. Unlike the switch above this one could be set at any point;
    // doing it here only means an investigation left running survives the
    // restart rather than starting again the first time the panel is opened.
    waveline::setDspProfiling(config_.diagnostics().dspProfiling);
    if (config_.diagnostics().dspProfiling)
        qInfo("waveline: DSP profiling on; per-stage timing and missed-cycle "
              "counts are being collected");

    // The watcher only reports *changes*, so a shell already running when the
    // daemon starts would never be seen without asking once.
    if (auto *iface = QDBusConnection::sessionBus().interface()) {
        shellPresent_ =
            iface->isServiceRegistered(QStringLiteral("org.glassbar.Settings"));
    }

    std::string err;
    if (!engine_.start(err)) {
        error = QString::fromStdString(err);
        return false;
    }

    // Let the registry populate before the graph looks for the microphone.
    QThread::msleep(400);

    graph_ = std::make_unique<waveline::MixerGraph>(engine_);
    graph_->setBrand(profile_.brand);
    graph_->setMicNodeMatch({});
    graph_->setSoftwareMicGain(true);
    if (!graph_->build(err)) {
        error = QString::fromStdString(err);
        return false;
    }

    router_ = std::make_unique<waveline::AppRouter>(engine_);
    soundShare_ = std::make_unique<waveline::SoundShareRouter>(engine_);
    updateStreamRouting();

    soundboard_ = std::make_unique<waveline::SoundboardEngine>(engine_);
    loadSoundboardBuffers();

    // Swapping microphones is done in the desktop's sound settings, not in
    // here, so that is what the daemon watches. Only meaningful without a
    // pinned profile -- MixerGraph::reconsiderMicNode() enforces that -- but
    // the callback is registered either way rather than conditionally, so the
    // behaviour cannot drift apart from the rule that decides it.
    engine_.setOnDefaultSourceChanged([this] {
        // Runs on the PipeWire thread loop. QTimer must be started from the
        // thread that owns it, so hop to ours before touching anything Qt.
        QMetaObject::invokeMethod(this, [this] { followDefaultSource(); },
                                  Qt::QueuedConnection);
    });

    // Meters. Best effort throughout: a mixer that will not start because it
    // could not create a meter would be a poor trade, so every failure here
    // only costs the display.
    meters_ = std::make_unique<waveline::LevelProbe>();
    std::string meterErr;
    if (!meters_->start(meterErr)) {
        lastError_ = QStringLiteral("meters unavailable: %1")
                         .arg(QString::fromStdString(meterErr));
        meters_.reset();
    } else {
        // channelSinkNames() is in channel order, which is the only way to pair
        // an id with its sink from out here.
        const auto sinks = graph_->channelSinkNames();
        const auto &chans = graph_->channels();
        for (size_t i = 0; i < chans.size() && i < sinks.size(); ++i)
            meters_->watch(chans[i].id, sinks[i], meterErr);
        meters_->watch("monitor-mix", waveline::MixerGraph::kMonitorMix, meterErr);
        meters_->watch("stream-mix", waveline::MixerGraph::kStreamMix, meterErr);
        meters_->watch("sound-share", waveline::MixerGraph::kSoundShareSink, meterErr);
    }

    // The config itself was read at the top of this function, ahead of the first
    // PipeWire context. Applying it waits until here, so nothing overwrites the
    // saved values with whatever the device happens to hold right now.
    applyProfile();
    if (meters_) {
        const Profile &p = config_.live();
        for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
            if (!it->micSource) continue;
            std::string mErr;
            const std::string id = it.key().toStdString();
            meters_->watch(id + "-mic", "waveline-" + id + "-mic", mErr, false);
        }
    }
    {
        std::string filterErr;
        graph_->ensureChannelFilters(filterErr);
    }
    QThread::msleep(1200);
    rewireGraph();
    QTimer::singleShot(500, this, [this] { finishAllPendingMidi(); });
    // After the monitor loopbacks exist and are holding their levels, never
    // before: unmuting a device while its loopback is still coming up at unity
    // is the exact moment this is meant to protect against.
    QTimer::singleShot(1500, this, [this] { restoreOutputsMutedAtShutdown(); });
    for (int ms : {2000, 5000}) {
        QTimer::singleShot(ms, this, [this, ms] {
            if (!graph_) return;
            std::string verifyErr;
            if (!graph_->verifyMixWiring(verifyErr)) {
                qWarning("waveline: mix wiring check failed at %dms: %s", ms,
                         verifyErr.c_str());
                scheduleRewire();
                return;
            }
            // verifyMixWiring() only asks whether the links exist. A path can
            // be fully linked and still carry nothing, so say so here rather
            // than report a clean graph over a silent mix. No automatic repair:
            // a stalled path is not fixed by rebuilding it (tried -- the
            // replacement stalls the same way), and rebuilding costs
            // descriptors, which is usually the thing that ran out.
            reportStalledPaths(ms);
        });
    }

    qInfo("waveline: microphone input is '%s'",
          graph_->micNode().empty() ? "(none found yet)"
                                    : graph_->micNode().c_str());
    applyMicInputVolume();
    syncAlsaAliases();

    // Last, and never fatal. A port already in use is the common failure here
    // and it has nothing to do with audio: refusing to start the mixer over it
    // would take someone's routing away to protect a convenience feature.
    // Owned by the unique_ptr and not by the QObject tree, like the graph and
    // the routers beside it: one owner is easier to reason about than two that
    // happen not to collide.
    companion_ = std::make_unique<CompanionServer>(this);
    connect(this, &MixerService::Changed, companion_.get(),
            &CompanionServer::notifyChanged);
    if (config_.companion().autoStart) {
        QString companionErr;
        if (!companion_->start(config_.companion().port, companionErr)) {
            qWarning("waveline: companion server did not start on port %d: %s",
                     config_.companion().port, qPrintable(companionErr));
        }
    }

    // Reassert the user's graph quantum. clock.force-quantum lives in
    // PipeWire's settings metadata, which does not survive PipeWire
    // restarting, so a choice made once has to be put back every time this
    // daemon starts or it silently reverts to the config-file default.
    //
    // Deferred rather than done here: the settings metadata arrives through
    // the registry and is routinely not bound yet at this point. A single
    // retry covers the ordinary case, and the setting is stored either way, so
    // the worst outcome of both attempts failing is the graph running at the
    // file default until the user touches the control -- not a lost setting.
    //
    // And rebuild the capture hops afterwards, for exactly the reason
    // SetGraphQuantum does (see the long comment there): the graph is built at
    // the *file* default -- 512 in 50-waveline-clock.conf -- and this assert
    // then yanks it somewhere else while the ALSA capture nodes are already
    // open and attached. That is a forced quantum transition, and it is the one
    // that leaves a capture resampler in a permanent resync loop.
    //
    // Missing here, it made the daemon come up broken at 2.7 ms and 5.3 ms and
    // stay broken until the user changed the setting by hand -- which "fixed"
    // it only because the interactive path does run the rebuild. 10.7 ms was
    // always fine on boot because 512 *is* the file default, so there was no
    // transition to survive. Same fault, same fix, different entry point.
    if (config_.audio().graphQuantum != 0) {
        // Shared so the two attempts can agree: the rebuild must happen once,
        // and the second attempt has to still run it if the first came too
        // early for the graph to exist.
        auto settled = std::make_shared<bool>(false);
        const auto assertQuantum = [this, settled] {
            const auto want = static_cast<uint32_t>(config_.audio().graphQuantum);
            const waveline::PwEngine::GraphClock clock = engine_.graphClock();
            std::string err;
            if (!engine_.setForcedQuantum(want, err)) {
                qWarning("waveline: could not restore graph quantum: %s",
                         err.c_str());
                return;
            }
            // Compared against the configured default, not against the forced
            // value: the forced value is whatever the *previous* attempt just
            // wrote, so testing it would make the second attempt conclude there
            // was no transition and skip a rebuild the first one never managed.
            // clock.quantum is what the graph was actually built at and does
            // not move.
            // Gating on a real transition is also what keeps this from
            // colliding with the rule in rewire(): a clean start whose stored
            // quantum already matches the file default gets no rebuild, because
            // there the graph really is up and working and tearing it down
            // would be a heal nobody asked for.
            const bool transition =
                clock.known && clock.quantum != 0 && want != clock.quantum;
            if (transition && !*settled) *settled = scheduleCaptureSettle({});
        };
        QTimer::singleShot(1500, this, assertQuantum);
        QTimer::singleShot(6000, this, assertQuantum);
    }

    // The headroom rule is a file, so it survives on its own -- but a config
    // restored from backup, or a write that failed last time because the unit
    // file was older than the setting, would otherwise stay unapplied forever.
    // Rewriting is free when the content already matches, and no restart is
    // triggered here: WirePlumber has already read whatever was on disk.
    if (config_.audio().outputHeadroom != 0) {
        QString headroomErr;
        if (!waveline::writeOutputHeadroom(config_.audio().outputHeadroom,
                                           &headroomErr))
            qWarning("waveline: output headroom: %s", qUtf8Printable(headroomErr));
    }

    hwTimer_.start();
    deadCardTimer_.start();
    soundboardReapTimer_.start();
    return true;
}

// ---- the installed device profile, for the GUI ---------------------------
QString MixerService::DeviceProfileId() const {
    return QString::fromStdString(profile_.id);
}

QString MixerService::DeviceBrand() const {
    return QString::fromStdString(profile_.brand);
}

bool MixerService::HasHardwareControls() const {
    return false;
}

void MixerService::scheduleSave() { saveTimer_.start(); }

void MixerService::flushPendingSave() {
    saveTimer_.stop();
    captureToProfile();
    if (!config_.save()) lastError_ = config_.lastError();
}

void MixerService::syncAlsaAliases() {
    if (!graph_) return;
    QString err;
    if (!waveline::syncAlsaAliases(*graph_, profile_.brand, &err))
        qWarning("waveline: ALSA aliases: %s", qUtf8Printable(err));
}

// Debounced on purpose: a WirePlumber restart brings every assigned sink back
// within a few milliseconds of the others, and each one lands here. Restarting
// the timer collapses that burst into one ladder.
void MixerService::scheduleMonitorLevelReassert() {
    monitorLevelPasses_ = 0;
    monitorLevelTimer_.start(600);
}

void MixerService::scheduleRewire() {
    rewireAttempts_ = 0;
    rewireTimer_.start();
}

bool MixerService::scheduleCaptureSettle(const QStringList &masterIds) {
    if (!graph_) return false;
    if (masterIds.isEmpty()) {
        for (const auto &bus : graph_->masterBuses())
            pendingSettleMasters_.insert(QString::fromStdString(bus.id));
    } else {
        for (const QString &id : masterIds) {
            if (!id.isEmpty()) pendingSettleMasters_.insert(id);
        }
    }
    if (pendingSettleMasters_.isEmpty()) return false;
    // Fresh settle: quiet phase first. Restarting the timer cancels an in-flight
    // quiet wait (single-shot).
    settlePass_ = 0;
    settleBatch_.clear();
    settleQueue_.clear();
    captureSettleTimer_.setInterval(1500);
    captureSettleTimer_.start();
    return true;
}

QStringList MixerService::mastersForCaptureNode(const QString &nodeName) const {
    QStringList out;
    if (nodeName.isEmpty()) return out;
    for (const MasterBusState &m : config_.live().masterBuses) {
        const QString match = effectiveMasterCaptureMatch(m.id, m.captureMatch);
        if (!match.isEmpty() && nodeName.startsWith(match)) out << m.id;
    }
    return out;
}

void MixerService::reportStalledPaths(int atMs) {
    if (!graph_) return;
    const std::vector<std::string> stalled = graph_->stalledPaths();
    if (stalled.empty()) return;

    QStringList names;
    names.reserve(static_cast<int>(stalled.size()));
    for (const std::string &n : stalled) names << QString::fromStdString(n);
    qWarning("waveline: %d path node(s) linked but not scheduled at %dms — "
             "these carry no audio though every link is present: %s",
             static_cast<int>(stalled.size()), atMs,
             qUtf8Printable(names.join(QStringLiteral(", "))));
    warnIfDescriptorsExhausted();
}

void MixerService::warnIfDescriptorsExhausted() {
    // The server allocates a memfd and eventfds per node, port, link and buffer,
    // and every filter instance here is its own client connection, so a full
    // mixer graph is genuinely expensive. On the stock 1024 soft limit a busy
    // setup reaches it, and then pw_core_create_object starts failing: loopbacks
    // instantiate but never get a driver and sit suspended, links are created
    // but stay in "init", and a mix goes silent while its meter still moves.
    // None of that looks like a descriptor problem, which is why it is worth
    // naming explicitly rather than leaving someone to find it.
    QDir procs(QStringLiteral("/proc"));
    const QStringList pids =
        procs.entryList(QStringList{QStringLiteral("[0-9]*")}, QDir::Dirs);
    for (const QString &pid : pids) {
        QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
        if (!comm.open(QIODevice::ReadOnly)) continue;
        if (comm.readAll().trimmed() != QByteArray("pipewire")) continue;

        const int used =
            QDir(QStringLiteral("/proc/%1/fd").arg(pid))
                .entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot |
                           QDir::System)
                .size();
        QFile limits(QStringLiteral("/proc/%1/limits").arg(pid));
        if (used <= 0 || !limits.open(QIODevice::ReadOnly)) return;

        long soft = 0;
        while (!limits.atEnd()) {
            const QString line = QString::fromLatin1(limits.readLine());
            if (!line.startsWith(QLatin1String("Max open files"))) continue;
            soft = QStringView{line}.mid(14).trimmed().split(QLatin1Char(' '),
                                                             Qt::SkipEmptyParts)
                       .value(0)
                       .toLong();
            break;
        }
        if (soft <= 0 || used < soft * 9 / 10) return;

        qWarning("waveline: the PipeWire server is near its file-descriptor "
                 "limit (%d of %ld). Past it, nodes and links fail to start and "
                 "parts of the mixer go silent without an obvious error. Raise "
                 "it with LimitNOFILE in a systemd drop-in for pipewire.service "
                 "(the Waveline installer writes one).",
                 used, soft);
        return;
    }
}

void MixerService::healDisconnectedMidiInputs() {
    if (!graph_) return;
    engine_.sync();

    bool configChanged = false;
    Profile &p = config_.live();
    for (MasterBusState &m : p.masterBuses) {
        if (m.busType != QLatin1String("midi") || m.midiPortMatch.isEmpty())
            continue;

        const int separator = m.midiPortMatch.indexOf(QLatin1Char('|'));
        if (separator < 0 || separator + 1 >= m.midiPortMatch.size()) continue;
        const QString portName = m.midiPortMatch.mid(separator + 1);

        const waveline::MasterBusRuntime *bus =
            graph_->masterBus(m.id.toStdString());
        if (!bus) continue;

        bool currentNodeIsLive = false;
        if (!bus->midiNode.empty()) {
            for (const auto &node : engine_.nodes()) {
                if (node.name == bus->midiNode && isMidiDeviceNode(node)) {
                    currentNodeIsLive = true;
                    break;
                }
            }
        }
        if (currentNodeIsLive) continue;

        // ALSA sequencer node names can change after USB re-enumeration. The
        // selected port label is stable, so use it only when it identifies one
        // live device unambiguously.
        QStringList candidateNodes;
        for (const auto &node : engine_.nodes()) {
            if (!isMidiDeviceNode(node)) continue;
            const auto ports = engine_.outputPortNames(node.name);
            if (std::find(ports.begin(), ports.end(), portName.toStdString()) !=
                ports.end())
                candidateNodes << QString::fromStdString(node.name);
        }
        candidateNodes.removeDuplicates();
        if (candidateNodes.size() != 1) continue;

        const QString freshMatch =
            candidateNodes.first() + QLatin1Char('|') + portName;
        if (m.midiPortMatch != freshMatch) {
            m.midiPortMatch = freshMatch;
            graph_->setMasterMidiPortMatch(m.id.toStdString(),
                                           freshMatch.toStdString());
            configChanged = true;
        }

        std::string err;
        if (graph_->rewireMasterMidiInput(m.id.toStdString(), err)) {
            qInfo("waveline: MIDI device reconnected for %s -> %s",
                  qUtf8Printable(m.id), qUtf8Printable(freshMatch));
        } else {
            qInfo("waveline: MIDI reconnect pending for %s: %s",
                  qUtf8Printable(m.id), err.c_str());
        }
    }

    if (configChanged) {
        scheduleSave();
        emit Changed();
    }
}

void MixerService::finishAllPendingMidi() {
    if (!graph_) return;
    for (const MasterBusState &m : config_.live().masterBuses) {
        if (m.busType != QLatin1String("midi") || m.midiPortMatch.isEmpty())
            continue;
        const std::string sid = m.id.toStdString();

        // applyProfile() already created this chain and its stream path, with
        // the same mono/software-gain settings and the same soundfont. Tearing
        // it down and building it again here is not free: destroying six filter
        // nodes and two loopbacks re-partitions PipeWire's driver graph half a
        // second after rewireGraph() settled it, and the Monitor Mix output
        // loopback does not survive the churn -- waveline-monitor-out-0-in ends
        // up in no driver group at all and suspends, while its playback end
        // stays with the sink. Every link still reads present in pw-link and
        // the Monitor meter still moves (it taps the sink's monitor port, which
        // is fed), but no audio reaches the hardware. Only rebuild a chain that
        // genuinely failed to come up.
        const waveline::MasterBusRuntime *bus = graph_->masterBus(sid);
        if (!bus || !bus->chain.synthReady) {
            std::string err;
            graph_->rebuildMasterMidiChain(sid, err);
            if (!err.empty())
                qWarning("waveline: MIDI chain rebuild for %s: %s",
                         qUtf8Printable(m.id), err.c_str());
        } else {
            std::string pathErr;
            graph_->ensureMidiStreamPath(sid, pathErr);
            if (!pathErr.empty())
                qWarning("waveline: MIDI stream path for %s: %s",
                         qUtf8Printable(m.id), pathErr.c_str());
        }
        finishNewMidiMaster(m.id);
    }
}

void MixerService::finishNewMidiMaster(const QString &masterId) {
    if (!graph_ || masterId.isEmpty()) return;
    applyMasterMixLevels(masterId);
    applyMasterInputVolume(masterId);
    midiWireTimer_.start();

    auto wireOnce = [this, masterId] {
        if (!graph_) return;
        const std::string sid = masterId.toStdString();
        std::string audioErr;
        // Audio chain first, and the MIDI bridge only once it is active -- see
        // the driver-group note on midiWireTimer_. Attaching the bridge to a
        // chain that has not settled captures it into the bridge's scheduling
        // group and the chain can never finish wiring.
        if (!graph_->linkPendingMidiAudioPaths(audioErr)) {
            if (!audioErr.empty())
                qInfo("waveline: MIDI audio wire for %s: %s",
                      qUtf8Printable(masterId), audioErr.c_str());
            return;
        }
        std::string inputErr;
        graph_->linkPendingMidiInputs(inputErr);
        if (!inputErr.empty())
            qInfo("waveline: MIDI input wire for %s: %s", qUtf8Printable(masterId),
                  inputErr.c_str());
        std::string verifyErr;
        if (graph_->verifyMasterMixWiring(sid, verifyErr))
            qInfo("waveline: MIDI device %s wired", qUtf8Printable(masterId));
    };
    for (int ms : {200, 600, 1500, 3000})
        QTimer::singleShot(ms, this, wireOnce);
    QTimer::singleShot(500, this, [this, masterId] { syncMasterMeters(masterId); });
}

void MixerService::finishMasterCaptureRebuild(const QString &masterId) {
    if (!graph_ || masterId.isEmpty()) return;
    const std::string sid = masterId.toStdString();
    if (const MasterBusState *m = masterBusState(config_.live(), masterId))
        graph_->setMasterSoftwareMonitor(sid, m->softwareMonitor);

    applyMasterFx(masterId);
    applyMasterInputVolume(masterId);
    applyMasterMixLevels(masterId);
    const Profile &p = config_.live();
    for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
        if (!it->micSource || !it->masterMicIds.contains(masterId)) continue;
        std::string chErr;
        graph_->rewireChannelMicSource(it.key().toStdString(), chErr);
        graph_->setChannelMicMonitor(it.key().toStdString(), it->micMonitor);
        applyChannelFx(it.key());
    }
    if (masterId == QLatin1String("mic")) applyMicFx();

    // module-loopback rewrites channelVolumes while the stream settles; keep
    // pushing the profile levels until it sticks.
    for (int ms : {50, 150, 400}) {
        QTimer::singleShot(ms, this, [this, masterId] {
            if (graph_) applyMasterMixLevels(masterId);
        });
    }
    syncAlsaAliases();

    // Heal only this master's mix legs. A full scheduleRewire() clears every
    // manual link in the graph — rebuilding C922 used to silence Wave:3, Meet,
    // and all channel paths when verify latched onto an unrelated master.
    std::string verifyErr;
    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
        if (attempt > 0) {
            std::string wireErr;
            engine_.sync();
            QThread::msleep(80 * attempt);
            if (!graph_->wireMasterPaths(sid, wireErr)) {
                qWarning("waveline: rewire master %s after rebuild (attempt %d): %s",
                         qUtf8Printable(masterId), attempt + 1, wireErr.c_str());
            }
            applyMasterMixLevels(masterId);
            engine_.sync();
        }
        ok = graph_->verifyMasterMixWiring(sid, verifyErr);
    }
    if (!ok) {
        qWarning("waveline: master %s mix wiring incomplete after rebuild: %s",
                 qUtf8Printable(masterId), verifyErr.c_str());
    }
    // The published source was torn down and rebuilt above, taking this bus's
    // meter tap with it.
    syncMasterMeters(masterId);
}

void MixerService::rebuildCaptureHops() {
    if (!graph_) return;

    // Phase 0: drop ALSA consumers and wait. Recreating while the device is
    // still negotiating (or doing a second recreate after a good hop) is what
    // left C922 "starts fine then robotic".
    if (settlePass_ == 0) {
        settleBatch_ = pendingSettleMasters_.values();
        pendingSettleMasters_.clear();
        settleQueue_.clear();
        if (settleBatch_.isEmpty()) return;

        for (const QString &id : settleBatch_) {
            if (auto *bus = graph_->masterBus(id.toStdString())) {
                if (!bus->captureNode.empty())
                    engine_.forgetLinksForNode(bus->captureNode);
                bus->captureNode.clear();
            }
        }
        settlePass_ = 1;
        qInfo("waveline: capture quiet for %d master(s), rebuild in 1.2s",
              static_cast<int>(settleBatch_.size()));
        captureSettleTimer_.setInterval(1200);
        captureSettleTimer_.start();
        return;
    }

    // Phase 1: one full DSP recreate per master (no second pass).
    if (settleQueue_.isEmpty()) {
        for (const QString &id : pendingSettleMasters_) {
            if (!settleBatch_.contains(id)) settleBatch_.append(id);
        }
        pendingSettleMasters_.clear();
        settleQueue_ = settleBatch_;
    }
    if (settleQueue_.isEmpty()) {
        settlePass_ = 0;
        settleBatch_.clear();
        captureSettleTimer_.setInterval(1500);
        return;
    }

    const QString id = settleQueue_.takeFirst();
    std::string err;
    if (!graph_->rebuildMasterHwCapture(id.toStdString(), err)) {
        qWarning("waveline: capture hop rebuild for %s: %s", qUtf8Printable(id),
                 err.c_str());
    } else {
        finishMasterCaptureRebuild(id);
        captureHealOnAppear_.remove(id);
        if (masterHasWave3Hw(id)) pollMasterHardware(id);
        qInfo("waveline: rebuilt capture hop for master '%s'", qUtf8Printable(id));
    }

    if (!settleQueue_.isEmpty() || !pendingSettleMasters_.isEmpty()) {
        QTimer::singleShot(50, this, &MixerService::rebuildCaptureHops);
        return;
    }

    settlePass_ = 0;
    settleBatch_.clear();
    captureSettleTimer_.setInterval(1500);
}

void MixerService::rewireChannelMonitorPath(const QString &channelId) {
    if (!graph_) return;
    std::string err;
    if (!graph_->rewireChannelMonitor(channelId.toStdString(), err))
        qWarning("waveline: monitor rewire for %s: %s", qUtf8Printable(channelId),
                 err.c_str());
}

void MixerService::rewireMicMonitorPath() {
    if (!graph_) return;
    std::string err;
    if (!graph_->rewireMicMonitor(err))
        qWarning("waveline: mic monitor rewire: %s", err.c_str());
}

void MixerService::syncMasterMeters(const QString &masterId) {
    if (!meters_ || !graph_) return;

    auto syncOne = [this](const QString &id) {
        const std::string sid = id.toStdString();
        const std::string srcKey = sid + "-src";
        meters_->unwatch(srcKey);

        const waveline::MasterBusRuntime *bus = graph_->masterBus(sid);
        if (!bus || !bus->chain.sourceReady) return;

        std::string err;
        // Filter nodes are not capture sources. Targeting one can silently
        // attach the probe to the default microphone instead. The virtual
        // source carries this bus's fully processed audio -- and it is where
        // Audio Sharing joins the bus (linkSoundShareNode), so a probe on any
        // earlier node reads the microphone alone and shared applications
        // never move the meter.
        meters_->watch(srcKey, waveline::masterSourceNode(sid), err, false);
    };

    if (!masterId.isEmpty()) {
        syncOne(masterId);
        return;
    }
    for (const MasterBusState &m : config_.live().masterBuses) syncOne(m.id);
}

// wireMicPaths() skips a capture device that is not in the registry, and the
// rewire that follows takes seconds -- all of it with captureHotplugArmed_
// still false, so a microphone plugged in during that window is dropped by the
// hotplug handler. The old behaviour covered this by accident: an absent device
// failed the rewire, and each of the eight retries re-attempted the wire. That
// storm is gone, so the recovery has to be deliberate.
//
// Nothing to do in the normal case: a device that got wired has a capture node,
// and one that is still unplugged has no matching node.
void MixerService::wireCaptureDevicesThatAppeared() {
    if (!graph_) return;
    QStringList need;
    for (const MasterBusState &m : config_.live().masterBuses) {
        if (m.busType == QLatin1String("midi")) continue;
        const std::string sid = m.id.toStdString();
        const waveline::MasterBusRuntime *bus = graph_->masterBus(sid);
        if (!bus || !bus->captureNode.empty()) continue;
        const QString match = effectiveMasterCaptureMatch(m.id, m.captureMatch);
        if (match.isEmpty()) continue;
        if (graph_->findCaptureNode(match.toStdString()).empty()) continue;
        qInfo("waveline: capture for master '%s' appeared during startup -- "
              "wiring it now",
              qUtf8Printable(m.id));
        graph_->setMasterCaptureMatch(sid, match.toStdString());
        // Same warm-up then settled rebuild the hotplug path uses.
        std::string primeErr;
        if (!graph_->primeMasterHwCapture(sid, primeErr)) {
            qWarning("waveline: capture warm-up for %s: %s",
                     qUtf8Printable(m.id), primeErr.c_str());
        }
        need << m.id;
    }
    if (!need.isEmpty()) scheduleCaptureSettle(need);
}

void MixerService::rewireGraph() {
    if (!graph_) return;
    engine_.clearLinks();
    engine_.sync();
    // Each step clears `err` on entry, so a failure has to be latched here or
    // the next successful step erases it and the retry below never fires.
    std::string err;
    bool ok = true;
    if (!graph_->ensureChannelPaths(err)) {
        ok = false;
        lastError_ = QStringLiteral("channel paths missing: %1")
                         .arg(QString::fromStdString(err));
        qWarning("waveline: channel paths missing: %s", err.c_str());
    }
    engine_.sync();
    graph_->ensureChannelFilters(err);
    engine_.sync();
    QThread::msleep(800);
    engine_.sync();
    const Profile &p = config_.live();
    for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
        if (!it->effectsEnabled) continue;
        const std::string id = it.key().toStdString();
        if (wantsInputNoiseFilter(p, *it))
            graph_->ensureChannelNoiseFilter(id, waveline::FxStage::Input);
        if (wantsOutputNoiseFilter(p, *it))
            graph_->ensureChannelNoiseFilter(id, waveline::FxStage::Output);
    }
    engine_.sync();
    if (!graph_->wireMicPaths(err)) {
        ok = false;
        lastError_ = QStringLiteral("mic path not wired: %1")
                         .arg(QString::fromStdString(err));
        qWarning("waveline: mic path not wired: %s", err.c_str());
    }
    // Push filter settings before wiring so nodes come up with the right chain.
    for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it)
        applyChannelFx(it.key());
    if (!graph_->wireAllChannelFx(err)) {
        ok = false;
        lastError_ = QStringLiteral("channel fx not wired: %1")
                         .arg(QString::fromStdString(err));
        qWarning("waveline: channel fx not wired: %s", err.c_str());
    }
    applySoundShareTargets();
    // Re-applied after a rewire because the filters may have been rebuilt, and
    // a rebuilt filter comes up with defaults rather than the user's chain.
    for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it)
        applyChannelFx(it.key());
    applyMicFx();
    // clearLinks + wireMicPaths recreates mix loopbacks at unity gain. Profile
    // volumes were applied earlier against the old paths — push them again.
    for (const MasterBusState &m : p.masterBuses) applyMasterMixLevels(m.id);
    // Same for the channel faders: the graph keeps the levels (snapshot() reads
    // them from it), but the loopbacks they live on were just recreated.
    graph_->applyAllChannelPathLevels();
    graph_->applyAllMonitorOutputGains();
    graph_->setStreamMixVolume(static_cast<float>(p.streamMixVolume));
    graph_->setStreamMixMuted(p.streamMixMuted);
    for (int ms : {50, 150, 400}) {
        QTimer::singleShot(ms, this, [this] {
            if (!graph_) return;
            for (const MasterBusState &m : config_.live().masterBuses)
                applyMasterMixLevels(m.id);
            graph_->applyAllMonitorOutputGains();
            const Profile &live = config_.live();
            graph_->setStreamMixVolume(static_cast<float>(live.streamMixVolume));
            graph_->setStreamMixMuted(live.streamMixMuted);
            std::string bindErr;
            graph_->refreshMonitorOutputBindings(bindErr);
            // Safety net for the one case the check above cannot see: a
            // loopback that *was* recreated writes its level by node name, and
            // if the replacement node is not registered yet that write lands on
            // the outgoing one. Re-asserting the levels once the dust settles
            // costs nothing and is the difference between a quiet output and a
            // painful one.
            QTimer::singleShot(600, this, [this] {
                if (graph_) graph_->applyAllMonitorOutputGains();
            });
            QTimer::singleShot(2000, this, [this] {
                if (graph_) graph_->applyAllMonitorOutputGains();
            });
        });
    }
    for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
        if (it->micSource)
            graph_->setChannelMicMonitor(it.key().toStdString(), it->micMonitor);
    }
    if (routing_ && router_) scheduleRouteAll();
    if (ok) {
        std::string verifyErr;
        if (!graph_->verifyMixWiring(verifyErr)) {
            ok = false;
            lastError_ = QStringLiteral("mix wiring incomplete: %1")
                             .arg(QString::fromStdString(verifyErr));
            qWarning("waveline: mix wiring incomplete: %s", verifyErr.c_str());
        }
    }
    if (ok && meters_) {
        // Meter taps attach at daemon start, before the graph is fully linked.
        // Reconnect bus meters after a successful rewire so they follow the live mix.
        std::string meterErr;
        meters_->unwatch("monitor-mix");
        meters_->watch("monitor-mix", waveline::MixerGraph::kMonitorMix, meterErr);
        meters_->unwatch("stream-mix");
        meters_->watch("stream-mix", waveline::MixerGraph::kStreamMix, meterErr);
    }
    if (!ok && rewireAttempts_ < 8) {
        ++rewireAttempts_;
        qWarning("waveline: rewire retry %d/8 in %dms (%s)",
                 rewireAttempts_, 400 * rewireAttempts_,
                 qUtf8Printable(lastError_));
        rewireTimer_.start(400 * rewireAttempts_);
    } else if (ok) {
        // Do not scheduleCaptureSettle here. The graph is already linked and
        // working; a quiet+rebuild pass tears that down for a "heal" nobody
        // asked for on a clean start. Hotplug uses captureHealOnAppear_.
        captureHotplugArmed_ = true;
        wireCaptureDevicesThatAppeared();
    }

    relinkTunerMidi();

    if (graph_) {
        std::string midiAudioErr;
        graph_->linkPendingMidiAudioPaths(midiAudioErr);
        if (!midiAudioErr.empty())
            qInfo("waveline: MIDI audio pending: %s", midiAudioErr.c_str());
        std::string midiErr;
        graph_->linkPendingMidiInputs(midiErr);
        if (!midiErr.empty())
            qInfo("waveline: MIDI input pending: %s", midiErr.c_str());
        bool hasMidi = false;
        for (const MasterBusState &m : config_.live().masterBuses) {
            if (m.busType == QLatin1String("midi")) {
                hasMidi = true;
                break;
            }
        }
        if (hasMidi)
            midiWireTimer_.start();
        else
            midiWireTimer_.stop();
    }
    syncMasterMeters();
    // Again once the graph has settled. A meter probe refuses to attach to
    // anything but its own source now, so one created while that source was
    // being recreated simply reads nothing until it is re-armed here.
    QTimer::singleShot(2500, this, [this] { syncMasterMeters(); });
    // Every rewire follows a device arriving or leaving, which is exactly when
    // there is a new name to remember.
    rememberMasterDeviceLabels();
}

void MixerService::captureToProfile() { config_.live() = snapshot(); }

Profile MixerService::snapshot() const {
    Profile p = config_.live();  // keeps fields we do not own here
    if (graph_) {
        p.channels.clear();
        for (const auto &c : graph_->channels()) {
            ChannelState s;
            s.streamVolume = c.streamVolume;
            s.monitorVolume = c.monitorVolume;
            s.streamMuted = c.streamMuted;
            s.monitorMuted = c.monitorMuted;
            p.channels.insert(QString::fromStdString(c.id), s);
        }
        p.monitorOutputs.clear();
        for (const waveline::MonitorOutputEntry &e : graph_->monitorOutputs()) {
            MonitorOutputState s;
            s.sink = QString::fromStdString(e.sink);
            s.description = QString::fromStdString(e.description);
            s.volume = e.volume;
            s.muted = e.muted;
            p.monitorOutputs.append(s);
        }
        p.streamMixVolume = graph_->streamMixVolume();
        p.streamMixMuted = graph_->streamMixMuted();
    }
    p.routingEnabled = routing_;
    if (graph_) {
        p.softwareMonitor = graph_->softwareMonitor();
        p.micStereo = graph_->micStereo();
        // monitorLevel stays the canonical field for the microphone's level in
        // the Monitor mix -- it predates the mic block and existing configs
        // carry it. mic.monitorVolume is written too so the file reads
        // consistently, but it is not the value that is loaded.
        p.monitorLevel = micMonitorLevel_;
        // The stored level, not the live one: while muted the graph is at
        // zero and saving that would lose the level being restored on unmute.
        if (!monitorMasterMuted_) p.monitorMaster = graph_->monitorMasterVolume();
        p.monitorMasterMuted = monitorMasterMuted_;
        p.mic.monitorVolume = micMonitorLevel_;
        p.mic.streamVolume = micStreamLevel_;
        p.mic.streamMuted = micStreamMuted_;
        p.mic.monitorMuted = micMonitorMuted_;
        if (auto *nc = graph_->noiseFilter()) p.noiseIntensity = nc->intensity();
        // The requested engine, not the running one: falling back to RNNoise
        // because DeepFilterNet was missing must not rewrite the user's choice.
        p.noiseEngine = QString::fromLatin1(
            waveline::noiseEngineId(graph_->requestedNoiseEngine()));
    }
    // Only while the device is actually present: reading zeros off a
    // disconnected microphone and saving them would erase the very settings this
    // is here to restore.
    syncPrimaryBusFromLegacy(p);
    for (MasterBusState &m : p.masterBuses) {
        if (const MasterHwSlot *hw = masterHwSlot(m.id); hw && hw->connected) {
            m.hardwareMonitor = hw->state.monitorPercent;
            m.micGainDb = hw->state.micGainDb;
            m.hwClipguard = hw->state.clipguard ? 1 : 0;
            m.hwMicMuted = hw->state.micMuted ? 1 : 0;
            m.hwHpVolumeDb = hw->state.hpVolumeDb;
            m.hwHpMuted = hw->state.hpMuted ? 1 : 0;
        }
        if (m.id == QLatin1String("mic")) {
            m.mix.streamVolume = micStreamLevel_;
            m.mix.monitorVolume = micMonitorLevel_;
            m.mix.streamMuted = micStreamMuted_;
            m.mix.monitorMuted = micMonitorMuted_;
            m.micStereo = graph_ ? graph_->micStereo() : m.micStereo;
            m.micMonitorFx = graph_ ? graph_->micMonitorFx() : m.micMonitorFx;
        }
        if (const waveline::MasterBusRuntime *rt = graph_->masterBus(m.id.toStdString())) {
            if (!rt->captureMatch.empty())
                m.captureMatch = QString::fromStdString(rt->captureMatch);
            else if (m.id == QLatin1String("mic"))
                m.captureMatch = effectiveMasterCaptureMatch(m.id, m.captureMatch);
            if (m.id != QLatin1String("mic"))
                m.softwareMonitor = rt->softwareMonitor;
        }
    }
    syncLegacyFromPrimaryBus(p);
    if (graph_) {
        for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
            QStringList ids;
            for (const std::string &id :
                 graph_->channelMasterMics(it.key().toStdString()))
                ids << QString::fromStdString(id);
            it->masterMicIds = ids;
        }
    }

    p.soundSharing.enabled = soundSharingEnabled_;
    p.soundSharing.streamVolume = soundShareStreamLevel_;
    p.soundSharing.monitorVolume = soundShareMonitorLevel_;
    p.soundSharing.streamMuted = soundShareStreamMuted_;
    p.soundSharing.monitorMuted = soundShareMonitorMuted_;
    if (soundShare_) {
        p.soundSharing.apps.clear();
        for (const auto &pattern : soundShare_->apps())
            p.soundSharing.apps.append(QString::fromStdString(pattern));
    }

    if (router_) {
        p.rules.clear();
        for (const auto &r : router_->rules()) {
            RuleState rs;
            rs.pattern = QString::fromStdString(r.pattern);
            rs.channel = QString::fromStdString(r.channelId);
            p.rules.append(rs);
        }
        p.appChannels.clear();
        for (const auto &[key, channel] : router_->manualOverrides()) {
            if (!waveline::isStableIdentityKey(key)) continue;
            p.appChannels.insert(QString::fromStdString(key),
                                 QString::fromStdString(channel));
        }
    }
    if (graph_) {
        if (auto *fx = graph_->micEffects())
            p.micFx = fromFxSettings(fx->settings());
        if (auto *dyn = graph_->micDynamics())
            p.micDynamics = fromDynamicsSettings(dyn->settings());
        // Channel effects are NOT read back from the filters. They are already
        // in p (a copy of the live profile) because every setter writes there
        // first, and a bypassed channel's filters hold neutral values --
        // harvesting those would overwrite the user's chain with a flat one the
        // moment anything triggered a save.
        for (const auto &c : graph_->channels()) {
            const QString id = QString::fromStdString(c.id);
            auto it = p.channelEffects.find(id);
            if (it == p.channelEffects.end())
                it = p.channelEffects.insert(id, ChannelEffectsState{});
            it->micGain = c.micSend;
            it->micMuted = graph_->channelMicMuted(c.id);
        }
    }
    return p;
}

// The primary strip used to drive monitorMaster (a global gain on every
// output). Fold it into the per-output levels so each device's slider is
// authoritative, matching how Stream mix output volume already works.
void MixerService::foldMonitorMaster(Profile &p) {
    if (p.monitorMaster == 1.0 && !p.monitorMasterMuted) return;
    if (p.monitorMasterMuted) {
        for (MonitorOutputState &s : p.monitorOutputs) s.muted = true;
    } else {
        for (MonitorOutputState &s : p.monitorOutputs) {
            if (!s.muted) s.volume = qBound(0.0, s.volume * p.monitorMaster, 1.0);
        }
    }
    p.monitorMaster = 1.0;
    p.monitorMasterMuted = false;
}

void MixerService::applyProfile() {
    Profile &p = config_.live();
    applyCreatedNodes_ = false;
    foldMonitorMaster(p);

    if (graph_) {
        for (auto it = p.channels.begin(); it != p.channels.end(); ++it) {
            const std::string id = it.key().toStdString();
            graph_->setVolume(id, waveline::Mix::Stream,
                              static_cast<float>(it->streamVolume));
            graph_->setVolume(id, waveline::Mix::Monitor,
                              static_cast<float>(it->monitorVolume));
            graph_->setMuted(id, waveline::Mix::Stream, it->streamMuted);
            graph_->setMuted(id, waveline::Mix::Monitor, it->monitorMuted);
        }
        // Master before the output: setMonitorOutput re-applies whatever the
        // master currently is when it (re)creates the path.
        monitorMasterMuted_ = p.monitorMasterMuted;
        graph_->setMonitorMasterVolume(static_cast<float>(p.monitorMaster));
        graph_->setMonitorMasterMuted(monitorMasterMuted_);
        if (!p.monitorOutputs.isEmpty()) {
            std::vector<waveline::MonitorOutputEntry> outs;
            outs.reserve(static_cast<size_t>(p.monitorOutputs.size()));
            for (const MonitorOutputState &s : p.monitorOutputs) {
                waveline::MonitorOutputEntry e;
                e.sink = s.sink.toStdString();
                e.description = s.description.toStdString();
                e.volume = static_cast<float>(s.volume);
                e.muted = s.muted;
                outs.push_back(e);
            }
            std::string err;
            graph_->setMonitorOutputs(outs, err);
        } else {
            // Nothing saved. The graph picked a default output at build time
            // but deliberately left it unbuilt; this is the first moment the
            // levels are known, so it is the first moment it may be built.
            const std::vector<waveline::MonitorOutputEntry> dflt =
                graph_->monitorOutputs();
            if (!dflt.empty()) {
                std::string err;
                graph_->setMonitorOutputs(dflt, err);
            }
            // Loopback nodes appear async; re-push levels once paths exist.
            for (int ms : {50, 150, 400}) {
                QTimer::singleShot(ms, this, [this] {
                    if (graph_) graph_->applyAllMonitorOutputGains();
                });
            }
        }
        graph_->setStreamMixVolume(static_cast<float>(p.streamMixVolume));
        graph_->setStreamMixMuted(p.streamMixMuted);
        if (!p.noiseEngine.isEmpty()) {
            std::string engineErr;
            const auto want = waveline::noiseEngineFromId(p.noiseEngine.toStdString());
            if (!graph_->setNoiseEngine(want, engineErr) &&
                want != waveline::NoiseEngine::RnNoise) {
                qWarning("waveline: noise engine %s unavailable, using RNNoise (%s)",
                         qUtf8Printable(p.noiseEngine), engineErr.c_str());
            }
        }
        applyMasterBuses();
        for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
            const std::string id = it.key().toStdString();
            if (it->effectsEnabled) {
                // Whether a filter had to be built matters to the caller: a
                // node that was just created has no links yet, and only a
                // rewire puts them there.
                if (it->input.noiseSuppression)
                    applyCreatedNodes_ |= graph_->ensureChannelNoiseFilter(
                        id, waveline::FxStage::Input);
                if (it->output.noiseSuppression)
                    applyCreatedNodes_ |= graph_->ensureChannelNoiseFilter(
                        id, waveline::FxStage::Output);
            }
            applyChannelFx(it.key());
            graph_->setChannelMonitorFx(id, it->monitorFx);
            graph_->setChannelMicSource(id, it->micSource);
            graph_->setChannelMicUseDeviceFx(id, it->inputUseDeviceFx);
            graph_->setChannelMicMonitor(id, it->micMonitor);
            graph_->setChannelMicSend(id, static_cast<float>(it->micGain));
            graph_->setChannelMicMuted(id, it->micMuted);
        }
    }

    if (router_ && !p.rules.isEmpty()) {
        std::vector<waveline::RoutingRule> rules;
        for (const auto &r : p.rules)
            rules.push_back({r.pattern.toStdString(), r.channel.toStdString()});
        router_->setRules(std::move(rules));
    }
    if (router_ && !p.appChannels.isEmpty()) {
        std::map<std::string, std::string> pins;
        for (auto it = p.appChannels.begin(); it != p.appChannels.end(); ++it)
            pins[it.key().toStdString()] = it.value().toStdString();
        router_->setManualOverrides(std::move(pins));
    }
    routing_ = p.routingEnabled;
    if (routing_ && router_) {
        std::string routeErr;
        router_->start(routeErr);
    }

    if (soundShare_) {
        soundSharingEnabled_ = p.soundSharing.enabled;
        soundShare_->setEnabled(soundSharingEnabled_);
        soundShareStreamLevel_ = p.soundSharing.streamVolume;
        soundShareMonitorLevel_ = p.soundSharing.monitorVolume;
        soundShareStreamMuted_ = p.soundSharing.streamMuted;
        soundShareMonitorMuted_ = p.soundSharing.monitorMuted;
        std::vector<std::string> patterns;
        for (const QString &a : p.soundSharing.apps)
            patterns.push_back(a.toStdString());
        soundShare_->setApps(std::move(patterns));
        if (graph_) {
            graph_->setSoundShareVolume(waveline::Mix::Stream,
                                        static_cast<float>(soundShareStreamLevel_));
            graph_->setSoundShareVolume(waveline::Mix::Monitor,
                                        static_cast<float>(soundShareMonitorLevel_));
            graph_->setSoundShareMuted(waveline::Mix::Stream, soundShareStreamMuted_);
            graph_->setSoundShareMuted(waveline::Mix::Monitor, soundShareMonitorMuted_);
        }
    }

    updateStreamRouting();
    if (p.routingEnabled) routing_ = true;
    if (p.soundSharing.enabled) {
        soundSharingEnabled_ = true;
        soundShare_->setEnabled(true);
        soundShare_->routeAll();
    }
    // Order matters: the stages have to exist before streams are pointed at
    // them, and rebuildAppGainStages() is what creates the ones this profile
    // asks for.
    rebuildAppGainStages();
    applyAppVolumes();

    // Apply saved hardware gain when a profile is loaded.
    if (const MasterBusState *m = masterBusState(config_.live(), QStringLiteral("mic"));
        m && m->micGainDb >= 0.0) {
        if (MasterHwSlot *hw = masterHwSlot(QStringLiteral("mic")); hw && hw->dev.isOpen()) {
            if (auto r = hw->dev.setMicGainDb(m->micGainDb); r)
                hw->state.micGainDb = m->micGainDb;
        }
    }

    syncAlsaAliases();
    emit Changed();
}

double MixerService::MicInputVolume() const {
    return config_.live().micInputVolume;
}

void MixerService::SetMicInputVolume(double volume) {
    config_.live().micInputVolume = std::clamp(volume, 0.0, 1.0);
    if (MasterBusState *m = masterBusState(config_.live(), QStringLiteral("mic")))
        m->micInputVolume = config_.live().micInputVolume;
    applyMicInputVolume();
    scheduleSave();
    emit Changed();
}

bool MixerService::MicInputMuted() const {
    return config_.live().micInputMuted;
}

void MixerService::SetMicInputMuted(bool muted) {
    config_.live().micInputMuted = muted;
    if (MasterBusState *m = masterBusState(config_.live(), QStringLiteral("mic")))
        m->micInputMuted = muted;
    applyMicInputVolume();
    scheduleSave();
    emit Changed();
}

void MixerService::applyMicInputVolume() {
    applyMasterInputVolume(QStringLiteral("mic"));
}

void MixerService::followDefaultSource() {
    if (!graph_) return;
    const std::string before = graph_->micNode();
    if (!graph_->reconsiderMicNode()) {
        // Nothing changed, and the user has quite possibly just tried to change
        // it. Saying why beats silence, which looks identical to the daemon not
        // having noticed at all -- and the usual reason is a deliberate refusal
        // rather than a failure.
        const std::string dflt = engine_.defaultSourceName();
        if (!profile_.alsaNodeMatch.empty()) {
            qInfo("waveline: default input changed, but this profile pins its "
                  "microphone to '%s' -- ignoring",
                  profile_.alsaNodeMatch.c_str());
        } else if (!dflt.empty() && dflt != before) {
            qInfo("waveline: default input is now '%s', which is not usable as "
                  "a microphone -- keeping '%s'",
                  dflt.c_str(), before.empty() ? "(none)" : before.c_str());
        }
        return;
    }
    qInfo("waveline: default input changed, microphone is now '%s'",
          graph_->micNode().empty() ? "(none)" : graph_->micNode().c_str());
    // The gain and mute belong to the microphone, not to the device that
    // happened to be there before it. A new one arrives at whatever level the
    // system left it at, so push ours onto it.
    applyMicInputVolume();
    // A full rewire rather than moving the one link: the microphone feeds the
    // noise filter, the EQ, the published source, both mixes and every
    // channel's own mic send, and rewireGraph() is the routine that already
    // knows all of those. Debounced, so flipping through devices in the sound
    // settings rebuilds once at the end rather than once per click.
    scheduleRewire();
    emit Changed();
}

void MixerService::updateStreamRouting() {
    // WirePlumber restarting wipes every routing decision we made: they live in
    // its "default" metadata, which is destroyed with it and comes back empty.
    // No stream node is added or removed, so none of the handlers below fire
    // and the applications simply stay wherever the fresh session manager put
    // them -- until wavelined itself is restarted.
    engine_.setOnSessionManagerRestarted([this] {
        QMetaObject::invokeMethod(
            this,
            [this] {
                reapplyStreamRouting();
                healMixWiringAfterSessionRestart();
            },
            Qt::QueuedConnection);
    });

    engine_.setOnNodeAdded([this](const waveline::PwNode &n) {
        // Registry and stream property callbacks run on PipeWire's thread loop.
        // Router state and Qt live on the daemon thread, so queue the work.
        QMetaObject::invokeMethod(
            this,
            [this, n] {
                bool overridesChanged = false;
                if (router_) overridesChanged = router_->refreshManualPinForNode(n);
                std::string err;
                if (soundSharingEnabled_ && soundShare_ &&
                    soundShare_->routeNode(n, err))
                    return;
                if (routing_ && router_ && router_->routeNode(n, err)) {
                    // A stream that has just been placed is exactly when
                    // another routing daemon takes it, and this path does not
                    // go through the retry ladder -- so arm the watch here too,
                    // or the commonest case of all (an app being launched)
                    // would be the one case never checked.
                    scheduleRouteVerify();
                }
                applyAppVolume(n.id);
                const QString app =
                    QString::fromStdString(waveline::appDisplayName(n));
                if (soundSharingEnabled_ &&
                    config_.live().soundSharing.appTargets.contains(app))
                    applySoundShareTargetForApp(app);
                // Everything above is too early for a stream that has just
                // started: its ports are not in the registry yet, so there is
                // nothing to link, and the session manager restores its own
                // stored volume a moment later, over ours. Both made a
                // restarted stream come back unshared and at 100% until the
                // target was re-picked by hand. Retry on a short ladder --
                // relinking keeps live links (linkPorts is a no-op on a pair
                // that is already up) and the volume is simply written again.
                //
                // The volume half only fires for apps with a stored level, and
                // if the app moves its own volume inside the ladder's two
                // seconds that change is adopted first, so a later rung writes
                // the app's value rather than reverting it.
                if (!n.isOurs && n.mediaClass == "Stream/Output/Audio") {
                    for (int ms : {250, 800, 2000}) {
                        QTimer::singleShot(ms, this, [this, n] {
                            if (!streamStillPresent(n)) return;
                            applyAppVolume(n.id);
                            // Resolved now, not at announce time: PipeWire
                            // fills application.process.* in later, and the
                            // name it settles on is the one the config is
                            // keyed by.
                            relinkSoundShareForApp(appNameForNode(n.id));
                        });
                    }
                }
                // Hotplug only. Startup registry floods are handled by the
                // initial graph wire and must not trigger duplicate links.
                if (captureHotplugArmed_ && !n.isOurs &&
                    n.mediaClass == "Audio/Source" &&
                    n.name.rfind("alsa_input.", 0) == 0) {
                    const QStringList ids =
                        mastersForCaptureNode(QString::fromStdString(n.name));
                    QStringList need;
                    for (const QString &id : ids) {
                        if (captureHealOnAppear_.contains(id)) {
                            need << id;
                            continue;
                        }
                        const auto *bus = graph_->masterBus(id.toStdString());
                        if (bus && bus->captureNode.empty()) need << id;
                    }
                    for (const QString &id : need) {
                        if (const MasterBusState *m =
                                masterBusState(config_.live(), id)) {
                            const std::string sid = id.toStdString();
                            graph_->setMasterCaptureMatch(
                                sid,
                                effectiveMasterCaptureMatch(id, m->captureMatch)
                                    .toStdString());
                            // Start the ALSA stream now so its clock and
                            // adaptive resampler negotiate during the warm-up,
                            // but keep the selector output silent. The settled
                            // rebuild below is then a restart of an established
                            // stream, which is the path proven reliable by the
                            // manual Rebuild action.
                            std::string primeErr;
                            if (!graph_->primeMasterHwCapture(sid, primeErr)) {
                                qWarning("waveline: capture warm-up for %s: %s",
                                         qUtf8Printable(id), primeErr.c_str());
                            }
                        }
                    }
                    // After the hidden warm-up, drop the hardware edge, wait
                    // for a clean clock boundary, then perform the same rebuild
                    // that is reliable from the UI.
                    if (!need.isEmpty()) scheduleCaptureSettle(need);
                }
                if (graph_ && isMidiDeviceNode(n)) {
                    // Ports are registered just after their node. Try once
                    // after that short window; the MIDI retry timer remains
                    // the fallback if enumeration takes longer.
                    QTimer::singleShot(100, this, [this] {
                        healDisconnectedMidiInputs();
                    });
                }
                // Assigned Monitor sink came back — recreate its sticky path.
                if (graph_ && !n.isOurs && n.mediaClass == "Audio/Sink" &&
                    n.name.rfind("waveline-", 0) != 0) {
                    bool assigned = false;
                    for (const auto &e : graph_->monitorOutputs()) {
                        if (e.sink == n.name) {
                            assigned = true;
                            break;
                        }
                    }
                    if (assigned) {
                        std::string bindErr;
                        graph_->refreshMonitorOutputBindings(bindErr);
                        // The path this just recreated is at unity until its
                        // level lands on the *new* node. See the ladder in the
                        // constructor.
                        scheduleMonitorLevelReassert();
                        emit Changed();
                    }
                }
                // Someone started recording. Debounced and compared before it
                // reaches the bus -- see micConsumerTimer_.
                if (n.mediaClass == "Stream/Input/Audio")
                    scheduleMicConsumerSignal();
                if (overridesChanged) scheduleSave();
            },
            Qt::QueuedConnection);
    });
    engine_.setOnNodeRemoved([this](const waveline::PwNode &n) {
        QMetaObject::invokeMethod(
            this,
            [this, n] {
                // The offending sink went away -- the user quit whatever was
                // taking the streams. Clear the complaint immediately rather
                // than leaving it up until the next time something is routed.
                if (routeConflicts_.remove(QString::fromStdString(n.name)) > 0)
                    emit Changed();

                if (n.mediaClass == "Stream/Input/Audio")
                    scheduleMicConsumerSignal();

                handleHardwareNodeGone(n);
            },
            Qt::QueuedConnection);
    });
}

// Everything that has to happen when a piece of hardware leaves, with no
// opinion about how we found out. Called from onNodeRemoved(), and from
// sweepDeadHardwareNodes() for the unplug PipeWire never reports.
void MixerService::handleHardwareNodeGone(const waveline::PwNode &n) {
    if (!n.isOurs && n.mediaClass == "Audio/Source" &&
        n.name.rfind("alsa_input.", 0) == 0) {
        const QStringList ids = mastersForCaptureNode(QString::fromStdString(n.name));
        // Device is gone. Keep the selector and every downstream monitor/mix
        // link alive; only this hardware edge goes. Replug adds that edge back
        // to the same selector input, so software monitoring resumes without
        // toggling it off and on.
        for (const QString &id : ids) {
            captureHealOnAppear_.insert(id);
            graph_->silenceMasterCapture(id.toStdString());
            if (auto *bus = graph_->masterBus(id.toStdString())) {
                bus->captureNode.clear();
            }
        }
    }
    if (graph_ && isMidiDeviceNode(n)) {
        QStringList disconnected;
        for (const auto &bus : graph_->masterBuses()) {
            if (bus.busType == "midi" && bus.midiNode == n.name)
                disconnected << QString::fromStdString(bus.id);
        }
        for (const QString &id : disconnected) {
            graph_->invalidateMasterMidiInput(id.toStdString());
            qInfo("waveline: MIDI device disconnected from %s", qUtf8Printable(id));
        }
    }
    // Assigned Monitor sink left — drop its path only. Keep the assignment so
    // we never fall through to speakers / default.
    if (graph_ && !n.isOurs && n.mediaClass == "Audio/Sink" &&
        n.name.rfind("waveline-", 0) != 0) {
        bool assigned = false;
        for (const auto &e : graph_->monitorOutputs()) {
            if (e.sink == n.name) {
                assigned = true;
                break;
            }
        }
        if (assigned) {
            std::string bindErr;
            graph_->refreshMonitorOutputBindings(bindErr);
            // The path this just recreated is at unity until its level lands
            // on the *new* node. See the ladder in the constructor.
            scheduleMonitorLevelReassert();
            emit Changed();
        }
    }
}

// Waking from sleep, via logind's PrepareForSleep(false).
//
// A suspend takes every ALSA capture stream down and reopens it on resume,
// with a fresh clock and a fresh adaptive resampler, underneath a graph that
// was never told any of it happened. No node is added or removed -- the
// registry looks identical across the cycle -- so nothing in here notices, in
// exactly the way nothing noticed a WirePlumber restart or an unplug.
//
// The symptom is the one the manual Rebuild button exists for: capture comes
// back robotic, or crackling, and stays that way until something renegotiates
// the clock. That button and this are the same operation, and the journal of
// any machine that suspends shows it being pressed by hand after every wake.
//
// So do it automatically. scheduleCaptureSettle({}) with no ids means every
// master, and it is the same quiet-then-rebuild used for cold start, hotplug
// and a forced quantum change: audio is held silent while the hardware edge
// settles, then each capture hop is recreated once, cleanly.
//
// Nothing is done on the way *down*. The machine is about to stop executing;
// anything scheduled here either does not run or runs into a suspending
// kernel, and the resume path above already assumes nothing about the state it
// wakes up in.
void MixerService::onPrepareForSleep(bool goingToSleep) {
    if (goingToSleep) return;

    // Two attempts, and the delay before the first is the point. USB devices
    // re-enumerate on resume and the ALSA cards behind them come back over the
    // following seconds; a rebuild issued into that is a rebuild of whatever
    // happens to exist at the time, which on this machine is sometimes nothing
    // at all. The second attempt covers a slow enumeration, and the shared
    // flag keeps a successful first one from being torn down and redone.
    //
    // scheduleCaptureSettle() returns false when there is nothing to settle --
    // no graph, or no master bus yet -- which is what makes the retry
    // meaningful rather than a duplicate.
    auto settled = std::make_shared<bool>(false);
    for (int ms : {2500, 6000}) {
        QTimer::singleShot(ms, this, [this, ms, settled] {
            if (*settled) return;
            if (!(*settled = scheduleCaptureSettle({}))) return;
            qInfo("waveline: resumed from sleep -- settling every capture "
                  "device (%dms after wake)", ms);
        });
    }
}

// The unplug the registry does not report.
//
// PipeWire does not reliably destroy an ALSA node when its card is pulled: the
// Device object goes, udev has already forgotten the card, and the node is
// left behind running. Measured here on PipeWire 1.6.8 / WirePlumber 0.5.16,
// and not something this project's rules cause -- it reproduces with the
// device's own 51-waveline-*.conf removed.
//
// The orphan alone is harmless. With nothing attached it falls to idle and
// then suspended, and a suspended node is neither scheduled nor a driver
// candidate. What makes it audible is our own capture link: that holds it
// running, so the graph clocks it every cycle, it has no new data to give, and
// it hands back its last buffer for as long as the link is up. That is the
// loop you hear from a microphone that is not plugged in -- and on the device
// driving the graph it is worse, because a running node with node.driver set
// stays eligible to be the clock while its hardware raises no interrupts.
//
// It cannot resolve itself: the link keeps the node alive, and the live node
// keeps onNodeRemoved() from ever firing, so the handler that would drop the
// link never runs.
//
// So believe the card rather than the registry. PwNode::alsaCard is already
// carried for the latency probe, and /proc/asound/card<N> disappears the
// moment the kernel releases the card.
void MixerService::sweepDeadHardwareNodes() {
    if (!graph_) return;

    QSet<uint32_t> seen;
    for (const waveline::PwNode &n : engine_.nodes()) {
        if (n.isOurs || n.alsaCard < 0) continue;
        seen.insert(n.id);
        if (QFile::exists(QStringLiteral("/proc/asound/card%1").arg(n.alsaCard)))
            continue;
        if (deadCardNodes_.contains(n.id)) continue;
        deadCardNodes_.insert(n.id);
        qWarning("waveline: %s is still in the graph but ALSA card %d is gone "
                 "-- unlinking and destroying the orphan",
                 n.name.c_str(), n.alsaCard);
        // Order matters, and each step earns its place.
        //
        // Links first: dropping them is what silences a capture orphan, and it
        // detaches the graph from the corpse before anything is destroyed, so
        // a destroy the server refuses still leaves a working stack.
        engine_.forgetLinksForNode(n.name);
        // Then the bookkeeping onNodeRemoved() would have done had it been
        // told. Run now rather than left to the removal event, because the
        // destroy below is best-effort.
        handleHardwareNodeGone(n);
        // And then the node itself. Unlinking is not enough on its own:
        //
        //  - a monitor output is a loopback module, not a linkPorts() link, so
        //    forgetLinksForNode() above does not detach it and the orphan sink
        //    keeps a consumer and stays running;
        //  - a running orphan still carries node.driver, so it stays eligible
        //    to be the graph clock while its hardware raises no interrupts --
        //    which stalls every output on the machine, not only its own;
        //  - and it answers to the same node.name as the device's replacement,
        //    so a path re-targeted after a replug can resolve onto the corpse.
        //    A hybrid like the Wave:3 replugged on a new card index hits all
        //    three at once.
        //
        // Destroying it collapses all of that: the graph re-elects a live
        // driver, and the name resolves to one node again. The removal event
        // then arrives through the ordinary path.
        engine_.destroyRegistryObject(n.id);
    }
    // Only ids still in the registry are remembered. One that finally did go
    // leaves nothing behind, so a replug -- which gets a new id -- is treated
    // as the new device it is.
    deadCardNodes_.intersect(seen);
}

void MixerService::scheduleRouteRetry() {
    if (!routing_ || !router_) return;
    routeRetryLeft_ = 4;
    routeRetryTimer_.start();
    scheduleRouteVerify();
}

// Four passes at 500 ms: long enough to outlast the retry ladder above, so a
// stream that is being taken back is seen being taken back rather than caught
// mid-move.
void MixerService::reapplyStreamRouting() {
    if (!router_) return;

    auto pass = [this] {
        if (routing_) {
            router_->routeAll();
        } else {
            // Automatic routing is off, so the rules are not ours to apply --
            // but a stream the user pinned by hand still belongs where they put
            // it, and that pin lived in the metadata that just died.
            for (const auto &n : engine_.nodes()) {
                if (n.isOurs || n.mediaClass != "Stream/Output/Audio") continue;
                const std::string ch = router_->assignedChannel(n);
                if (ch.empty()) continue;
                std::string err;
                router_->moveStream(n.id, ch, err);
            }
        }
        // routeAll skips a stream whose channel does not resolve (no rule, no
        // process id), which would leave an app with a stage playing into a
        // sink nothing is listening to.
        for (auto it = appGainStages_.constBegin();
             it != appGainStages_.constEnd(); ++it)
            retargetAppToStage(it.key());
        if (soundSharingEnabled_ && soundShare_) soundShare_->routeAll();
    };

    // A ladder, not one pass. The replacement WirePlumber spends the next few
    // hundred milliseconds restoring its own saved targets, and a single
    // re-route issued now is simply overwritten by it -- which looks exactly
    // like having done nothing at all.
    pass();
    for (int ms : {300, 900, 2000}) QTimer::singleShot(ms, this, pass);

    scheduleRouteVerify();
    emit Changed();
}

// Verify then rewire, exactly as the startup ladder does, and for the same
// reason: rewireGraph() clears every link and builds the graph again, which is
// audible, so it is worth running only where the links really are gone.
//
// Two passes rather than one. The replacement session manager keeps re-linking
// for a few hundred milliseconds after its metadata appears -- the same window
// reapplyStreamRouting()'s ladder exists for -- and a graph repaired inside it
// is simply torn down again. scheduleRewire() is debounced, so both passes
// failing costs one rebuild, not two.
void MixerService::healMixWiringAfterSessionRestart() {
    for (int ms : {1500, 4000}) {
        QTimer::singleShot(ms, this, [this, ms] {
            if (!graph_) return;
            std::string err;
            if (graph_->verifyMixWiring(err)) return;
            qWarning("waveline: session manager restart cleared the mix wiring "
                     "at %dms (%s) -- rebuilding",
                     ms, err.c_str());
            scheduleRewire();
        });
    }
}

void MixerService::scheduleRouteVerify() {
    if (!routing_ || !router_) return;
    routeVerifyLeft_ = 4;
    routeBounces_.clear();
    routeBounceDetail_.clear();
    routeVerifyTimer_.start();
}

void MixerService::recordRouteBounces() {
    if (!router_) return;
    for (const waveline::StreamContention &c : router_->misroutedStreams()) {
        ++routeBounces_[c.nodeId];
        routeBounceDetail_[c.nodeId] = c;
    }
}

void MixerService::settleRouteConflicts() {
    const QStringList &dismissed = config_.diagnostics().dismissedRoutingSinks;
    bool changed = false;

    // A window's worth of passes reports *every* misplaced stream, not only
    // the one that started it -- so a sink that stole nothing across a whole
    // window is no longer stealing, and its warning has served its purpose.
    // This is what un-sticks the banner when someone turns the other program's
    // "process all streams" off rather than quitting it.
    QSet<QString> sinksSeen;
    for (const waveline::StreamContention &c : routeBounceDetail_)
        sinksSeen.insert(QString::fromStdString(c.sinkName));
    for (const QString &sink : routeConflicts_.keys()) {
        if (sinksSeen.contains(sink)) continue;
        routeConflicts_.remove(sink);
        changed = true;
    }

    for (auto it = routeBounces_.constBegin(); it != routeBounces_.constEnd(); ++it) {
        // Two passes of the ladder, 400 ms apart. See routeBounces_ for why
        // one is not enough to accuse anything.
        if (it.value() < 2) continue;
        const auto detail = routeBounceDetail_.constFind(it.key());
        if (detail == routeBounceDetail_.constEnd()) continue;

        const QString sink = QString::fromStdString(detail->sinkName);
        if (dismissed.contains(sink)) continue;
        if (routeConflicts_.contains(sink)) continue;

        routeConflicts_.insert(sink, *detail);
        changed = true;
        // Also in the log, because the people most likely to hit this are the
        // ones who will be shown a journal by somebody helping them.
        qWarning("routing conflict: \"%s\" keeps being moved off channel \"%s\" to "
                 "sink \"%s\" (%s). Another program is managing audio routing; "
                 "waveline has stopped trying.",
                 detail->appName.c_str(), detail->channelId.c_str(),
                 detail->sinkName.c_str(), detail->sinkLabel.c_str());
    }

    routeBounces_.clear();
    routeBounceDetail_.clear();
    if (changed) emit Changed();
}

QStringList MixerService::StreamRoutingConflicts() const {
    QStringList rows;
    for (const waveline::StreamContention &c : routeConflicts_) {
        rows << QStringLiteral("%1\t%2\t%3\t%4")
                    .arg(QString::fromStdString(c.appName),
                         QString::fromStdString(c.channelId),
                         QString::fromStdString(c.sinkName),
                         QString::fromStdString(c.sinkLabel));
    }
    return rows;
}

void MixerService::DismissStreamRoutingConflict(const QString &sinkName) {
    if (sinkName.isEmpty()) return;
    QStringList &dismissed = config_.diagnostics().dismissedRoutingSinks;
    if (!dismissed.contains(sinkName)) dismissed.append(sinkName);
    routeConflicts_.remove(sinkName);
    scheduleSave();
    emit Changed();
}

QStringList MixerService::DismissedStreamRoutingConflicts() const {
    return config_.diagnostics().dismissedRoutingSinks;
}

void MixerService::ClearStreamRoutingConflictDismissals() {
    if (config_.diagnostics().dismissedRoutingSinks.isEmpty()) return;
    config_.diagnostics().dismissedRoutingSinks.clear();
    scheduleSave();
    emit Changed();
}

void MixerService::scheduleRouteAll() {
    if (!routing_ || !router_) return;
    engine_.sync();
    router_->routeAll();
    scheduleRouteRetry();
}

void MixerService::markMasterDisconnected(const QString &masterId) {
    MasterHwSlot *hw = masterHwSlot(masterId);
    if (!hw) return;
    hw->dev.close();
    if (!hw->connected) return;
    hw->connected = false;
    hw->settleTicks = 0;
    qInfo("waveline: %s disconnected (master %s)", profile_.brand.c_str(),
          qUtf8Printable(masterId));
    emit Changed();
}

void MixerService::pollHardware() {
    if (!graph_) return;
    // Unconditional, and before the vendor poll below: capture latency is
    // measured for every input device, including the ones with no vendor
    // protocol, which is most of them.
    sampleCaptureDelays();
    for (const MasterBusState &m : config_.live().masterBuses) {
        if (masterHasWave3Hw(m.id)) pollMasterHardware(m.id);
    }
}

void MixerService::pollMasterHardware(const QString &masterId) {
    if (!masterHasWave3Hw(masterId)) return;
    MasterHwSlot &hw = *masterHwSlot(masterId);
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;

    if (hw.backoffTicks > 0) {
        --hw.backoffTicks;
        return;
    }
    // Every USB transfer below is a blocking ioctl on this, the thread that also
    // serves D-Bus and the rest of the daemon. A device that is present but has
    // stopped answering -- a Wave:3 wedged after a reboot is the case seen --
    // makes each one cost its full 1s timeout, and at a 250ms poll the event
    // loop never gets a turn: the mixer stops answering and the graph looks
    // stuck rewiring forever. So a failing device is polled progressively less
    // often, up to 10s apart, until it answers again.
    const auto backOff = [&hw] {
        hw.failStreak = std::min(hw.failStreak + 1, 10);
        hw.backoffTicks = std::min(4 * hw.failStreak, 40);
    };

    const std::string prefix =
        effectiveMasterCaptureMatch(masterId, m->captureMatch).toStdString();
    if (prefix.empty() && masterId != QLatin1String("mic")) {
        markMasterDisconnected(masterId);
        backOff();
        return;
    }

    if (!hw.dev.isOpen()) {
        if (!hw.dev.openMatching(prefix)) {
            markMasterDisconnected(masterId);
            backOff();
            return;
        }
        // Opening the node is not bus traffic, so it is safe here; the identity
        // read that used to follow it immediately is, and the device has only
        // just appeared. Hold every transfer off until it has settled -- see
        // kHwOpenGateTicks for what goes wrong when one lands mid-probe.
        hw.openGateTicks = kHwOpenGateTicks;
        hw.needInfo = true;
    }

    if (hw.openGateTicks > 0) {
        --hw.openGateTicks;
        return;
    }

    if (hw.needInfo) {
        waveline::ClaimGuard guard(hw.dev);
        if (guard) hw.dev.readInfo(hw.info);
        hw.needInfo = false;
    }

    waveline::ClaimGuard guard(hw.dev);
    if (!guard) {
        if (guard.result().error != waveline::Error::Busy)
            markMasterDisconnected(masterId);
        backOff();
        return;
    }

    waveline::State fresh;
    if (auto r = hw.dev.readState(fresh); !r) {
        if (r.error != waveline::Error::Busy) markMasterDisconnected(masterId);
        backOff();
        return;
    }

    hw.failStreak = 0;
    hw.backoffTicks = 0;

    const bool was = hw.connected;
    hw.connected = true;
    if (!was) {
        hw.settleTicks = kHwSettleTicks;
        qInfo("waveline: %s connected for master %s (%s), restoring saved hardware",
              profile_.brand.c_str(), qUtf8Printable(masterId),
              hw.dev.nodePath().c_str());
    }

    if (hw.settleTicks > 0) {
        restoreMasterHardwareState(masterId, fresh);
        if (--hw.settleTicks == 0)
            qInfo("waveline: master %s hardware settled at monitor %d%%, gain %.1f dB",
                  qUtf8Printable(masterId), fresh.monitorPercent, fresh.micGainDb);
    }

    const bool changed = !was || fresh.raw != hw.state.raw ||
                         fresh.micGainDb != hw.state.micGainDb ||
                         fresh.hpVolumeDb != hw.state.hpVolumeDb;
    hw.state = fresh;
    if (!changed) return;
    emit Changed();

    if (hw.settleTicks == 0) scheduleSave();
}

void MixerService::restoreMasterHardwareState(const QString &masterId,
                                              waveline::State &fresh) {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    MasterHwSlot *hw = masterHwSlot(masterId);
    if (!hw || !hw->dev.isOpen()) return;

    if (m->hwMicMuted >= 0 && (m->hwMicMuted == 1) != fresh.micMuted) {
        if (auto r = hw->dev.setMicMute(m->hwMicMuted == 1); r)
            fresh.micMuted = (m->hwMicMuted == 1);
    }
    if (m->hwClipguard >= 0 && (m->hwClipguard == 1) != fresh.clipguard) {
        if (auto r = hw->dev.setClipguard(m->hwClipguard == 1); r)
            fresh.clipguard = (m->hwClipguard == 1);
    }
    if (m->hardwareMonitor >= 0 && m->hardwareMonitor != fresh.monitorPercent) {
        if (auto r = hw->dev.setMonitorPercent(m->hardwareMonitor); r)
            hw->dev.readState(fresh);
    }
    if (m->micGainDb >= 0.0 && std::fabs(m->micGainDb - fresh.micGainDb) > 0.4) {
        if (auto r = hw->dev.setMicGainDb(m->micGainDb); r) fresh.micGainDb = m->micGainDb;
    }
    if (m->hwHpMuted >= 0 && (m->hwHpMuted == 1) != fresh.hpMuted) {
        if (auto r = hw->dev.setHpMute(m->hwHpMuted == 1); r)
            fresh.hpMuted = (m->hwHpMuted == 1);
    }
    if (m->hwHpVolumeDb <= 0.0 &&
        std::fabs(m->hwHpVolumeDb - fresh.hpVolumeDb) > 0.4) {
        if (auto r = hw->dev.setHpVolumeDb(m->hwHpVolumeDb); r)
            fresh.hpVolumeDb = m->hwHpVolumeDb;
    }
}

// ---- channels -------------------------------------------------------------

QStringList MixerService::ChannelIds() const {
    QStringList out;
    if (graph_)
        for (const auto &c : graph_->channels())
            out << QString::fromStdString(c.id);
    return out;
}

QStringList MixerService::Channels() const {
    QStringList out;
    if (!graph_) return out;
    for (const auto &c : graph_->channels()) {
        const QString id = QString::fromStdString(c.id);
        out << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6")
                   .arg(id, ChannelName(id))
                   .arg(c.streamVolume)
                   .arg(c.monitorVolume)
                   .arg(c.streamMuted ? 1 : 0)
                   .arg(c.monitorMuted ? 1 : 0);
    }
    return out;
}

QString MixerService::ChannelName(const QString &id) const {
    // The user's title wins over the built-in one everywhere a name is shown.
    const QString custom = config_.live().channelNames.value(id);
    if (!custom.isEmpty()) return custom;
    if (!graph_) return {};
    for (const auto &c : graph_->channels())
        if (QString::fromStdString(c.id) == id)
            return QString::fromStdString(c.name);
    return {};
}

void MixerService::SetChannelName(const QString &id, const QString &name) {
    if (id.isEmpty()) return;
    Profile &p = config_.live();
    const QString title = name.trimmed();
    // Blank, or the built-in title typed back in, clears the override rather
    // than pinning a copy of the default.
    QString builtin;
    if (graph_) {
        for (const auto &c : graph_->channels())
            if (QString::fromStdString(c.id) == id)
                builtin = QString::fromStdString(c.name);
    }
    if (title.isEmpty() || title == builtin) {
        if (p.channelNames.remove(id) == 0) return;
    } else {
        if (p.channelNames.value(id) == title) return;
        p.channelNames.insert(id, title);
    }
    scheduleSave();
    emit Changed();
}

QStringList MixerService::CardAppearances() const {
    QStringList out;
    const Profile &p = config_.live();
    for (auto it = p.cardAppearance.begin(); it != p.cardAppearance.end(); ++it) {
        out << QStringLiteral("%1\t%2\t%3").arg(it.key(), it->color, it->icon);
    }
    return out;
}

void MixerService::SetCardAppearance(const QString &key, const QString &color,
                                     const QString &icon) {
    if (key.isEmpty()) return;
    Profile &p = config_.live();
    CardAppearanceState a;
    // Validated rather than trusted: this goes into a config file the mixer
    // reads back as a colour, and "" is how a card says "follow the theme".
    // The daemon links no GUI library, so the check is by hand -- #rrggbb, the
    // only form anything here writes.
    const QString hex = color.trimmed().toLower();
    bool hexOk = hex.size() == 7 && hex.startsWith(QLatin1Char('#'));
    for (int i = 1; hexOk && i < hex.size(); ++i) {
        const QChar c = hex.at(i);
        hexOk = (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
    }
    if (hexOk) a.color = hex;
    a.icon = icon.trimmed();

    if (a.color.isEmpty() && a.icon.isEmpty()) {
        if (p.cardAppearance.remove(key) == 0) return;
    } else {
        const auto it = p.cardAppearance.constFind(key);
        if (it != p.cardAppearance.constEnd() && it->color == a.color &&
            it->icon == a.icon)
            return;
        p.cardAppearance.insert(key, a);
    }
    scheduleSave();
    emit Changed();
}

double MixerService::ChannelVolume(const QString &id, const QString &mix) const {
    if (!graph_) return 0.0;
    for (const auto &c : graph_->channels())
        if (QString::fromStdString(c.id) == id)
            return parseMix(mix) == waveline::Mix::Stream ? c.streamVolume
                                                       : c.monitorVolume;
    return 0.0;
}

bool MixerService::ChannelMuted(const QString &id, const QString &mix) const {
    if (!graph_) return false;
    for (const auto &c : graph_->channels())
        if (QString::fromStdString(c.id) == id)
            return parseMix(mix) == waveline::Mix::Stream ? c.streamMuted
                                                       : c.monitorMuted;
    return false;
}

void MixerService::SetChannelVolume(const QString &id, const QString &mix,
                                    double volume) {
    if (!graph_) return;
    graph_->setVolume(id.toStdString(), parseMix(mix),
                      static_cast<float>(qBound(0.0, volume, 1.5)));
    scheduleSave();
    emit Changed();
}

void MixerService::SetChannelMuted(const QString &id, const QString &mix,
                                   bool muted) {
    if (!graph_) return;
    graph_->setMuted(id.toStdString(), parseMix(mix), muted);
    scheduleSave();
    emit Changed();
}

void MixerService::SetMicVolume(const QString &mix, double volume) {
    if (!graph_) return;
    const double v = qBound(0.0, volume, 1.5);
    const waveline::Mix m = parseMix(mix);
    graph_->setMicVolume(m, static_cast<float>(v));
    if (m == waveline::Mix::Monitor) micMonitorLevel_ = v;
    else micStreamLevel_ = v;
    Profile &p = config_.live();
    if (MasterBusState *bus = masterBusState(p, QStringLiteral("mic"))) {
        if (m == waveline::Mix::Monitor) bus->mix.monitorVolume = v;
        else bus->mix.streamVolume = v;
        p.monitorLevel = micMonitorLevel_;
        syncLegacyFromPrimaryBus(p);
    }
    scheduleSave();
    emit Changed();
}

double MixerService::MicVolume(const QString &mix) const {
    return parseMix(mix) == waveline::Mix::Monitor ? micMonitorLevel_ : micStreamLevel_;
}

bool MixerService::MicMixMuted(const QString &mix) const {
    return parseMix(mix) == waveline::Mix::Monitor ? micMonitorMuted_ : micStreamMuted_;
}

bool MixerService::SoftwareMonitor() const {
    return graph_ ? graph_->softwareMonitor() : false;
}

void MixerService::SetSoftwareMonitor(bool on) {
    if (!graph_) return;
    Profile &p = config_.live();
    if (p.softwareMonitor == on && graph_->softwareMonitor() == on) return;
    p.softwareMonitor = on;
    if (MasterBusState *m = masterBusState(p, QStringLiteral("mic")))
        m->softwareMonitor = on;
    graph_->setSoftwareMonitor(on);
    if (on) {
        graph_->setMicVolume(waveline::Mix::Monitor,
                             static_cast<float>(micMonitorLevel_));
        graph_->setMicMuted(waveline::Mix::Monitor, micMonitorMuted_);
    }
    scheduleSave();
    emit Changed();
}

double MixerService::NoiseIntensity() const {
    auto *nc = graph_ ? graph_->noiseFilter() : nullptr;
    return nc ? nc->intensity() : 1.0;
}

void MixerService::SetNoiseIntensity(double value) {
    if (!graph_) return;
    Profile &p = config_.live();
    const double v = qBound(0.0, value, 1.0);
    if (qFuzzyCompare(p.noiseIntensity, v)) return;
    p.noiseIntensity = v;
    if (MasterBusState *m = masterBusState(p, QStringLiteral("mic")))
        m->noiseIntensity = v;
    applyMicFx();
    scheduleSave();
    emit Changed();
}

QString MixerService::ChannelEffects(const QString &channelId,
                                     const QString &stage) const {
    if (!graph_) return QString();
    ChannelFxState st;
    const Profile &p = config_.live();
    if (channelId == QLatin1String("mic")) {
        st = parseFxStage(stage) == waveline::FxStage::Output ? p.masterOutput.fx
                                                              : p.micFx;
    } else {
        // From the profile, so a bypassed channel still reports the EQ curve
        // the user dialled in rather than the flat one currently loaded.
        const auto it = p.channelEffects.constFind(channelId);
        if (it == p.channelEffects.constEnd()) return QString();
        st = (parseFxStage(stage) == waveline::FxStage::Output ? it->output : it->input).fx;
    }
    return fxToTabString(toFxSettings(st));
}

void MixerService::SetChannelEffects(const QString &channelId, const QString &stage,
                                     bool lowCut, int lowCutHz, bool eq, double lowDb,
                                     double midDb, double highDb) {
    if (!graph_) return;
    waveline::ChannelFxSettings s;
    s.lowCut = lowCut;
    s.lowCutHz = (lowCutHz == 120) ? 120 : 80;
    s.eq = eq;
    s.lowDb = static_cast<float>(qBound(-12.0, lowDb, 12.0));
    s.midDb = static_cast<float>(qBound(-12.0, midDb, 12.0));
    s.highDb = static_cast<float>(qBound(-12.0, highDb, 12.0));
    if (channelId == QLatin1String("mic")) {
        Profile &p = config_.live();
        if (parseFxStage(stage) == waveline::FxStage::Output) {
            applyEasyFxFields(p.masterOutput.fx, s);
            applyMasterOutputFx();
        } else {
            applyEasyFxFields(p.micFx, s);
            applyMicFx();
        }
    } else {
        Profile &p = config_.live();
        auto it = p.channelEffects.find(channelId);
        if (it == p.channelEffects.end())
            it = p.channelEffects.insert(channelId, ChannelEffectsState{});
        applyEasyFxFields(
            (parseFxStage(stage) == waveline::FxStage::Output ? it->output : it->input).fx,
            s);
        applyChannelFx(channelId);
    }
    scheduleSave();
    emit Changed();
}

void MixerService::SetChannelProEq(const QString &channelId, const QString &stage,
                                   bool advanced, const QString &bands) {
    if (!graph_) return;
    Profile &p = config_.live();
    if (channelId == QLatin1String("mic")) {
        if (parseFxStage(stage) == waveline::FxStage::Output) {
            applyProEqFields(p.masterOutput.fx, advanced, bands);
            applyMasterOutputFx();
        } else {
            applyProEqFields(p.micFx, advanced, bands);
            applyMicFx();
        }
    } else {
        auto it = p.channelEffects.find(channelId);
        if (it == p.channelEffects.end())
            it = p.channelEffects.insert(channelId, ChannelEffectsState{});
        applyProEqFields(
            (parseFxStage(stage) == waveline::FxStage::Output ? it->output : it->input).fx,
            advanced, bands);
        applyChannelFx(channelId);
    }
    scheduleSave();
    emit Changed();
}

// The filters are derived state. Reading a channel's settings back out of them
// would report a bypassed chain as "noise suppression off" and, once snapshot()
// saved that, would have quietly erased the user's settings for good. The
// stored profile is the answer to every per-channel query.
void MixerService::applyChannelFx(const QString &channelId) {
    if (!graph_ || channelId == QLatin1String("mic")) return;
    const Profile &p = config_.live();
    const auto it = p.channelEffects.constFind(channelId);
    if (it == p.channelEffects.constEnd()) return;
    const std::string id = channelId.toStdString();
    const bool on = it->effectsEnabled;

    const ChannelFxStageState input = effectiveInputStage(p, *it);
    const ChannelFxStageState output = effectiveOutputStage(p, *it);
    const QString inputMasterId = resolveMasterBusId(p, it->inputEffectSourceMasterId);
    bool inputActive = on;
    if (!inputMasterId.isEmpty()) {
        const MasterBusState *m = findMasterBus(p, inputMasterId);
        inputActive = on && m && m->micEffectsEnabled;
    }
    const DuckingState duck = on ? effectiveDucking(p, *it) : DuckingState{};
    const LufsLimiterState lufsCfg = effectiveLufsLimiter(p, *it);

    const waveline::CreativeFxSettings inputCreative =
        inputActive ? toCreativeFxSettings(input.creativeFx) : waveline::CreativeFxSettings{};
    waveline::ChannelFxSettings inputFx =
        inputActive ? toFxSettings(input.fx) : waveline::ChannelFxSettings{};
    if (inputActive && inputCreative.eq.enabled) inputFx.eq = false;

    const waveline::CreativeFxSettings outputCreative =
        on ? toCreativeFxSettings(output.creativeFx) : waveline::CreativeFxSettings{};
    waveline::ChannelFxSettings outputFx =
        on ? toFxSettings(output.fx) : waveline::ChannelFxSettings{};
    if (on && outputCreative.eq.enabled) outputFx.eq = false;

    graph_->setChannelEffects(id, waveline::FxStage::Input, inputFx);
    graph_->setChannelDynamics(
        id, waveline::FxStage::Input,
        inputActive ? withDeEsser(toDynamicsSettings(input.dynamics), input.deEsser,
                                  input.deEsserIntensity)
                    : waveline::DynamicsSettings{});
    graph_->setChannelCreativeFx(id, waveline::FxStage::Input, inputCreative);
    graph_->setChannelNoiseSuppression(id, waveline::FxStage::Input,
                                       inputActive && input.noiseSuppression,
                                       static_cast<float>(input.noiseIntensity));

    graph_->setChannelEffects(id, waveline::FxStage::Output, outputFx);
    graph_->setChannelDynamics(
        id, waveline::FxStage::Output,
        on ? withDeEsser(toDynamicsSettings(output.dynamics), output.deEsser,
                         output.deEsserIntensity)
           : waveline::DynamicsSettings{});
    graph_->setChannelCreativeFx(id, waveline::FxStage::Output, outputCreative);
    graph_->setChannelNoiseSuppression(id, waveline::FxStage::Output,
                                       on && output.noiseSuppression,
                                       static_cast<float>(output.noiseIntensity));
    graph_->setChannelDucking(id, toDuckingSettings(duck));
    graph_->setChannelLufsLimiter(id, toLufsLimiterSettings(lufsCfg));
}

void MixerService::applyMicFx() {
    const Profile &p = config_.live();
    for (const MasterBusState &m : p.masterBuses) applyMasterFx(m.id);
}

void MixerService::applyMasterOutputFx() {
    if (!graph_) return;
    const Profile &p = config_.live();
    for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
        if (!it->outputEffectSourceMasterId.isEmpty()) applyChannelFx(it.key());
    }
}

QString MixerService::MicDynamics() const {
    return dynamicsToTabString(config_.live().micDynamics);
}

void MixerService::SetMicDynamics(bool gate, double gateThresholdDb, double gateAttackMs,
                                  double gateReleaseMs, bool compressor,
                                  double compThresholdDb, double compRatio,
                                  double compAttackMs, double compReleaseMs,
                                  double compKneeDb, double makeupGainDb, bool autoMakeup,
                                  bool limiter, double limitThresholdDb,
                                  double limitAttackMs, double limitReleaseMs) {
    if (!graph_) return;
    config_.live().micDynamics =
        dynamicsFromArgs(gate, gateThresholdDb, gateAttackMs, gateReleaseMs, compressor,
                         compThresholdDb, compRatio, compAttackMs, compReleaseMs, compKneeDb,
                         makeupGainDb, autoMakeup, limiter, limitThresholdDb, limitAttackMs,
                         limitReleaseMs);
    applyMicFx();
    scheduleSave();
    emit Changed();
}

QString MixerService::ChannelDynamics(const QString &channelId,
                                      const QString &stage) const {
    if (!graph_) return QString();
    const Profile &p = config_.live();
    if (channelId == QLatin1String("mic")) {
        const DynamicsState &d =
            parseFxStage(stage) == waveline::FxStage::Output ? p.masterOutput.dynamics
                                                             : p.micDynamics;
        return dynamicsToTabString(d);
    }
    const auto it = p.channelEffects.constFind(channelId);
    if (it == p.channelEffects.constEnd()) return QString();
    const DynamicsState &d =
        (parseFxStage(stage) == waveline::FxStage::Output ? it->output : it->input)
            .dynamics;
    return dynamicsToTabString(d);
}

void MixerService::SetChannelDynamics(const QString &channelId, const QString &stage,
                                      bool gate, double gateThresholdDb, double gateAttackMs,
                                      double gateReleaseMs, bool compressor,
                                      double compThresholdDb, double compRatio,
                                      double compAttackMs, double compReleaseMs,
                                      double compKneeDb, double makeupGainDb,
                                      bool autoMakeup, bool limiter,
                                      double limitThresholdDb, double limitAttackMs,
                                      double limitReleaseMs) {
    if (!graph_) return;
    const DynamicsState d =
        dynamicsFromArgs(gate, gateThresholdDb, gateAttackMs, gateReleaseMs, compressor,
                         compThresholdDb, compRatio, compAttackMs, compReleaseMs, compKneeDb,
                         makeupGainDb, autoMakeup, limiter, limitThresholdDb, limitAttackMs,
                         limitReleaseMs);
    if (channelId == QLatin1String("mic")) {
        Profile &p = config_.live();
        if (parseFxStage(stage) == waveline::FxStage::Output) {
            p.masterOutput.dynamics = d;
            applyMasterOutputFx();
        } else {
            p.micDynamics = d;
            applyMicFx();
        }
    } else {
        Profile &p = config_.live();
        auto it = p.channelEffects.find(channelId);
        if (it == p.channelEffects.end())
            it = p.channelEffects.insert(channelId, ChannelEffectsState{});
        (parseFxStage(stage) == waveline::FxStage::Output ? it->output : it->input)
            .dynamics = d;
        applyChannelFx(channelId);
    }
    scheduleSave();
    emit Changed();
}

QString MixerService::ChannelDucking(const QString &channelId) const {
    if (!graph_) return QString();
    const Profile &p = config_.live();
    if (channelId == QLatin1String("mic"))
        return duckingToTabString(p.masterOutputDucking);
    const auto it = p.channelEffects.constFind(channelId);
    if (it == p.channelEffects.constEnd()) return QString();
    return duckingToTabString(it->ducking);
}

void MixerService::SetChannelDucking(const QString &channelId, bool enabled,
                                     double intensity, const QString &sources,
                                     double holdSec) {
    if (!graph_) return;
    Profile &p = config_.live();
    // No scheduleRewire() here: a source change only moves sidechain links, and
    // MixerGraph::setChannelDucking() swaps those in place. A full rewire clears
    // every link in the graph and rebuilds it, which cut audio on every edit.
    if (channelId == QLatin1String("mic")) {
        p.masterOutputDucking = duckingFromArgs(enabled, sources, intensity, holdSec);
        applyMasterOutputFx();
        scheduleSave();
        emit Changed();
        return;
    }
    auto it = p.channelEffects.find(channelId);
    if (it == p.channelEffects.end())
        it = p.channelEffects.insert(channelId, ChannelEffectsState{});
    it->ducking = duckingFromArgs(enabled, sources, intensity, holdSec);
    applyChannelFx(channelId);
    scheduleSave();
    emit Changed();
}

QString MixerService::ChannelLufsLimiter(const QString &channelId) const {
    if (!graph_) return QString();
    const Profile &p = config_.live();
    if (channelId == QLatin1String("mic"))
        return QStringLiteral("%1\t%2")
            .arg(p.masterOutputLufsLimiter.enabled ? 1 : 0)
            .arg(p.masterOutputLufsLimiter.maxLufs);
    const auto it = p.channelEffects.constFind(channelId);
    if (it == p.channelEffects.constEnd()) return QString();
    return QStringLiteral("%1\t%2")
        .arg(it->lufsLimiter.enabled ? 1 : 0)
        .arg(it->lufsLimiter.maxLufs);
}

void MixerService::SetChannelLufsLimiter(const QString &channelId, bool enabled,
                                         double maxLufs) {
    if (!graph_) return;
    Profile &p = config_.live();
    maxLufs = qBound(-40.0, maxLufs, -6.0);
    if (channelId == QLatin1String("mic")) {
        const bool wasEnabled = p.masterOutputLufsLimiter.enabled;
        p.masterOutputLufsLimiter.enabled = enabled;
        p.masterOutputLufsLimiter.maxLufs = maxLufs;
        applyMasterOutputFx();
        if (wasEnabled != enabled) {
            for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
                if (!it->outputEffectSourceMasterId.isEmpty())
                    rewireChannelMonitorPath(it.key());
            }
        }
        scheduleSave();
        emit Changed();
        return;
    }
    auto it = p.channelEffects.find(channelId);
    if (it == p.channelEffects.end())
        it = p.channelEffects.insert(channelId, ChannelEffectsState{});
    const bool wasEnabled = it->lufsLimiter.enabled;
    it->lufsLimiter.enabled = enabled;
    it->lufsLimiter.maxLufs = maxLufs;
    applyChannelFx(channelId);
    if (wasEnabled != enabled)
        rewireChannelMonitorPath(channelId);
    scheduleSave();
    emit Changed();
}

// A spec that fails to round-trip through decode/encode (garbage from a
// misbehaving client, or hand-edited config) is still stored as given --
// decodeCreativeFx() already falls back field-by-field on read, so there is
// nothing to reject here, unlike a numeric arg that could be out of range.
QString MixerService::ChannelCreativeFx(const QString &channelId,
                                        const QString &stage) const {
    if (!graph_) return QString();
    const Profile &p = config_.live();
    if (channelId == QLatin1String("mic")) {
        const CreativeFxState &c =
            parseFxStage(stage) == waveline::FxStage::Output ? p.masterOutput.creativeFx
                                                             : p.micCreativeFx;
        return c.spec;
    }
    const auto it = p.channelEffects.constFind(channelId);
    if (it == p.channelEffects.constEnd()) return QString();
    const CreativeFxState &c =
        (parseFxStage(stage) == waveline::FxStage::Output ? it->output : it->input)
            .creativeFx;
    return c.spec;
}

void MixerService::SetChannelCreativeFx(const QString &channelId, const QString &stage,
                                        const QString &spec) {
    if (!graph_) return;
    CreativeFxState c;
    c.spec = spec;
    if (channelId == QLatin1String("mic")) {
        Profile &p = config_.live();
        if (parseFxStage(stage) == waveline::FxStage::Output) {
            p.masterOutput.creativeFx = c;
            applyMasterOutputFx();
        } else {
            p.micCreativeFx = c;
            if (MasterBusState *m = masterBusState(p, QStringLiteral("mic")))
                m->micCreativeFx = c;
            applyMicFx();
        }
    } else {
        Profile &p = config_.live();
        auto it = p.channelEffects.find(channelId);
        if (it == p.channelEffects.end())
            it = p.channelEffects.insert(channelId, ChannelEffectsState{});
        (parseFxStage(stage) == waveline::FxStage::Output ? it->output : it->input)
            .creativeFx = c;
        applyChannelFx(channelId);
    }
    scheduleSave();
    emit Changed();
}

QString MixerService::MasterCreativeFx(const QString &masterId) const {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->micCreativeFx.spec : QString();
}

void MixerService::SetMasterCreativeFx(const QString &masterId, const QString &spec) {
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    m->micCreativeFx.spec = spec;
    if (masterId == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());
    applyMasterFx(masterId);
    scheduleSave();
    emit Changed();
}

QString MixerService::MasterRackCreativeFx(const QString &masterId) const {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->rackCreativeFx.spec : QString();
}

void MixerService::SetMasterRackCreativeFx(const QString &masterId, const QString &spec) {
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    m->rackCreativeFx.spec = spec;
    if (masterId == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());
    applyMasterFx(masterId);
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterRackMode(const QString &masterId) const {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m && m->rackMode;
}

void MixerService::SetMasterRackMode(const QString &masterId, bool on) {
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    m->rackMode = on;
    applyMasterFx(masterId);
    scheduleSave();
    emit Changed();
}

bool MixerService::ChannelEffectsEnabled(const QString &channelId) const {
    const Profile &p = config_.live();
    const auto it = p.channelEffects.constFind(channelId);
    return it == p.channelEffects.constEnd() ? true : it->effectsEnabled;
}

void MixerService::SetChannelEffectsEnabled(const QString &channelId, bool on) {
    if (!graph_) return;
    Profile &p = config_.live();
    auto it = p.channelEffects.find(channelId);
    if (it == p.channelEffects.end())
        it = p.channelEffects.insert(channelId, ChannelEffectsState{});
    if (it->effectsEnabled == on) return;
    it->effectsEnabled = on;
    // Enabling can need a filter that was never built, because the chain is
    // only materialised for stages that asked for it.
    bool created = false;
    if (on) {
        if (wantsInputNoiseFilter(p, *it))
            created |= graph_->ensureChannelNoiseFilter(channelId.toStdString(),
                                                        waveline::FxStage::Input);
        if (wantsOutputNoiseFilter(p, *it))
            created |= graph_->ensureChannelNoiseFilter(channelId.toStdString(),
                                                        waveline::FxStage::Output);
    }
    applyChannelFx(channelId);
    if (created) scheduleRewire();
    scheduleSave();
    emit Changed();
}

bool MixerService::ChannelMonitorFx(const QString &channelId) const {
    const Profile &p = config_.live();
    const auto it = p.channelEffects.constFind(channelId);
    return it != p.channelEffects.constEnd() && it->monitorFx;
}

void MixerService::SetChannelMonitorFx(const QString &channelId, bool on) {
    if (!graph_) return;
    Profile &p = config_.live();
    auto it = p.channelEffects.find(channelId);
    if (it == p.channelEffects.end())
        it = p.channelEffects.insert(channelId, ChannelEffectsState{});
    if (it->monitorFx == on) return;
    it->monitorFx = on;
    graph_->setChannelMonitorFx(channelId.toStdString(), on);
    rewireChannelMonitorPath(channelId);
    scheduleSave();
    emit Changed();
}

bool MixerService::MicEffectsEnabled() const {
    return config_.live().micEffectsEnabled;
}

void MixerService::SetMicEffectsEnabled(bool on) {
    if (!graph_) return;
    Profile &p = config_.live();
    if (p.micEffectsEnabled == on) return;
    p.micEffectsEnabled = on;
    applyMicFx();
    scheduleSave();
    emit Changed();
}

bool MixerService::MicMonitorFx() const { return config_.live().micMonitorFx; }

void MixerService::SetMicMonitorFx(bool on) {
    if (!graph_) return;
    Profile &p = config_.live();
    if (p.micMonitorFx == on) return;
    p.micMonitorFx = on;
    graph_->setMicMonitorFx(on);
    rewireMicMonitorPath();
    scheduleSave();
    emit Changed();
}

bool MixerService::ChannelMicMuted(const QString &channelId) const {
    return graph_ && graph_->channelMicMuted(channelId.toStdString());
}

void MixerService::SetChannelMicMuted(const QString &channelId, bool muted) {
    if (!graph_) return;
    if (!graph_->setChannelMicMuted(channelId.toStdString(), muted)) return;
    scheduleSave();
    emit Changed();
}

bool MixerService::ChannelEffectSourceMaster(const QString &channelId,
                                             const QString &stage) const {
    const Profile &p = config_.live();
    const auto it = p.channelEffects.constFind(channelId);
    if (it == p.channelEffects.constEnd()) return false;
    const QString &raw = parseFxStage(stage) == waveline::FxStage::Output
                             ? it->outputEffectSourceMasterId
                             : it->inputEffectSourceMasterId;
    return !resolveMasterBusId(p, raw).isEmpty();
}

QString MixerService::ChannelEffectSourceMasterId(const QString &channelId,
                                                  const QString &stage) const {
    const Profile &p = config_.live();
    const auto it = p.channelEffects.constFind(channelId);
    if (it == p.channelEffects.constEnd()) return {};
    const QString &raw = parseFxStage(stage) == waveline::FxStage::Output
                             ? it->outputEffectSourceMasterId
                             : it->inputEffectSourceMasterId;
    return resolveMasterBusId(p, raw);
}

void MixerService::SetChannelEffectSourceMaster(const QString &channelId,
                                                const QString &stage,
                                                const QString &masterId) {
    if (!graph_ || channelId == QLatin1String("mic")) return;
    Profile &p = config_.live();
    auto it = p.channelEffects.find(channelId);
    if (it == p.channelEffects.end())
        it = p.channelEffects.insert(channelId, ChannelEffectsState{});
    const bool isOutput = parseFxStage(stage) == waveline::FxStage::Output;
    const QString resolved =
        masterId.isEmpty() ? QString() : resolveMasterBusId(p, masterId);
    QString &stored =
        isOutput ? it->outputEffectSourceMasterId : it->inputEffectSourceMasterId;
    if (stored == resolved) return;
    stored = resolved;
    // Picking a device to inherit settings from takes the channel back out of
    // "use device effects" -- its own chain has to be in the path to apply them.
    if (!isOutput && !resolved.isEmpty() && it->inputUseDeviceFx) {
        it->inputUseDeviceFx = false;
        graph_->setChannelMicUseDeviceFx(channelId.toStdString(), false);
        if (it->micSource) {
            std::string wireErr;
            graph_->rewireChannelMicSource(channelId.toStdString(), wireErr);
        }
    }
    it->inputUseMasterEffects = !it->inputEffectSourceMasterId.isEmpty();
    it->outputUseMasterEffects = !it->outputEffectSourceMasterId.isEmpty();
    const bool useMaster = !resolved.isEmpty();
    bool created = false;
    if (it->effectsEnabled && useMaster) {
        if (isOutput ? wantsOutputNoiseFilter(p, *it) : wantsInputNoiseFilter(p, *it))
            created |= graph_->ensureChannelNoiseFilter(channelId.toStdString(),
                                                        isOutput ? waveline::FxStage::Output
                                                                 : waveline::FxStage::Input);
    }
    applyChannelFx(channelId);
    if (created) scheduleRewire();
    scheduleSave();
    emit Changed();
}

bool MixerService::ChannelMicUseDeviceFx(const QString &channelId) const {
    const Profile &p = config_.live();
    const auto it = p.channelEffects.constFind(channelId);
    return it != p.channelEffects.constEnd() && it->inputUseDeviceFx;
}

void MixerService::SetChannelMicUseDeviceFx(const QString &channelId, bool on) {
    if (!graph_ || channelId == QLatin1String("mic")) return;
    Profile &p = config_.live();
    auto it = p.channelEffects.find(channelId);
    if (it == p.channelEffects.end())
        it = p.channelEffects.insert(channelId, ChannelEffectsState{});
    if (it->inputUseDeviceFx == on) return;
    it->inputUseDeviceFx = on;
    // The two input modes are exclusive: inheriting a device's *settings* into
    // this channel's chain means nothing once the chain is out of the path.
    if (on) {
        it->inputEffectSourceMasterId.clear();
        it->inputUseMasterEffects = false;
    }
    graph_->setChannelMicUseDeviceFx(channelId.toStdString(), on);
    if (it->micSource) {
        std::string err;
        graph_->rewireChannelMicSource(channelId.toStdString(), err);
    }
    scheduleSave();
    emit Changed();
}

bool MixerService::ChannelMicSource(const QString &channelId) const {
    const Profile &p = config_.live();
    const auto it = p.channelEffects.constFind(channelId);
    return it != p.channelEffects.constEnd() && it->micSource;
}

void MixerService::SetChannelMicSource(const QString &channelId, bool on) {
    if (!graph_) return;
    Profile &p = config_.live();
    auto it = p.channelEffects.find(channelId);
    if (it == p.channelEffects.end())
        it = p.channelEffects.insert(channelId, ChannelEffectsState{});
    if (it->micSource == on) return;
    it->micSource = on;
    graph_->setChannelMicSource(channelId.toStdString(), on);
    // The meter tap follows the source. It cannot be left in place while the
    // node is gone: a capture stream with no target attaches to the default
    // source instead, and the strip would meter the bare microphone.
    if (meters_) {
        const std::string key = channelId.toStdString() + "-mic";
        if (on) {
            std::string mErr;
            meters_->watch(key, "waveline-" + channelId.toStdString() + "-mic", mErr,
                           false);
        } else {
            meters_->unwatch(key);
        }
    }
    if (on) {
        applyChannelFx(channelId);
        std::string err;
        if (!graph_->rewireChannelMicSource(channelId.toStdString(), err))
            qWarning("waveline: mic source wire for %s: %s", qUtf8Printable(channelId),
                     err.c_str());
    }
    syncAlsaAliases();
    scheduleSave();
    emit Changed();
}

bool MixerService::ChannelMicMonitor(const QString &channelId) const {
    const Profile &p = config_.live();
    const auto it = p.channelEffects.constFind(channelId);
    return it != p.channelEffects.constEnd() && it->micMonitor;
}

void MixerService::SetChannelMicMonitor(const QString &channelId, bool on) {
    if (!graph_) return;
    Profile &p = config_.live();
    auto it = p.channelEffects.find(channelId);
    if (it == p.channelEffects.end())
        it = p.channelEffects.insert(channelId, ChannelEffectsState{});
    if (!it->micSource || it->micMonitor == on) return;
    it->micMonitor = on;
    graph_->setChannelMicMonitor(channelId.toStdString(), on);
    scheduleSave();
    emit Changed();
}

bool MixerService::ChannelNoiseSuppression(const QString &channelId,
                                           const QString &stage) const {
    const Profile &p = config_.live();
    if (channelId == QLatin1String("mic")) {
        return parseFxStage(stage) == waveline::FxStage::Output
                   ? p.masterOutput.noiseSuppression
                   : p.noiseSuppression;
    }
    if (!graph_) return false;
    const auto it = p.channelEffects.constFind(channelId);
    if (it == p.channelEffects.constEnd()) return false;
    return parseFxStage(stage) == waveline::FxStage::Output ? it->output.noiseSuppression
                                                         : it->input.noiseSuppression;
}

void MixerService::SetChannelNoiseSuppression(const QString &channelId,
                                              const QString &stage, bool on) {
    if (!graph_) return;
    if (channelId == QLatin1String("mic")) {
        Profile &p = config_.live();
        if (parseFxStage(stage) == waveline::FxStage::Output) {
            if (p.masterOutput.noiseSuppression == on) return;
            p.masterOutput.noiseSuppression = on;
            bool created = false;
            if (on) {
                for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
                    if (it->outputEffectSourceMasterId.isEmpty() || !it->effectsEnabled)
                        continue;
                    created |= graph_->ensureChannelNoiseFilter(it.key().toStdString(),
                                                                waveline::FxStage::Output);
                }
            }
            applyMasterOutputFx();
            if (created) scheduleRewire();
        } else {
            if (p.noiseSuppression == on) return;
            p.noiseSuppression = on;
            bool created = false;
            if (on) {
                for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
                    if (it->inputEffectSourceMasterId.isEmpty() || !it->effectsEnabled)
                        continue;
                    created |= graph_->ensureChannelNoiseFilter(it.key().toStdString(),
                                                                waveline::FxStage::Input);
                }
            }
            applyMicFx();
            if (created) scheduleRewire();
        }
    } else {
        Profile &p = config_.live();
        auto it = p.channelEffects.find(channelId);
        if (it == p.channelEffects.end())
            it = p.channelEffects.insert(channelId, ChannelEffectsState{});
        const waveline::FxStage st = parseFxStage(stage);
        (st == waveline::FxStage::Output ? it->output : it->input).noiseSuppression = on;
        const bool created =
            graph_->ensureChannelNoiseFilter(channelId.toStdString(), st);
        // Stored either way; applied only when the channel is not bypassed.
        applyChannelFx(channelId);
        if (created) scheduleRewire();
    }
    scheduleSave();
    emit Changed();
}

double MixerService::ChannelNoiseIntensity(const QString &channelId,
                                           const QString &stage) const {
    const Profile &p = config_.live();
    if (channelId == QLatin1String("mic")) {
        return parseFxStage(stage) == waveline::FxStage::Output
                   ? p.masterOutput.noiseIntensity
                   : p.noiseIntensity;
    }
    if (!graph_) return 1.0;
    const auto it = p.channelEffects.constFind(channelId);
    if (it == p.channelEffects.constEnd()) return 1.0;
    return parseFxStage(stage) == waveline::FxStage::Output ? it->output.noiseIntensity
                                                         : it->input.noiseIntensity;
}

void MixerService::SetChannelNoiseIntensity(const QString &channelId,
                                            const QString &stage, double value) {
    if (!graph_) return;
    const double v = qBound(0.0, value, 1.0);
    if (channelId == QLatin1String("mic")) {
        Profile &p = config_.live();
        if (parseFxStage(stage) == waveline::FxStage::Output) {
            if (qFuzzyCompare(p.masterOutput.noiseIntensity, v)) return;
            p.masterOutput.noiseIntensity = v;
            applyMasterOutputFx();
        } else {
            if (qFuzzyCompare(p.noiseIntensity, v)) return;
            p.noiseIntensity = v;
            applyMicFx();
        }
    } else {
        Profile &p = config_.live();
        auto it = p.channelEffects.find(channelId);
        if (it == p.channelEffects.end())
            it = p.channelEffects.insert(channelId, ChannelEffectsState{});
        (parseFxStage(stage) == waveline::FxStage::Output ? it->output : it->input)
            .noiseIntensity = v;
        applyChannelFx(channelId);
    }
    scheduleSave();
}

double MixerService::ChannelMicSend(const QString &channelId) const {
    if (!graph_) return 0.0;
    return graph_->channelMicSend(channelId.toStdString());
}

void MixerService::SetChannelMicSend(const QString &channelId, double level) {
    if (!graph_) return;
    // Unity is the ceiling, matching the fader. This gain sits after the
    // channel taps its input devices; a source that arrives quiet is turned up
    // at the device -- its input level or preamp -- rather than here, where the
    // makeup would also amplify whatever the chain picked up.
    graph_->setChannelMicSend(channelId.toStdString(),
                              static_cast<float>(qBound(0.0, level, 1.0)));
    scheduleSave();
    emit Changed();
}

QStringList MixerService::Levels() const {
    QStringList out;
    std::map<std::string, float> peaks;
    if (meters_) {
        // monitor-mix is the shared bus before per-output faders; the
        // per-output rows apply their own mute/volume in the UI and need the
        // raw bus level here. Input device cards meter their own send instead,
        // from "<id>-src" scaled by that card's Monitor fader.
        for (const auto &[key, peak] : meters_->levels()) {
            peaks.insert({key, peak});
            out << QStringLiteral("%1\t%2")
                       .arg(QString::fromStdString(key))
                       .arg(peak);
        }
    }
    // The microphone's before/after levels and the speech probability ride
    // along here rather than being three more methods. Everything a meter
    // needs then costs the GUI exactly one round trip, which is what makes it
    // affordable to poll fast enough for a smooth display.
    if (graph_) {
        for (const auto &bus : graph_->masterBuses()) {
            const QString prefix = QString::fromStdString(bus.id);
            if (auto *nc = graph_->masterNoiseFilter(bus.id)) {
                out << prefix + QStringLiteral("-in\t") + QString::number(nc->inputRms());
                out << prefix + QStringLiteral("-out\t") + QString::number(nc->outputRms());
                if (waveline::isPrimaryMaster(bus.id))
                    out << QStringLiteral("speech\t%1").arg(nc->speechProbability());
                continue;
            }
            // MIDI buses run no noise filter, so the source tap is the only
            // level they have. Report it under -out too, which is what the
            // effects window's "after" meter reads.
            const auto it = peaks.find(bus.id + "-src");
            if (it != peaks.end())
                out << prefix + QStringLiteral("-out\t") + QString::number(it->second);
        }
        for (const auto &c : graph_->channels()) {
            if (c.id == "mic") continue;
            if (auto *nc = graph_->channelNoiseFilter(c.id, waveline::FxStage::Input)) {
                const QString id = QString::fromStdString(c.id);
                out << id + QStringLiteral("-in\t") + QString::number(nc->inputRms());
                out << id + QStringLiteral("-out\t") + QString::number(nc->outputRms());
            }
        }
    }
    return out;
}

// ----------------------------------------------------------------- tuner

QStringList MixerService::TunerSources() const {
    QStringList out;
    for (const QString &row : CaptureDevices())
        out << QStringLiteral("audio\t") + row;
    for (const QString &row : MidiDevices())
        out << QStringLiteral("midi\t") + row;
    return out;
}

QString MixerService::TunerStart(const QString &kind, const QString &source) {
    if (source.isEmpty()) return QStringLiteral("no tuner source selected");
    if (!tuner_) tuner_ = std::make_unique<waveline::Tuner>();

    // Already listening to exactly this: leave the tap alone rather than
    // restarting it, so reopening the window does not blank a live reading.
    if (tuner_->active() && tuner_->sourceKind() == kind.toStdString() &&
        tuner_->source() == source.toStdString())
        return {};

    engine_.forgetLinksForNode(waveline::Tuner::midiNodeName());

    std::string error;
    if (!tuner_->start(kind.toStdString(), source.toStdString(), error)) {
        lastError_ = QString::fromStdString(error);
        qWarning("tuner: %s", error.c_str());
        return lastError_;
    }

    if (kind == QLatin1String("midi")) {
        if (!linkTunerMidi(source, error)) {
            tuner_->stop();
            lastError_ = QString::fromStdString(error);
            qWarning("tuner: %s", error.c_str());
            return lastError_;
        }
    }
    return {};
}

bool MixerService::linkTunerMidi(const QString &source, std::string &error) {
    if (!tuner_) {
        error = "tuner not running";
        return false;
    }
    // "node|port", the same match MidiDevices() hands out.
    const int sep = source.indexOf(QLatin1Char('|'));
    const std::string node =
        (sep < 0 ? source : source.left(sep)).toStdString();
    const std::string hint = sep < 0 ? std::string{} : source.mid(sep + 1).toStdString();

    const std::string port = engine_.resolveMidiOutputPort(node, hint);
    if (port.empty()) {
        error = "no MIDI output port on " + node;
        return false;
    }
    // The filter registers its port a moment after pw_filter_connect returns.
    if (!engine_.waitForPort(waveline::Tuner::midiNodeName(),
                             waveline::Tuner::midiPortName(), false, 1000)) {
        error = "tuner MIDI input not ready";
        return false;
    }
    return engine_.linkPorts(node, port, waveline::Tuner::midiNodeName(),
                             waveline::Tuner::midiPortName(), error, true);
}

void MixerService::relinkTunerMidi() {
    if (!tuner_ || !tuner_->active()) return;
    if (tuner_->sourceKind() != "midi") return;
    // A rewire drops every manual link in the graph, this one included, and a
    // tuner that goes quiet because somebody changed a routing elsewhere would
    // look like a broken instrument.
    std::string error;
    if (!linkTunerMidi(QString::fromStdString(tuner_->source()), error))
        qWarning("tuner: could not relink MIDI input: %s", error.c_str());
}

void MixerService::TunerStop() {
    // Destroyed rather than merely stopped: the tuner owns a PipeWire context
    // and a thread loop, and there is no reason to keep either around for a
    // window nobody has open.
    tuner_.reset();
    engine_.forgetLinksForNode(waveline::Tuner::midiNodeName());
}

bool MixerService::TunerActive() const { return tuner_ && tuner_->active(); }

void MixerService::PlayTunerReference(double hz, int ms) {
    if (!tuner_ || !tuner_->active()) return;
    tuner_->playReference(hz, ms, waveline::MixerGraph::kMonitorMix);
}

QString MixerService::TunerReading() const {
    if (!tuner_) return QStringLiteral("0\t0\t0\t-1\t0");
    const waveline::TunerReading r = tuner_->reading();
    return QStringLiteral("%1\t%2\t%3\t%4\t%5")
        .arg(r.frequencyHz)
        .arg(r.confidence)
        .arg(r.level)
        .arg(r.midiNote)
        .arg(r.bendCents);
}

double MixerService::MonitorLevel() const {
    if (!graph_) return 1.0;
    return micMonitorLevel_;
}

void MixerService::SetMonitorLevel(double volume) {
    if (!graph_) return;
    micMonitorLevel_ = qBound(0.0, volume, 1.5);
    graph_->setMicVolume(waveline::Mix::Monitor, static_cast<float>(micMonitorLevel_));
    scheduleSave();
    emit Changed();
}

double MixerService::MonitorOutputVolume() const {
    // While muted the graph gain is zero, so reporting it would drag the master
    // fader to the bottom on every refresh instead of leaving it where it was
    // set -- muting a channel strip does not move its fader, and this is the
    // same control. The stored level is the one the fader is showing, and the
    // one unmuting restores. snapshot() already treats it as authoritative for
    // exactly this reason.
    if (monitorMasterMuted_) return config_.live().monitorMaster;
    return graph_ ? graph_->monitorMasterVolume() : 1.0;
}

void MixerService::SetMonitorOutputVolume(double volume) {
    if (!graph_) return;
    volume = qBound(0.0, volume, 1.0);
    config_.live().monitorMaster = volume;
    // Moving the fader while muted is an unmute: the alternative is a control
    // that visibly moves and changes nothing.
    monitorMasterMuted_ = false;
    graph_->setMonitorMasterVolume(static_cast<float>(volume));
    scheduleSave();
    emit Changed();
}

bool MixerService::MonitorOutputMuted() const { return monitorMasterMuted_; }

void MixerService::SetMonitorOutputMuted(bool muted) {
    if (!graph_ || muted == monitorMasterMuted_) return;
    monitorMasterMuted_ = muted;
    graph_->setMonitorMasterMuted(muted);
    flushPendingSave();
    emit Changed();
}

bool MixerService::MicStereo() const {
    return graph_ ? graph_->micStereo() : true;
}

void MixerService::SetMicStereo(bool on) {
    if (!graph_ || on == graph_->micStereo()) return;
    Profile &p = config_.live();
    p.micStereo = on;
    if (MasterBusState *m = masterBusState(p, QStringLiteral("mic")))
        m->micStereo = on;
    std::string err;
    if (!graph_->setMasterMicStereo(waveline::kPrimaryMasterId, on, err)) {
        lastError_ = QString::fromStdString(err);
        return;
    }
    graph_->setSoftwareMonitor(p.softwareMonitor);
    graph_->setMicVolume(waveline::Mix::Stream,
                         static_cast<float>(micStreamLevel_));
    graph_->setMicMuted(waveline::Mix::Stream, micStreamMuted_);
    if (p.softwareMonitor) {
        graph_->setMicVolume(waveline::Mix::Monitor,
                             static_cast<float>(micMonitorLevel_));
        graph_->setMicMuted(waveline::Mix::Monitor, micMonitorMuted_);
    }
    scheduleSave();
    emit Changed();
}

void MixerService::SetMicMuted(const QString &mix, bool muted) {
    if (!graph_) return;
    const waveline::Mix m = parseMix(mix);
    if (m == waveline::Mix::Monitor) micMonitorMuted_ = muted;
    else micStreamMuted_ = muted;
    if (m == waveline::Mix::Monitor && !graph_->softwareMonitor()) {
        scheduleSave();
        emit Changed();
        return;
    }
    graph_->setMicMuted(m, muted);
    scheduleSave();
    emit Changed();
}

// ---- hardware --------------------------------------------------------------

bool MixerService::DeviceConnected() const {
    const MasterHwSlot *hw = masterHwSlot(QStringLiteral("mic"));
    return hw && hw->connected;
}

QString MixerService::DeviceFirmware() const {
    return MasterDeviceFirmware(QStringLiteral("mic"));
}

bool MixerService::Clipguard() const { return MasterClipguard(QStringLiteral("mic")); }

void MixerService::SetClipguard(bool on) { SetMasterClipguard(QStringLiteral("mic"), on); }

int MixerService::HardwareMonitor() const {
    return MasterHardwareMonitor(QStringLiteral("mic"));
}

void MixerService::SetHardwareMonitor(int percent) {
    SetMasterHardwareMonitor(QStringLiteral("mic"), percent);
}

bool MixerService::MicMuted() const { return MasterMicMuted(QStringLiteral("mic")); }

void MixerService::SetHardwareMicMute(bool muted) {
    SetMasterHardwareMicMute(QStringLiteral("mic"), muted);
}

double MixerService::MicGainDb() const { return MasterMicGainDb(QStringLiteral("mic")); }

void MixerService::SetMicGainDb(double db) { SetMasterMicGainDb(QStringLiteral("mic"), db); }

double MixerService::HeadphoneVolumeDb() const {
    return MasterHeadphoneVolumeDb(QStringLiteral("mic"));
}

bool MixerService::HeadphoneMuted() const {
    return MasterHeadphoneMuted(QStringLiteral("mic"));
}

void MixerService::SetHeadphoneVolumeDb(double db) {
    SetMasterHeadphoneVolumeDb(QStringLiteral("mic"), db);
}

void MixerService::SetHeadphoneMuted(bool muted) {
    SetMasterHeadphoneMuted(QStringLiteral("mic"), muted);
}

// ---- multi-master buses ----------------------------------------------------

QString MixerService::masterDeviceLabel(const MasterBusState &m,
                                        const QStringList &captures,
                                        const QStringList &midis,
                                        bool *connected) const {
    const bool midi = m.busType == QLatin1String("midi");
    const QString match = midi ? m.midiPortMatch : m.captureMatch;
    // A bus with nothing pinned follows whatever is default, so there is no
    // device for it to be missing.
    if (connected) *connected = true;
    if (match.isEmpty()) return {};

    for (const QString &row : midi ? midis : captures) {
        const int tab = row.indexOf(QLatin1Char('\t'));
        if (tab > 0 && row.left(tab) == match) return row.mid(tab + 1);
    }
    if (connected) *connected = false;

    // Unplugged: what it called itself the last time it was here. Kept in the
    // profile precisely for this, so the name does not change depending on
    // whether the device happens to be connected.
    if (!m.deviceLabel.isEmpty()) return m.deviceLabel;

    // Never seen this device -- a profile written before the label was stored,
    // or one carried over from another machine. A MIDI match is "node|port"
    // and the port half is the readable part; a capture match is an ALSA node
    // name, which the table that auto-names the strips turns into a brand.
    if (midi) {
        const int bar = match.indexOf(QLatin1Char('|'));
        QString label = bar < 0 ? match : match.mid(bar + 1);
        if (label.endsWith(QStringLiteral(" (capture)"))) label.chop(10);
        return label;
    }
    const QString brand =
        QString::fromStdString(waveline::masterCaptureBrand(match.toStdString()));
    return brand == QLatin1String("Waveline") ? match : brand;
}

bool MixerService::rememberMasterDeviceLabels() {
    Profile &p = config_.live();
    const QStringList captures = CaptureDevices();
    bool anyMidi = false;
    for (const MasterBusState &m : p.masterBuses)
        anyMidi = anyMidi || m.busType == QLatin1String("midi");
    const QStringList midis = anyMidi ? MidiDevices() : QStringList{};

    bool changed = false;
    for (MasterBusState &m : p.masterBuses) {
        bool present = false;
        const QString label = masterDeviceLabel(m, captures, midis, &present);
        // Only while the device is here: the label read back for an absent one
        // is the remembered value (or a guess at it), and writing that back
        // would make the guess permanent.
        if (!present || label.isEmpty() || label == m.deviceLabel) continue;
        m.deviceLabel = label;
        changed = true;
    }
    if (changed) scheduleSave();
    return changed;
}

// Points the probe at whatever capture hardware exists right now. Called from
// the 4 Hz hardware poll, so the rolling median is built from readings spread
// across time rather than from a burst taken when a client happens to ask --
// a single reading of ALSA's `delay` is a phase of the buffer cycle, not a
// latency.
void MixerService::sampleCaptureDelays() {
    delayProbe_.beginSweep();
    for (const auto &n : engine_.nodes()) {
        if (n.isOurs || n.mediaClass != "Audio/Source") continue;
        if (n.alsaCard < 0) continue;
        delayProbe_.track(waveline::findCapturePcm(n.alsaCard));
    }
    // A device that has gone must stop being reported. Without this an
    // unplugged microphone keeps answering with the last latency it had, which
    // is the most convincing kind of wrong number.
    delayProbe_.forgetUntracked();
    delayProbe_.sample();
}

// Input latency, in microseconds, keyed by capture node name. Built in one
// pass because MasterBuses() wants it for every bus at once, and the node list
// is a scan of everything PipeWire knows about.
//
// Every figure here was read from the kernel. Nothing in this function knows
// what a quantum is, and that is the point: the version it replaces computed
// the answer from PipeWire's Latency param plus the graph clock plus the
// node's own node.latency request, and ranked two microphones that measured
// identically on the hardware 4x apart purely because one of them had asked
// for a short cycle and the other had not.
QHash<QString, qint64> MixerService::captureLatencies() const {
    QHash<QString, qint64> out;
    for (const auto &n : engine_.nodes()) {
        if (n.isOurs || n.alsaCard < 0) continue;
        // Reported before the measurement is even looked at. This device's
        // delay is set inside the device, so the capture-side figure is a
        // fraction of the truth -- and showing that fraction ranks a slow
        // microphone above a fast one, which is the exact failure the measured
        // number was introduced to fix.
        if (n.hidesLatency) {
            out.insert(QString::fromStdString(n.name), kLatencyHidden);
            continue;
        }
        const auto ref = waveline::findCapturePcm(n.alsaCard);
        if (!ref.valid()) continue;
        const qint64 us = delayProbe_.medianUs(ref);
        if (us == waveline::AlsaDelayProbe::kUnknown) continue;
        out.insert(QString::fromStdString(n.name), us);
    }
    return out;
}

QStringList MixerService::MasterBuses() const {
    QStringList out;
    const Profile &p = config_.live();
    // Once for the whole list rather than once per bus: both are a scan of
    // every node PipeWire knows about.
    const QStringList captures = CaptureDevices();
    const QHash<QString, qint64> latencies = captureLatencies();
    bool anyMidi = false;
    for (const MasterBusState &m : p.masterBuses)
        anyMidi = anyMidi || m.busType == QLatin1String("midi");
    const QStringList midis = anyMidi ? MidiDevices() : QStringList{};
    // A bus with nothing pinned records whatever the default input happens to
    // be, so that is the device whose latency it pays.
    const QString defaultSource =
        QString::fromStdString(engine_.defaultSourceName());

    for (const MasterBusState &m : p.masterBuses) {
        const MasterHwSlot *hw = masterHwSlot(m.id);
        bool connected = true;
        // The desktop's name for the pinned capture device, when it has one.
        const QString label = waveline::DesktopNames::instance().apply(
            m.captureMatch, masterDeviceLabel(m, captures, midis, &connected));
        // MIDI carries no audio and so no capture buffering; -1 is "nothing to
        // report", which the UI shows as no latency line at all.
        const bool midi = m.busType == QLatin1String("midi");
        const QString node = m.captureMatch.isEmpty() ? defaultSource : m.captureMatch;
        const qint64 latencyUs =
            (midi || node.isEmpty()) ? -1 : latencies.value(node, -1);
        out << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9")
                   .arg(m.id, m.name, midi ? m.midiPortMatch : m.captureMatch,
                        m.busType)
                   .arg(m.id == QLatin1String("mic") ? 1 : 0)
                   .arg(hw && hw->connected ? 1 : 0)
                   .arg(label)
                   .arg(connected ? 1 : 0)
                   .arg(latencyUs);
    }
    return out;
}

// ---- graph clock ---------------------------------------------------------

int MixerService::GraphQuantum() const { return config_.audio().graphQuantum; }

int MixerService::EffectiveGraphQuantum() const {
    const auto clock = engine_.graphClock();
    if (!clock.known) return 0;
    return static_cast<int>(clock.forcedQuantum ? clock.forcedQuantum
                                                : clock.quantum);
}

void MixerService::SetGraphQuantum(int frames) {
    // 0 releases the pin. Anything else is bounded by what PipeWire will
    // accept: below 32 is a real-time scheduling problem rather than a latency
    // setting, and 8192 is its own clock.quantum-limit.
    if (frames != 0 && (frames < 32 || frames > 8192)) return;
    if (config_.audio().graphQuantum == frames) return;
    config_.audio().graphQuantum = frames;
    scheduleSave();

    std::string err;
    if (!engine_.setForcedQuantum(static_cast<uint32_t>(frames), err)) {
        // Not fatal and not rolled back. The value is stored, and start()
        // asserts it again once PipeWire is up -- which is the case this
        // fails in.
        qWarning("waveline: could not set graph quantum: %s", err.c_str());
    }

    // Reopen every capture hop afterwards, or changing the latency leaves
    // inputs sounding robotic.
    //
    // A graph quantum change moves the ratio the capture resampler is running
    // at, and on an ALSA follower whose consumer stays attached across the
    // change that resampler can land in a permanent resync loop rather than
    // re-converging -- audibly robotic, and it stays that way until the node
    // is reopened. Every capture profile under devices/ sets node.lock-quantum
    // to stop exactly this, but lock-quantum only blocks *negotiated* changes;
    // 51-waveline-wave3.conf spells it out: "Forced quantum changes via
    // metadata still work." This control is a forced change, so it is the one
    // path that goes straight through the protection those profiles provide.
    //
    // The symptom reported from the field matches that mechanism precisely,
    // and rules out the obvious alternative of "the value is too low": it
    // happens going *up* to 85 ms as readily as going down to 5 ms, because
    // the fault is in the transition and not the destination. Setting another
    // latency does not heal it -- that is one more transition. Hotplugging the
    // same device never triggers it, because a fresh open negotiates cleanly.
    //
    // So do automatically what the manual rebuild button does. This is the
    // same two-phase quiet/rebuild used for hotplug and for
    // RebuildMasterCapture(): a quiet wait, then one clean recreate, which
    // gives every input a fresh clock negotiation at the new quantum. Passing
    // no ids means every master, and re-calling restarts the single-shot
    // timer, so flipping through several settings rebuilds once at the end
    // rather than once per step.
    scheduleCaptureSettle({});
    emit Changed();
}

int MixerService::OutputHeadroom() const { return config_.audio().outputHeadroom; }

int MixerService::EffectiveOutputHeadroom() const {
    // Only PCI sinks, because that is exactly what the rule matches. Including
    // USB devices here would report a permanent disagreement: a microphone with
    // a headphone output carries headroom from its own device profile and
    // always will.
    int seen = 0;
    bool any = false;
    for (const waveline::PwNode &n : engine_.nodes()) {
        if (n.isOurs || n.alsaHeadroom < 0) continue;
        if (n.mediaClass != "Audio/Sink") continue;
        if (n.name.rfind("alsa_output.pci-", 0) != 0) continue;
        if (any && n.alsaHeadroom != seen) return kHeadroomMixed;
        seen = n.alsaHeadroom;
        any = true;
    }
    // "Nothing to compare against" is not "zero". A machine with no PCI output
    // has nothing the rule can apply to, and for the first moments after start
    // no sink has been bound yet -- reporting 0 for either would put a "waiting
    // for a restart" warning in front of a user who is not waiting for one.
    return any ? seen : kHeadroomUnknown;
}

void MixerService::SetOutputHeadroom(int frames) {
    if (frames < 0 || frames > 8192) return;
    if (config_.audio().outputHeadroom == frames) return;
    config_.audio().outputHeadroom = frames;
    scheduleSave();

    QString err;
    if (!waveline::writeOutputHeadroom(frames, &err)) {
        // Stored anyway. The write fails when the unit file predates this
        // setting (no ~/.config/wireplumber in ReadWritePaths), and that is
        // fixed by reinstalling rather than by forgetting what was asked for --
        // start() writes it again next boot.
        lastError_ = err;
        qWarning("waveline: output headroom: %s", qUtf8Printable(err));
    }
    // No restart from here. It costs every stream on the machine a dropout, so
    // it is the user's call and the GUI asks; RestartWirePlumber() is the
    // separate step.
    emit Changed();
}

void MixerService::RestartWirePlumber() {
    QString err;
    if (!waveline::restartWirePlumber(&err)) {
        lastError_ = err;
        qWarning("waveline: %s", qUtf8Printable(err));
    }
}

bool MixerService::RealtimeScheduling() const { return config_.audio().realtime; }

void MixerService::SetRealtimeScheduling(bool on) {
    if (config_.audio().realtime == on) return;
    config_.audio().realtime = on;
    // Written now, not through scheduleSave(): the next thing that happens to
    // this process is the mixer restarting it, and a 1500 ms debounce loses the
    // setting to the restart it exists to be read by. Same work the debounce
    // timer does, just not later.
    flushPendingSave();
    // Saved and nothing else. The contexts that carry the old decision are
    // still running and will keep carrying it until they are built again; the
    // mixer restarts the daemon for that, after asking. Reporting it as done
    // here would be a lie that GraphDiagnostics() immediately contradicts.
    qInfo("waveline: real-time scheduling set to %s; applies when the daemon "
          "restarts", on ? "on" : "off");
    emit Changed();
}

bool MixerService::DspProfiling() const { return config_.diagnostics().dspProfiling; }

void MixerService::SetDspProfiling(bool on) {
    if (config_.diagnostics().dspProfiling == on) return;
    config_.diagnostics().dspProfiling = on;
    // Live, and nothing is rebuilt: the audio threads read one flag, so this
    // is the rare setting where "applied" and "saved" are the same instant.
    // Enabling also resets every counter, which is why the panel can say
    // "since you turned it on" and mean it.
    waveline::setDspProfiling(on);
    scheduleSave();
    qInfo("waveline: DSP profiling %s", on ? "on" : "off");
    emit Changed();
}

// One chain's stages added up. Averages sum because expectations do; the peaks
// only sum into a worst case that assumes every stage spiked in the same
// cycle, which is why it is never presented as the number that was measured.
namespace {

struct ChainTotals {
    double avgUs = 0.0;
    double peakUs = 0.0;
    double cycleUs = 0.0;
    uint64_t xruns = 0;
    uint64_t overruns = 0;
    uint32_t latencyFrames = 0;
    double driverXrunMs = 0.0;
    int running = 0;
};

ChainTotals sumStages(const std::vector<waveline::DspStageStats> &stages) {
    ChainTotals t;
    for (const auto &s : stages) {
        // Idle stages contribute nothing at all, delay included. A filter node
        // that exists but is not being scheduled has no audio going through it,
        // so whatever it would buffer is not being buffered -- counting its
        // declared delay put 20 ms on a channel whose second noise filter was
        // built for a published microphone nobody was recording from.
        if (s.idle()) continue;
        ++t.running;
        t.latencyFrames += s.latencyFrames;
        t.avgUs += s.avgUs;
        t.peakUs += s.maxUs;
        t.xruns += s.xruns;
        t.overruns += s.overruns;
        // Every stage in a driver group is handed the same cycle, so this is
        // an agreement rather than an average; taking the largest keeps a
        // stage that has just started on a stale figure from shrinking it.
        if (s.cycleUs > t.cycleUs) t.cycleUs = s.cycleUs;
        if (s.driverXrunMs > t.driverXrunMs) t.driverXrunMs = s.driverXrunMs;
    }
    return t;
}

QString msLabel(double us) { return QStringLiteral("%1 ms").arg(us / 1000.0, 0, 'f', 2); }

}  // namespace

QStringList MixerService::GraphDiagnostics() const {
    QStringList out;
    const auto row = [&out](const QString &k, const QString &v,
                            const QString &d = {}) {
        out << QStringLiteral("%1\t%2\t%3").arg(k, v, d);
    };

    const auto clock = engine_.graphClock();
    if (!clock.known) {
        row(tr("Graph clock"), tr("unknown"),
            tr("PipeWire has not reported its settings yet."));
        return out;
    }

    const uint32_t rate = clock.forcedRate ? clock.forcedRate : clock.rate;
    row(tr("Sample rate"),
        rate ? tr("%1 Hz").arg(rate) : tr("unknown"),
        clock.forcedRate ? tr("pinned") : tr("from configuration"));

    const uint32_t q = clock.forcedQuantum ? clock.forcedQuantum : clock.quantum;
    QString qDetail;
    if (clock.forcedQuantum)
        qDetail = tr("pinned; cannot renegotiate");
    else if (clock.minQuantum && clock.maxQuantum &&
             clock.minQuantum != clock.maxQuantum)
        qDetail = tr("negotiated, may move between %1 and %2")
                      .arg(clock.minQuantum)
                      .arg(clock.maxQuantum);
    else
        qDetail = tr("from configuration");
    row(tr("Graph cycle"),
        (q && rate) ? tr("%1 frames, %2 ms")
                          .arg(q)
                          .arg(q * 1000.0 / rate, 0, 'f', 1)
                    : tr("unknown"),
        qDetail);

    // Whether the audio threads are real-time, reported next to the cycle above
    // because that is the pair that decides whether this machine glitches: an
    // ordinary thread makes a 10.7 ms deadline on a loaded machine and misses a
    // 2.7 ms one. Counted live rather than assumed, because the way this fails
    // is that rtkit grants the first two dozen requests and refuses the rest --
    // so "did we ask for real-time" and "are we real-time" have different
    // answers, and only the second one is audible.
    const auto rt = waveline::scanDataLoops();
    if (!waveline::realtimeEnabled()) {
        // Checked before the counts, because every branch below reads a graph
        // with no real-time threads in it as a fault and sends the user to
        // install.sh. Switched off on purpose is not a fault, and saying so is
        // the difference between a setting and a bug report.
        row(tr("Real-time scheduling"), tr("off"),
            rt.dataLoops > 0
                ? tr("Turned off in Latency & Diagnostics. All %1 audio threads "
                     "run at ordinary priority, so a busy CPU can starve them "
                     "and be heard as clicks -- most likely at small buffer "
                     "sizes.")
                      .arg(rt.dataLoops)
                : tr("Turned off in Latency & Diagnostics."));
    } else if (rt.dataLoops == 0) {
        row(tr("Real-time scheduling"), tr("unknown"),
            tr("No PipeWire data loops found in this process yet."));
    } else if (rt.complete()) {
        row(tr("Real-time scheduling"),
            tr("%1 of %1 audio threads").arg(rt.dataLoops),
            rt.minPrio == rt.maxPrio
                ? tr("priority %1. Audio is scheduled ahead of ordinary work.")
                      .arg(rt.maxPrio)
                : tr("priority %1-%2. Audio is scheduled ahead of ordinary "
                     "work.")
                      .arg(rt.minPrio)
                      .arg(rt.maxPrio));
    } else {
        row(tr("Real-time scheduling"),
            tr("%1 of %2 audio threads").arg(rt.realtime).arg(rt.dataLoops),
            tr("%1 threads run at ordinary priority and a busy CPU will "
               "starve them, which is heard as clicks. This is rtkit's "
               "25-thread-per-user limit; Waveline's graph asks for more. "
               "Re-run install.sh to grant real-time privileges directly, "
               "then log out and back in.")
                .arg(rt.dataLoops - rt.realtime));
    }

    // The line this whole exercise was missing. A capture device winning the
    // driver role is how a 32 kHz webcam ended up clocking a guitar adapter,
    // and nothing anywhere said so.
    const QString driver = QString::fromStdString(engine_.driverNodeName());
    if (driver.isEmpty()) {
        row(tr("Driving the graph"), tr("nothing"),
            tr("No node is driving. Audio will not run in this state."));
    } else {
        const bool isInput = driver.startsWith(QLatin1String("alsa_input."));
        row(tr("Driving the graph"), driver,
            isInput ? tr("An input is the clock. Outputs should outrank inputs "
                         "-- is 50-waveline-driver-policy.conf installed?")
                    : tr("An output is the clock, which is what you want."));
    }

    // ---- what Waveline's own processing costs ------------------------------
    //
    // Everything above this point is the machine's latency: the buffer, the
    // clock, the device. None of it says what happens between a microphone
    // arriving and reaching the mix, and on a chain running noise suppression,
    // an EQ, a compressor and a creative rack that is the larger half of the
    // question "why does this glitch".
    const bool profiling = config_.diagnostics().dspProfiling;
    const std::vector<waveline::DspChainLoad> masterChains =
        graph_ ? graph_->masterDspChains() : std::vector<waveline::DspChainLoad>{};
    const std::vector<waveline::DspChainLoad> channelChains =
        graph_ ? graph_->channelDspChains() : std::vector<waveline::DspChainLoad>{};

    if (!profiling) {
        row(tr("DSP profiling"), tr("off"),
            tr("Switch it on in the Diagnostics tab to measure what each input "
               "device's effects cost per cycle, and to count the cycles they "
               "missed. It applies live -- nothing restarts and no audio is "
               "interrupted -- and is off by default because a measurement "
               "nobody is reading is not worth the two clock reads per stage "
               "per cycle it costs."));
    } else {
        // The missed-cycle count, and the only line here that is a fault
        // rather than a figure. PipeWire flags the individual node that did
        // not finish in time, so this can name the stage instead of leaving
        // six candidates and a shrug.
        uint64_t xruns = 0;
        uint64_t overruns = 0;
        uint64_t cycles = 0;
        int stages = 0;
        double droppedMs = 0.0;
        QString worst;
        QString slowest;
        uint64_t worstXruns = 0;
        uint64_t worstOverruns = 0;
        const auto label = [](const waveline::DspChainLoad &c,
                              const waveline::DspStageStats &s) {
            return tr("%1 on %2").arg(QString::fromStdString(s.kind),
                                      QString::fromStdString(c.name));
        };
        const auto tally = [&](const std::vector<waveline::DspChainLoad> &chains) {
            for (const auto &c : chains)
                for (const auto &s : c.stages) {
                    if (s.idle()) continue;
                    ++stages;
                    xruns += s.xruns;
                    overruns += s.overruns;
                    if (s.cycles > cycles) cycles = s.cycles;
                    if (s.driverXrunMs > droppedMs) droppedMs = s.driverXrunMs;
                    if (s.xruns > worstXruns) {
                        worstXruns = s.xruns;
                        worst = label(c, s);
                    }
                    if (s.overruns > worstOverruns) {
                        worstOverruns = s.overruns;
                        slowest = label(c, s);
                    }
                }
        };
        tally(masterChains);
        tally(channelChains);

        row(tr("DSP profiling"), tr("on"),
            tr("Timing %1 running stages across %2 input devices and %3 "
               "channels. Every figure below is per graph cycle and was reset "
               "when profiling was switched on.")
                .arg(stages)
                .arg(masterChains.size())
                .arg(channelChains.size()));

        // Two counts, deliberately reported together. xruns is PipeWire's
        // verdict -- it flags the node that did not finish -- and overruns is
        // ours, measured from the clock: this stage's callback alone took
        // longer than the whole cycle it was given. Ours can fire without
        // PipeWire's on a graph that recovered, and it is the earlier warning
        // of the two; PipeWire's is the one that was audible.
        if (xruns == 0 && overruns == 0) {
            row(tr("Missed cycles"), tr("none"),
                cycles == 0
                    ? tr("Nothing has been scheduled yet.")
                    : tr("No stage has missed its deadline or overrun its cycle "
                         "in %1 cycles. This is the number to watch when "
                         "lowering the graph cycle or adding effects.")
                          .arg(cycles));
        } else if (xruns == 0) {
            row(tr("Missed cycles"), tr("none, but %1 overruns").arg(overruns),
                tr("PipeWire has not reported a missed deadline, but %1 spent "
                   "longer than a whole cycle on its own %2 times in %3 cycles. "
                   "The graph absorbed it this time; it is the warning that "
                   "comes before the clicks do.")
                    .arg(slowest)
                    .arg(worstOverruns)
                    .arg(cycles));
        } else {
            QString detail =
                tr("A stage did not finish its work before the next cycle "
                   "began, which is heard as a click each time. Worst offender: "
                   "%1, %2 of them. ")
                    .arg(worst)
                    .arg(worstXruns);
            if (overruns > 0)
                detail += tr("%1 cycles were also overrun by a single stage. ")
                              .arg(overruns);
            if (droppedMs > 0.0)
                detail += tr("The graph clock reports %1 ms of audio dropped. ")
                              .arg(droppedMs, 0, 'f', 1);
            detail += tr("Either raise the graph cycle above, or turn off "
                         "whichever stage the breakdown below shows as the "
                         "expensive one.");
            row(tr("Missed cycles"), tr("%1 in %2 cycles").arg(xruns).arg(cycles),
                detail);
        }
    }

    // One chain's stages as a row, keyed by the capture node it hangs off so it
    // can be printed directly beneath that device's measured delay. The two
    // numbers are about the same microphone and mean entirely different things,
    // and half a table apart is how they get read as one.
    std::map<std::string, const waveline::DspChainLoad *> chainByNode;
    for (const waveline::DspChainLoad &c : masterChains) {
        const auto *bus = graph_ ? graph_->masterBus(c.id) : nullptr;
        if (bus && !bus->captureNode.empty()) chainByNode[bus->captureNode] = &c;
    }
    std::set<std::string> chainsReported;

    const auto dspRow = [&](const waveline::DspChainLoad &c, const QString &label) {
        chainsReported.insert(c.id);
        if (!profiling) return;

        const ChainTotals t = sumStages(c.stages);
        const QString what = tr("%1 — DSP").arg(label);
        // Declared by the stage, not timed: a filter that buffers a frame
        // before it can emit one pays that delay on any CPU, and it is the
        // only part of this row that belongs in the same units as the device
        // delay above it. See engine/dspprobe.h.
        const QString delay =
            t.latencyFrames > 0
                ? tr("+%1 ms delay").arg(t.latencyFrames / 48.0, 0, 'f', 1)
                : tr("no added delay");

        if (t.running == 0) {
            row(what, delay,
                tr("No stage in this chain has been scheduled since profiling "
                   "was switched on, so there is no cost to report. An input "
                   "with nothing listening to it sits idle like this."));
            return;
        }

        QString value = tr("%1, %2 per cycle").arg(delay, msLabel(t.avgUs));
        if (t.cycleUs > 0)
            value = tr("%1, %2 per cycle (%3%)")
                        .arg(delay, msLabel(t.avgUs))
                        .arg(t.avgUs * 100.0 / t.cycleUs, 0, 'f', 1);

        // Where the time actually goes. This is the line the whole feature is
        // for: "noise suppression 0.31, EQ 0.02" ends an argument about
        // whether the EQ is expensive in one glance.
        QStringList parts;
        for (const auto &s : c.stages) {
            if (s.idle()) continue;
            parts << tr("%1 %2").arg(QString::fromStdString(s.kind), msLabel(s.avgUs));
        }
        QString detail = tr("Stages: %1. ").arg(parts.join(QStringLiteral(", ")));
        detail += tr("Worst case %1 if every stage peaks in the same cycle").arg(
            msLabel(t.peakUs));
        if (t.cycleUs > 0)
            detail += tr(", against a %1 cycle").arg(msLabel(t.cycleUs));
        detail += QStringLiteral(". ");
        if (t.overruns > 0)
            detail += tr("%1 cycles where this chain alone ran longer than the "
                         "whole cycle. ")
                          .arg(t.overruns);
        detail += t.xruns > 0 ? tr("%1 missed cycles.").arg(t.xruns)
                              : tr("No missed cycles.");
        row(what, value, detail);
    };

    // Per-input measured delay, straight from ALSA.
    for (const auto &n : engine_.nodes()) {
        if (n.isOurs || n.mediaClass != "Audio/Source") continue;
        if (n.alsaCard < 0) continue;
        const auto ref = waveline::findCapturePcm(n.alsaCard);
        if (!ref.valid()) continue;
        const auto d = delayProbe_.detail(ref);
        const QString name = QString::fromStdString(
            n.description.empty() ? n.name : n.description);
        // Called on every path out of this iteration, so the DSP line follows
        // its device whether or not the delay could be measured -- an
        // unmeasurable device is not a device with no effects on it.
        const auto withDsp = [&] {
            auto it = chainByNode.find(n.name);
            if (it != chainByNode.end()) dspRow(*it->second, name);
        };

        if (n.hidesLatency) {
            row(name, tr("N/A"),
                tr("This device processes audio before handing it over, so its "
                   "delay is set inside it and cannot be measured from here. "
                   "Capture-side alone reads %1 ms, which is only part of it.")
                    .arg(d.medianUs == waveline::AlsaDelayProbe::kUnknown
                             ? tr("?")
                             : QString::number(d.medianUs / 1000.0, 'f', 1)));
            withDsp();
            continue;
        }

        if (d.medianUs == waveline::AlsaDelayProbe::kUnknown) {
            QString why;
            if (!d.running)
                why = tr("stream is not running");
            else if (d.rejected > 0 && d.samples == 0)
                // Not necessarily a broken device. A capture card with nothing
                // connected to its input runs its PCM and reports delay 0,
                // which is arguably honest and is certainly not a latency --
                // so the reading is dropped, but the device is not blamed for
                // it.
                why = tr("no usable reading (%1 dropped) -- an idle capture "
                         "card with no source does this").arg(d.rejected);
            else
                // The sample count is in the message on purpose. "Not enough
                // readings yet" that never resolves and "not enough readings
                // yet" that is filling up look identical otherwise, and the
                // first is a bug while the second is the first two seconds
                // after a device appears.
                why = tr("%1 of %2 readings so far (card %3, pcm%4c)")
                          .arg(d.samples).arg(5).arg(ref.card).arg(ref.device);
            row(name, tr("not measured"), why);
            withDsp();
            continue;
        }

        QString detail = tr("median of %1 readings, %2-%3 ms")
                             .arg(d.samples)
                             .arg(d.minUs / 1000.0, 0, 'f', 1)
                             .arg(d.maxUs / 1000.0, 0, 'f', 1);
        if (d.rate > 0)
            detail += tr("; %1 Hz, period %2").arg(d.rate).arg(d.periodSize);
        if (d.rejected > 0)
            detail += tr("; %1 rejected").arg(d.rejected);
        row(name, tr("%1 ms").arg(d.medianUs / 1000.0, 0, 'f', 1), detail);
        withDsp();
    }

    // Input devices the loop above never reached: a MIDI bus has no capture
    // PCM, and an unplugged microphone leaves its chain running on silence.
    // Both still have effects on them, and a chain that vanishes from the
    // table the moment its device does is a chain nobody can profile.
    for (const waveline::DspChainLoad &c : masterChains) {
        if (chainsReported.count(c.id)) continue;
        dspRow(c, QString::fromStdString(c.name));
    }

    // Everything the channel strips run, one row each. Not broken down by
    // device because it is not per device: this is what an application's audio
    // costs on its way through the mixer, and it is charged to the same cycle
    // as the microphone chains above.
    for (const waveline::DspChainLoad &c : channelChains)
        dspRow(c, QString::fromStdString(c.name));

    return out;
}

QStringList MixerService::CaptureDevices() const {
    QStringList out;
    for (const auto &n : engine_.nodes()) {
        if (n.isOurs || n.mediaClass != "Audio/Source") continue;
        if (!isCaptureDeviceNode(n.name)) continue;
        // Deliberately the device's own description, with no rename overlay
        // applied: this list is what rememberMasterDeviceLabels() files in the
        // profile as "what this device calls itself", and a desktop's display
        // name written in there would outlive the desktop that chose it. The
        // overlay goes on where a label is shown -- MasterBuses, ShellInputs.
        out << QStringLiteral("%1\t%2")
                   .arg(QString::fromStdString(n.name),
                        QString::fromStdString(n.description));
    }
    return out;
}

// ---- desktop shell integration ---------------------------------------------

QStringList MixerService::ShellInputs() const {
    QStringList out;
    const Profile &p = config_.live();
    const QStringList captures = CaptureDevices();
    bool anyMidi = false;
    for (const MasterBusState &m : p.masterBuses)
        anyMidi = anyMidi || m.busType == QLatin1String("midi");
    const QStringList midis = anyMidi ? MidiDevices() : QStringList{};

    // One pass over Levels() rather than one lookup per device: it is already
    // a whole-graph walk, and a panel calls this several times a second.
    QHash<QString, double> levels;
    for (const QString &row : Levels()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() >= 2) levels.insert(f[0], f[1].toDouble());
    }

    const int ncLimit = config_.shell().noiseSuppressionInputs;
    int index = 0;
    for (const MasterBusState &m : p.masterBuses) {
        // A MIDI bus is an instrument, not a microphone. It has no input gain
        // and no noise suppression, and a shell listing it among the machine's
        // microphones would be describing something that is not one.
        if (m.busType == QLatin1String("midi")) continue;

        bool connected = true;
        const QString label = waveline::DesktopNames::instance().apply(
            m.captureMatch, masterDeviceLabel(m, captures, midis, &connected));
        out << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10")
                   .arg(m.id, m.name, label)
                   .arg(connected ? 1 : 0)
                   .arg(MasterMicInputVolume(m.id))
                   .arg(MasterMicInputMuted(m.id) ? 1 : 0)
                   .arg(MasterNoiseSuppression(m.id) ? 1 : 0)
                   .arg(MasterNoiseIntensity(m.id))
                   .arg(levels.value(m.id + QStringLiteral("-out"), 0.0))
                   .arg(index < ncLimit ? 1 : 0);
        ++index;
    }
    return out;
}

QStringList MixerService::ShellOutputs() const {
    QStringList out;
    if (!graph_) return out;

    double busLevel = 0.0;
    for (const QString &row : Levels()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() >= 2 && f[0] == QLatin1String("monitor-mix")) {
            busLevel = f[1].toDouble();
            break;
        }
    }

    size_t i = 0;
    for (const waveline::MonitorOutputEntry &e : graph_->monitorOutputs()) {
        const bool online = graph_->monitorOutputOnline(i);
        QString desc = QString::fromStdString(e.description);
        if (desc.isEmpty()) desc = QString::fromStdString(e.sink);
        desc = waveline::DesktopNames::instance().apply(QString::fromStdString(e.sink), desc);
        const double level = (e.muted || !online) ? 0.0 : busLevel * e.volume;
        out << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6")
                   .arg(QString::fromStdString(e.sink), desc)
                   .arg(e.volume)
                   .arg(e.muted ? 1 : 0)
                   .arg(online ? 1 : 0)
                   .arg(level);
        ++i;
    }
    return out;
}

QStringList MixerService::MicrophoneConsumers() const {
    QStringList out;
    // Our own process. Every node and every stream this daemon creates carries
    // it, which is the one test that does not depend on having guessed all the
    // names we gave them -- and the names are not enough on their own: the
    // capture end of a loopback is named by PipeWire, not by us.
    const uint32_t self = static_cast<uint32_t>(::getpid());

    for (const auto &n : engine_.nodes()) {
        if (n.mediaClass != "Stream/Input/Audio") continue;
        if (n.isOurs || n.processId == self) continue;

        // Which node is feeding it decides what this stream *is*. A capture
        // linked from an Audio/Sink is reading that sink's monitor -- a screen
        // recorder taking desktop audio, or a meter -- and is not microphone
        // use however much it looks like it from the stream alone.
        waveline::PwNode src;
        QString sourceLabel;
        if (engine_.streamSourceNode(n.id, src)) {
            if (src.mediaClass == "Audio/Sink") continue;
            sourceLabel = QString::fromStdString(
                src.description.empty() ? src.name : src.description);
        } else {
            // Linked to nothing yet. Ordinary for the moment after a stream
            // appears, so it is reported without a source rather than dropped:
            // a shell that hid it would flicker the indicator on every start.
            sourceLabel = QString();
        }

        out << QStringLiteral("%1\t%2\t%3\t%4")
                   .arg(n.id)
                   .arg(QString::fromStdString(waveline::appDisplayName(n)),
                        QString::fromStdString(n.processBinary), sourceLabel);
    }
    return out;
}

int MixerService::ShellNoiseSuppressionInputs() const {
    return config_.shell().noiseSuppressionInputs;
}

void MixerService::SetShellNoiseSuppressionInputs(int count) {
    const int clamped = std::clamp(count, 0, 16);
    if (config_.shell().noiseSuppressionInputs == clamped) return;
    config_.shell().noiseSuppressionInputs = clamped;
    scheduleSave();
    emit Changed();
}

bool MixerService::ShellClientPresent() const { return shellPresent_; }

void MixerService::scheduleMicConsumerSignal() {
    if (!micConsumerTimer_.isActive()) micConsumerTimer_.start();
}

QStringList MixerService::MidiDevices() const {
    QStringList out;
    for (const auto &n : engine_.nodes()) {
        if (!isMidiDeviceNode(n)) continue;
        for (const std::string &port : engine_.outputPortNames(n.name)) {
            const QString match =
                QString::fromStdString(n.name) + QLatin1Char('|') +
                QString::fromStdString(port);
            QString label = QString::fromStdString(port);
            if (label.endsWith(QStringLiteral(" (capture)")))
                label.chop(10);
            const QString nodeLabel = QString::fromStdString(n.description);
            if (!nodeLabel.isEmpty() && nodeLabel != QString::fromStdString(n.name))
                label = nodeLabel + QStringLiteral(" — ") + label;
            out << QStringLiteral("%1\t%2").arg(match, label);
        }
    }
    return out;
}

bool MixerService::isMidiDeviceNode(const waveline::PwNode &n) const {
    if (n.isOurs || n.name.rfind("waveline-", 0) == 0) return false;
    if (n.mediaClass.find("Midi") == std::string::npos &&
        n.mediaClass.find("midi") == std::string::npos)
        return false;
    return true;
}

QString MixerService::pickUnusedMidiDevice() const {
    QSet<QString> used;
    const Profile &p = config_.live();
    for (const MasterBusState &m : p.masterBuses) {
        if (m.busType != QLatin1String("midi")) continue;
        if (!m.midiPortMatch.isEmpty()) used.insert(m.midiPortMatch);
    }
    if (graph_) {
        for (const auto &bus : graph_->masterBuses()) {
            if (bus.busType == "midi" && !bus.midiPortMatch.empty())
                used.insert(QString::fromStdString(bus.midiPortMatch));
        }
    }
    auto inUse = [&](const QString &match) {
        if (used.contains(match)) return true;
        const int sep = match.indexOf(QLatin1Char('|'));
        if (sep >= 0 && used.contains(match.left(sep))) return true;
        for (const QString &u : used) {
            if (u.contains(QLatin1Char('|'))) continue;
            if (match.startsWith(u + QLatin1Char('|'))) return true;
        }
        return false;
    };
    for (const QString &row : MidiDevices()) {
        const int tab = row.indexOf(QLatin1Char('\t'));
        const QString match = tab < 0 ? row : row.left(tab);
        if (!inUse(match)) return match;
    }
    return {};
}

QString MixerService::AddMasterBus(const QString &name) {
    return AddMasterBusEx(name, QStringLiteral("capture"), {});
}

QString MixerService::AddMasterBusEx(const QString &name, const QString &busType,
                                     const QString &deviceMatch) {
    (void)name;
    if (!graph_) return {};
    Profile &p = config_.live();
    syncPrimaryBusFromLegacy(p);

    // Drop graph buses that were left behind when a previous add rolled back
    // config but the chain nodes were already created.
    for (const auto &bus : graph_->masterBuses()) {
        if (waveline::isPrimaryMaster(bus.id)) continue;
        if (!masterBusState(p, QString::fromStdString(bus.id))) {
            std::string err;
            graph_->removeMasterBus(bus.id, err);
        }
    }

    if (p.masterBuses.size() >= static_cast<int>(waveline::kMaxMasterBuses)) {
        lastError_ = tr("Maximum number of input devices reached.");
        return {};
    }
    const QString id = nextMasterId();
    if (id.isEmpty()) {
        lastError_ = tr("Could not allocate an input device id.");
        return {};
    }
    MasterBusState m;
    m.id = id;
    m.busType = busType.isEmpty() ? QStringLiteral("capture") : busType;
    // How a device arrives, rather than whatever the struct happens to default
    // to: noise suppression off, because a device is added before anyone has
    // heard it and the denoiser is the one stage that can make a source sound
    // wrong rather than merely unprocessed -- and the monitor fed from the FX
    // chain (the violet heartbeat), so what you hear while setting the device
    // up is what the mixes get.
    m.noiseSuppression = false;
    m.micMonitorFx = true;
    if (m.busType == QLatin1String("midi")) {
        m.midiPortMatch =
            deviceMatch.isEmpty() ? pickUnusedMidiDevice() : deviceMatch;
        m.micStereo = false;
        for (const MasterBusState &other : p.masterBuses) {
            if (other.busType != QLatin1String("midi") ||
                other.soundfontPath.isEmpty())
                continue;
            m.soundfontPath = other.soundfontPath;
            m.soundfontPaths = other.soundfontPaths;
            break;
        }
    } else {
        m.captureMatch =
            deviceMatch.isEmpty() ? pickUnusedCaptureDevice() : deviceMatch;
    }
    p.masterBuses.append(m);
    assignMasterNames(p, profile_, graph_.get());
    if (MasterBusState *added = masterBusState(p, id)) {
        (void)added;
    }

    const MasterBusState *added = masterBusState(p, id);
    if (!added) {
        p.masterBuses.removeLast();
        return {};
    }

    std::string err;
    if (!graph_->addMasterBus(id.toStdString(), added->name.toStdString(),
                              added->busType.toStdString(), err)) {
        p.masterBuses.removeLast();
        assignMasterNames(p, profile_, graph_.get());
        std::string rmErr;
        graph_->removeMasterBus(id.toStdString(), rmErr);
        lastError_ = QString::fromStdString(err);
        return {};
    }

    graph_->setMasterCaptureMatch(id.toStdString(), added->captureMatch.toStdString());
    if (added->busType == QLatin1String("midi")) {
        graph_->setMasterMidiPortMatch(id.toStdString(),
                                       added->midiPortMatch.toStdString());
        if (!added->soundfontPath.isEmpty())
            graph_->setMasterSoundfontPath(id.toStdString(),
                                           added->soundfontPath.toStdString());
    }

    for (const MasterBusState &bus : p.masterBuses)
        graph_->setMasterName(bus.id.toStdString(), bus.name.toStdString());

    applyMasterFx(id);
    rememberMasterDeviceLabels();

    syncLegacyFromPrimaryBus(p);
    syncAlsaAliases();
    if (added->busType == QLatin1String("midi"))
        finishNewMidiMaster(id);
    else
        scheduleRewire();
    scheduleSave();
    emit Changed();
    return id;
}

void MixerService::RemoveMasterBus(const QString &id) {
    if (!graph_ || id == QLatin1String("mic")) return;
    if (meters_) {
        const std::string sid = id.toStdString();
        meters_->unwatch(sid + "-in");
        meters_->unwatch(sid + "-out");
        meters_->unwatch(sid + "-src");
    }
    Profile &p = config_.live();
    if (!masterBusState(p, id)) {
        lastError_ = tr("No such input device: %1").arg(id);
        return;
    }

    if (graph_->masterBus(id.toStdString())) {
        std::string err;
        if (!graph_->removeMasterBus(id.toStdString(), err)) {
            lastError_ = QString::fromStdString(err);
            return;
        }
    }

    scrubMasterReferences(p, id);
    for (int i = 0; i < p.masterBuses.size(); ++i) {
        if (p.masterBuses[i].id == id) {
            p.masterBuses.removeAt(i);
            break;
        }
    }
    masterHw_.erase(id);
    assignMasterNames(p, profile_, graph_.get());
    if (graph_) {
        for (const MasterBusState &bus : p.masterBuses)
            graph_->setMasterName(bus.id.toStdString(), bus.name.toStdString());
    }
    syncAlsaAliases();
    scheduleSave();
    emit Changed();
}

bool MixerService::RebuildMasterCapture(const QString &id) {
    if (!graph_ || id.isEmpty()) {
        lastError_ = tr("No such input device.");
        return false;
    }
    if (!graph_->masterBus(id.toStdString())) {
        lastError_ = tr("No such input device: %1").arg(id);
        return false;
    }
    // Rebuilding immediately is probabilistic: freshly-created filter nodes
    // can attach while PipeWire is still changing the graph's rate/quantum,
    // leaving the capture resampler in a permanent resync loop. The same
    // two-phase quiet/rebuild used for hotplug gives every microphone a clean
    // clock negotiation before audio is admitted to the DSP graph.
    scheduleCaptureSettle({id});
    qInfo("waveline: scheduled settled capture rebuild for master '%s' (manual)",
          qUtf8Printable(id));
    emit Changed();
    return true;
}

void MixerService::SetMasterCaptureDevice(const QString &id, const QString &nodeMatch) {
    if (!graph_) return;
    MasterBusState *m = masterBusState(config_.live(), id);
    if (!m) return;
    const QString match = effectiveMasterCaptureMatch(id, nodeMatch);
    if (m->captureMatch == match) return;
    const QString oldMatch = m->captureMatch;

    markMasterDisconnected(id);

    m->captureMatch = match;
    Profile &p = config_.live();
    if (!m->nameCustom) assignMasterNames(p, profile_, graph_.get());
    graph_->setMasterCaptureMatch(id.toStdString(), match.toStdString());
    graph_->setMasterName(id.toStdString(), m->name.toStdString());
    if (id == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());

    engine_.sync();
    std::string err;
    const std::string sid = id.toStdString();
    if (!graph_->relinkMasterHwCapture(sid, err)) {
        // Keep the old selector input active if the newly selected node races
        // away. No global rewire: deleting a hardware capture link is what
        // stalls PipeWire's clock and silences every output.
        m->captureMatch = oldMatch;
        graph_->setMasterCaptureMatch(sid, oldMatch.toStdString());
        std::string rollbackErr;
        graph_->relinkMasterHwCapture(sid, rollbackErr);
        if (!m->nameCustom) assignMasterNames(p, profile_, graph_.get());
        graph_->setMasterName(sid, m->name.toStdString());
        if (id == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());
        lastError_ = QString::fromStdString(err);
        qWarning("waveline: capture selection for %s failed: %s",
                 qUtf8Printable(id), err.c_str());
    } else {
        applyMasterFx(id);
        applyMasterInputVolume(id);
        graph_->applyMasterPathLevels(sid);
        if (masterHasWave3Hw(id)) pollMasterHardware(id);
        qInfo("waveline: capture selected for %s -> %s", qUtf8Printable(id),
              qUtf8Printable(match));
    }
    // The device is here right now, which is the only time its own name can be
    // read; whichever branch above ran, the bus points at a device it should
    // remember the name of.
    rememberMasterDeviceLabels();
    scheduleSave();
    emit Changed();
}

void MixerService::SetMasterMidiPort(const QString &id, const QString &nodeMatch) {
    if (!graph_) return;
    MasterBusState *m = masterBusState(config_.live(), id);
    if (!m || m->busType != QLatin1String("midi")) return;
    if (m->midiPortMatch == nodeMatch) return;
    m->midiPortMatch = nodeMatch;
    Profile &p = config_.live();
    if (!m->nameCustom) assignMasterNames(p, profile_, graph_.get());
    graph_->setMasterMidiPortMatch(id.toStdString(), nodeMatch.toStdString());
    graph_->setMasterName(id.toStdString(), m->name.toStdString());
    // Relink only the MIDI hardware edge — a full rewire tears down the synth
    // chain and often leaves synth->gain stuck inactive.
    const QString masterId = id;
    QTimer::singleShot(0, this, [this, masterId] {
        if (!graph_) return;
        std::string err;
        if (!graph_->rewireMasterMidiInput(masterId.toStdString(), err))
            qWarning("waveline: MIDI port for %s: %s", qUtf8Printable(masterId),
                     err.c_str());
    });
    rememberMasterDeviceLabels();
    scheduleSave();
    emit Changed();
}

QStringList MixerService::MasterSoundfonts(const QString &id) const {
    QStringList out;
    const MasterBusState *m = masterBusState(config_.live(), id);
    if (!m) return out;
    for (const QString &path : m->soundfontPaths) {
        out << QStringLiteral("%1\t%2")
                   .arg(path, path == m->soundfontPath ? QStringLiteral("1")
                                                       : QStringLiteral("0"));
    }
    if (m->soundfontPath.isEmpty()) return out;
    bool listed = false;
    for (const QString &row : out) {
        if (row.startsWith(m->soundfontPath + QLatin1Char('\t'))) {
            listed = true;
            break;
        }
    }
    if (!listed)
        out << QStringLiteral("%1\t1").arg(m->soundfontPath);
    return out;
}

void MixerService::AddMasterSoundfont(const QString &id, const QString &path) {
    if (path.isEmpty()) return;
    MasterBusState *m = masterBusState(config_.live(), id);
    if (!m || m->busType != QLatin1String("midi")) return;
    if (m->soundfontPaths.contains(path)) return;
    m->soundfontPaths.append(path);
    if (m->soundfontPath.isEmpty()) {
        m->soundfontPath = path;
        if (graph_) graph_->setMasterSoundfontPath(id.toStdString(), path.toStdString());
    }
    scheduleSave();
    emit Changed();
}

void MixerService::RemoveMasterSoundfont(const QString &id, const QString &path) {
    MasterBusState *m = masterBusState(config_.live(), id);
    if (!m || m->busType != QLatin1String("midi")) return;
    m->soundfontPaths.removeAll(path);
    if (m->soundfontPath == path) {
        m->soundfontPath = m->soundfontPaths.isEmpty() ? QString() : m->soundfontPaths.first();
        if (graph_)
            graph_->setMasterSoundfontPath(id.toStdString(),
                                           m->soundfontPath.toStdString());
    }
    scheduleSave();
    emit Changed();
}

void MixerService::SetMasterSoundfont(const QString &id, const QString &path) {
    MasterBusState *m = masterBusState(config_.live(), id);
    if (!m || m->busType != QLatin1String("midi")) return;
    if (m->soundfontPath == path) return;
    m->soundfontPath = path;
    if (!path.isEmpty() && !m->soundfontPaths.contains(path))
        m->soundfontPaths.append(path);
    if (graph_) graph_->setMasterSoundfontPath(id.toStdString(), path.toStdString());
    scheduleSave();
    emit Changed();
}

void MixerService::SetMasterName(const QString &id, const QString &name) {
    Profile &p = config_.live();
    MasterBusState *m = masterBusState(p, id);
    if (!m) return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        m->nameCustom = false;
        assignMasterNames(p, profile_, graph_.get());
    } else {
        Profile scratch = p;
        for (MasterBusState &sm : scratch.masterBuses) {
            if (sm.id == id) sm.nameCustom = false;
        }
        assignMasterNames(scratch, profile_, graph_.get());
        const MasterBusState *autoBus = masterBusState(scratch, id);
        const QString autoName = autoBus ? autoBus->name : QString();
        if (!autoName.isEmpty() && trimmed == autoName) {
            m->nameCustom = false;
            m->name = autoName;
        } else {
            m->name = trimmed;
            m->nameCustom = true;
        }
    }
    if (graph_) graph_->setMasterName(id.toStdString(), m->name.toStdString());
    syncAlsaAliases();
    scheduleSave();
    emit Changed();
}

void MixerService::SetChannelMasterMic(const QString &channelId, const QString &masterId) {
    SetChannelMasterMics(channelId, {masterId});
}

void MixerService::SetChannelMasterMics(const QString &channelId,
                                        const QStringList &masterIds) {
    if (!graph_) return;
    Profile &p = config_.live();
    const QStringList resolved = resolveMasterMicIds(p, masterIds);
    auto it = p.channelEffects.find(channelId);
    if (it == p.channelEffects.end())
        it = p.channelEffects.insert(channelId, ChannelEffectsState{});
    if (it->masterMicIds == resolved) return;
    it->masterMicIds = resolved;
    graph_->setChannelMasterMics(channelId.toStdString(), toStdIds(resolved));
    if (it->micSource) {
        std::string err;
        graph_->rewireChannelMicSource(channelId.toStdString(), err);
    }
    scheduleSave();
    emit Changed();
}

QString MixerService::ChannelMasterMic(const QString &channelId) const {
    return ChannelMasterMics(channelId).value(0, QStringLiteral("mic"));
}

QStringList MixerService::ChannelMasterMics(const QString &channelId) const {
    const Profile &p = config_.live();
    const auto it = p.channelEffects.constFind(channelId);
    if (it == p.channelEffects.constEnd()) return {QStringLiteral("mic")};
    return resolveMasterMicIds(p, it->masterMicIds);
}

bool MixerService::HasHardwareControlsFor(const QString &masterId) const {
    return masterHasWave3Hw(masterId);
}

bool MixerService::MasterDeviceConnected(const QString &masterId) const {
    const MasterHwSlot *hw = masterHwSlot(masterId);
    return hw && hw->connected;
}

QString MixerService::MasterDeviceFirmware(const QString &masterId) const {
    const MasterHwSlot *hw = masterHwSlot(masterId);
    return hw ? QString::fromStdString(hw->info.firmwareVersion) : QString();
}

double MixerService::MasterMicVolume(const QString &masterId, const QString &mix) const {
    if (masterId == QLatin1String("mic"))
        return MicVolume(mix);
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return 0.0;
    return parseMix(mix) == waveline::Mix::Monitor ? m->mix.monitorVolume
                                                   : m->mix.streamVolume;
}

void MixerService::SetMasterMicVolume(const QString &masterId, const QString &mix,
                                      double volume) {
    if (masterId == QLatin1String("mic")) {
        SetMicVolume(mix, volume);
        return;
    }
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    const double v = qBound(0.0, volume, 1.5);
    if (parseMix(mix) == waveline::Mix::Monitor)
        m->mix.monitorVolume = v;
    else
        m->mix.streamVolume = v;
    if (graph_) {
        const waveline::Mix wm = parseMix(mix);
        graph_->setMasterVolume(masterId.toStdString(), wm, static_cast<float>(v));
    }
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterMicMixMuted(const QString &masterId, const QString &mix) const {
    if (masterId == QLatin1String("mic")) return MicMixMuted(mix);
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return false;
    return parseMix(mix) == waveline::Mix::Monitor ? m->mix.monitorMuted : m->mix.streamMuted;
}

void MixerService::SetMasterMicMixMuted(const QString &masterId, const QString &mix,
                                        bool muted) {
    if (masterId == QLatin1String("mic")) {
        SetMicMuted(mix, muted);
        return;
    }
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    if (parseMix(mix) == waveline::Mix::Monitor)
        m->mix.monitorMuted = muted;
    else
        m->mix.streamMuted = muted;
    if (graph_) {
        const waveline::Mix wm = parseMix(mix);
        graph_->setMasterMuted(masterId.toStdString(), wm, muted);
    }
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterMicEffectsEnabled(const QString &masterId) const {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->micEffectsEnabled : true;
}

void MixerService::SetMasterMicEffectsEnabled(const QString &masterId, bool on) {
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    m->micEffectsEnabled = on;
    if (masterId == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());
    applyMasterFx(masterId);
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterMicMonitorFx(const QString &masterId) const {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->micMonitorFx : false;
}

void MixerService::SetMasterMicMonitorFx(const QString &masterId, bool on) {
    if (!graph_) return;
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    m->micMonitorFx = on;
    graph_->setMasterMicMonitorFx(masterId.toStdString(), on);
    if (masterId == QLatin1String("mic")) {
        syncLegacyFromPrimaryBus(config_.live());
        rewireMicMonitorPath();
    } else {
        std::string err;
        graph_->rewireMasterSoftwareMonitor(masterId.toStdString(), err);
    }
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterMicStereo(const QString &masterId) const {
    if (masterId == QLatin1String("mic")) return MicStereo();
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->micStereo : true;
}

void MixerService::SetMasterMicStereo(const QString &masterId, bool on) {
    if (masterId == QLatin1String("mic")) {
        SetMicStereo(on);
        return;
    }
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m || !graph_) return;
    if (m->micStereo == on) return;
    m->micStereo = on;
    std::string err;
    if (!graph_->setMasterMicStereo(masterId.toStdString(), on, err)) {
        lastError_ = QString::fromStdString(err);
        return;
    }
    graph_->setMasterSoftwareMonitor(masterId.toStdString(), m->softwareMonitor);
    graph_->setMasterVolume(masterId.toStdString(), waveline::Mix::Stream,
                            static_cast<float>(m->mix.streamVolume));
    graph_->setMasterMuted(masterId.toStdString(), waveline::Mix::Stream,
                           m->mix.streamMuted);
    if (m->softwareMonitor) {
        graph_->setMasterVolume(masterId.toStdString(), waveline::Mix::Monitor,
                                static_cast<float>(m->mix.monitorVolume));
        graph_->setMasterMuted(masterId.toStdString(), waveline::Mix::Monitor,
                               m->mix.monitorMuted);
    }
    scheduleSave();
    emit Changed();
}

double MixerService::MasterMicInputVolume(const QString &masterId) const {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->micInputVolume : 1.0;
}

void MixerService::SetMasterMicInputVolume(const QString &masterId, double volume) {
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    m->micInputVolume = std::clamp(volume, 0.0, 1.0);
    if (masterId == QLatin1String("mic")) {
        config_.live().micInputVolume = m->micInputVolume;
        syncLegacyFromPrimaryBus(config_.live());
    }
    applyMasterInputVolume(masterId);
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterMicInputMuted(const QString &masterId) const {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->micInputMuted : false;
}

void MixerService::SetMasterMicInputMuted(const QString &masterId, bool muted) {
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    m->micInputMuted = muted;
    if (masterId == QLatin1String("mic")) {
        config_.live().micInputMuted = muted;
        syncLegacyFromPrimaryBus(config_.live());
    }
    applyMasterInputVolume(masterId);
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterDeEsser(const QString &masterId) const {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->deEsser : false;
}

void MixerService::SetMasterDeEsser(const QString &masterId, bool on) {
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m || m->deEsser == on) return;
    m->deEsser = on;
    applyMasterFx(masterId);
    scheduleSave();
    emit Changed();
}

double MixerService::MasterDeEsserIntensity(const QString &masterId) const {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->deEsserIntensity : 0.5;
}

void MixerService::SetMasterDeEsserIntensity(const QString &masterId, double value) {
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    const double v = std::clamp(value, 0.0, 1.0);
    if (qFuzzyCompare(m->deEsserIntensity, v)) return;
    m->deEsserIntensity = v;
    applyMasterFx(masterId);
    scheduleSave();
    emit Changed();
}

// The stage a channel id and stage name point at. "mic" is the shared pair:
// its input stage is the primary input device's own microphone, its output
// stage the App Audio template every inheriting channel mirrors.
ChannelFxStageState *MixerService::deEsserStage(const QString &channelId,
                                                const QString &stage) {
    Profile &p = config_.live();
    const bool output = parseFxStage(stage) == waveline::FxStage::Output;
    if (channelId == QLatin1String("mic")) {
        if (output) return &p.masterOutput;
        return nullptr;   // handled by the master-bus pair above
    }
    auto it = p.channelEffects.find(channelId);
    if (it == p.channelEffects.end())
        it = p.channelEffects.insert(channelId, ChannelEffectsState{});
    return output ? &it->output : &it->input;
}

bool MixerService::ChannelDeEsser(const QString &channelId,
                                  const QString &stage) const {
    if (channelId == QLatin1String("mic") &&
        parseFxStage(stage) != waveline::FxStage::Output)
        return MasterDeEsser(channelId);
    auto *self = const_cast<MixerService *>(this);
    const ChannelFxStageState *st = self->deEsserStage(channelId, stage);
    return st ? st->deEsser : false;
}

void MixerService::SetChannelDeEsser(const QString &channelId, const QString &stage,
                                     bool on) {
    if (channelId == QLatin1String("mic") &&
        parseFxStage(stage) != waveline::FxStage::Output) {
        SetMasterDeEsser(channelId, on);
        return;
    }
    ChannelFxStageState *st = deEsserStage(channelId, stage);
    if (!st || st->deEsser == on) return;
    st->deEsser = on;
    if (channelId == QLatin1String("mic"))
        applyMasterOutputFx();
    else
        applyChannelFx(channelId);
    scheduleSave();
    emit Changed();
}

double MixerService::ChannelDeEsserIntensity(const QString &channelId,
                                             const QString &stage) const {
    if (channelId == QLatin1String("mic") &&
        parseFxStage(stage) != waveline::FxStage::Output)
        return MasterDeEsserIntensity(channelId);
    auto *self = const_cast<MixerService *>(this);
    const ChannelFxStageState *st = self->deEsserStage(channelId, stage);
    return st ? st->deEsserIntensity : 0.5;
}

void MixerService::SetChannelDeEsserIntensity(const QString &channelId,
                                              const QString &stage, double value) {
    if (channelId == QLatin1String("mic") &&
        parseFxStage(stage) != waveline::FxStage::Output) {
        SetMasterDeEsserIntensity(channelId, value);
        return;
    }
    ChannelFxStageState *st = deEsserStage(channelId, stage);
    if (!st) return;
    const double v = std::clamp(value, 0.0, 1.0);
    if (qFuzzyCompare(st->deEsserIntensity, v)) return;
    st->deEsserIntensity = v;
    if (channelId == QLatin1String("mic"))
        applyMasterOutputFx();
    else
        applyChannelFx(channelId);
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterNoiseSuppression(const QString &masterId) const {
    if (masterId == QLatin1String("mic")) return NoiseSuppression();
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->noiseSuppression : true;
}

void MixerService::SetMasterNoiseSuppression(const QString &masterId, bool on) {
    if (masterId == QLatin1String("mic")) {
        SetNoiseSuppression(on);
        return;
    }
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m || m->noiseSuppression == on) return;
    m->noiseSuppression = on;
    applyMasterFx(masterId);
    scheduleSave();
    emit Changed();
}

double MixerService::MasterNoiseIntensity(const QString &masterId) const {
    if (masterId == QLatin1String("mic")) return NoiseIntensity();
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->noiseIntensity : 1.0;
}

void MixerService::SetMasterNoiseIntensity(const QString &masterId, double value) {
    if (masterId == QLatin1String("mic")) {
        SetNoiseIntensity(value);
        return;
    }
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    const double v = qBound(0.0, value, 1.0);
    if (qFuzzyCompare(m->noiseIntensity, v)) return;
    m->noiseIntensity = v;
    applyMasterFx(masterId);
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterSoftwareMonitor(const QString &masterId) const {
    if (masterId == QLatin1String("mic")) return SoftwareMonitor();
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? m->softwareMonitor : false;
}

void MixerService::SetMasterSoftwareMonitor(const QString &masterId, bool on) {
    if (masterId == QLatin1String("mic")) {
        SetSoftwareMonitor(on);
        return;
    }
    if (!graph_) return;
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m || m->softwareMonitor == on) return;
    m->softwareMonitor = on;
    graph_->setMasterSoftwareMonitor(masterId.toStdString(), on);
    scheduleSave();
    emit Changed();
}

QString MixerService::MasterChannelEffects(const QString &masterId,
                                           const QString &stage) const {
    if (masterId == QLatin1String("mic")) return ChannelEffects(masterId, stage);
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m || parseFxStage(stage) == waveline::FxStage::Output) return QString();
    return fxToTabString(toFxSettings(m->micFx));
}

void MixerService::SetMasterChannelEffects(const QString &masterId, const QString &stage,
                                           bool lowCut, int lowCutHz, bool eq, double lowDb,
                                           double midDb, double highDb) {
    if (!graph_) return;
    waveline::ChannelFxSettings s;
    s.lowCut = lowCut;
    s.lowCutHz = (lowCutHz == 120) ? 120 : 80;
    s.eq = eq;
    s.lowDb = static_cast<float>(qBound(-12.0, lowDb, 12.0));
    s.midDb = static_cast<float>(qBound(-12.0, midDb, 12.0));
    s.highDb = static_cast<float>(qBound(-12.0, highDb, 12.0));
    Profile &p = config_.live();
    if (masterId == QLatin1String("mic")) {
        if (parseFxStage(stage) == waveline::FxStage::Output) {
            applyEasyFxFields(p.masterOutput.fx, s);
            applyMasterOutputFx();
        } else {
            if (MasterBusState *m = masterBusState(p, masterId))
                applyEasyFxFields(m->micFx, s);
            syncLegacyFromPrimaryBus(p);
            applyMasterFx(masterId);
        }
    } else if (MasterBusState *m = masterBusState(p, masterId)) {
        applyEasyFxFields(m->micFx, s);
        applyMasterFx(masterId);
    }
    scheduleSave();
    emit Changed();
}

void MixerService::SetMasterProEq(const QString &masterId, const QString &stage,
                                  bool advanced, const QString &bands) {
    if (!graph_) return;
    Profile &p = config_.live();
    if (masterId == QLatin1String("mic")) {
        if (parseFxStage(stage) == waveline::FxStage::Output) {
            applyProEqFields(p.masterOutput.fx, advanced, bands);
            applyMasterOutputFx();
        } else {
            if (MasterBusState *m = masterBusState(p, masterId))
                applyProEqFields(m->micFx, advanced, bands);
            syncLegacyFromPrimaryBus(p);
            applyMasterFx(masterId);
        }
    } else if (MasterBusState *m = masterBusState(p, masterId)) {
        applyProEqFields(m->micFx, advanced, bands);
        applyMasterFx(masterId);
    }
    scheduleSave();
    emit Changed();
}

QString MixerService::MasterMicDynamics(const QString &masterId) const {
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    return m ? dynamicsToTabString(m->micDynamics) : QString();
}

void MixerService::SetMasterMicDynamics(const QString &masterId, bool gate,
                                        double gateThresholdDb, double gateAttackMs,
                                        double gateReleaseMs, bool compressor,
                                        double compThresholdDb, double compRatio,
                                        double compAttackMs, double compReleaseMs,
                                        double compKneeDb, double makeupGainDb,
                                        bool autoMakeup, bool limiter,
                                        double limitThresholdDb, double limitAttackMs,
                                        double limitReleaseMs) {
    MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    m->micDynamics =
        dynamicsFromArgs(gate, gateThresholdDb, gateAttackMs, gateReleaseMs, compressor,
                         compThresholdDb, compRatio, compAttackMs, compReleaseMs, compKneeDb,
                         makeupGainDb, autoMakeup, limiter, limitThresholdDb, limitAttackMs,
                         limitReleaseMs);
    if (masterId == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());
    applyMasterFx(masterId);
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterClipguard(const QString &masterId) const {
    const MasterHwSlot *hw = masterHwSlot(masterId);
    return hw ? hw->state.clipguard : false;
}

void MixerService::SetMasterClipguard(const QString &masterId, bool on) {
    MasterHwSlot *hw = masterHwSlot(masterId);
    if (!hw || !hw->dev.isOpen()) return;
    hw->settleTicks = 0;
    waveline::ClaimGuard guard(hw->dev);
    if (!guard) return;
    if (auto r = hw->dev.setClipguard(on); !r)
        lastError_ = QString::fromStdString(r.message);
    else
        hw->dev.readState(hw->state);
    if (MasterBusState *m = masterBusState(config_.live(), masterId)) {
        m->hwClipguard = on ? 1 : 0;
        if (masterId == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());
    }
    scheduleSave();
    emit Changed();
}

int MixerService::MasterHardwareMonitor(const QString &masterId) const {
    const MasterHwSlot *hw = masterHwSlot(masterId);
    return hw ? hw->state.monitorPercent : 0;
}

void MixerService::SetMasterHardwareMonitor(const QString &masterId, int percent) {
    MasterHwSlot *hw = masterHwSlot(masterId);
    if (!hw || !hw->dev.isOpen()) return;
    percent = qBound(0, percent, 100);
    hw->settleTicks = 0;
    waveline::ClaimGuard guard(hw->dev);
    if (!guard) return;
    if (auto r = hw->dev.setMonitorPercent(percent); !r) {
        lastError_ = QString::fromStdString(r.message);
        return;
    }
    hw->state.monitorPercent = percent;
    if (MasterBusState *m = masterBusState(config_.live(), masterId)) {
        m->hardwareMonitor = percent;
        if (masterId == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());
    }
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterMicMuted(const QString &masterId) const {
    const MasterHwSlot *hw = masterHwSlot(masterId);
    return hw ? hw->state.micMuted : false;
}

void MixerService::SetMasterHardwareMicMute(const QString &masterId, bool muted) {
    MasterHwSlot *hw = masterHwSlot(masterId);
    if (!hw || !hw->dev.isOpen()) return;
    hw->settleTicks = 0;
    waveline::ClaimGuard guard(hw->dev);
    if (!guard) return;
    if (auto r = hw->dev.setMicMute(muted); !r)
        lastError_ = QString::fromStdString(r.message);
    else
        hw->dev.readState(hw->state);
    if (MasterBusState *m = masterBusState(config_.live(), masterId)) {
        m->hwMicMuted = muted ? 1 : 0;
        if (masterId == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());
    }
    scheduleSave();
    emit Changed();
}

double MixerService::MasterMicGainDb(const QString &masterId) const {
    const MasterHwSlot *hw = masterHwSlot(masterId);
    return hw ? hw->state.micGainDb : 0.0;
}

void MixerService::SetMasterMicGainDb(const QString &masterId, double db) {
    MasterHwSlot *hw = masterHwSlot(masterId);
    if (!hw) return;
    db = qBound(0.0, db, 40.0);
    hw->settleTicks = 0;
    if (auto r = hw->dev.setMicGainDb(db); !r) {
        lastError_ = QString::fromStdString(r.message);
        return;
    }
    hw->state.micGainDb = db;
    if (MasterBusState *m = masterBusState(config_.live(), masterId)) {
        m->micGainDb = db;
        if (masterId == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());
    }
    scheduleSave();
    emit Changed();
}

double MixerService::MasterHeadphoneVolumeDb(const QString &masterId) const {
    const MasterHwSlot *hw = masterHwSlot(masterId);
    return hw ? hw->state.hpVolumeDb : 0.0;
}

void MixerService::SetMasterHeadphoneVolumeDb(const QString &masterId, double db) {
    MasterHwSlot *hw = masterHwSlot(masterId);
    if (!hw) return;
    db = qBound(waveline::kHpVolumeMinDb, db, waveline::kHpVolumeMaxDb);
    hw->settleTicks = 0;
    if (auto r = hw->dev.setHpVolumeDb(db); !r) {
        lastError_ = QString::fromStdString(r.message);
        return;
    }
    hw->state.hpVolumeDb = db;
    if (MasterBusState *m = masterBusState(config_.live(), masterId)) {
        m->hwHpVolumeDb = db;
        if (masterId == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());
    }
    scheduleSave();
    emit Changed();
}

bool MixerService::MasterHeadphoneMuted(const QString &masterId) const {
    const MasterHwSlot *hw = masterHwSlot(masterId);
    return hw ? hw->state.hpMuted : false;
}

void MixerService::SetMasterHeadphoneMuted(const QString &masterId, bool muted) {
    MasterHwSlot *hw = masterHwSlot(masterId);
    if (!hw || !hw->dev.isOpen()) return;
    hw->settleTicks = 0;
    waveline::ClaimGuard guard(hw->dev);
    if (!guard) return;
    if (auto r = hw->dev.setHpMute(muted); !r)
        lastError_ = QString::fromStdString(r.message);
    else
        hw->dev.readState(hw->state);
    if (MasterBusState *m = masterBusState(config_.live(), masterId)) {
        m->hwHpMuted = muted ? 1 : 0;
        if (masterId == QLatin1String("mic")) syncLegacyFromPrimaryBus(config_.live());
    }
    scheduleSave();
    emit Changed();
}

// ---- sound sharing ---------------------------------------------------------

bool MixerService::SoundSharingEnabled() const { return soundSharingEnabled_; }

void MixerService::SetSoundSharingEnabled(bool on) {
    if (!soundShare_ || on == soundSharingEnabled_) return;
    soundSharingEnabled_ = on;
    soundShare_->setEnabled(on);
    if (on) {
        std::string err;
        soundShare_->start(err);
        updateStreamRouting();
        soundShare_->routeAll();
    } else {
        soundShare_->stop();
        updateStreamRouting();
    }
    // The per-app microphone targets are a separate mechanism from the
    // sound-share sink handled above, and they have to follow the same switch.
    applySoundShareTargets();
    scheduleSave();
    emit Changed();
}

double MixerService::SoundSharingVolume(const QString &mix) const {
    return parseMix(mix) == waveline::Mix::Monitor ? soundShareMonitorLevel_
                                                  : soundShareStreamLevel_;
}

void MixerService::SetSoundSharingVolume(const QString &mix, double volume) {
    if (!graph_) return;
    const double v = qBound(0.0, volume, 1.5);
    const waveline::Mix m = parseMix(mix);
    graph_->setSoundShareVolume(m, static_cast<float>(v));
    if (m == waveline::Mix::Monitor) soundShareMonitorLevel_ = v;
    else soundShareStreamLevel_ = v;
    scheduleSave();
    emit Changed();
}

bool MixerService::SoundSharingMuted(const QString &mix) const {
    return parseMix(mix) == waveline::Mix::Monitor ? soundShareMonitorMuted_
                                                : soundShareStreamMuted_;
}

void MixerService::SetSoundSharingMuted(const QString &mix, bool muted) {
    if (!graph_) return;
    const waveline::Mix m = parseMix(mix);
    graph_->setSoundShareMuted(m, muted);
    if (m == waveline::Mix::Monitor) soundShareMonitorMuted_ = muted;
    else soundShareStreamMuted_ = muted;
    scheduleSave();
    emit Changed();
}

QString MixerService::appNameForNode(uint nodeId) const {
    for (const auto &n : engine_.nodes())
        if (n.id == nodeId)
            return QString::fromStdString(waveline::appDisplayName(n));
    return {};
}

QStringList MixerService::SoundSharingTargets() const {
    QStringList out;
    if (!graph_) return out;
    int slot = 0;
    for (const MasterBusState &m : config_.live().masterBuses) {
        ++slot;
        const QString label =
            m.name.isEmpty() ? QStringLiteral("Input #%1").arg(slot) : m.name;
        out << QStringLiteral("%1\t%2").arg(m.id, label);
    }
    for (const auto &c : graph_->channels()) {
        if (!ChannelMicSource(QString::fromStdString(c.id))) continue;
        out << QStringLiteral("%1\t%2")
                   .arg(QString::fromStdString(c.id),
                        QString::fromStdString(c.name));
    }
    return out;
}

void MixerService::SetSoundSharingAppTarget(uint nodeId, const QString &target) {
    const QString app = appNameForNode(nodeId);
    if (app.isEmpty()) return;
    auto &targets = config_.live().soundSharing.appTargets;
    if (target.isEmpty()) targets.remove(app);
    else targets.insert(app, target);
    applySoundShareTargetForApp(app);
    scheduleSave();
    emit Changed();
}

// An application's audio is *added* to a microphone: its output ports are
// linked into the published source's inputs, which sum. The application keeps
// playing wherever it already was, so sharing your game into a call does not
// take it out of your headphones.
// PipeWire node names want to be terse and predictable; application names are
// neither ("WEBRTC VoiceEngine").
static std::string shareGainNode(const QString &app) {
    QString sane;
    for (const QChar &c : app.toLower())
        sane += c.isLetterOrNumber() ? c : QLatin1Char('-');
    return ("waveline-share-" + sane).toStdString();
}

waveline::GainFilter *MixerService::shareGain(const QString &app) {
    auto it = shareGains_.find(app);
    if (it != shareGains_.end()) return it->second.get();
    auto g = std::make_unique<waveline::GainFilter>();
    std::string err;
    if (!g->start(shareGainNode(app),
                  QStringLiteral("%1 share (%2)")
                      .arg(QString::fromStdString(profile_.brand), app)
                      .toStdString(),
                  2, err))
        return nullptr;
    auto *raw = g.get();
    shareGains_.emplace(app, std::move(g));
    return raw;
}

double MixerService::SoundSharingAppLevel(uint nodeId) const {
    const QString app = appNameForNode(nodeId);
    if (app.isEmpty()) return 1.0;
    return config_.live().soundSharing.appGains.value(app, 1.0);
}

void MixerService::SetSoundSharingAppLevel(uint nodeId, double level) {
    const QString app = appNameForNode(nodeId);
    if (app.isEmpty()) return;
    const double v = qBound(0.0, level, 1.0);
    config_.live().soundSharing.appGains.insert(app, v);
    // No rewire: the gain node is already in the path, so this is immediate.
    if (auto *g = shareGain(app)) g->setGain(static_cast<float>(v));
    scheduleSave();
    emit Changed();
}

void MixerService::applySoundShareTargetForApp(const QString &app) {
    if (!graph_ || app.isEmpty()) return;

    engine_.forgetLinksForNode(shareGainNode(app));

    if (!soundSharingEnabled_) {
        shareGains_.erase(app);
        return;
    }

    const QString target = config_.live().soundSharing.appTargets.value(app);
    if (target.isEmpty()) {
        shareGains_.erase(app);
        return;
    }

    relinkSoundShareForApp(app);
}

void MixerService::relinkSoundShareForApp(const QString &app) {
    if (!graph_ || app.isEmpty() || !soundSharingEnabled_) return;
    if (config_.live().soundSharing.appTargets.value(app).isEmpty()) return;

    for (const auto &n : engine_.nodes()) {
        if (n.isOurs || n.mediaClass != "Stream/Output/Audio") continue;
        if (QString::fromStdString(waveline::appDisplayName(n)) != app) continue;
        std::string err;
        linkSoundShareNode(n, err);
    }
}

bool MixerService::streamStillPresent(const waveline::PwNode &node) const {
    for (const auto &cur : engine_.nodes())
        if (cur.id == node.id) return cur.name == node.name;
    return false;
}

bool MixerService::linkSoundShareNode(const waveline::PwNode &n, std::string &error) {
    error.clear();
    if (!graph_ || !soundSharingEnabled_) return false;
    if (n.isOurs || n.mediaClass != "Stream/Output/Audio") return false;

    const QString app = QString::fromStdString(waveline::appDisplayName(n));
    const QString target = config_.live().soundSharing.appTargets.value(app);
    if (target.isEmpty()) return false;

    const bool masterTarget =
        target == QLatin1String("mic") || graph_->masterBus(target.toStdString()) != nullptr;
    if (!masterTarget && !ChannelMicSource(target)) return false;
    const std::string dst =
        masterTarget ? waveline::masterSourceNode(target.toStdString())
                     : ("waveline-" + target.toStdString() + "-mic");

    const bool srcMono = !engine_.hasPort(n.name, "output_FL", true);
    std::string sl = srcMono ? "output_MONO" : "output_FL";
    std::string sr = srcMono ? "output_MONO" : "output_FR";
    std::string src = n.name;

    auto *g = shareGain(app);
    if (!g) return false;
    g->setGain(static_cast<float>(config_.live().soundSharing.appGains.value(app, 1.0)));
    const std::string gn = shareGainNode(app);
    if (!engine_.waitForPort(gn, "input_FL", false, 2000) ||
        !engine_.linkPorts(src, sl, gn, "input_FL", error) ||
        !engine_.linkPorts(src, sr, gn, "input_FR", error))
        return false;
    src = gn;
    sl = "output_FL";
    sr = "output_FR";

    if (masterTarget) {
        if (!engine_.linkPorts(src, sl, dst, "input_MONO", error)) return false;
        if (sl != sr) engine_.linkPorts(src, sr, dst, "input_MONO", error);
    } else {
        if (!engine_.linkPorts(src, sl, dst, "input_FL", error)) return false;
        engine_.linkPorts(src, sr, dst, "input_FR", error);
    }
    return true;
}

void MixerService::applySoundShareTargets() {
    if (!graph_) return;

    // Sharing switched off: drop every per-app gain node. Destroying the node
    // takes its links down with it, which is the same mechanism a cleared
    // target uses below.
    //
    // This check is the whole reason the master toggle works. Without it the
    // per-app microphone links were rebuilt on every graph change regardless,
    // so turning Audio Sharing off tore down the legacy sound-share sink and
    // left every app still feeding the microphones -- the toggle appeared to do
    // nothing at all. The targets stay in the config meanwhile, so switching
    // back on restores exactly what was set up before.
    if (!soundSharingEnabled_) {
        for (auto it = shareGains_.begin(); it != shareGains_.end(); ++it)
            engine_.forgetLinksForNode(shareGainNode(it->first));
        shareGains_.clear();
        return;
    }

    const auto &targets = config_.live().soundSharing.appTargets;
    for (auto it = shareGains_.begin(); it != shareGains_.end();)
        it = targets.value(it->first).isEmpty() ? shareGains_.erase(it)
                                                : std::next(it);
    if (targets.isEmpty()) return;

    for (auto it = targets.begin(); it != targets.end(); ++it) {
        if (!it.value().isEmpty()) applySoundShareTargetForApp(it.key());
    }
}

namespace {

struct ListedApp {
    uint32_t nodeId = 0;
    std::string name;
    std::string channel;
};

std::vector<ListedApp> mergedPlaybackApps(const waveline::PwEngine &engine,
                                          waveline::AppRouter *router) {
    std::map<std::string, ListedApp> merged;
    for (const auto &n : engine.nodes()) {
        if (n.isOurs || n.mediaClass != "Stream/Output/Audio") continue;
        const std::string name = waveline::appDisplayName(n);
        if (name.empty() || name.rfind("waveline-", 0) == 0) continue;
        ListedApp &e = merged[waveline::appMergeKey(n)];
        if (e.nodeId == 0 || n.id < e.nodeId) e.nodeId = n.id;
        e.name = name;
        if (router) {
            const std::string ch = router->assignedChannel(n);
            if (!ch.empty()) e.channel = ch;
        }
    }
    std::vector<ListedApp> out;
    out.reserve(merged.size());
    for (auto &[_, row] : merged) out.push_back(std::move(row));
    std::sort(out.begin(), out.end(),
              [](const ListedApp &a, const ListedApp &b) { return a.name < b.name; });
    return out;
}

// By value, and it matters: PwEngine::nodes() hands back a fresh vector, so a
// pointer into it dangles the moment the expression that called it ends.
// Reading through that pointer worked by luck for a short application name --
// std::string keeps those inside the struct, in freed-but-untouched memory --
// and read rubbish for anything longer than the small-string buffer, which is
// why "Spotify" and "Zen" took a volume change and "jellyfin-desktop" never
// did: the merge key came out garbage, matched no node, and the loop below set
// nothing while the value was still stored in the profile.
std::optional<waveline::PwNode> findNode(const waveline::PwEngine &engine,
                                         uint32_t nodeId) {
    for (const auto &n : engine.nodes()) {
        if (n.id == nodeId) return n;
    }
    return std::nullopt;
}

}  // namespace

QStringList MixerService::SoundSharingApps() const {
    QStringList out;
    for (const ListedApp &row : mergedPlaybackApps(engine_, router_.get())) {
        const QString qname = QString::fromStdString(row.name);
        out << QStringLiteral("%1\t%2\t%3\t%4")
                   .arg(row.nodeId)
                   .arg(qname)
                   .arg(config_.live().soundSharing.appTargets.value(qname))
                   .arg(config_.live().soundSharing.appGains.value(qname, 1.0));
    }
    return out;
}

void MixerService::SetSoundSharingApp(uint nodeId, bool shared) {
    if (!soundShare_) return;

    std::string appName;
    for (const auto &n : engine_.nodes()) {
        if (n.id == nodeId) {
            appName = n.appName.empty() ? n.name : n.appName;
            break;
        }
    }
    if (appName.empty()) return;

    const QString qname = QString::fromStdString(appName);
    QStringList apps = config_.live().soundSharing.apps;
    if (shared) {
        if (!apps.contains(qname, Qt::CaseInsensitive)) apps.append(qname);
    } else {
        apps.removeIf([&](const QString &a) {
            return a.compare(qname, Qt::CaseInsensitive) == 0;
        });
    }
    config_.live().soundSharing.apps = apps;

    std::vector<std::string> patterns;
    for (const QString &a : apps) patterns.push_back(a.toStdString());
    soundShare_->setApps(std::move(patterns));

    std::string err;
    if (shared) {
        if (!soundShare_->shareStream(nodeId, appName, err))
            lastError_ = QString::fromStdString(err);
    } else {
        if (!soundShare_->unshareStream(nodeId, appName, err))
            lastError_ = QString::fromStdString(err);
    }

    if (shared && !soundSharingEnabled_) SetSoundSharingEnabled(true);
    scheduleSave();
    emit Changed();
}

// ---- noise suppression -----------------------------------------------------

bool MixerService::NoiseSuppression() const {
    return config_.live().noiseSuppression;
}

void MixerService::SetNoiseSuppression(bool on) {
    if (!graph_) return;
    Profile &p = config_.live();
    if (p.noiseSuppression == on) return;
    p.noiseSuppression = on;
    if (MasterBusState *m = masterBusState(p, QStringLiteral("mic")))
        m->noiseSuppression = on;
    applyMicFx();
    scheduleSave();
    emit Changed();
}

double MixerService::NoiseInputLevel() const {
    auto *nc = graph_ ? graph_->noiseFilter() : nullptr;
    return nc ? nc->inputRms() : 0.0;
}

double MixerService::NoiseOutputLevel() const {
    auto *nc = graph_ ? graph_->noiseFilter() : nullptr;
    return nc ? nc->outputRms() : 0.0;
}

double MixerService::SpeechProbability() const {
    auto *nc = graph_ ? graph_->noiseFilter() : nullptr;
    return nc ? nc->speechProbability() : 0.0;
}

QString MixerService::NoiseEngine() const {
    if (!graph_) return QStringLiteral("rnnoise");
    return QString::fromLatin1(waveline::noiseEngineId(graph_->noiseEngine()));
}

QString MixerService::SetNoiseEngine(const QString &engine) {
    if (!graph_) return QStringLiteral("mixer graph is not running");

    const waveline::NoiseEngine want =
        waveline::noiseEngineFromId(engine.toStdString());
    // Compared against the *requested* engine, not the running one. After a
    // fallback those differ, and picking the fallback explicitly has to be
    // recorded as a choice rather than skipped as a no-op -- otherwise the
    // config would keep asking for the engine the user just moved away from.
    if (want == graph_->requestedNoiseEngine() && want == graph_->noiseEngine())
        return QString();

    std::string error;
    if (!graph_->setNoiseEngine(want, error)) {
        // Deliberately not saved: persisting an engine this machine cannot run
        // would have the daemon retry the same failure on every start.
        return error.empty() ? QStringLiteral("could not switch engine")
                             : QString::fromStdString(error);
    }
    scheduleSave();
    emit Changed();
    return QString();
}

QStringList MixerService::NoiseEngines() const {
    // Default first, so the selector reads top-down from "what you probably
    // want" to "the fallback".
    const struct { waveline::NoiseEngine id; const char *label; } kEngines[] = {
        {waveline::NoiseEngine::DeepFilterNet, "DeepFilterNet (Default)"},
        {waveline::NoiseEngine::RnNoise, "RNNoise (lighter)"},
    };

    QStringList rows;
    for (const auto &e : kEngines) {
        std::string reason;
        const bool ok = waveline::noiseEngineAvailable(e.id, reason);
        rows << QStringLiteral("%1\t%2\t%3\t%4")
                    .arg(QString::fromLatin1(waveline::noiseEngineId(e.id)),
                         QString::fromLatin1(e.label),
                         ok ? QStringLiteral("1") : QStringLiteral("0"),
                         QString::fromStdString(reason));
    }
    return rows;
}

// ---- routing ----------------------------------------------------------------

QStringList MixerService::Apps() const {
    QStringList out;
    for (const ListedApp &row : mergedPlaybackApps(engine_, router_.get())) {
        const QString name = QString::fromStdString(row.name);
        out << QStringLiteral("%1\t%2\t%3\t%4")
                   .arg(row.nodeId)
                   .arg(name, QString::fromStdString(row.channel))
                   .arg(appVolumeForName(name));
    }
    return out;
}

double MixerService::appVolumeForName(const QString &app) const {
    if (app.isEmpty()) return 1.0;
    return config_.live().appVolumes.value(app, 1.0);
}

double MixerService::AppVolume(uint nodeId) const {
    return appVolumeForName(appNameForNode(nodeId));
}

// ---- per-application gain stage ---------------------------------------------
//
// The Applications slider is a stage of our own, not a remote control for the
// application's volume. Those are two different things and the mixer used to
// conflate them: SetAppVolume wrote channelVolumes on the application's own
// stream node, which is the very property a browser's in-page slider writes
// (pipewire-pulse maps pa_context_set_sink_input_volume onto it). One value,
// two controls -- so moving either one shoved the other, and the loser was
// whoever wrote first.
//
// The fix is to stop sharing the property. An app with a level set gets its own
// sink to play into, and a loopback carries that sink into the channel:
//
//   App -> waveline-app-<key> -> [loopback, holds the level] -> waveline-ch-<id>
//
// Now the app's own volume and ours are separate gains in series, which is what
// the two sliders always looked like they were.
//
// Built lazily. The overwhelming majority of streams are never touched in the
// mixer, and an unconditional stage would put a sink, a loopback and a buffer
// of latency under every notification sound and browser tab on the machine.
static std::string appStageSane(const QString &app) {
    QString sane;
    for (const QChar &c : app.toLower())
        sane += c.isLetterOrNumber() ? c : QLatin1Char('-');
    return sane.toStdString();
}

static std::string appStageSink(const QString &app) {
    return "waveline-app-" + appStageSane(app);
}

// The handle names the loopback's two nodes ("<handle>-in"/"-out"), so it has
// to carry the waveline- prefix: isOurs is what normally keeps our own nodes
// out of the Apps list, but routeNode and appDisplayName both fall back to that
// prefix, and a path named app-gain-spotify would be the one place in the graph
// where those checks disagree.
static std::string appStagePath(const QString &app) {
    return "waveline-app-gain-" + appStageSane(app);
}

QString MixerService::appStageChannel(const QString &app) const {
    if (!router_) return QString();
    // Where the app is routed today: the stage feeds the channel sink its
    // streams would otherwise have gone to themselves.
    for (const auto &n : engine_.nodes()) {
        if (n.isOurs || n.mediaClass != "Stream/Output/Audio") continue;
        if (QString::fromStdString(waveline::appDisplayName(n)) != app) continue;
        // A manual pin counts even with automatic routing off -- the user put
        // the app on that channel by hand, so the mixer is in its path.
        std::string ch = router_->assignedChannel(n);
        // Not placed yet. Only ask the rules where it *would* go when automatic
        // routing is actually on; otherwise this app is not ours to move.
        if (ch.empty() && routing_) ch = router_->channelForNode(n);
        if (!ch.empty()) return QString::fromStdString(ch);
    }
    return QString();
}

bool MixerService::ensureAppGainStage(const QString &app) {
    if (app.isEmpty() || !router_) return false;
    if (appGainStages_.contains(app)) return true;

    const QString channel = appStageChannel(app);
    // No resolved channel means no sink to feed. Leave the app alone rather
    // than parking its audio on a stage that goes nowhere.
    if (channel.isEmpty()) return false;

    const std::string sink = appStageSink(app);
    const std::string handle = appStagePath(app);
    std::string err;
    if (!engine_.addNullSink(sink,
                             QStringLiteral("%1 (%2)")
                                 .arg(app, QString::fromStdString(profile_.brand))
                                 .toStdString(),
                             2, err)) {
        lastError_ = QString::fromStdString(err);
        return false;
    }

    waveline::PwEngine::PathSpec spec;
    spec.handle = handle;
    spec.source = sink;
    spec.sourceIsSink = true;   // capture the per-app sink's monitor
    spec.target = "waveline-ch-" + channel.toStdString();
    spec.description = QStringLiteral("%1 level").arg(app).toStdString();
    spec.volume = static_cast<float>(appVolumeForName(app));
    // The app's audio must not follow the default sink if its channel goes
    // away: that would dump it straight to the speakers, bypassing the mixer.
    spec.stickyTarget = true;
    if (!engine_.addPath(spec, err)) {
        lastError_ = QString::fromStdString(err);
        engine_.removeNode(sink);
        return false;
    }

    AppGainStage stage;
    stage.sinkName = sink;
    stage.pathHandle = handle;
    stage.channelId = channel;
    appGainStages_.insert(app, stage);

    // Routing has to know, or the next time this app's stream appears the
    // router sends it to the channel sink and silently bypasses the stage.
    router_->setStageSink(app.toStdString(), sink);

    // The sink was created a moment ago and reaches the registry
    // asynchronously; a target.object naming a sink the session manager cannot
    // resolve yet is simply dropped. Retry on the short ladder used for the
    // same problem on new streams -- setStreamTarget is idempotent, so the
    // rungs after the one that works cost nothing.
    retargetAppToStage(app);
    for (int ms : {250, 800, 2000}) {
        QTimer::singleShot(ms, this, [this, app] {
            if (appGainStages_.contains(app)) retargetAppToStage(app);
        });
    }
    return true;
}

void MixerService::retargetAppToStage(const QString &app) {
    const auto it = appGainStages_.constFind(app);
    if (it == appGainStages_.constEnd()) return;
    std::string err;
    for (const auto &n : engine_.nodes()) {
        if (n.isOurs || n.mediaClass != "Stream/Output/Audio") continue;
        if (QString::fromStdString(waveline::appDisplayName(n)) != app) continue;
        if (!engine_.setStreamTarget(n.id, it->sinkName, err))
            lastError_ = QString::fromStdString(err);
    }
}

void MixerService::applyAppVolume(uint nodeId) {
    if (nodeId == 0) return;
    const QString app = appNameForNode(nodeId);
    if (app.isEmpty()) return;
    // Nothing is ever written to the application's own stream volume any more.
    // A new stream only has to be pointed at its app's stage -- the level lives
    // on the loopback and outlives the stream, which is why pausing and
    // unpausing a video no longer disturbs anything.
    //
    // Building it here too, not only in SetAppVolume: an app carrying a stored
    // level from a previous session is usually not running when the profile
    // loads, so this is the first moment its channel can be resolved at all.
    if (!appGainStages_.contains(app)) {
        if (std::abs(appVolumeForName(app) - 1.0) < 0.005) return;
        if (!ensureAppGainStage(app)) return;
        return;  // ensureAppGainStage retargets on the way out
    }
    retargetAppToStage(app);
}

void MixerService::applyAppVolumes() {
    for (const ListedApp &row : mergedPlaybackApps(engine_, router_.get()))
        applyAppVolume(row.nodeId);
}

void MixerService::rebuildAppGainStages() {
    const auto &wanted = config_.live().appVolumes;
    auto wants = [&](const QString &app) {
        const auto it = wanted.constFind(app);
        return it != wanted.constEnd() && std::abs(it.value() - 1.0) >= 0.005;
    };

    // Reconcile, do not rebuild. Tearing every stage down and putting it back
    // costs a sink and a loopback per app on a switch between two profiles that
    // may well ask for the same levels, and dropping a sink out from under a
    // playing stream is audible. Only what actually differs is touched.
    QStringList stale;
    for (auto it = appGainStages_.constBegin(); it != appGainStages_.constEnd();
         ++it) {
        if (!wants(it.key())) stale << it.key();
    }
    for (const QString &app : stale) {
        const AppGainStage stage = appGainStages_.take(app);
        // Order matters: drop the routing override first, so the streams this
        // releases are sent to the channel sink rather than back at a sink that
        // is about to stop existing.
        if (router_) router_->clearStageSink(app.toStdString());
        std::string err;
        for (const auto &n : engine_.nodes()) {
            if (n.isOurs || n.mediaClass != "Stream/Output/Audio") continue;
            if (QString::fromStdString(waveline::appDisplayName(n)) != app) continue;
            if (!stage.channelId.isEmpty())
                engine_.setStreamTarget(
                    n.id, "waveline-ch-" + stage.channelId.toStdString(), err);
        }
        engine_.removePath(stage.pathHandle);
        engine_.removeNode(stage.sinkName);
    }

    for (auto it = wanted.constBegin(); it != wanted.constEnd(); ++it) {
        if (!wants(it.key())) continue;
        if (appGainStages_.contains(it.key())) {
            // Kept: the level is all that can have changed.
            engine_.setPathVolume(appGainStages_[it.key()].pathHandle,
                                  static_cast<float>(it.value()));
            continue;
        }
        // May fail when the app is not running -- there is no channel to feed
        // yet. applyAppVolume() builds it when the stream turns up.
        ensureAppGainStage(it.key());
    }
}

void MixerService::SetAppVolume(uint nodeId, double volume) {
    const QString app = appNameForNode(nodeId);
    if (app.isEmpty()) return;
    const double v = qBound(0.0, volume, 1.5);
    config_.live().appVolumes.insert(app, v);

    // Unity and no stage is the default state: nothing to build, nothing to
    // write. A stage that already exists is *kept* at unity rather than torn
    // down -- dragging a slider through 100% would otherwise rip the app's sink
    // out from under it and put it back a moment later.
    if (!appGainStages_.contains(app) && std::abs(v - 1.0) >= 0.005)
        ensureAppGainStage(app);

    if (const auto it = appGainStages_.constFind(app);
        it != appGainStages_.constEnd()) {
        engine_.setPathVolume(it->pathHandle, static_cast<float>(v));
    } else if (std::abs(v - 1.0) >= 0.005) {
        // No stage and none buildable: the app is not on a waveline channel, so
        // there is no signal path of ours to put a gain in. All that is left is
        // the application's own volume, the way pavucontrol would do it. It is
        // a one-shot write -- nothing re-asserts it -- so this cannot come back
        // to fight the in-app slider on the next stream restart.
        const std::string key = [&]() -> std::string {
            if (const auto ref = findNode(engine_, nodeId))
                return waveline::appMergeKey(*ref);
            return {};
        }();
        for (const auto &n : engine_.nodes()) {
            if (n.isOurs || n.mediaClass != "Stream/Output/Audio") continue;
            if (!key.empty() && waveline::appMergeKey(n) != key) continue;
            if (key.empty() &&
                QString::fromStdString(waveline::appDisplayName(n)) != app)
                continue;
            engine_.setNodeVolumeById(n.id, static_cast<float>(v), false, 2);
        }
    }

    scheduleSave();
    emit Changed();
}

void MixerService::MoveApp(uint nodeId, const QString &channelId) {
    if (!router_) return;
    const auto ref = findNode(engine_, nodeId);
    if (!ref) return;
    const QString app = QString::fromStdString(waveline::appDisplayName(*ref));
    const std::string key = waveline::appMergeKey(*ref);
    const std::string channel = channelId.toStdString();
    std::string err;
    for (const auto &n : engine_.nodes()) {
        if (n.isOurs || n.mediaClass != "Stream/Output/Audio") continue;
        if (waveline::appMergeKey(n) != key) continue;
        // pinStream records the manual override and moves the stream; with a
        // stage in play it resolves to the stage sink, so the stream stays put
        // and it is the stage's output that has to follow the channel.
        if (!router_->pinStream(n.id, channel, err))
            lastError_ = QString::fromStdString(err);
    }
    if (const auto it = appGainStages_.find(app); it != appGainStages_.end()) {
        if (engine_.setPathTarget(it->pathHandle, "waveline-ch-" + channel, err))
            it->channelId = channelId;
        else
            lastError_ = QString::fromStdString(err);
    }
    scheduleSave();
    emit Changed();
}

bool MixerService::RoutingEnabled() const { return routing_; }

void MixerService::SetRoutingEnabled(bool on) {
    if (!router_ || on == routing_) return;
    routing_ = on;
    if (on) {
        std::string err;
        router_->start(err);
        if (!err.empty()) lastError_ = QString::fromStdString(err);
        scheduleRouteAll();
    } else {
        router_->stop();
    }
    updateStreamRouting();
    scheduleSave();
    emit Changed();
}

// ============================================================== soundboard

QString MixerService::soundboardDataDir() const {
    // Beside config.json rather than under GenericDataLocation: ConfigStore's
    // constructor already proved ~/.config/waveline is writable by creating
    // it (mkpath) the moment the daemon starts, and a stray root-owned
    // ~/.local/share/waveline from an earlier system-wide install step (a
    // models/ directory some other component created there) is not
    // something a per-user data file should have to contend with.
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/waveline/soundboard");
}

SoundboardSoundState *MixerService::findSoundboardSound(const QString &id) {
    auto &sounds = config_.soundboard().sounds;
    for (auto &s : sounds) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

const SoundboardSoundState *MixerService::findSoundboardSound(const QString &id) const {
    for (const auto &s : config_.soundboard().sounds) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

// A small, comma-separated peak set for the panel's mini waveform -- far
// coarser than AnalyzeSoundboardSource's 400 (that one draws a whole trim
// editor; this one draws a glance-sized strip in a rack row), and computed
// once here rather than by the GUI, which links no audio and cannot decode
// anything itself.
//
// Spans [trimStartMs, trimEndMs), not the whole file: the mini waveform is
// meant to show what the sound actually plays, and showing the parts that
// were trimmed away as if they were still part of the clip is exactly the
// bug this range parameter exists to not have.
static QString soundboardPeaksFor(const waveline::SoundboardBuffer &buf, int trimStartMs,
                                  int trimEndMs) {
    const uint32_t start = static_cast<uint32_t>(std::max(0, trimStartMs)) * 48;
    const uint32_t end = trimEndMs > 0 ? static_cast<uint32_t>(trimEndMs) * 48 : 0;
    const std::vector<float> peaks = waveline::soundboardPeaks(buf, 40, start, end);
    QStringList out;
    out.reserve(static_cast<int>(peaks.size()));
    for (float p : peaks) out << QString::number(p, 'f', 3);
    return out.join(QLatin1Char(','));
}

void MixerService::loadSoundboardBuffers() {
    soundboardBuffers_.clear();
    for (auto &s : config_.soundboard().sounds) {
        std::string err;
        waveline::SoundboardBuffer buf = waveline::decodeSoundFile(s.file.toStdString(), err);
        if (!buf.valid()) {
            qWarning("waveline: soundboard sound %s ('%s') failed to decode: %s",
                     qPrintable(s.id), qPrintable(s.name), err.c_str());
            continue;
        }
        // Always re-derived, not just backfilled when empty: sounds added
        // before this covered the trim range (rather than the whole file)
        // existed are otherwise stuck showing the untrimmed waveform forever,
        // and recomputing 40 samples on startup costs nothing worth guarding.
        s.peaks = soundboardPeaksFor(buf, s.trimStartMs, s.trimEndMs);
        soundboardBuffers_.insert(
            s.id, std::make_shared<waveline::SoundboardBuffer>(std::move(buf)));
    }
}

QStringList MixerService::SoundboardSounds() const {
    QStringList out;
    for (const auto &s : config_.soundboard().sounds) {
        double durationMs = -1.0;
        const auto it = soundboardBuffers_.constFind(s.id);
        if (it != soundboardBuffers_.constEnd() && it.value()) durationMs = it.value()->durationMs();
        out << QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8")
                   .arg(s.id, s.name)
                   .arg(s.volume)
                   .arg(s.trimStartMs)
                   .arg(s.trimEndMs)
                   .arg(durationMs)
                   .arg(s.file, s.peaks);
    }
    return out;
}

QString MixerService::SoundboardSettings() const {
    const SoundboardState &b = config_.soundboard();
    return QStringLiteral("%1\t%2\t%3\t%4")
        .arg(b.channelId, b.shareTarget)
        .arg(b.shareVolume)
        .arg(b.localVolume);
}

void MixerService::SetSoundboardChannel(const QString &channelId) {
    config_.soundboard().channelId = channelId.isEmpty() ? QStringLiteral("sfx") : channelId;
    scheduleSave();
    emit Changed();
}

void MixerService::SetSoundboardShareTarget(const QString &target) {
    config_.soundboard().shareTarget = target;
    scheduleSave();
    emit Changed();
}

void MixerService::SetSoundboardShareVolume(double volume) {
    config_.soundboard().shareVolume = qBound(0.0, volume, 1.0);
    scheduleSave();
    emit Changed();
}

void MixerService::SetSoundboardLocalVolume(double volume) {
    config_.soundboard().localVolume = qBound(0.0, volume, 1.5);
    scheduleSave();
    emit Changed();
}

// "wav" or "mp3", falling back to "wav" for anything else -- AddSoundboardSound
// already refused the file if decodeSoundFile() could not read it, so by the
// time this runs the extension is cosmetic (it only names the stored copy).
static QString soundboardExtFor(const QString &sourcePath) {
    const QString ext = QFileInfo(sourcePath).suffix().toLower();
    return ext == QLatin1String("mp3") ? ext : QStringLiteral("wav");
}

QString MixerService::AddSoundboardSound(const QString &name, const QString &sourcePath,
                                         int trimStartMs, int trimEndMs, double volume) {
    std::string err;
    waveline::SoundboardBuffer buf = waveline::decodeSoundFile(sourcePath.toStdString(), err);
    if (!buf.valid()) {
        lastError_ = QString::fromStdString(err);
        return QString();
    }

    const auto &sounds = config_.soundboard().sounds;
    QString id;
    for (int attempt = 0; attempt < 500 && id.isEmpty(); ++attempt) {
        const QString candidate = QStringLiteral("%1").arg(
            QRandomGenerator::global()->bounded(10000), 4, 10, QLatin1Char('0'));
        bool taken = false;
        for (const auto &s : sounds) {
            if (s.id == candidate) { taken = true; break; }
        }
        if (!taken) id = candidate;
    }
    if (id.isEmpty()) {
        lastError_ = QStringLiteral("could not allocate a soundboard id");
        return QString();
    }

    const QString dir = soundboardDataDir();
    QDir().mkpath(dir);
    const QString destPath =
        dir + QLatin1Char('/') + id + QLatin1Char('.') + soundboardExtFor(sourcePath);
    QFile::remove(destPath);
    if (!QFile::copy(sourcePath, destPath)) {
        lastError_ = QStringLiteral("could not copy '%1' into the soundboard library")
                         .arg(sourcePath);
        return QString();
    }

    SoundboardSoundState sound;
    sound.id = id;
    sound.name = name.isEmpty() ? QStringLiteral("Sound %1").arg(id) : name;
    sound.file = destPath;
    sound.volume = qBound(0.0, volume, 2.0);
    sound.trimStartMs = std::max(0, trimStartMs);
    sound.trimEndMs = std::max(0, trimEndMs);
    sound.peaks = soundboardPeaksFor(buf, sound.trimStartMs, sound.trimEndMs);
    config_.soundboard().sounds.append(sound);
    soundboardBuffers_.insert(id, std::make_shared<waveline::SoundboardBuffer>(std::move(buf)));

    scheduleSave();
    emit Changed();
    return id;
}

bool MixerService::UpdateSoundboardSound(const QString &id, const QString &name,
                                         const QString &newSourcePath, int trimStartMs,
                                         int trimEndMs, double volume) {
    SoundboardSoundState *sound = findSoundboardSound(id);
    if (!sound) return false;

    // A caller passing back the file already on record (accidentally, or to
    // mean "no change") is not a real replacement -- treated as one, the
    // remove-then-copy below deletes newSourcePath out from under itself
    // before ever reading it, since it and the destination are the same
    // file. Guarded here rather than trusted to every caller, GUI included:
    // see SoundboardWindow::onEditRequested for the one case that used to
    // actually send this.
    if (!newSourcePath.isEmpty() && newSourcePath != sound->file) {
        std::string err;
        waveline::SoundboardBuffer buf =
            waveline::decodeSoundFile(newSourcePath.toStdString(), err);
        if (!buf.valid()) {
            lastError_ = QString::fromStdString(err);
            return false;
        }
        const QString destPath = soundboardDataDir() + QLatin1Char('/') + id + QLatin1Char('.') +
                                 soundboardExtFor(newSourcePath);
        if (destPath != sound->file) QFile::remove(sound->file);
        if (destPath != newSourcePath) QFile::remove(destPath);
        if (!QFile::copy(newSourcePath, destPath)) {
            lastError_ = QStringLiteral("could not copy '%1' into the soundboard library")
                             .arg(newSourcePath);
            return false;
        }
        sound->file = destPath;
        soundboardBuffers_.insert(id, std::make_shared<waveline::SoundboardBuffer>(std::move(buf)));
    }

    if (!name.isEmpty()) sound->name = name;
    sound->trimStartMs = std::max(0, trimStartMs);
    sound->trimEndMs = std::max(0, trimEndMs);
    sound->volume = qBound(0.0, volume, 2.0);

    // Re-derived from the *new* trim range whenever it (or the file) could
    // have changed, not only when the file itself was replaced above: the
    // whole point of this call is often exactly to move the trim, and the
    // mini waveform showing what used to be cut is the bug this exists to
    // not have. Uses whichever buffer is now on record for this id --
    // freshly decoded above if the file was replaced, otherwise the one
    // already cached from when this sound was added or the daemon started.
    if (const auto it = soundboardBuffers_.constFind(id);
        it != soundboardBuffers_.constEnd() && it.value()) {
        sound->peaks = soundboardPeaksFor(*it.value(), sound->trimStartMs, sound->trimEndMs);
    }

    scheduleSave();
    emit Changed();
    return true;
}

void MixerService::RemoveSoundboardSound(const QString &id) {
    auto &sounds = config_.soundboard().sounds;
    for (int i = 0; i < sounds.size(); ++i) {
        if (sounds[i].id != id) continue;
        if (soundboard_) soundboard_->stop(id.toStdString());
        QFile::remove(sounds[i].file);
        sounds.removeAt(i);
        break;
    }
    soundboardBuffers_.remove(id);
    scheduleSave();
    emit Changed();
}

void MixerService::ReorderSoundboardSounds(const QStringList &idsInOrder) {
    auto &sounds = config_.soundboard().sounds;
    QList<SoundboardSoundState> reordered;
    reordered.reserve(sounds.size());
    for (const QString &id : idsInOrder) {
        for (const auto &s : sounds) {
            if (s.id == id) { reordered.append(s); break; }
        }
    }
    for (const auto &s : sounds) {
        if (!idsInOrder.contains(s.id)) reordered.append(s);
    }
    sounds = reordered;
    scheduleSave();
    emit Changed();
}

QString MixerService::PlaySoundboardSound(const QString &id) {
    if (!graph_ || !soundboard_) return QStringLiteral("mixer not ready");
    SoundboardSoundState *sound = findSoundboardSound(id);
    if (!sound) return QStringLiteral("no such sound: %1").arg(id);
    const auto bufIt = soundboardBuffers_.constFind(id);
    if (bufIt == soundboardBuffers_.constEnd() || !bufIt.value())
        return QStringLiteral("'%1' has not decoded successfully").arg(sound->name);

    const SoundboardState &board = config_.soundboard();
    waveline::SoundboardPlaySpec spec;
    const QString channelId = board.channelId.isEmpty() ? QStringLiteral("sfx") : board.channelId;
    spec.localTarget = ("waveline-ch-" + channelId).toStdString();
    spec.localGain = static_cast<float>(sound->volume * board.localVolume);
    spec.trimStartFrames = static_cast<uint32_t>(std::max(0, sound->trimStartMs) * 48);
    spec.trimEndFrames =
        sound->trimEndMs > 0 ? static_cast<uint32_t>(sound->trimEndMs * 48) : 0;
    spec.description = ("Soundboard: " + sound->name).toStdString();

    if (!board.shareTarget.isEmpty()) {
        const QString target = board.shareTarget;
        const bool masterTarget =
            target == QLatin1String("mic") || graph_->masterBus(target.toStdString()) != nullptr;
        if (masterTarget || ChannelMicSource(target)) {
            spec.shareTarget = masterTarget
                                   ? waveline::masterSourceNode(target.toStdString())
                                   : ("waveline-" + target.toStdString() + "-mic");
            spec.shareTargetMono = masterTarget;
            spec.shareGain = static_cast<float>(sound->volume * board.shareVolume);
        }
    }

    // SoundboardVoice::start() (reached through SoundboardEngine::play()
    // below) blocks until the new voice's PipeWire ports are registered and
    // linked -- PwEngine::waitForPort() polling the registry, ~100ms typical
    // on this machine, measured. Every D-Bus call this daemon answers is
    // dispatched from this same thread, so calling play() straight from here
    // used to freeze the *entire* daemon -- not just this one method -- for
    // that whole stretch on every single press of a soundboard pad; that
    // freeze was the visible GUI stutter reported against every play, not
    // just slowness in this one call. The Companion server calls this same
    // method directly (CompanionServer::handleCommand's "soundboard.play"),
    // as an ordinary in-process C++ call rather than through the D-Bus
    // dispatcher, and shares this daemon's one Qt event loop just as much as
    // any D-Bus caller does -- so it deserves the same fix, not a narrower
    // one.
    //
    // Everything above this comment reads only config_/soundboardBuffers_,
    // main-thread-only state, so it stays here; only the actual blocking
    // call moves off this thread. That's safe to do because
    // SoundboardEngine::play() locks its own mutex_ around voices_, and
    // PwEngine's waitForPort()/linkPorts()/sync() lock their own state too
    // (nodesMutex, pw_thread_loop_lock) -- both were already written to
    // tolerate being called from somewhere other than the graph's own
    // thread, which is exactly what a worker thread here is.
    //
    // calledFromDBus() is what actually distinguishes the two callers:
    // setDelayedReply()/connection()/message() only mean anything when a
    // real D-Bus call is being dispatched right now -- calling them for the
    // Companion's direct call crashed inside Qt's own
    // QDBusContext::setDelayedReply(), because there was no such call for
    // it to attach the delayed reply to. The Companion path skips all three
    // and does not wait for a reply at all: its own call site
    // (CompanionServer::handleCommand) already discards this method's
    // return value, so there was never a synchronous answer for it to lose.
    const bool viaDBus = calledFromDBus();
    if (viaDBus) setDelayedReply(true);
    const QDBusConnection replyConn = viaDBus ? connection() : QDBusConnection(QString());
    const QDBusMessage replyMsg = viaDBus ? message() : QDBusMessage();
    waveline::SoundboardEngine *boardEngine = soundboard_.get();
    const QPointer<MixerService> guard(this);
    const std::string soundIdStd = id.toStdString();
    auto buffer = bufIt.value();

    std::thread([boardEngine, soundIdStd, buffer, spec, viaDBus, replyConn, replyMsg,
                guard]() mutable {
        std::string err;
        const bool ok = boardEngine->play(soundIdStd, std::move(buffer), std::move(spec), err);
        // Marshalled back rather than sent/emitted straight from this
        // thread: QDBusConnection::send() is documented thread-safe, but
        // Changed() also has to reach whatever else in MixerService is
        // connected to it, and that code was never written to expect a
        // call from off the main thread.
        QMetaObject::invokeMethod(
            qApp,
            [guard, viaDBus, replyConn, replyMsg, ok, err]() {
                if (viaDBus) {
                    QDBusMessage reply =
                        replyMsg.createReply(ok ? QString() : QString::fromStdString(err));
                    replyConn.send(reply);
                }
                if (guard && ok) emit guard->Changed();
            },
            Qt::QueuedConnection);
    }).detach();

    // Ignored when viaDBus: setDelayedReply(true) means the real reply is
    // the one sent from the worker thread above, once play() actually
    // finishes. The Companion caller ignores it regardless (see above).
    return QString();
}

void MixerService::StopSoundboardSound(const QString &id) {
    if (soundboard_) soundboard_->stop(id.toStdString());
    emit Changed();
}

void MixerService::StopAllSoundboardSounds() {
    if (soundboard_) soundboard_->stopAll();
    emit Changed();
}

QStringList MixerService::SoundboardPlayingIds() const {
    QStringList out;
    if (!soundboard_) return out;
    for (const auto &id : soundboard_->playing()) out << QString::fromStdString(id);
    return out;
}

QStringList MixerService::SoundboardProgress() const {
    QStringList out;
    if (!soundboard_) return out;
    for (const auto &id : soundboard_->playing()) {
        out << QStringLiteral("%1\t%2")
                   .arg(QString::fromStdString(id))
                   .arg(soundboard_->progress(id));
    }
    return out;
}

QString MixerService::AnalyzeSoundboardSource(const QString &path) {
    std::string err;
    waveline::SoundboardBuffer buf = waveline::decodeSoundFile(path.toStdString(), err);
    if (!buf.valid()) {
        lastError_ = QString::fromStdString(err);
        return QString();
    }
    const std::vector<float> peaks = waveline::soundboardPeaks(buf, 400);
    QStringList peakStrs;
    peakStrs.reserve(static_cast<int>(peaks.size()));
    for (float p : peaks) peakStrs << QString::number(p, 'f', 4);
    return QStringLiteral("%1\t%2").arg(buf.durationMs()).arg(peakStrs.join(QLatin1Char(',')));
}

void MixerService::PreviewSoundboardTrim(const QString &path, int trimStartMs, int trimEndMs,
                                         double volume) {
    if (!soundboard_) return;
    std::string err;
    waveline::SoundboardBuffer buf = waveline::decodeSoundFile(path.toStdString(), err);
    if (!buf.valid()) {
        lastError_ = QString::fromStdString(err);
        return;
    }
    auto shared = std::make_shared<waveline::SoundboardBuffer>(std::move(buf));
    soundboard_->stop("__preview__");

    waveline::SoundboardPlaySpec spec;
    spec.localTarget = "waveline-ch-system";
    spec.localGain = static_cast<float>(qBound(0.0, volume, 2.0));
    spec.trimStartFrames = static_cast<uint32_t>(std::max(0, trimStartMs) * 48);
    spec.trimEndFrames = trimEndMs > 0 ? static_cast<uint32_t>(trimEndMs * 48) : 0;
    spec.description = "Soundboard preview";
    if (!soundboard_->play("__preview__", shared, spec, err))
        lastError_ = QString::fromStdString(err);
    emit Changed();
}

void MixerService::StopSoundboardPreview() {
    if (soundboard_) soundboard_->stop("__preview__");
}

// ---- outputs -----------------------------------------------------------------

QStringList MixerService::Outputs() const {
    QStringList out;
    for (const auto &n : engine_.nodes()) {
        if (n.isOurs || n.mediaClass != "Audio/Sink") continue;
        if (n.name.rfind("waveline-", 0) == 0) continue;  // never our own mixes
        out << QStringLiteral("%1\t%2")
                   .arg(QString::fromStdString(n.name),
                        waveline::DesktopNames::instance().apply(n.name, n.description));
    }
    return out;
}

QStringList MixerService::MonitorOutputs() const {
    QStringList out;
    if (!graph_) return out;
    for (const waveline::MonitorOutputEntry &e : graph_->monitorOutputs())
        out << QString::fromStdString(e.sink);
    return out;
}

QStringList MixerService::MonitorOutputStates() const {
    QStringList out;
    if (!graph_) return out;
    size_t i = 0;
    for (const waveline::MonitorOutputEntry &e : graph_->monitorOutputs()) {
        const bool online = graph_->monitorOutputOnline(i);
        QString desc = QString::fromStdString(e.description);
        if (desc.isEmpty()) desc = QString::fromStdString(e.sink);
        desc = waveline::DesktopNames::instance().apply(QString::fromStdString(e.sink), desc);
        out << QStringLiteral("%1\t%2\t%3\t%4\t%5")
                   .arg(QString::fromStdString(e.sink))
                   .arg(e.volume)
                   .arg(e.muted ? 1 : 0)
                   .arg(online ? 1 : 0)
                   .arg(desc);
        ++i;
    }
    return out;
}

QString MixerService::MonitorOutput() const {
    const QStringList outs = MonitorOutputs();
    return outs.isEmpty() ? QString() : outs.front();
}

void MixerService::SetMonitorOutput(const QString &sinkName) {
    SetMonitorOutputAt(0, sinkName);
}

void MixerService::SetMonitorOutputAt(int index, const QString &sinkName) {
    if (!graph_ || sinkName.isEmpty() || index < 0) return;
    std::string err;
    if (!graph_->setMonitorOutputAt(static_cast<size_t>(index), sinkName.toStdString(),
                                    err))
        lastError_ = QString::fromStdString(err);
    scheduleSave();
    emit Changed();
}

void MixerService::AddMonitorOutput(const QString &sinkName) {
    if (!graph_ || sinkName.isEmpty()) return;
    if (graph_->monitorOutputs().size() >= waveline::MixerGraph::kMaxMonitorOutputs)
        return;
    std::string err;
    if (!graph_->addMonitorOutput(sinkName.toStdString(), err))
        lastError_ = QString::fromStdString(err);
    scheduleSave();
    emit Changed();
}

void MixerService::RemoveMonitorOutput(int index) {
    if (!graph_ || index < 1) return;
    std::string err;
    if (!graph_->removeMonitorOutput(static_cast<size_t>(index), err))
        lastError_ = QString::fromStdString(err);
    scheduleSave();
    emit Changed();
}

void MixerService::SetMonitorOutputVolumeAt(int index, double volume) {
    if (!graph_ || index < 0) return;
    volume = qBound(0.0, volume, 1.0);
    if (index < static_cast<int>(config_.live().monitorOutputs.size())) {
        config_.live().monitorOutputs[index].volume = volume;
        config_.live().monitorOutputs[index].muted = false;
    }
    graph_->setMonitorOutputVolume(static_cast<size_t>(index),
                                   static_cast<float>(volume));
    scheduleSave();
    emit Changed();
}

void MixerService::SetMonitorOutputMutedAt(int index, bool muted) {
    if (!graph_ || index < 0) return;
    if (index < static_cast<int>(config_.live().monitorOutputs.size()))
        config_.live().monitorOutputs[index].muted = muted;
    graph_->setMonitorOutputMuted(static_cast<size_t>(index), muted);
    flushPendingSave();
    emit Changed();
}

double MixerService::StreamMixVolume() const {
    if (!graph_) return config_.live().streamMixVolume;
    return graph_->streamMixVolume();
}

void MixerService::SetStreamMixVolume(double volume) {
    if (!graph_) return;
    volume = qBound(0.0, volume, 1.0);
    config_.live().streamMixVolume = volume;
    config_.live().streamMixMuted = false;
    graph_->setStreamMixVolume(static_cast<float>(volume));
    scheduleSave();
    emit Changed();
}

bool MixerService::StreamMixMuted() const {
    return graph_ ? graph_->streamMixMuted() : config_.live().streamMixMuted;
}

void MixerService::SetStreamMixMuted(bool muted) {
    if (!graph_) return;
    config_.live().streamMixMuted = muted;
    graph_->setStreamMixMuted(muted);
    flushPendingSave();
    emit Changed();
}

// ---- web companion ------------------------------------------------------------

bool MixerService::CompanionRunning() const {
    return companion_ && companion_->running();
}

int MixerService::CompanionPort() const {
    // The stored port even while the server is up: the two only differ in the
    // moment between a failed move and the user picking another number, and
    // showing the old one then would say the move had not been asked for.
    return config_.companion().port;
}

QString MixerService::SetCompanionPort(int port) {
    if (port < 1024 || port > 65535)
        return QStringLiteral("Port must be between 1024 and 65535.");
    if (port == config_.companion().port && !CompanionRunning()) return {};

    config_.companion().port = port;
    scheduleSave();

    QString error;
    if (companion_ && companion_->running()) {
        // Moving a running server drops whoever is on the old port. Nothing can
        // avoid that -- their page is pointed at a number that is about to stop
        // answering -- so it is done plainly rather than half-done.
        companion_->stop();
        if (!companion_->start(port, error)) {
            emit Changed();
            return error;
        }
    }
    emit Changed();
    return {};
}

bool MixerService::CompanionAutoStart() const {
    return config_.companion().autoStart;
}

void MixerService::SetCompanionAutoStart(bool on) {
    if (config_.companion().autoStart == on) return;
    config_.companion().autoStart = on;
    scheduleSave();
    emit Changed();
}

QString MixerService::CompanionStart() {
    if (!companion_) return QStringLiteral("The mixer is still starting up.");
    if (companion_->running()) return {};
    QString error;
    if (!companion_->start(config_.companion().port, error)) return error;
    emit Changed();
    return {};
}

void MixerService::CompanionStop() {
    if (!companion_ || !companion_->running()) return;
    companion_->stop();
    emit Changed();
}

QStringList MixerService::CompanionStatus() const {
    QStringList out;
    const bool up = CompanionRunning();
    out << QStringLiteral("%1\t%2\t%3")
               .arg(up ? 1 : 0)
               .arg(CompanionPort())
               .arg(companion_ ? companion_->clientCount() : 0);
    if (up) out += companion_->addresses();
    return out;
}

// ---- profiles -----------------------------------------------------------------

QStringList MixerService::Profiles() const { return config_.profileNames(); }
QString MixerService::ActiveProfile() const { return config_.activeProfile(); }
QString MixerService::ConfigPath() const { return config_.path(); }

MixerService::ProfileLoadPlan MixerService::planProfileLoad(
    const Profile &stored) const {
    ProfileLoadPlan plan;
    plan.profile = stored;

    const Profile &live = config_.live();

    // Live order first: switching profiles must not reshuffle the input strips
    // just because a profile happens to list its devices in another order.
    QList<MasterBusState> merged;
    QSet<QString> placed;
    QSet<QString> carried;
    for (const MasterBusState &cur : live.masterBuses) {
        placed.insert(cur.id);
        if (const MasterBusState *want = masterBusState(stored, cur.id)) {
            merged.append(*want);
        } else {
            merged.append(cur);
            carried.insert(cur.id);
            plan.migrated = true;
        }
    }
    for (const MasterBusState &want : stored.masterBuses) {
        if (placed.contains(want.id)) continue;
        merged.append(want);
        plan.addedMasters << want.id;
        plan.migrated = true;
    }
    plan.profile.masterBuses = merged;

    // Channel mic sends that feed off a carried-across bus. Without this the
    // device stays on screen and stops reaching the channels it was reaching,
    // which is the same silence this whole path exists to avoid -- only harder
    // to find, because nothing visibly went away.
    if (!carried.isEmpty()) {
        for (auto it = live.channelEffects.constBegin();
             it != live.channelEffects.constEnd(); ++it) {
            QStringList keep;
            for (const QString &id : it->masterMicIds)
                if (carried.contains(id)) keep << id;
            if (keep.isEmpty()) continue;
            auto dst = plan.profile.channelEffects.find(it.key());
            if (dst == plan.profile.channelEffects.end()) {
                // The profile has no opinion about this channel at all, so the
                // same rule applies as for the bus itself: leave it as it is.
                plan.profile.channelEffects.insert(it.key(), *it);
                continue;
            }
            for (const QString &id : keep)
                if (!dst->masterMicIds.contains(id)) dst->masterMicIds << id;
        }
    }

    // Settings whose setters relink a path rather than just writing a number.
    // Only the ones that actually changed: a monitor path that is relinked for
    // nothing still drops and remakes its links, which is a click on a path
    // that was working.
    const auto fxOf = [](const Profile &p, const QString &id) {
        const auto it = p.channelEffects.constFind(id);
        return it == p.channelEffects.constEnd() ? ChannelEffectsState{} : *it;
    };
    QSet<QString> channelIds;
    for (auto it = live.channelEffects.constBegin();
         it != live.channelEffects.constEnd(); ++it)
        channelIds.insert(it.key());
    for (auto it = plan.profile.channelEffects.constBegin();
         it != plan.profile.channelEffects.constEnd(); ++it)
        channelIds.insert(it.key());
    for (const QString &id : channelIds) {
        const ChannelEffectsState before = fxOf(live, id);
        const ChannelEffectsState after = fxOf(plan.profile, id);
        if (before.monitorFx != after.monitorFx) plan.monitorPathChannels << id;
        if (before.micSource != after.micSource ||
            (after.micSource && before.masterMicIds != after.masterMicIds))
            plan.micSourceChannels << id;
    }
    for (const MasterBusState &m : plan.profile.masterBuses) {
        const MasterBusState *cur = masterBusState(live, m.id);
        if (!cur) continue;
        if (cur->micMonitorFx != m.micMonitorFx ||
            cur->softwareMonitor != m.softwareMonitor)
            plan.monitorPathMasters << m.id;
    }

    // What each surviving bus has to do to get there.
    for (const MasterBusState &m : plan.profile.masterBuses) {
        const MasterBusState *cur = masterBusState(live, m.id);
        if (!cur) continue;  // being added; nothing to relink yet
        if (cur->busType != m.busType) {
            // capture <-> midi on one id is a different chain end to end, and
            // there is no cheap way there. Vanishingly rare -- a bus's type is
            // fixed when it is created -- so treat it as an add and rewire.
            plan.addedMasters << m.id;
        } else if (m.busType == QLatin1String("midi")) {
            if (cur->midiPortMatch != m.midiPortMatch)
                plan.recapturedMasters << m.id;
        } else if (effectiveMasterCaptureMatch(m.id, cur->captureMatch) !=
                   effectiveMasterCaptureMatch(m.id, m.captureMatch)) {
            plan.recapturedMasters << m.id;
        }
    }

    return plan;
}

void MixerService::relinkMasterDevice(const QString &masterId) {
    if (!graph_) return;
    const MasterBusState *m = masterBusState(config_.live(), masterId);
    if (!m) return;
    const std::string sid = masterId.toStdString();

    if (m->busType == QLatin1String("midi")) {
        // Only the hardware edge. Rebuilding the synth chain here leaves
        // synth->gain stuck inactive, exactly as it does from the port picker.
        std::string err;
        if (!graph_->rewireMasterMidiInput(sid, err))
            qWarning("waveline: MIDI port for %s after profile load: %s",
                     qUtf8Printable(masterId), err.c_str());
        return;
    }

    engine_.sync();
    std::string err;
    if (!graph_->relinkMasterHwCapture(sid, err)) {
        // Left on whatever it was on. No global rewire to "fix" it: deleting a
        // hardware capture link is what stalls PipeWire's clock and silences
        // every output, which is a much worse outcome than one bus still
        // listening to the device it was already listening to.
        lastError_ = QString::fromStdString(err);
        qWarning("waveline: capture for %s after profile load: %s",
                 qUtf8Printable(masterId), err.c_str());
        return;
    }
    applyMasterFx(masterId);
    applyMasterInputVolume(masterId);
    graph_->applyMasterPathLevels(sid);
    if (masterHasWave3Hw(masterId)) pollMasterHardware(masterId);
}

void MixerService::applyProfileFixups(const ProfileLoadPlan &plan) {
    if (!graph_) return;
    for (const QString &id : plan.recapturedMasters) relinkMasterDevice(id);
    for (const QString &id : plan.monitorPathChannels) rewireChannelMonitorPath(id);
    for (const QString &id : plan.micSourceChannels) {
        std::string err;
        if (!graph_->rewireChannelMicSource(id.toStdString(), err) && !err.empty())
            qWarning("waveline: channel %s mic source after profile load: %s",
                     qUtf8Printable(id), err.c_str());
    }
    for (const QString &id : plan.monitorPathMasters) {
        if (id == QLatin1String("mic")) {
            rewireMicMonitorPath();
        } else {
            std::string err;
            if (!graph_->rewireMasterSoftwareMonitor(id.toStdString(), err) &&
                !err.empty())
                qWarning("waveline: master %s monitor path after profile load: %s",
                         qUtf8Printable(id), err.c_str());
        }
    }
}

void MixerService::reassertMixLevels() {
    for (int ms : {50, 150, 400}) {
        QTimer::singleShot(ms, this, [this] {
            if (!graph_) return;
            const Profile &p = config_.live();
            for (const MasterBusState &m : p.masterBuses) applyMasterMixLevels(m.id);
            graph_->applyAllChannelPathLevels();
            graph_->applyAllMonitorOutputGains();
            graph_->setStreamMixVolume(static_cast<float>(p.streamMixVolume));
            graph_->setStreamMixMuted(p.streamMixMuted);
        });
    }
}

bool MixerService::LoadProfile(const QString &name) {
    const Profile *stored = config_.find(name);
    if (!stored) return false;

    const ProfileLoadPlan plan = planProfileLoad(*stored);

    config_.live() = plan.profile;
    config_.setActive(name);
    applyProfile();

    // applyProfile() may have had to build a channel's noise filter, and a node
    // that was just created has no links yet.
    if (!plan.addedMasters.isEmpty() || applyCreatedNodes_) {
        // Nodes had to be created, and creating one means new links to make.
        // Same cost as pressing "+" on the input row, and for the same reason.
        qInfo("waveline: profile '%s' adds %d input device(s)%s, rewiring",
              qUtf8Printable(name), static_cast<int>(plan.addedMasters.size()),
              applyCreatedNodes_ ? " and a channel filter" : "");
        scheduleRewire();
    } else {
        // The fast path, and the point of all of this: the graph already has
        // exactly the nodes this profile wants, so nothing is torn down. What
        // did change relinks one path at a time; everything else is a volume
        // write applyProfile() has already made.
        applyProfileFixups(plan);
        reassertMixLevels();
    }

    // Written back so the migration is real, and so the next load of this
    // profile finds nothing left to reconcile and takes the fast path outright.
    // config_.live() rather than plan.profile: applyProfile() normalises a few
    // fields on the way in (the old global monitor master folds into the
    // per-output levels), and the profile should carry what actually happened.
    if (plan.migrated) config_.put(name, config_.live());

    if (!config_.save()) lastError_ = config_.lastError();
    emit Changed();
    return true;
}

void MixerService::SaveProfile(const QString &name) {
    // Snapshot live state straight into the NEW profile. Capturing into the
    // active one first would overwrite the profile being copied from, so
    // "save as" silently modified the profile you were still on.
    config_.saveAs(name, snapshot());  // also marks it active
    if (!config_.save()) lastError_ = config_.lastError();
    emit Changed();
}

bool MixerService::DeleteProfile(const QString &name) {
    // Only removes the snapshot; the live mixer is left exactly as it is.
    if (!config_.remove(name)) return false;
    if (!config_.save()) lastError_ = config_.lastError();
    emit Changed();
    return true;
}

bool MixerService::RenameProfile(const QString &from, const QString &to) {
    // A pure relabelling: nothing about the live mixer changes, so there is
    // deliberately no applyProfile() or rewire here even when the profile being
    // renamed is the active one.
    if (!config_.rename(from, to)) return false;
    if (!config_.save()) lastError_ = config_.lastError();
    emit Changed();
    return true;
}

namespace {
// What an exported file says it is. Checked on the way back in so a JSON file
// that happens to parse -- someone's whole config.json, say -- is refused
// rather than silently turning into an empty profile.
constexpr const char *kExportMagic = "waveline-profile";
constexpr int kExportVersion = 1;

// How far two levels may differ and still count as the same setting. Levels are
// 0..1 and the GUI shows whole percent, so this is a tenth of the smallest
// change anyone can make on purpose -- while still absorbing the drift a
// module-loopback introduces when it rewrites its own channelVolumes.
constexpr double kLevelEpsilon = 1e-3;

bool jsonEquivalent(const QJsonValue &a, const QJsonValue &b) {
    if (a.isDouble() && b.isDouble())
        return std::abs(a.toDouble() - b.toDouble()) <= kLevelEpsilon;
    if (a.isObject() && b.isObject()) {
        const QJsonObject ao = a.toObject();
        const QJsonObject bo = b.toObject();
        if (ao.keys() != bo.keys()) return false;
        for (auto it = ao.begin(); it != ao.end(); ++it)
            if (!jsonEquivalent(it.value(), bo.value(it.key()))) return false;
        return true;
    }
    if (a.isArray() && b.isArray()) {
        const QJsonArray aa = a.toArray();
        const QJsonArray ba = b.toArray();
        if (aa.size() != ba.size()) return false;
        for (int i = 0; i < aa.size(); ++i)
            if (!jsonEquivalent(aa.at(i), ba.at(i))) return false;
        return true;
    }
    return a == b;
}
}  // namespace

bool MixerService::ProfileMatchesLive(const QString &name) const {
    const Profile *stored = config_.find(name);
    if (!stored) return false;
    // Against the reconciled and normalised profile, not the raw one. A stored
    // profile that predates two of the input devices differs from live state in
    // exactly the way loading it is defined to ignore, and reporting that as
    // "unsaved changes" would warn about the migration rather than about the
    // user's work. Same for the monitor-master fold, which live state has been
    // through and a profile written before it was introduced has not.
    Profile want = planProfileLoad(*stored).profile;
    foldMonitorMaster(want);
    return jsonEquivalent(ConfigStore::toJson(want),
                          ConfigStore::toJson(snapshot()));
}

QString MixerService::ExportProfile(const QString &name) const {
    const Profile *p = config_.find(name);
    if (!p) return {};

    QJsonObject root;
    root[QStringLiteral("waveline")] = QLatin1String(kExportMagic);
    root[QStringLiteral("version")] = kExportVersion;
    // Carried so an import has a name to suggest. Advisory only: the importer
    // decides what it ends up called, because the name may already be taken on
    // the machine the file lands on.
    root[QStringLiteral("name")] = name;
    root[QStringLiteral("exported")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root[QStringLiteral("profile")] = ConfigStore::toJson(*p);

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool MixerService::ImportProfile(const QString &name, const QString &json) {
    if (name.trimmed().isEmpty()) return false;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        lastError_ = QStringLiteral("not a profile file (%1)").arg(err.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    QJsonObject body;
    if (root.value(QStringLiteral("waveline")).toString() ==
        QLatin1String(kExportMagic)) {
        if (root.value(QStringLiteral("version")).toInt(kExportVersion) >
            kExportVersion) {
            lastError_ = QStringLiteral(
                "that profile was exported by a newer version of Waveline");
            return false;
        }
        body = root.value(QStringLiteral("profile")).toObject();
    } else if (root.contains(QStringLiteral("channels")) ||
               root.contains(QStringLiteral("masterBuses"))) {
        // A bare profile object, as toJson() writes it. Accepted so a profile
        // lifted straight out of a config.json by hand still imports -- which
        // is exactly what someone recovering a backup will try first.
        body = root;
    } else {
        lastError_ = QStringLiteral("not a Waveline profile");
        return false;
    }

    config_.put(name.trimmed(), ConfigStore::fromJson(body));
    if (!config_.save()) lastError_ = config_.lastError();
    emit Changed();
    return true;
}

void MixerService::Save() {
    captureToProfile();
    if (!config_.save()) lastError_ = config_.lastError();
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "configstore.h"

#include "engine/appidentity.h"
#include "engine/creativefxspec.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

namespace {
constexpr int kVersion = 1;

void syncPrimaryFromMasterBuses(Profile &p) {
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

MasterBusState primaryFromLegacyProfile(const Profile &p) {
    MasterBusState m;
    m.id = QStringLiteral("mic");
    m.name = QStringLiteral("Input #1");
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
    return m;
}

}  // namespace

bool isLegacyAutoInputName(const QString &name) {
    if (name == QLatin1String("Master")) return true;
    static const QRegularExpression legacy(
        QStringLiteral(R"(^Input Device #\d+$)"));
    return legacy.match(name).hasMatch();
}

void clearLegacyAutoInputNames(Profile &p) {
    for (MasterBusState &m : p.masterBuses) {
        if (m.nameCustom && isLegacyAutoInputName(m.name)) m.nameCustom = false;
    }
}

ConfigStore::ConfigStore() {
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString dir = base + QStringLiteral("/waveline");
    QDir().mkpath(dir);
    path_ = dir + QStringLiteral("/config.json");

    // Carried over from when this was a Wave:3-only tool. Profiles are the
    // user's own work -- hours of levels and EQ in some cases -- and silently
    // starting from an empty config after an upgrade would look exactly like
    // having lost them. Copied rather than moved, and only when there is
    // nothing here yet, so a downgrade still finds its old file intact.
    if (!QFile::exists(path_)) {
        const QString old = base + QStringLiteral("/wave3/config.json");
        if (QFile::exists(old)) QFile::copy(old, path_);
    }
}

QStringList ConfigStore::profileNames() const {
    QStringList names = profiles_.keys();
    names.sort();
    return names;
}

void ConfigStore::saveAs(const QString &name, const Profile &p) {
    profiles_.insert(name, p);
    active_ = name;
}

bool ConfigStore::loadInto(const QString &name, Profile &out) {
    auto it = profiles_.find(name);
    if (it == profiles_.end()) return false;
    out = *it;
    active_ = name;
    return true;
}

bool ConfigStore::remove(const QString &name) {
    if (!profiles_.contains(name)) return false;
    profiles_.remove(name);
    if (active_ == name) active_.clear();
    return true;
}

const Profile *ConfigStore::find(const QString &name) const {
    auto it = profiles_.constFind(name);
    return it == profiles_.constEnd() ? nullptr : &it.value();
}

void ConfigStore::put(const QString &name, const Profile &p) {
    profiles_.insert(name, p);
}

void ConfigStore::setActive(const QString &name) {
    if (profiles_.contains(name)) active_ = name;
}

bool ConfigStore::rename(const QString &from, const QString &to) {
    if (from.isEmpty() || to.isEmpty()) return false;
    if (from == to) return true;
    if (!profiles_.contains(from) || profiles_.contains(to)) return false;
    profiles_.insert(to, profiles_.take(from));
    if (active_ == from) active_ = to;
    return true;
}

QJsonObject ConfigStore::toJson(const Profile &in) {
    Profile p = in;
    if (!p.masterBuses.isEmpty()) syncPrimaryFromMasterBuses(p);

    QJsonObject o;

    QJsonObject channels;
    for (auto it = p.channels.begin(); it != p.channels.end(); ++it) {
        QJsonObject c;
        c[QStringLiteral("streamVolume")] = it->streamVolume;
        c[QStringLiteral("monitorVolume")] = it->monitorVolume;
        c[QStringLiteral("streamMuted")] = it->streamMuted;
        c[QStringLiteral("monitorMuted")] = it->monitorMuted;
        channels[it.key()] = c;
    }
    o[QStringLiteral("channels")] = channels;

    QJsonObject mic;
    mic[QStringLiteral("streamVolume")] = p.mic.streamVolume;
    mic[QStringLiteral("monitorVolume")] = p.mic.monitorVolume;
    mic[QStringLiteral("streamMuted")] = p.mic.streamMuted;
    mic[QStringLiteral("monitorMuted")] = p.mic.monitorMuted;
    o[QStringLiteral("mic")] = mic;

    QJsonArray monitorOuts;
    for (const MonitorOutputState &out : p.monitorOutputs) {
        if (out.sink.isEmpty()) continue;
        QJsonObject mo;
        mo[QStringLiteral("sink")] = out.sink;
        if (!out.description.isEmpty())
            mo[QStringLiteral("description")] = out.description;
        mo[QStringLiteral("volume")] = out.volume;
        mo[QStringLiteral("muted")] = out.muted;
        monitorOuts.append(mo);
    }
    o[QStringLiteral("monitorOutputs")] = monitorOuts;
    o[QStringLiteral("streamMixVolume")] = p.streamMixVolume;
    o[QStringLiteral("streamMixMuted")] = p.streamMixMuted;
    o[QStringLiteral("noiseSuppression")] = p.noiseSuppression;
    o[QStringLiteral("routingEnabled")] = p.routingEnabled;
    o[QStringLiteral("softwareMonitor")] = p.softwareMonitor;
    o[QStringLiteral("monitorLevel")] = p.monitorLevel;
    o[QStringLiteral("monitorMaster")] = p.monitorMaster;
    o[QStringLiteral("monitorMasterMuted")] = p.monitorMasterMuted;
    o[QStringLiteral("noiseIntensity")] = p.noiseIntensity;
    o[QStringLiteral("noiseEngine")] = p.noiseEngine;
    o[QStringLiteral("micStereo")] = p.micStereo;
    o[QStringLiteral("micInputVolume")] = p.micInputVolume;
    o[QStringLiteral("micInputMuted")] = p.micInputMuted;
    o[QStringLiteral("hardwareMonitor")] = p.hardwareMonitor;
    o[QStringLiteral("micGainDb")] = p.micGainDb;
    o[QStringLiteral("hwClipguard")] = p.hwClipguard;
    o[QStringLiteral("hwMicMuted")] = p.hwMicMuted;
    o[QStringLiteral("hwHpVolumeDb")] = p.hwHpVolumeDb;
    o[QStringLiteral("hwHpMuted")] = p.hwHpMuted;

    QJsonObject soundSharing;
    soundSharing[QStringLiteral("enabled")] = p.soundSharing.enabled;
    soundSharing[QStringLiteral("streamVolume")] = p.soundSharing.streamVolume;
    soundSharing[QStringLiteral("monitorVolume")] = p.soundSharing.monitorVolume;
    soundSharing[QStringLiteral("streamMuted")] = p.soundSharing.streamMuted;
    soundSharing[QStringLiteral("monitorMuted")] = p.soundSharing.monitorMuted;
    QJsonArray sharedApps;
    for (const QString &a : p.soundSharing.apps) sharedApps.append(a);
    soundSharing[QStringLiteral("apps")] = sharedApps;
    QJsonObject shareTargets;
    for (auto it = p.soundSharing.appTargets.begin();
         it != p.soundSharing.appTargets.end(); ++it)
        shareTargets[it.key()] = it.value();
    soundSharing[QStringLiteral("appTargets")] = shareTargets;
    QJsonObject shareGains;
    for (auto it = p.soundSharing.appGains.begin();
         it != p.soundSharing.appGains.end(); ++it)
        shareGains[it.key()] = it.value();
    soundSharing[QStringLiteral("appGains")] = shareGains;
    o[QStringLiteral("soundSharing")] = soundSharing;

    auto fxToJson = [](const ChannelFxState &fx) {
        QJsonObject o;
        o[QStringLiteral("lowCut")] = fx.lowCut;
        o[QStringLiteral("lowCutHz")] = fx.lowCutHz;
        o[QStringLiteral("eq")] = fx.eq;
        o[QStringLiteral("lowDb")] = fx.lowDb;
        o[QStringLiteral("midDb")] = fx.midDb;
        o[QStringLiteral("highDb")] = fx.highDb;
        o[QStringLiteral("eqAdvanced")] = fx.eqAdvanced;
        if (!fx.proEqBands.isEmpty())
            o[QStringLiteral("proEqBands")] = fx.proEqBands;
        return o;
    };
    o[QStringLiteral("micFx")] = fxToJson(p.micFx);
    auto dynToJson = [](const DynamicsState &d) {
        QJsonObject o;
        o[QStringLiteral("gate")] = d.gate;
        o[QStringLiteral("gateThresholdDb")] = d.gateThresholdDb;
        o[QStringLiteral("gateAttackMs")] = d.gateAttackMs;
        o[QStringLiteral("gateReleaseMs")] = d.gateReleaseMs;
        o[QStringLiteral("compressor")] = d.compressor;
        o[QStringLiteral("compThresholdDb")] = d.compThresholdDb;
        o[QStringLiteral("compRatio")] = d.compRatio;
        o[QStringLiteral("compAttackMs")] = d.compAttackMs;
        o[QStringLiteral("compReleaseMs")] = d.compReleaseMs;
        o[QStringLiteral("compKneeDb")] = d.compKneeDb;
        o[QStringLiteral("makeupGainDb")] = d.makeupGainDb;
        o[QStringLiteral("autoMakeup")] = d.autoMakeup;
        o[QStringLiteral("limiter")] = d.limiter;
        o[QStringLiteral("limitThresholdDb")] = d.limitThresholdDb;
        o[QStringLiteral("limitAttackMs")] = d.limitAttackMs;
        o[QStringLiteral("limitReleaseMs")] = d.limitReleaseMs;
        return o;
    };
    o[QStringLiteral("micDynamics")] = dynToJson(p.micDynamics);
    auto creativeToJson = [](const CreativeFxState &c) {
        QJsonObject o;
        o[QStringLiteral("spec")] = c.spec;
        return o;
    };
    o[QStringLiteral("micCreativeFx")] = creativeToJson(p.micCreativeFx);
    o[QStringLiteral("micEffectsEnabled")] = p.micEffectsEnabled;
    o[QStringLiteral("micMonitorFx")] = p.micMonitorFx;

    QJsonArray masterArr;
    for (const MasterBusState &m : p.masterBuses) {
        QJsonObject mb;
        mb[QStringLiteral("id")] = m.id;
        mb[QStringLiteral("busType")] = m.busType;
        mb[QStringLiteral("name")] = m.name;
        mb[QStringLiteral("nameCustom")] = m.nameCustom;
        mb[QStringLiteral("captureMatch")] = m.captureMatch;
        mb[QStringLiteral("midiPortMatch")] = m.midiPortMatch;
        mb[QStringLiteral("deviceLabel")] = m.deviceLabel;
        mb[QStringLiteral("soundfontPath")] = m.soundfontPath;
        QJsonArray sfArr;
        for (const QString &sf : m.soundfontPaths) sfArr.append(sf);
        mb[QStringLiteral("soundfontPaths")] = sfArr;
        QJsonObject mix;
        mix[QStringLiteral("streamVolume")] = m.mix.streamVolume;
        mix[QStringLiteral("monitorVolume")] = m.mix.monitorVolume;
        mix[QStringLiteral("streamMuted")] = m.mix.streamMuted;
        mix[QStringLiteral("monitorMuted")] = m.mix.monitorMuted;
        mb[QStringLiteral("mix")] = mix;
        mb[QStringLiteral("micFx")] = fxToJson(m.micFx);
        mb[QStringLiteral("micDynamics")] = dynToJson(m.micDynamics);
        mb[QStringLiteral("micCreativeFx")] = creativeToJson(m.micCreativeFx);
        mb[QStringLiteral("rackCreativeFx")] = creativeToJson(m.rackCreativeFx);
        mb[QStringLiteral("rackMode")] = m.rackMode;
        mb[QStringLiteral("micEffectsEnabled")] = m.micEffectsEnabled;
        mb[QStringLiteral("micMonitorFx")] = m.micMonitorFx;
        mb[QStringLiteral("noiseSuppression")] = m.noiseSuppression;
        mb[QStringLiteral("noiseIntensity")] = m.noiseIntensity;
        mb[QStringLiteral("deEsser")] = m.deEsser;
        mb[QStringLiteral("deEsserIntensity")] = m.deEsserIntensity;
        mb[QStringLiteral("softwareMonitor")] = m.softwareMonitor;
        mb[QStringLiteral("micStereo")] = m.micStereo;
        mb[QStringLiteral("micInputVolume")] = m.micInputVolume;
        mb[QStringLiteral("micInputMuted")] = m.micInputMuted;
        mb[QStringLiteral("hardwareMonitor")] = m.hardwareMonitor;
        mb[QStringLiteral("micGainDb")] = m.micGainDb;
        mb[QStringLiteral("hwClipguard")] = m.hwClipguard;
        mb[QStringLiteral("hwMicMuted")] = m.hwMicMuted;
        mb[QStringLiteral("hwHpVolumeDb")] = m.hwHpVolumeDb;
        mb[QStringLiteral("hwHpMuted")] = m.hwHpMuted;
        masterArr.append(mb);
    }
    o[QStringLiteral("masterBuses")] = masterArr;

    auto fxStageToJson = [&fxToJson, &dynToJson, &creativeToJson](const ChannelFxStageState &st) {
        QJsonObject o;
        o[QStringLiteral("fx")] = fxToJson(st.fx);
        o[QStringLiteral("dynamics")] = dynToJson(st.dynamics);
        o[QStringLiteral("creativeFx")] = creativeToJson(st.creativeFx);
        o[QStringLiteral("noiseSuppression")] = st.noiseSuppression;
        o[QStringLiteral("noiseIntensity")] = st.noiseIntensity;
        o[QStringLiteral("deEsser")] = st.deEsser;
        o[QStringLiteral("deEsserIntensity")] = st.deEsserIntensity;
        return o;
    };

    o[QStringLiteral("masterOutput")] = fxStageToJson(p.masterOutput);
    QJsonObject masterDuck;
    masterDuck[QStringLiteral("enabled")] = p.masterOutputDucking.enabled;
    masterDuck[QStringLiteral("intensity")] = p.masterOutputDucking.intensity;
    masterDuck[QStringLiteral("thresholdDb")] = p.masterOutputDucking.thresholdDb;
    masterDuck[QStringLiteral("depthDb")] = p.masterOutputDucking.depthDb;
    masterDuck[QStringLiteral("attackMs")] = p.masterOutputDucking.attackMs;
    masterDuck[QStringLiteral("releaseMs")] = p.masterOutputDucking.releaseMs;
    masterDuck[QStringLiteral("holdSec")] = p.masterOutputDucking.holdSec;
    QJsonArray masterDuckSrc;
    for (const DuckingSourceState &src : p.masterOutputDucking.sources) {
        QJsonObject so;
        so[QStringLiteral("kind")] = src.kind;
        if (!src.channelId.isEmpty())
            so[QStringLiteral("channel")] = src.channelId;
        masterDuckSrc.append(so);
    }
    masterDuck[QStringLiteral("sources")] = masterDuckSrc;
    o[QStringLiteral("masterOutputDucking")] = masterDuck;
    QJsonObject masterLufs;
    masterLufs[QStringLiteral("enabled")] = p.masterOutputLufsLimiter.enabled;
    masterLufs[QStringLiteral("maxLufs")] = p.masterOutputLufsLimiter.maxLufs;
    o[QStringLiteral("masterOutputLufsLimiter")] = masterLufs;

    QJsonObject channelEffects;
    for (auto it = p.channelEffects.begin(); it != p.channelEffects.end(); ++it) {
        QJsonObject ce;
        ce[QStringLiteral("input")] = fxStageToJson(it->input);
        ce[QStringLiteral("output")] = fxStageToJson(it->output);
        ce[QStringLiteral("micGain")] = it->micGain;
        ce[QStringLiteral("micMuted")] = it->micMuted;
        ce[QStringLiteral("effectsEnabled")] = it->effectsEnabled;
        ce[QStringLiteral("monitorFx")] = it->monitorFx;
        ce[QStringLiteral("micSource")] = it->micSource;
        ce[QStringLiteral("micMonitor")] = it->micMonitor;
        QJsonObject duck;
        duck[QStringLiteral("enabled")] = it->ducking.enabled;
        duck[QStringLiteral("intensity")] = it->ducking.intensity;
        duck[QStringLiteral("thresholdDb")] = it->ducking.thresholdDb;
        duck[QStringLiteral("depthDb")] = it->ducking.depthDb;
        duck[QStringLiteral("attackMs")] = it->ducking.attackMs;
        duck[QStringLiteral("releaseMs")] = it->ducking.releaseMs;
        duck[QStringLiteral("holdSec")] = it->ducking.holdSec;
        QJsonArray srcArr;
        for (const DuckingSourceState &src : it->ducking.sources) {
            QJsonObject so;
            so[QStringLiteral("kind")] = src.kind;
            if (!src.channelId.isEmpty())
                so[QStringLiteral("channel")] = src.channelId;
            srcArr.append(so);
        }
        duck[QStringLiteral("sources")] = srcArr;
        ce[QStringLiteral("ducking")] = duck;
        QJsonObject lufs;
        lufs[QStringLiteral("enabled")] = it->lufsLimiter.enabled;
        lufs[QStringLiteral("maxLufs")] = it->lufsLimiter.maxLufs;
        ce[QStringLiteral("lufsLimiter")] = lufs;
        ce[QStringLiteral("inputUseMasterEffects")] =
            !it->inputEffectSourceMasterId.isEmpty();
        ce[QStringLiteral("outputUseMasterEffects")] =
            !it->outputEffectSourceMasterId.isEmpty();
        if (!it->inputEffectSourceMasterId.isEmpty())
            ce[QStringLiteral("inputEffectSourceMasterId")] =
                it->inputEffectSourceMasterId;
        if (!it->outputEffectSourceMasterId.isEmpty())
            ce[QStringLiteral("outputEffectSourceMasterId")] =
                it->outputEffectSourceMasterId;
        ce[QStringLiteral("inputUseDeviceFx")] = it->inputUseDeviceFx;
        ce[QStringLiteral("masterMicId")] = it->primaryMasterMicId();
        QJsonArray micIds;
        for (const QString &id : it->masterMicIds) micIds.append(id);
        ce[QStringLiteral("masterMicIds")] = micIds;
        channelEffects[it.key()] = ce;
    }
    o[QStringLiteral("channelEffects")] = channelEffects;

    QJsonArray rules;
    for (const auto &r : p.rules) {
        QJsonObject ro;
        ro[QStringLiteral("pattern")] = r.pattern;
        ro[QStringLiteral("channel")] = r.channel;
        rules.append(ro);
    }
    o[QStringLiteral("rules")] = rules;

    QJsonObject appChannels;
    for (auto it = p.appChannels.begin(); it != p.appChannels.end(); ++it)
        appChannels[it.key()] = it.value();
    o[QStringLiteral("appChannels")] = appChannels;

    QJsonObject appVolumes;
    for (auto it = p.appVolumes.begin(); it != p.appVolumes.end(); ++it)
        appVolumes[it.key()] = it.value();
    o[QStringLiteral("appVolumes")] = appVolumes;

    QJsonArray mutedAtStop;
    for (const QString &sink : p.sinksMutedAtStop) mutedAtStop.append(sink);
    o[QStringLiteral("sinksMutedAtStop")] = mutedAtStop;

    QJsonObject cards;
    for (auto it = p.cardAppearance.begin(); it != p.cardAppearance.end(); ++it) {
        QJsonObject card;
        card[QStringLiteral("color")] = it->color;
        card[QStringLiteral("icon")] = it->icon;
        cards[it.key()] = card;
    }
    o[QStringLiteral("cardAppearance")] = cards;

    QJsonObject channelNames;
    for (auto it = p.channelNames.begin(); it != p.channelNames.end(); ++it)
        channelNames[it.key()] = it.value();
    o[QStringLiteral("channelNames")] = channelNames;
    return o;
}

Profile ConfigStore::fromJson(const QJsonObject &o) {
    Profile p;
    const QJsonObject channels = o[QStringLiteral("channels")].toObject();
    for (auto it = channels.begin(); it != channels.end(); ++it) {
        const QJsonObject c = it.value().toObject();
        ChannelState s;
        s.streamVolume = c[QStringLiteral("streamVolume")].toDouble(1.0);
        s.monitorVolume = c[QStringLiteral("monitorVolume")].toDouble(1.0);
        s.streamMuted = c[QStringLiteral("streamMuted")].toBool(false);
        s.monitorMuted = c[QStringLiteral("monitorMuted")].toBool(false);
        p.channels.insert(it.key(), s);
    }

    const QJsonObject mic = o[QStringLiteral("mic")].toObject();
    p.mic.streamVolume = mic[QStringLiteral("streamVolume")].toDouble(1.0);
    p.mic.monitorVolume = mic[QStringLiteral("monitorVolume")].toDouble(1.0);
    p.mic.streamMuted = mic[QStringLiteral("streamMuted")].toBool(false);
    p.mic.monitorMuted = mic[QStringLiteral("monitorMuted")].toBool(false);

    p.monitorOutputs.clear();
    const QJsonArray monitorOuts = o[QStringLiteral("monitorOutputs")].toArray();
    for (const QJsonValue &v : monitorOuts) {
        if (p.monitorOutputs.size() >= 5) break;
        MonitorOutputState out;
        if (v.isObject()) {
            const QJsonObject mo = v.toObject();
            out.sink = mo[QStringLiteral("sink")].toString();
            out.description = mo[QStringLiteral("description")].toString();
            out.volume = mo[QStringLiteral("volume")].toDouble(1.0);
            out.muted = mo[QStringLiteral("muted")].toBool(false);
        } else {
            out.sink = v.toString();
        }
        if (!out.sink.isEmpty()) p.monitorOutputs.append(out);
    }
    if (p.monitorOutputs.isEmpty()) {
        const QString legacy = o[QStringLiteral("monitorOutput")].toString();
        if (!legacy.isEmpty())
            p.monitorOutputs.append(MonitorOutputState{legacy, QString(), 1.0, false});
    }
    p.streamMixVolume = o[QStringLiteral("streamMixVolume")].toDouble(1.0);
    p.streamMixMuted = o[QStringLiteral("streamMixMuted")].toBool(false);
    p.noiseSuppression = o[QStringLiteral("noiseSuppression")].toBool(true);
    p.routingEnabled = o[QStringLiteral("routingEnabled")].toBool(true);
    p.softwareMonitor = o[QStringLiteral("softwareMonitor")].toBool(true);
    p.monitorLevel = o[QStringLiteral("monitorLevel")].toDouble(1.0);
    p.monitorMaster = o[QStringLiteral("monitorMaster")].toDouble(1.0);
    p.monitorMasterMuted =
        o[QStringLiteral("monitorMasterMuted")].toBool(false);
    p.noiseIntensity = o[QStringLiteral("noiseIntensity")].toDouble(1.0);
    p.noiseEngine =
        o[QStringLiteral("noiseEngine")].toString(QStringLiteral("deepfilternet"));
    p.micStereo = o[QStringLiteral("micStereo")].toBool(true);
    p.micInputVolume = o[QStringLiteral("micInputVolume")].toDouble(1.0);
    p.micInputMuted = o[QStringLiteral("micInputMuted")].toBool(false);
    p.hardwareMonitor = o[QStringLiteral("hardwareMonitor")].toInt(-1);
    p.micGainDb = o[QStringLiteral("micGainDb")].toDouble(-1.0);
    p.hwClipguard = o[QStringLiteral("hwClipguard")].toInt(-1);
    p.hwMicMuted = o[QStringLiteral("hwMicMuted")].toInt(-1);
    p.hwHpVolumeDb = o[QStringLiteral("hwHpVolumeDb")].toDouble(1.0);
    p.hwHpMuted = o[QStringLiteral("hwHpMuted")].toInt(-1);

    const QJsonObject soundSharing = o[QStringLiteral("soundSharing")].toObject();
    p.soundSharing.enabled = soundSharing[QStringLiteral("enabled")].toBool(false);
    p.soundSharing.streamVolume =
        soundSharing[QStringLiteral("streamVolume")].toDouble(1.0);
    p.soundSharing.monitorVolume =
        soundSharing[QStringLiteral("monitorVolume")].toDouble(1.0);
    p.soundSharing.streamMuted =
        soundSharing[QStringLiteral("streamMuted")].toBool(false);
    p.soundSharing.monitorMuted =
        soundSharing[QStringLiteral("monitorMuted")].toBool(false);
    p.soundSharing.apps.clear();
    for (const QJsonValue &v : soundSharing[QStringLiteral("apps")].toArray())
        p.soundSharing.apps.append(v.toString());
    p.soundSharing.appTargets.clear();
    const QJsonObject shareTargets =
        soundSharing[QStringLiteral("appTargets")].toObject();
    for (auto it = shareTargets.begin(); it != shareTargets.end(); ++it)
        p.soundSharing.appTargets.insert(it.key(), it.value().toString());
    p.soundSharing.appGains.clear();
    const QJsonObject shareGains =
        soundSharing[QStringLiteral("appGains")].toObject();
    for (auto it = shareGains.begin(); it != shareGains.end(); ++it)
        p.soundSharing.appGains.insert(it.key(), it.value().toDouble(1.0));

    auto fxFromJson = [](const QJsonObject &o, ChannelFxState &fx) {
        fx.lowCut = o[QStringLiteral("lowCut")].toBool(false);
        fx.lowCutHz = o[QStringLiteral("lowCutHz")].toInt(80);
        if (fx.lowCutHz != 80 && fx.lowCutHz != 120) fx.lowCutHz = 80;
        fx.eq = o[QStringLiteral("eq")].toBool(false);
        fx.lowDb = o[QStringLiteral("lowDb")].toDouble(0.0);
        fx.midDb = o[QStringLiteral("midDb")].toDouble(0.0);
        fx.highDb = o[QStringLiteral("highDb")].toDouble(0.0);
        fx.eqAdvanced = o[QStringLiteral("eqAdvanced")].toBool(false);
        fx.proEqBands = o[QStringLiteral("proEqBands")].toString();
    };
    fxFromJson(o[QStringLiteral("micFx")].toObject(), p.micFx);
    auto dynFromJson = [](const QJsonObject &o, DynamicsState &d) {
        d.gate = o[QStringLiteral("gate")].toBool(false);
        d.gateThresholdDb = o[QStringLiteral("gateThresholdDb")].toDouble(-50.0);
        d.gateAttackMs = o[QStringLiteral("gateAttackMs")].toDouble(3.0);
        d.gateReleaseMs = o[QStringLiteral("gateReleaseMs")].toDouble(300.0);
        d.compressor = o[QStringLiteral("compressor")].toBool(false);
        d.compThresholdDb = o[QStringLiteral("compThresholdDb")].toDouble(-24.0);
        d.compRatio = o[QStringLiteral("compRatio")].toDouble(4.0);
        d.compAttackMs = o[QStringLiteral("compAttackMs")].toDouble(5.0);
        d.compReleaseMs = o[QStringLiteral("compReleaseMs")].toDouble(150.0);
        d.compKneeDb = o[QStringLiteral("compKneeDb")].toDouble(6.0);
        d.makeupGainDb = o[QStringLiteral("makeupGainDb")].toDouble(0.0);
        d.autoMakeup = o[QStringLiteral("autoMakeup")].toBool(true);
        d.limiter = o[QStringLiteral("limiter")].toBool(true);
        d.limitThresholdDb = o[QStringLiteral("limitThresholdDb")].toDouble(-3.0);
        d.limitAttackMs = o[QStringLiteral("limitAttackMs")].toDouble(1.0);
        d.limitReleaseMs = o[QStringLiteral("limitReleaseMs")].toDouble(50.0);
    };
    dynFromJson(o[QStringLiteral("micDynamics")].toObject(), p.micDynamics);
    auto creativeFromJson = [](const QJsonObject &o, CreativeFxState &c) {
        if (o.contains(QStringLiteral("spec"))) {
            c.spec = o[QStringLiteral("spec")].toString();
            return;
        }
        if (o.isEmpty()) {
            c.spec.clear();
            return;
        }
        // Pre-overhaul config: reverb/overdrive/delay were three flat
        // fields. Rebuilt into the new spec so an upgraded profile keeps
        // sounding exactly like it did before, with every new effect
        // defaulted off.
        waveline::CreativeFxSettings s;
        s.reverb.enabled = o[QStringLiteral("reverbEnabled")].toBool(false);
        s.reverb.size = static_cast<float>(o[QStringLiteral("reverbDecay")].toDouble(0.5));
        s.overdrive.enabled = o[QStringLiteral("overdriveEnabled")].toBool(false);
        s.overdrive.drive =
            static_cast<float>(o[QStringLiteral("overdriveIntensity")].toDouble(0.5));
        s.delay.enabled = o[QStringLiteral("delayEnabled")].toBool(false);
        s.delay.timeMs = static_cast<float>(o[QStringLiteral("delayMs")].toDouble(250.0));
        c.spec = QString::fromStdString(waveline::encodeCreativeFx(s));
    };
    creativeFromJson(o[QStringLiteral("micCreativeFx")].toObject(), p.micCreativeFx);
    p.micEffectsEnabled = o[QStringLiteral("micEffectsEnabled")].toBool(true);
    p.micMonitorFx = o[QStringLiteral("micMonitorFx")].toBool(true);

    p.masterBuses.clear();
    const QJsonArray masterArr = o[QStringLiteral("masterBuses")].toArray();
    if (!masterArr.isEmpty()) {
        for (const QJsonValue &v : masterArr) {
            const QJsonObject mb = v.toObject();
            MasterBusState m;
            m.id = mb[QStringLiteral("id")].toString(QStringLiteral("mic"));
            m.busType = mb[QStringLiteral("busType")].toString(QStringLiteral("capture"));
            m.name = mb[QStringLiteral("name")].toString();
            m.nameCustom = mb[QStringLiteral("nameCustom")].toBool(false);
            m.captureMatch = mb[QStringLiteral("captureMatch")].toString();
            m.midiPortMatch = mb[QStringLiteral("midiPortMatch")].toString();
            m.deviceLabel = mb[QStringLiteral("deviceLabel")].toString();
            m.soundfontPath = mb[QStringLiteral("soundfontPath")].toString();
            const QJsonArray sfArr = mb[QStringLiteral("soundfontPaths")].toArray();
            for (const QJsonValue &sv : sfArr) {
                const QString path = sv.toString();
                if (!path.isEmpty()) m.soundfontPaths.append(path);
            }
            (void)mb[QStringLiteral("bridgeToPrimary")];  // legacy, ignored
            const QJsonObject mix = mb[QStringLiteral("mix")].toObject();
            if (!mix.isEmpty()) {
                m.mix.streamVolume = mix[QStringLiteral("streamVolume")].toDouble(1.0);
                m.mix.monitorVolume = mix[QStringLiteral("monitorVolume")].toDouble(1.0);
                m.mix.streamMuted = mix[QStringLiteral("streamMuted")].toBool(false);
                m.mix.monitorMuted = mix[QStringLiteral("monitorMuted")].toBool(false);
            }
            fxFromJson(mb[QStringLiteral("micFx")].toObject(), m.micFx);
            dynFromJson(mb[QStringLiteral("micDynamics")].toObject(), m.micDynamics);
            creativeFromJson(mb[QStringLiteral("micCreativeFx")].toObject(), m.micCreativeFx);
            creativeFromJson(mb[QStringLiteral("rackCreativeFx")].toObject(), m.rackCreativeFx);
            m.rackMode = mb[QStringLiteral("rackMode")].toBool(false);
            m.micEffectsEnabled =
                mb[QStringLiteral("micEffectsEnabled")].toBool(true);
            m.micMonitorFx = mb[QStringLiteral("micMonitorFx")].toBool(true);
            const bool isPrimary = m.id == QStringLiteral("mic");
            m.noiseSuppression = mb.contains(QStringLiteral("noiseSuppression"))
                                     ? mb[QStringLiteral("noiseSuppression")].toBool(true)
                                     : isPrimary ? p.noiseSuppression : true;
            m.noiseIntensity = mb.contains(QStringLiteral("noiseIntensity"))
                                     ? mb[QStringLiteral("noiseIntensity")].toDouble(1.0)
                                     : isPrimary ? p.noiseIntensity : 1.0;
            m.deEsser = mb[QStringLiteral("deEsser")].toBool(false);
            m.deEsserIntensity =
                std::clamp(mb[QStringLiteral("deEsserIntensity")].toDouble(0.5), 0.0, 1.0);
            m.softwareMonitor = mb.contains(QStringLiteral("softwareMonitor"))
                                    ? mb[QStringLiteral("softwareMonitor")].toBool(false)
                                    : isPrimary ? p.softwareMonitor : false;
            m.micStereo = mb[QStringLiteral("micStereo")].toBool(true);
            m.micInputVolume = mb[QStringLiteral("micInputVolume")].toDouble(1.0);
            m.micInputMuted = mb[QStringLiteral("micInputMuted")].toBool(false);
            m.hardwareMonitor = mb[QStringLiteral("hardwareMonitor")].toInt(-1);
            m.micGainDb = mb[QStringLiteral("micGainDb")].toDouble(-1.0);
            m.hwClipguard = mb[QStringLiteral("hwClipguard")].toInt(-1);
            m.hwMicMuted = mb[QStringLiteral("hwMicMuted")].toInt(-1);
            m.hwHpVolumeDb = mb[QStringLiteral("hwHpVolumeDb")].toDouble(1.0);
            m.hwHpMuted = mb[QStringLiteral("hwHpMuted")].toInt(-1);
            p.masterBuses.append(m);
        }
    } else {
        p.masterBuses.append(primaryFromLegacyProfile(p));
    }
    if (p.masterBuses.first().name.isEmpty())
        p.masterBuses.first().name = QStringLiteral("Input #1");
    clearLegacyAutoInputNames(p);
    syncPrimaryFromMasterBuses(p);

    {
        const QJsonObject mo = o[QStringLiteral("masterOutput")].toObject();
        if (!mo.isEmpty()) {
            fxFromJson(mo[QStringLiteral("fx")].toObject(), p.masterOutput.fx);
            dynFromJson(mo[QStringLiteral("dynamics")].toObject(), p.masterOutput.dynamics);
            creativeFromJson(mo[QStringLiteral("creativeFx")].toObject(),
                             p.masterOutput.creativeFx);
            p.masterOutput.noiseSuppression =
                mo[QStringLiteral("noiseSuppression")].toBool(false);
            p.masterOutput.noiseIntensity =
                mo[QStringLiteral("noiseIntensity")].toDouble(1.0);
            p.masterOutput.deEsser = mo[QStringLiteral("deEsser")].toBool(false);
            p.masterOutput.deEsserIntensity = std::clamp(
                mo[QStringLiteral("deEsserIntensity")].toDouble(0.5), 0.0, 1.0);
        }
        const QJsonObject md = o[QStringLiteral("masterOutputDucking")].toObject();
        if (!md.isEmpty()) {
            p.masterOutputDucking.enabled = md[QStringLiteral("enabled")].toBool(false);
            p.masterOutputDucking.intensity = md[QStringLiteral("intensity")].toDouble(0.75);
            p.masterOutputDucking.thresholdDb = md[QStringLiteral("thresholdDb")].toDouble(-32.0);
            p.masterOutputDucking.depthDb = md[QStringLiteral("depthDb")].toDouble(-18.0);
            p.masterOutputDucking.attackMs = md[QStringLiteral("attackMs")].toDouble(100.0);
            p.masterOutputDucking.releaseMs = md[QStringLiteral("releaseMs")].toDouble(450.0);
            p.masterOutputDucking.holdSec = md[QStringLiteral("holdSec")].toDouble(3.0);
            p.masterOutputDucking.sources.clear();
            const QJsonArray srcArr = md[QStringLiteral("sources")].toArray();
            for (const QJsonValue &v : srcArr) {
                const QJsonObject so = v.toObject();
                DuckingSourceState src;
                src.kind = so[QStringLiteral("kind")].toString(QStringLiteral("master_mic"));
                src.channelId = so[QStringLiteral("channel")].toString();
                if (src.kind == QLatin1String("master_mic") && src.channelId.isEmpty())
                    src.channelId = QStringLiteral("mic");
                p.masterOutputDucking.sources.append(src);
            }
        }
        const QJsonObject ml = o[QStringLiteral("masterOutputLufsLimiter")].toObject();
        if (!ml.isEmpty()) {
            p.masterOutputLufsLimiter.enabled = ml[QStringLiteral("enabled")].toBool(false);
            p.masterOutputLufsLimiter.maxLufs = ml[QStringLiteral("maxLufs")].toDouble(-18.0);
        }
    }

    const QJsonObject channelEffects = o[QStringLiteral("channelEffects")].toObject();
    for (auto it = channelEffects.begin(); it != channelEffects.end(); ++it) {
        const QJsonObject ce = it.value().toObject();
        ChannelEffectsState s;

        auto loadStage = [&](const char *key, ChannelFxStageState &st) {
            const QJsonObject stage = ce[QString::fromLatin1(key)].toObject();
            if (stage.isEmpty()) return;
            fxFromJson(stage[QStringLiteral("fx")].toObject(), st.fx);
            dynFromJson(stage[QStringLiteral("dynamics")].toObject(), st.dynamics);
            creativeFromJson(stage[QStringLiteral("creativeFx")].toObject(), st.creativeFx);
            st.noiseSuppression =
                stage[QStringLiteral("noiseSuppression")].toBool(false);
            st.noiseIntensity =
                stage[QStringLiteral("noiseIntensity")].toDouble(1.0);
            st.deEsser = stage[QStringLiteral("deEsser")].toBool(false);
            st.deEsserIntensity = std::clamp(
                stage[QStringLiteral("deEsserIntensity")].toDouble(0.5), 0.0, 1.0);
        };
        loadStage("input", s.input);
        loadStage("output", s.output);

        // Older configs stored a single fx/nc block at the top level.
        if (ce.contains(QStringLiteral("fx"))) {
            fxFromJson(ce[QStringLiteral("fx")].toObject(), s.input.fx);
            s.input.noiseSuppression =
                ce[QStringLiteral("noiseSuppression")].toBool(false);
            s.input.noiseIntensity =
                ce[QStringLiteral("noiseIntensity")].toDouble(1.0);
        }

        // Clamped, not just read: the fader stops at unity now, and a profile
        // written while it went to 400% would otherwise run louder than
        // anything the window can show or undo.
        s.micGain = qBound(0.0, ce[QStringLiteral("micGain")].toDouble(1.0), 1.0);
        s.micMuted = ce[QStringLiteral("micMuted")].toBool(false);
        // Configs written before the master bypass existed have their chain on.
        s.effectsEnabled = ce[QStringLiteral("effectsEnabled")].toBool(true);
        s.monitorFx = ce[QStringLiteral("monitorFx")].toBool(true);
        s.micSource = ce[QStringLiteral("micSource")].toBool(false);
        s.micMonitor = ce[QStringLiteral("micMonitor")].toBool(false);
        s.inputUseMasterEffects =
            ce[QStringLiteral("inputUseMasterEffects")].toBool(false);
        s.outputUseMasterEffects =
            ce[QStringLiteral("outputUseMasterEffects")].toBool(false);
        s.inputEffectSourceMasterId =
            ce[QStringLiteral("inputEffectSourceMasterId")].toString();
        s.outputEffectSourceMasterId =
            ce[QStringLiteral("outputEffectSourceMasterId")].toString();
        if (s.inputEffectSourceMasterId.isEmpty() && s.inputUseMasterEffects)
            s.inputEffectSourceMasterId = QStringLiteral("mic");
        if (s.outputEffectSourceMasterId.isEmpty() && s.outputUseMasterEffects)
            s.outputEffectSourceMasterId = QStringLiteral("mic");
        s.inputUseDeviceFx = ce[QStringLiteral("inputUseDeviceFx")].toBool(false);
        // masterMicIds is the list; masterMicId is what versions before it wrote
        // and is still written out, so a profile stays readable both ways.
        s.masterMicIds.clear();
        for (const QJsonValue &v : ce[QStringLiteral("masterMicIds")].toArray()) {
            const QString id = v.toString();
            if (!id.isEmpty() && !s.masterMicIds.contains(id)) s.masterMicIds << id;
        }
        if (s.masterMicIds.isEmpty())
            s.masterMicIds
                << ce[QStringLiteral("masterMicId")].toString(QStringLiteral("mic"));
        const QJsonObject duck = ce[QStringLiteral("ducking")].toObject();
        if (!duck.isEmpty()) {
            s.ducking.enabled = duck[QStringLiteral("enabled")].toBool(false);
            s.ducking.intensity = duck[QStringLiteral("intensity")].toDouble(0.75);
            s.ducking.thresholdDb = duck[QStringLiteral("thresholdDb")].toDouble(-32.0);
            s.ducking.depthDb = duck[QStringLiteral("depthDb")].toDouble(-18.0);
            s.ducking.attackMs = duck[QStringLiteral("attackMs")].toDouble(100.0);
            s.ducking.releaseMs = duck[QStringLiteral("releaseMs")].toDouble(450.0);
            s.ducking.holdSec = duck[QStringLiteral("holdSec")].toDouble(3.0);
            s.ducking.sources.clear();
            const QJsonArray srcArr = duck[QStringLiteral("sources")].toArray();
            if (!srcArr.isEmpty()) {
                for (const QJsonValue &v : srcArr) {
                    const QJsonObject so = v.toObject();
                    DuckingSourceState src;
                    src.kind = so[QStringLiteral("kind")].toString(
                        QStringLiteral("master_mic"));
                    src.channelId = so[QStringLiteral("channel")].toString();
                    if (src.kind == QLatin1String("master_mic") && src.channelId.isEmpty())
                        src.channelId = QStringLiteral("mic");
                    s.ducking.sources.append(src);
                }
            } else if (duck.contains(QStringLiteral("sidechain"))) {
                // Migrate single sidechain to sources list.
                const QString sc =
                    duck[QStringLiteral("sidechain")].toString(QStringLiteral("master"));
                DuckingSourceState src;
                if (sc == QLatin1String("master") || sc == QLatin1String("master_mic")) {
                    src.kind = QStringLiteral("master_mic");
                    src.channelId = QStringLiteral("mic");
                } else if (sc == QLatin1String("channel")) {
                    src.kind = QStringLiteral("channel_mic");
                    src.channelId = it.key();
                } else {
                    src.kind = QStringLiteral("channel_mic");
                    src.channelId = sc;
                }
                s.ducking.sources.append(src);
            }
            if (s.ducking.sources.isEmpty()) {
                s.ducking.sources.append(
                    DuckingSourceState{QStringLiteral("master_mic"), {}});
            }
        }
        const QJsonObject lufs = ce[QStringLiteral("lufsLimiter")].toObject();
        if (!lufs.isEmpty()) {
            s.lufsLimiter.enabled = lufs[QStringLiteral("enabled")].toBool(false);
            s.lufsLimiter.maxLufs = lufs[QStringLiteral("maxLufs")].toDouble(-18.0);
        }
        p.channelEffects.insert(it.key(), s);
    }
    // Migrate older configs that stored Voice effects at the top level.
    if (p.channelEffects.isEmpty() && o.contains(QStringLiteral("voiceFx"))) {
        ChannelEffectsState voice;
        fxFromJson(o[QStringLiteral("voiceFx")].toObject(), voice.input.fx);
        voice.input.noiseSuppression =
            o[QStringLiteral("voiceNoiseSuppression")].toBool(false);
        voice.input.noiseIntensity =
            o[QStringLiteral("voiceNoiseIntensity")].toDouble(1.0);
        p.channelEffects.insert(QStringLiteral("voice"), voice);
    }

    for (const QJsonValue &v : o[QStringLiteral("rules")].toArray()) {
        const QJsonObject ro = v.toObject();
        RuleState r;
        r.pattern = ro[QStringLiteral("pattern")].toString();
        r.channel = ro[QStringLiteral("channel")].toString();
        if (!r.pattern.isEmpty() && !r.channel.isEmpty()) p.rules.append(r);
    }
    p.appChannels.clear();
    const QJsonObject appChannelsObj = o[QStringLiteral("appChannels")].toObject();
    for (auto it = appChannelsObj.begin(); it != appChannelsObj.end(); ++it) {
        if (!it.value().isString()) continue;
        const QString key = it.key();
        const std::string k = key.toStdString();
        if (!waveline::isStableIdentityKey(k)) continue;
        p.appChannels.insert(key, it.value().toString());
    }
    p.appVolumes.clear();
    const QJsonObject appVolumesObj = o[QStringLiteral("appVolumes")].toObject();
    for (auto it = appVolumesObj.begin(); it != appVolumesObj.end(); ++it) {
        const double v = it.value().toDouble(1.0);
        p.appVolumes.insert(it.key(), std::clamp(v, 0.0, 1.5));
    }

    p.sinksMutedAtStop.clear();
    for (const QJsonValue &v : o[QStringLiteral("sinksMutedAtStop")].toArray()) {
        const QString sink = v.toString();
        if (!sink.isEmpty()) p.sinksMutedAtStop << sink;
    }

    p.cardAppearance.clear();
    const QJsonObject cards = o[QStringLiteral("cardAppearance")].toObject();
    for (auto it = cards.begin(); it != cards.end(); ++it) {
        const QJsonObject card = it.value().toObject();
        CardAppearanceState a;
        a.color = card[QStringLiteral("color")].toString();
        a.icon = card[QStringLiteral("icon")].toString();
        // A card that says nothing is a card with no entry: keeps the file
        // free of rows that only record "the user opened the dialog once".
        if (a.color.isEmpty() && a.icon.isEmpty()) continue;
        p.cardAppearance.insert(it.key(), a);
    }

    p.channelNames.clear();
    const QJsonObject channelNames = o[QStringLiteral("channelNames")].toObject();
    for (auto it = channelNames.begin(); it != channelNames.end(); ++it) {
        const QString name = it.value().toString().trimmed();
        if (!name.isEmpty()) p.channelNames.insert(it.key(), name);
    }
    return p;
}

bool ConfigStore::load() {
    QFile f(path_);
    if (!f.exists()) return true;  // first run
    if (!f.open(QIODevice::ReadOnly)) {
        lastError_ = QStringLiteral("cannot read %1: %2").arg(path_, f.errorString());
        return false;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // Keep the defaults rather than refusing to start. A corrupt config
        // must not cost the user their audio routing.
        lastError_ = QStringLiteral("%1 is not valid JSON (%2); using defaults")
                         .arg(path_, err.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    if (root[QStringLiteral("version")].toInt(kVersion) > kVersion) {
        lastError_ = QStringLiteral("%1 was written by a newer version; using defaults")
                         .arg(path_);
        return false;
    }

    const QJsonObject profiles = root[QStringLiteral("profiles")].toObject();
    profiles_.clear();
    for (auto it = profiles.begin(); it != profiles.end(); ++it)
        profiles_.insert(it.key(), fromJson(it.value().toObject()));

    if (root.contains(QStringLiteral("live")))
        live_ = fromJson(root[QStringLiteral("live")].toObject());

    const QJsonObject companion = root[QStringLiteral("companion")].toObject();
    companion_.autoStart = companion[QStringLiteral("autoStart")].toBool(false);
    companion_.port = companion[QStringLiteral("port")].toInt(8787);
    if (companion_.port < 1024 || companion_.port > 65535) companion_.port = 8787;

    const QJsonObject audio = root[QStringLiteral("audio")].toObject();
    audio_.graphQuantum = audio[QStringLiteral("graphQuantum")].toInt(0);
    // Anything outside PipeWire's own bounds is not a quantum, it is a typo or
    // a config from a future version. Falling back to 0 ("leave it alone")
    // rather than clamping: a value we do not understand is not a preference we
    // should act on a nearby approximation of.
    if (audio_.graphQuantum != 0 &&
        (audio_.graphQuantum < 32 || audio_.graphQuantum > 8192))
        audio_.graphQuantum = 0;

    audio_.outputHeadroom = audio[QStringLiteral("outputHeadroom")].toInt(512);
    // Same reasoning as the quantum: an unreadable value means no rule rather
    // than a guess. The ceiling is generous because headroom is pure buffer --
    // 8192 frames is 170 ms, which is absurd for monitoring and still a thing
    // somebody may deliberately want on a machine that stutters.
    if (audio_.outputHeadroom < 0 || audio_.outputHeadroom > 8192)
        audio_.outputHeadroom = 0;

    // Defaults to true, including for every config written before this setting
    // existed: a missing key must mean "the way it has always worked", not
    // "off". toBool's default argument is doing that job.
    audio_.realtime = audio[QStringLiteral("realtime")].toBool(true);

    const QJsonObject diagnostics = root[QStringLiteral("diagnostics")].toObject();
    diagnostics_.dismissedRoutingSinks.clear();
    for (const QJsonValue &v :
         diagnostics[QStringLiteral("dismissedRoutingSinks")].toArray()) {
        const QString name = v.toString();
        if (!name.isEmpty()) diagnostics_.dismissedRoutingSinks.append(name);
    }
    diagnostics_.dspProfiling =
        diagnostics[QStringLiteral("dspProfiling")].toBool(false);

    const QJsonObject shell = root[QStringLiteral("shell")].toObject();
    shell_.noiseSuppressionInputs =
        shell[QStringLiteral("noiseSuppressionInputs")].toInt(1);
    // Negative is not a count, and a missing key must mean the default rather
    // than zero -- toInt's argument is doing that job. The ceiling is well past
    // any real input-device count; anything at or above it already means "all".
    if (shell_.noiseSuppressionInputs < 0) shell_.noiseSuppressionInputs = 1;
    if (shell_.noiseSuppressionInputs > 16) shell_.noiseSuppressionInputs = 16;

    {
        // Older configs stored "soundboard" as a bare array of sounds, with
        // no global settings; toObject() on that is empty, so the fields
        // below just take their defaults ("sfx", not shared) exactly as if
        // this were a first run.
        const QJsonObject board = root[QStringLiteral("soundboard")].toObject();
        soundboard_.channelId =
            board[QStringLiteral("channelId")].toString(QStringLiteral("sfx"));
        soundboard_.shareTarget = board[QStringLiteral("shareTarget")].toString();
        soundboard_.shareVolume = board[QStringLiteral("shareVolume")].toDouble(1.0);
        soundboard_.localVolume = board[QStringLiteral("localVolume")].toDouble(1.0);

        soundboard_.sounds.clear();
        const QJsonArray sounds = board.contains(QStringLiteral("sounds"))
                                      ? board[QStringLiteral("sounds")].toArray()
                                      // Pre-migration shape: the array itself.
                                      : root[QStringLiteral("soundboard")].toArray();
        for (const QJsonValue &v : sounds) {
            const QJsonObject s = v.toObject();
            SoundboardSoundState sound;
            sound.id = s[QStringLiteral("id")].toString();
            sound.name = s[QStringLiteral("name")].toString();
            sound.file = s[QStringLiteral("file")].toString();
            if (sound.id.isEmpty() || sound.file.isEmpty()) continue;
            sound.volume = s[QStringLiteral("volume")].toDouble(1.0);
            sound.trimStartMs = s[QStringLiteral("trimStartMs")].toInt(0);
            sound.trimEndMs = s[QStringLiteral("trimEndMs")].toInt(0);
            sound.peaks = s[QStringLiteral("peaks")].toString();
            soundboard_.sounds.append(sound);
        }
    }

    active_ = root[QStringLiteral("activeProfile")].toString();
    if (!active_.isEmpty() && !profiles_.contains(active_)) active_.clear();
    return true;
}

bool ConfigStore::save() const {
    QJsonObject root;
    root[QStringLiteral("version")] = kVersion;
    root[QStringLiteral("activeProfile")] = active_;
    root[QStringLiteral("live")] = toJson(live_);

    QJsonObject profiles;
    for (auto it = profiles_.begin(); it != profiles_.end(); ++it)
        profiles[it.key()] = toJson(it.value());
    root[QStringLiteral("profiles")] = profiles;

    QJsonObject companion;
    companion[QStringLiteral("autoStart")] = companion_.autoStart;
    companion[QStringLiteral("port")] = companion_.port;
    root[QStringLiteral("companion")] = companion;

    QJsonObject audio;
    audio[QStringLiteral("graphQuantum")] = audio_.graphQuantum;
    audio[QStringLiteral("outputHeadroom")] = audio_.outputHeadroom;
    audio[QStringLiteral("realtime")] = audio_.realtime;
    root[QStringLiteral("audio")] = audio;

    QJsonObject diagnostics;
    QJsonArray dismissed;
    for (const QString &name : diagnostics_.dismissedRoutingSinks) dismissed.append(name);
    diagnostics[QStringLiteral("dismissedRoutingSinks")] = dismissed;
    diagnostics[QStringLiteral("dspProfiling")] = diagnostics_.dspProfiling;
    root[QStringLiteral("diagnostics")] = diagnostics;

    QJsonObject shell;
    shell[QStringLiteral("noiseSuppressionInputs")] = shell_.noiseSuppressionInputs;
    root[QStringLiteral("shell")] = shell;

    QJsonArray soundboardSounds;
    for (const SoundboardSoundState &sound : soundboard_.sounds) {
        QJsonObject s;
        s[QStringLiteral("id")] = sound.id;
        s[QStringLiteral("name")] = sound.name;
        s[QStringLiteral("file")] = sound.file;
        s[QStringLiteral("volume")] = sound.volume;
        s[QStringLiteral("trimStartMs")] = sound.trimStartMs;
        s[QStringLiteral("trimEndMs")] = sound.trimEndMs;
        s[QStringLiteral("peaks")] = sound.peaks;
        soundboardSounds.append(s);
    }
    QJsonObject soundboard;
    soundboard[QStringLiteral("channelId")] = soundboard_.channelId;
    soundboard[QStringLiteral("shareTarget")] = soundboard_.shareTarget;
    soundboard[QStringLiteral("shareVolume")] = soundboard_.shareVolume;
    soundboard[QStringLiteral("localVolume")] = soundboard_.localVolume;
    soundboard[QStringLiteral("sounds")] = soundboardSounds;
    root[QStringLiteral("soundboard")] = soundboard;

    // QSaveFile writes to a temporary and renames on commit, so an interrupted
    // write leaves the previous config intact instead of a truncated one.
    QSaveFile f(path_);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        lastError_ = QStringLiteral("cannot write %1: %2").arg(path_, f.errorString());
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        lastError_ = QStringLiteral("cannot commit %1: %2").arg(path_, f.errorString());
        return false;
    }
    return true;
}

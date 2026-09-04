// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "mixerclient.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QProcess>

#include "device/deviceprofile.h"
#include "engine/creativefxspec.h"

namespace {
constexpr const char *kService = "org.waveline.Mixer";
constexpr const char *kPath = "/org/waveline/Mixer";
constexpr const char *kIface = "org.waveline.Mixer";

// The reply to ChannelEffects and MasterChannelEffects. The last two fields
// arrived after the first six and a daemon older than the mixer will not send
// them, so their absence means "the flat default", not "malformed".
ChannelFxInfo parseFxReply(const QString &reply) {
    ChannelFxInfo fx;
    const QStringList f = reply.split(QLatin1Char('\t'));
    if (f.size() < 6) return fx;
    fx.lowCut = f[0].toInt() != 0;
    fx.lowCutHz = f[1].toInt();
    fx.eq = f[2].toInt() != 0;
    fx.lowDb = f[3].toDouble();
    fx.midDb = f[4].toDouble();
    fx.highDb = f[5].toDouble();
    if (f.size() >= 8) {
        fx.eqAdvanced = f[6].toInt() != 0;
        fx.proEqBands = f[7];
    }
    return fx;
}

CreativeFxInfo fromEngineCreativeFx(const waveline::CreativeFxSettings &s) {
    CreativeFxInfo v;
    v.bitcrusher = {s.bitcrusher.enabled, s.bitcrusher.bitDepth, s.bitcrusher.sampleRateReduction,
                    s.bitcrusher.mix};
    v.overdrive = {s.overdrive.enabled, s.overdrive.drive, s.overdrive.tone,
                   s.overdrive.outputDb, s.overdrive.mix};
    v.chorus = {s.chorus.enabled, s.chorus.rateHz, s.chorus.depthMs, s.chorus.feedback,
               s.chorus.mix};
    v.flanger = {s.flanger.enabled, s.flanger.rateHz, s.flanger.depthMs, s.flanger.feedback,
                s.flanger.mix};
    v.phaser = {s.phaser.enabled, s.phaser.rateHz, s.phaser.depth, s.phaser.feedback,
               s.phaser.mix};
    v.tremolo = {s.tremolo.enabled, s.tremolo.rateHz, s.tremolo.depth, s.tremolo.shape,
                s.tremolo.mix};
    v.delay = {s.delay.enabled,   s.delay.timeMs,  s.delay.feedback,
              s.delay.mix,       s.delay.damping, s.delay.pingPong};
    v.reverb = {s.reverb.enabled, s.reverb.size, s.reverb.damping, s.reverb.predelayMs,
               s.reverb.mix};
    v.eq = {s.eq.enabled, s.eq.gain, s.eq.bass, s.eq.mid, s.eq.treble, s.eq.presence,
           s.eq.master};
    v.ringMod = {s.ringMod.enabled, s.ringMod.frequencyHz, s.ringMod.fineTuneHz, s.ringMod.mix};
    v.envFilter = {s.envFilter.enabled, s.envFilter.sensitivity, s.envFilter.attackMs,
                   s.envFilter.releaseMs, s.envFilter.resonance, s.envFilter.mix};
    v.pitch = {s.pitch.enabled, s.pitch.semitones, s.pitch.detuneCents, s.pitch.mix};
    v.reverseDelay = {s.reverseDelay.enabled, s.reverseDelay.timeMs, s.reverseDelay.feedback,
                      s.reverseDelay.smoothing, s.reverseDelay.mix};
    v.tapeSat = {s.tapeSat.enabled, s.tapeSat.drive, s.tapeSat.flutter, s.tapeSat.age,
                s.tapeSat.mix};
    for (int i = 0; i < waveline::kFxStageCount; ++i) v.order[i] = s.order[i];
    v.presentCount = s.presentCount;
    return v;
}

waveline::CreativeFxSettings toEngineCreativeFx(const CreativeFxInfo &v) {
    waveline::CreativeFxSettings s;
    s.bitcrusher = {v.bitcrusher.enabled, static_cast<float>(v.bitcrusher.bitDepth),
                    static_cast<float>(v.bitcrusher.sampleRateReduction),
                    static_cast<float>(v.bitcrusher.mix)};
    s.overdrive = {v.overdrive.enabled, static_cast<float>(v.overdrive.drive),
                   static_cast<float>(v.overdrive.tone), static_cast<float>(v.overdrive.outputDb),
                   static_cast<float>(v.overdrive.mix)};
    s.chorus = {v.chorus.enabled, static_cast<float>(v.chorus.rateHz),
               static_cast<float>(v.chorus.depthMs), static_cast<float>(v.chorus.feedback),
               static_cast<float>(v.chorus.mix)};
    s.flanger = {v.flanger.enabled, static_cast<float>(v.flanger.rateHz),
                static_cast<float>(v.flanger.depthMs), static_cast<float>(v.flanger.feedback),
                static_cast<float>(v.flanger.mix)};
    s.phaser = {v.phaser.enabled, static_cast<float>(v.phaser.rateHz),
               static_cast<float>(v.phaser.depth), static_cast<float>(v.phaser.feedback),
               static_cast<float>(v.phaser.mix)};
    s.tremolo = {v.tremolo.enabled, static_cast<float>(v.tremolo.rateHz),
                static_cast<float>(v.tremolo.depth), v.tremolo.shape,
                static_cast<float>(v.tremolo.mix)};
    s.delay = {v.delay.enabled,
              static_cast<float>(v.delay.timeMs),
              static_cast<float>(v.delay.feedback),
              static_cast<float>(v.delay.mix),
              static_cast<float>(v.delay.damping),
              v.delay.pingPong};
    s.reverb = {v.reverb.enabled, static_cast<float>(v.reverb.size),
               static_cast<float>(v.reverb.damping), static_cast<float>(v.reverb.predelayMs),
               static_cast<float>(v.reverb.mix)};
    s.eq = {v.eq.enabled,       static_cast<float>(v.eq.gain),
           static_cast<float>(v.eq.bass),     static_cast<float>(v.eq.mid),
           static_cast<float>(v.eq.treble),   static_cast<float>(v.eq.presence),
           static_cast<float>(v.eq.master)};
    s.ringMod = {v.ringMod.enabled, static_cast<float>(v.ringMod.frequencyHz),
                static_cast<float>(v.ringMod.fineTuneHz), static_cast<float>(v.ringMod.mix)};
    s.envFilter = {v.envFilter.enabled, static_cast<float>(v.envFilter.sensitivity),
                  static_cast<float>(v.envFilter.attackMs),
                  static_cast<float>(v.envFilter.releaseMs),
                  static_cast<float>(v.envFilter.resonance), static_cast<float>(v.envFilter.mix)};
    s.pitch = {v.pitch.enabled, static_cast<float>(v.pitch.semitones),
              static_cast<float>(v.pitch.detuneCents), static_cast<float>(v.pitch.mix)};
    s.reverseDelay = {v.reverseDelay.enabled, static_cast<float>(v.reverseDelay.timeMs),
                      static_cast<float>(v.reverseDelay.feedback),
                      static_cast<float>(v.reverseDelay.smoothing),
                      static_cast<float>(v.reverseDelay.mix)};
    s.tapeSat = {v.tapeSat.enabled, static_cast<float>(v.tapeSat.drive),
                static_cast<float>(v.tapeSat.flutter), static_cast<float>(v.tapeSat.age),
                static_cast<float>(v.tapeSat.mix)};
    for (int i = 0; i < waveline::kFxStageCount; ++i) s.order[i] = v.order[i];
    s.presentCount = v.presentCount;
    return s;
}
}  // namespace

QString encodeCreativeFxSpec(const CreativeFxInfo &info) {
    return QString::fromStdString(waveline::encodeCreativeFx(toEngineCreativeFx(info)));
}

CreativeFxInfo decodeCreativeFxSpec(const QString &spec) {
    return fromEngineCreativeFx(waveline::decodeCreativeFx(spec.toStdString()));
}

MixerClient::MixerClient(QObject *parent) : QObject(parent) {
    iface_ = new QDBusInterface(QLatin1String(kService), QLatin1String(kPath),
                                QLatin1String(kIface),
                                QDBusConnection::sessionBus(), this);

    // Deliberately refresh() and not SIGNAL(changed()): relaying the daemon's
    // signal straight through announced "something changed" while channels()
    // still served the cache from the last poll, up to 400 ms stale. Every
    // write the user made came back as the value from *before* it -- a fader
    // or mute button snapped back the instant it was touched, then jumped to
    // the right place when the poll finally caught up. Reads are ordered
    // behind our own writes on this connection, so refetching here always
    // observes the write that prompted the signal.
    QDBusConnection::sessionBus().connect(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
        QStringLiteral("Changed"), this, SLOT(refresh()));

    // Two poll rates, because the two kinds of state could not be more
    // different. A full refresh is ~47 blocking round trips (channels() alone
    // is 26); levels are one. Running both at the meter's rate cost ~390
    // calls/second and still only moved the meters 8 times a second.
    //
    // Everything that only changes when somebody touches something.
    poll_.setInterval(400);
    connect(&poll_, &QTimer::timeout, this, &MixerClient::pollState);

    // The meters. One call, fast enough that the widgets' interpolation has
    // something recent to aim at.
    levelPoll_.setInterval(16);
    connect(&levelPoll_, &QTimer::timeout, this, &MixerClient::pollLevels);

    // The daemon may start after the GUI, or be restarted under it.
    reconnect_.setInterval(2000);
    connect(&reconnect_, &QTimer::timeout, this, &MixerClient::probe);
    reconnect_.start();

    // Before the first probe: the window reads the profile while it is being
    // constructed, and with no daemon yet there is nowhere else to get it.
    readLocalProfile();

    probe();
}

void MixerClient::probe() {
    const bool ok = iface_ && iface_->isValid();
    if (ok != available_) {
        available_ = ok;
        // A daemon that went away may come back as a different build, or with a
        // different microphone plugged in and a different profile installed.
        profileValid_ = false;
        // Back to what the file says, rather than leaving the last daemon's
        // answer standing after it is gone.
        if (!ok) readLocalProfile();
        if (ok) { poll_.start(); levelPoll_.start(); }
        else    { poll_.stop();  levelPoll_.stop();
                  levelCache_.clear(); channelCache_.clear(); }
        emit availabilityChanged(available_);
        // Fill the cache before anyone reads it: rebuildStrips() runs off
        // changed() and would otherwise see no channels at all.
        if (ok) pollState(); else emit changed();
    }
}

void MixerClient::pollLevels() {
    levelCache_ = fetchLevels();
    emit levelsChanged();
}

void MixerClient::setPollingEnabled(bool on) {
    // Nothing to draw while the window is hidden, so stop paying for it.
    if (!available_) return;
    if (on) { poll_.start(); levelPoll_.start(); }
    else    { poll_.stop();  levelPoll_.stop(); }
}

void MixerClient::refresh() { pollState(); }

QString MixerClient::lastError() const {
    return get<QString>("LastError");
}

void MixerClient::pollState() {
    channelCache_ = fetchChannels();
    emit changed();
}

template <typename T>
T MixerClient::get(const char *method, const T &fallback) const {
    if (!iface_ || !iface_->isValid()) return fallback;
    QDBusReply<T> reply = iface_->call(QLatin1String(method));
    return reply.isValid() ? reply.value() : fallback;
}

void MixerClient::call(const char *method, const QVariantList &args) {
    if (!iface_ || !iface_->isValid()) return;
    iface_->callWithArgumentList(QDBus::NoBlock, QLatin1String(method), args);
}

QList<ChannelInfo> MixerClient::fetchChannels() const {
    QList<ChannelInfo> out;
    // "id\tname\tstreamVol\tmonitorVol\tstreamMuted\tmonitorMuted" per row.
    // One round trip: assembling this from ChannelIds/ChannelName/
    // ChannelVolume/ChannelMuted took 26, and three separate places in the UI
    // wanted it on every refresh.
    for (const QString &row : get<QStringList>("Channels")) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 6) continue;
        ChannelInfo c;
        c.id = f[0];
        c.name = f[1];
        c.streamVolume = f[2].toDouble();
        c.monitorVolume = f[3].toDouble();
        c.streamMuted = f[4].toInt() != 0;
        c.monitorMuted = f[5].toInt() != 0;
        out.push_back(c);
    }
    return out;
}

QList<OutputInfo> MixerClient::outputs() const {
    QList<OutputInfo> out;
    // Each entry is "node.name\tdescription": the API avoids custom D-Bus
    // structs so it stays usable from gdbus.
    for (const QString &row : get<QStringList>("Outputs")) {
        const int tab = row.indexOf(QLatin1Char('\t'));
        OutputInfo o;
        o.name = tab < 0 ? row : row.left(tab);
        o.description = tab < 0 ? row : row.mid(tab + 1);
        out.push_back(o);
    }
    return out;
}

QList<AppInfo> MixerClient::apps() const {
    QList<AppInfo> out;
    // "nodeId\tname\tchannel" per entry.
    for (const QString &row : get<QStringList>("Apps")) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 2) continue;
        AppInfo a;
        a.nodeId = f[0].toUInt();
        a.name = f[1];
        a.channelId = f.size() > 2 ? f[2] : QString();
        a.volume = f.size() > 3 ? f[3].toDouble() : 1.0;
        out.push_back(a);
    }
    return out;
}

// ---------------------------------------------------------------- soundboard

QList<SoundboardSoundInfo> MixerClient::soundboardSounds() const {
    QList<SoundboardSoundInfo> out;
    // "id\tname\tvolume\ttrimStartMs\ttrimEndMs\tdurationMs\tfile\tpeaks" per row.
    for (const QString &row : get<QStringList>("SoundboardSounds")) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 7) continue;
        SoundboardSoundInfo s;
        s.id = f[0];
        s.name = f[1];
        s.volume = f[2].toDouble();
        s.trimStartMs = f[3].toInt();
        s.trimEndMs = f[4].toInt();
        s.durationMs = f[5].toDouble();
        s.file = f[6];
        s.peaks = f.value(7);
        out.push_back(s);
    }
    return out;
}

QStringList MixerClient::soundboardPlayingIds() const {
    return get<QStringList>("SoundboardPlayingIds");
}

QHash<QString, double> MixerClient::soundboardProgress() const {
    QHash<QString, double> out;
    for (const QString &row : get<QStringList>("SoundboardProgress")) {
        const int tab = row.indexOf(QLatin1Char('\t'));
        if (tab < 0) continue;
        out.insert(row.left(tab), row.mid(tab + 1).toDouble());
    }
    return out;
}

SoundboardSettingsInfo MixerClient::soundboardSettings() const {
    SoundboardSettingsInfo s;
    // "channelId\tshareTarget\tshareVolume\tlocalVolume"
    const QStringList f = get<QString>("SoundboardSettings").split(QLatin1Char('\t'));
    if (f.size() < 4) return s;
    s.channelId = f[0].isEmpty() ? QStringLiteral("sfx") : f[0];
    s.shareTarget = f[1];
    s.shareVolume = f[2].toDouble();
    s.localVolume = f[3].toDouble();
    return s;
}

void MixerClient::setSoundboardChannel(const QString &channelId) {
    call("SetSoundboardChannel", {channelId});
}
void MixerClient::setSoundboardShareTarget(const QString &target) {
    call("SetSoundboardShareTarget", {target});
}
void MixerClient::setSoundboardShareVolume(double volume) {
    call("SetSoundboardShareVolume", {volume});
}
void MixerClient::setSoundboardLocalVolume(double volume) {
    call("SetSoundboardLocalVolume", {volume});
}

QString MixerClient::addSoundboardSound(const QString &name, const QString &sourcePath,
                                        int trimStartMs, int trimEndMs, double volume) {
    if (!iface_ || !iface_->isValid()) return QString();
    QDBusReply<QString> r = iface_->call(QStringLiteral("AddSoundboardSound"), name,
                                         sourcePath, trimStartMs, trimEndMs, volume);
    return r.isValid() ? r.value() : QString();
}

bool MixerClient::updateSoundboardSound(const QString &id, const QString &name,
                                        const QString &newSourcePath, int trimStartMs,
                                        int trimEndMs, double volume) {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("UpdateSoundboardSound"), id, name,
                                      newSourcePath, trimStartMs, trimEndMs, volume);
    return r.isValid() && r.value();
}

void MixerClient::removeSoundboardSound(const QString &id) {
    call("RemoveSoundboardSound", {id});
}
void MixerClient::reorderSoundboardSounds(const QStringList &idsInOrder) {
    call("ReorderSoundboardSounds", {idsInOrder});
}

void MixerClient::playSoundboardSound(const QString &id, std::function<void(QString)> onResult) {
    if (!iface_ || !iface_->isValid()) {
        if (onResult) onResult(QStringLiteral("wavelined is not running"));
        return;
    }
    // asyncCall(), not call(): see the header comment on why this one
    // specifically cannot be a blocking QDBusReply the way the rest of this
    // file's read-and-return calls are.
    auto *watcher =
        new QDBusPendingCallWatcher(iface_->asyncCall(QStringLiteral("PlaySoundboardSound"), id), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [onResult](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                const QDBusPendingReply<QString> reply = *w;
                if (onResult)
                    onResult(reply.isError() ? reply.error().message()
                                              : reply.value());
            });
}
void MixerClient::stopSoundboardSound(const QString &id) {
    call("StopSoundboardSound", {id});
}
void MixerClient::stopAllSoundboardSounds() { call("StopAllSoundboardSounds", {}); }

QString MixerClient::analyzeSoundboardSource(const QString &path) const {
    if (!iface_ || !iface_->isValid()) return QString();
    QDBusReply<QString> r = iface_->call(QStringLiteral("AnalyzeSoundboardSource"), path);
    return r.isValid() ? r.value() : QString();
}
void MixerClient::previewSoundboardTrim(const QString &path, int trimStartMs, int trimEndMs,
                                        double volume) {
    call("PreviewSoundboardTrim", {path, trimStartMs, trimEndMs, volume});
}
void MixerClient::stopSoundboardPreview() { call("StopSoundboardPreview", {}); }

QStringList MixerClient::profiles() const { return get<QStringList>("Profiles"); }
QString MixerClient::activeProfile() const { return get<QString>("ActiveProfile"); }

void MixerClient::moveApp(uint nodeId, const QString &channelId) {
    call("MoveApp", {nodeId, channelId});
}
void MixerClient::loadProfile(const QString &name) { call("LoadProfile", {name}); }
void MixerClient::saveProfile(const QString &name) { call("SaveProfile", {name}); }
void MixerClient::deleteProfile(const QString &name) { call("DeleteProfile", {name}); }

bool MixerClient::renameProfile(const QString &from, const QString &to) {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("RenameProfile"), from, to);
    return r.isValid() && r.value();
}

QString MixerClient::exportProfile(const QString &name) const {
    if (!iface_ || !iface_->isValid()) return {};
    QDBusReply<QString> r = iface_->call(QStringLiteral("ExportProfile"), name);
    return r.isValid() ? r.value() : QString();
}

bool MixerClient::profileMatchesLive(const QString &name) const {
    if (name.isEmpty() || !iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("ProfileMatchesLive"), name);
    // An older daemon does not answer this. Claiming the profile has drifted
    // would put a warning in front of every switch it could not substantiate,
    // so take the quiet answer instead.
    return !r.isValid() || r.value();
}

bool MixerClient::importProfile(const QString &name, const QString &json) {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("ImportProfile"), name, json);
    return r.isValid() && r.value();
}

void MixerClient::readLocalProfile() const {
    // The same file the daemon reads, so the two cannot disagree. Read here as
    // well because the window is built before the daemon is necessarily up,
    // and "is there a hardware panel" has to be answered then rather than
    // deferred -- an answer that arrives late means a panel that appears out
    // of nowhere, or worse, one that is briefly there and then is not.
    const waveline::DeviceProfile p = waveline::DeviceProfile::load();
    deviceBrand_ = QString::fromStdString(p.brand);
    hasHardwareControls_ = p.hardwareControls;
}

void MixerClient::fetchProfile() const {
    if (profileValid_ || !available_) return;
    // The daemon is authoritative once it is up: it is the process that
    // actually opened the device and published the nodes under these names.
    // An older one answers none of this, and its behaviour is the Wave:3-only
    // one that predates profiles -- so an empty brand from a *connected*
    // daemon means "assume the Wave:3", not "assume generic".
    const QString brand = get<QString>("DeviceBrand");
    if (brand.isEmpty()) {
        deviceBrand_ = QStringLiteral("Waveline");
        hasHardwareControls_ = false;
    } else {
        deviceBrand_ = brand;
        hasHardwareControls_ = get<bool>("HasHardwareControls");
    }
    profileValid_ = true;
}

QString MixerClient::deviceBrand() const { fetchProfile(); return deviceBrand_; }
bool MixerClient::hasHardwareControls() const {
    fetchProfile();
    return hasHardwareControls_;
}

double MixerClient::micInputVolume() const {
    return get<double>("MicInputVolume", 1.0);
}
bool MixerClient::micInputMuted() const { return get<bool>("MicInputMuted"); }
void MixerClient::setMicInputVolume(double volume) {
    call("SetMicInputVolume", {volume});
}
void MixerClient::setMicInputMuted(bool muted) {
    call("SetMicInputMuted", {muted});
}

bool MixerClient::deviceConnected() const { return get<bool>("DeviceConnected"); }
QString MixerClient::deviceFirmware() const { return get<QString>("DeviceFirmware"); }
bool MixerClient::clipguard() const { return get<bool>("Clipguard"); }
int MixerClient::hardwareMonitor() const { return get<int>("HardwareMonitor"); }
bool MixerClient::micMuted() const { return get<bool>("MicMuted"); }
double MixerClient::micGainDb() const { return get<double>("MicGainDb"); }
void MixerClient::setHeadphoneVolumeDb(double db) {
    call("SetHeadphoneVolumeDb", {db});
}
void MixerClient::setHeadphoneMuted(bool muted) {
    call("SetHeadphoneMuted", {muted});
}
bool MixerClient::headphoneMuted() const { return get<bool>("HeadphoneMuted"); }
double MixerClient::headphoneVolumeDb() const {
    return get<double>("HeadphoneVolumeDb");
}
bool MixerClient::soundSharingEnabled() const {
    return get<bool>("SoundSharingEnabled");
}
double MixerClient::soundSharingVolume(const QString &mix) const {
    if (!iface_ || !iface_->isValid()) return 1.0;
    QDBusReply<double> r = iface_->call(QStringLiteral("SoundSharingVolume"), mix);
    return r.isValid() ? r.value() : 1.0;
}
QStringList MixerClient::soundSharingApps() const {
    return get<QStringList>("SoundSharingApps");
}
QStringList MixerClient::soundSharingTargets() const {
    return get<QStringList>("SoundSharingTargets");
}
double MixerClient::soundSharingAppLevel(uint nodeId) const {
    if (!iface_ || !iface_->isValid()) return 1.0;
    QDBusReply<double> r =
        iface_->call(QStringLiteral("SoundSharingAppLevel"), nodeId);
    return r.isValid() ? r.value() : 1.0;
}
bool MixerClient::noiseSuppression() const { return get<bool>("NoiseSuppression"); }
bool MixerClient::micEffectsEnabled() const { return get<bool>("MicEffectsEnabled", true); }
bool MixerClient::micMonitorFx() const { return get<bool>("MicMonitorFx"); }
double MixerClient::noiseInputLevel() const { return get<double>("NoiseInputLevel"); }
double MixerClient::noiseOutputLevel() const {
    return get<double>("NoiseOutputLevel");
}
double MixerClient::speechProbability() const {
    return get<double>("SpeechProbability");
}
bool MixerClient::routingEnabled() const { return get<bool>("RoutingEnabled"); }
bool MixerClient::softwareMonitor() const { return get<bool>("SoftwareMonitor"); }
bool MixerClient::micStereo() const { return get<bool>("MicStereo"); }
double MixerClient::monitorLevel() const { return get<double>("MonitorLevel", 1.0); }
bool MixerClient::monitorOutputMuted() const {
    return get<bool>("MonitorOutputMuted");
}
double MixerClient::monitorOutputVolume() const {
    return get<double>("MonitorOutputVolume", 1.0);
}
double MixerClient::streamMixVolume() const {
    return get<double>("StreamMixVolume", 1.0);
}
bool MixerClient::streamMixMuted() const {
    return get<bool>("StreamMixMuted");
}
double MixerClient::noiseIntensity() const {
    return get<double>("NoiseIntensity", 1.0);
}

QString MixerClient::noiseEngine() const {
    const QString id = get<QString>("NoiseEngine");
    return id.isEmpty() ? QStringLiteral("rnnoise") : id;
}

QList<MixerClient::NoiseEngineInfo> MixerClient::noiseEngines() const {
    QList<NoiseEngineInfo> out;
    for (const QString &row : get<QStringList>("NoiseEngines")) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 3) continue;
        NoiseEngineInfo e;
        e.id = f[0];
        e.label = f[1];
        e.available = f[2] == QLatin1String("1");
        if (f.size() > 3) e.reason = f[3];
        out.append(e);
    }
    // With the daemon down there is nothing to ask, but the selector still has
    // to show something truthful rather than an empty list.
    if (out.isEmpty())
        out.append({QStringLiteral("rnnoise"), QStringLiteral("RNNoise (lighter)"),
                    true, QString()});
    return out;
}

CompanionInfo MixerClient::companion() const {
    CompanionInfo info;
    if (!iface_ || !iface_->isValid()) return info;
    QDBusReply<QStringList> r = iface_->call(QStringLiteral("CompanionStatus"));
    if (!r.isValid() || r.value().isEmpty()) return info;

    const QStringList rows = r.value();
    const QStringList head = rows.first().split(QLatin1Char('\t'));
    if (head.size() >= 3) {
        info.running = head[0].toInt() != 0;
        info.port = head[1].toInt();
        info.clients = head[2].toInt();
    }
    info.addresses = rows.mid(1);
    // Autostart is a stored setting rather than live state, so it is not in the
    // status row. One extra round trip, on a panel that refreshes when it is
    // open and nowhere else.
    info.autoStart = get<bool>("CompanionAutoStart", false);
    return info;
}

QString MixerClient::companionStart() {
    if (!iface_ || !iface_->isValid()) return tr("wavelined is not running");
    QDBusReply<QString> r = iface_->call(QStringLiteral("CompanionStart"));
    if (!r.isValid()) return r.error().message();
    return r.value();
}

void MixerClient::companionStop() { call("CompanionStop", {}); }

QString MixerClient::setCompanionPort(int port) {
    if (!iface_ || !iface_->isValid()) return tr("wavelined is not running");
    QDBusReply<QString> r = iface_->call(QStringLiteral("SetCompanionPort"), port);
    if (!r.isValid()) return r.error().message();
    return r.value();
}

void MixerClient::setCompanionAutoStart(bool on) {
    call("SetCompanionAutoStart", {on});
}

QString MixerClient::setNoiseEngine(const QString &id) {
    if (!iface_ || !iface_->isValid()) return tr("wavelined is not running");
    QDBusReply<QString> r = iface_->call(QStringLiteral("SetNoiseEngine"), id);
    if (!r.isValid()) return r.error().message();
    return r.value();
}

ChannelFxInfo MixerClient::channelEffects(const QString &channelId,
                                          const QString &stage) const {
    ChannelFxInfo fx;
    if (!iface_ || !iface_->isValid()) return fx;
    QDBusReply<QString> r =
        iface_->call(QStringLiteral("ChannelEffects"), channelId, stage);
    if (!r.isValid()) return fx;
    return parseFxReply(r.value());
}

MicDynamicsInfo MixerClient::micDynamics() const {
    MicDynamicsInfo d;
    if (!iface_ || !iface_->isValid()) return d;
    QDBusReply<QString> r = iface_->call(QStringLiteral("MicDynamics"));
    if (!r.isValid()) return d;
    const QStringList f = r.value().split(QLatin1Char('\t'));
    if (f.size() < 16) return d;
    d.gate = f[0].toInt() != 0;
    d.gateThresholdDb = f[1].toDouble();
    d.gateAttackMs = f[2].toDouble();
    d.gateReleaseMs = f[3].toDouble();
    d.compressor = f[4].toInt() != 0;
    d.compThresholdDb = f[5].toDouble();
    d.compRatio = f[6].toDouble();
    d.compAttackMs = f[7].toDouble();
    d.compReleaseMs = f[8].toDouble();
    d.compKneeDb = f[9].toDouble();
    d.makeupGainDb = f[10].toDouble();
    d.autoMakeup = f[11].toInt() != 0;
    d.limiter = f[12].toInt() != 0;
    d.limitThresholdDb = f[13].toDouble();
    d.limitAttackMs = f[14].toDouble();
    d.limitReleaseMs = f[15].toDouble();
    return d;
}

MicDynamicsInfo MixerClient::channelDynamics(const QString &channelId,
                                             const QString &stage) const {
    MicDynamicsInfo d;
    if (!iface_ || !iface_->isValid()) return d;
    QDBusReply<QString> r =
        iface_->call(QStringLiteral("ChannelDynamics"), channelId, stage);
    if (!r.isValid()) return d;
    const QStringList f = r.value().split(QLatin1Char('\t'));
    if (f.size() < 16) return d;
    d.gate = f[0].toInt() != 0;
    d.gateThresholdDb = f[1].toDouble();
    d.gateAttackMs = f[2].toDouble();
    d.gateReleaseMs = f[3].toDouble();
    d.compressor = f[4].toInt() != 0;
    d.compThresholdDb = f[5].toDouble();
    d.compRatio = f[6].toDouble();
    d.compAttackMs = f[7].toDouble();
    d.compReleaseMs = f[8].toDouble();
    d.compKneeDb = f[9].toDouble();
    d.makeupGainDb = f[10].toDouble();
    d.autoMakeup = f[11].toInt() != 0;
    d.limiter = f[12].toInt() != 0;
    d.limitThresholdDb = f[13].toDouble();
    d.limitAttackMs = f[14].toDouble();
    d.limitReleaseMs = f[15].toDouble();
    return d;
}

bool MixerClient::channelNoiseSuppression(const QString &channelId,
                                          const QString &stage) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("ChannelNoiseSuppression"), channelId, stage);
    return r.isValid() && r.value();
}

bool MixerClient::channelEffectsEnabled(const QString &channelId) const {
    // Defaults to on: a daemon too old to know the method still has a chain.
    if (!iface_ || !iface_->isValid()) return true;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("ChannelEffectsEnabled"), channelId);
    return r.isValid() ? r.value() : true;
}

bool MixerClient::channelMonitorFx(const QString &channelId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("ChannelMonitorFx"), channelId);
    return r.isValid() && r.value();
}

bool MixerClient::channelMicSource(const QString &channelId, bool *ok) const {
    if (ok) *ok = false;
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("ChannelMicSource"), channelId);
    if (!r.isValid()) return false;
    if (ok) *ok = true;
    return r.value();
}

bool MixerClient::channelMicUseDeviceFx(const QString &channelId, bool *ok) const {
    if (ok) *ok = false;
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("ChannelMicUseDeviceFx"), channelId);
    if (!r.isValid()) return false;
    if (ok) *ok = true;
    return r.value();
}

void MixerClient::setChannelMicUseDeviceFx(const QString &channelId, bool on) {
    call("SetChannelMicUseDeviceFx", {channelId, on});
}

bool MixerClient::channelMicMonitor(const QString &channelId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("ChannelMicMonitor"), channelId);
    return r.isValid() && r.value();
}

bool MixerClient::channelMicMuted(const QString &channelId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("ChannelMicMuted"), channelId);
    return r.isValid() && r.value();
}

double MixerClient::channelNoiseIntensity(const QString &channelId,
                                          const QString &stage) const {
    if (!iface_ || !iface_->isValid()) return 1.0;
    QDBusReply<double> r =
        iface_->call(QStringLiteral("ChannelNoiseIntensity"), channelId, stage);
    return r.isValid() ? r.value() : 1.0;
}

double MixerClient::channelMicSend(const QString &channelId) const {
    if (!iface_ || !iface_->isValid()) return 0.0;
    QDBusReply<double> r =
        iface_->call(QStringLiteral("ChannelMicSend"), channelId);
    return r.isValid() ? r.value() : 0.0;
}

bool MixerClient::channelEffectSourceMaster(const QString &channelId,
                                            const QString &stage) const {
    return !channelEffectSourceMasterId(channelId, stage).isEmpty();
}

QString MixerClient::channelEffectSourceMasterId(const QString &channelId,
                                                 const QString &stage) const {
    if (!iface_ || !iface_->isValid()) return {};
    QDBusReply<QString> r =
        iface_->call(QStringLiteral("ChannelEffectSourceMasterId"), channelId, stage);
    return r.isValid() ? r.value() : QString();
}

QHash<QString, double> MixerClient::fetchLevels() const {
    QHash<QString, double> out;
    // "key\tpeak" per entry, same tab-separated convention as Outputs and Apps.
    for (const QString &row : get<QStringList>("Levels")) {
        const int tab = row.indexOf(QLatin1Char('\t'));
        if (tab < 0) continue;
        out.insert(row.left(tab), row.mid(tab + 1).toDouble());
    }
    return out;
}

double MixerClient::micVolume(const QString &mix) const {
    if (!iface_ || !iface_->isValid()) return 1.0;
    QDBusReply<double> r = iface_->call(QStringLiteral("MicVolume"), mix);
    return r.isValid() ? r.value() : 1.0;
}

bool MixerClient::micMixMuted(const QString &mix) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("MicMixMuted"), mix);
    return r.isValid() && r.value();
}

void MixerClient::setChannelVolume(const QString &id, const QString &mix, double v) {
    call("SetChannelVolume", {id, mix, v});
}
void MixerClient::setChannelMuted(const QString &id, const QString &mix, bool m) {
    call("SetChannelMuted", {id, mix, m});
}
void MixerClient::setClipguard(bool on) { call("SetClipguard", {on}); }
void MixerClient::setHardwareMonitor(int percent) {
    call("SetHardwareMonitor", {percent});
}
void MixerClient::setNoiseSuppression(bool on) { call("SetNoiseSuppression", {on}); }
void MixerClient::setMicEffectsEnabled(bool on) { call("SetMicEffectsEnabled", {on}); }
void MixerClient::setMicMonitorFx(bool on) { call("SetMicMonitorFx", {on}); }
QString MixerClient::monitorOutput() const {
    const QStringList outs = monitorOutputs();
    return outs.isEmpty() ? QString() : outs.front();
}

int MixerClient::graphQuantum() const { return get<int>("GraphQuantum", 0); }

int MixerClient::effectiveGraphQuantum() const {
    return get<int>("EffectiveGraphQuantum", 0);
}

QList<StreamConflictInfo> MixerClient::streamRoutingConflicts() const {
    QList<StreamConflictInfo> out;
    if (!iface_ || !iface_->isValid()) return out;
    QDBusReply<QStringList> r =
        iface_->call(QStringLiteral("StreamRoutingConflicts"));
    if (!r.isValid()) return out;
    for (const QString &row : r.value()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 4) continue;
        StreamConflictInfo c;
        c.appName = f[0];
        c.channelId = f[1];
        c.sinkName = f[2];
        c.sinkLabel = f[3];
        out.append(c);
    }
    return out;
}

void MixerClient::dismissStreamRoutingConflict(const QString &sinkName) {
    call("DismissStreamRoutingConflict", {sinkName});
}

QStringList MixerClient::dismissedStreamRoutingConflicts() const {
    if (!iface_ || !iface_->isValid()) return {};
    QDBusReply<QStringList> r =
        iface_->call(QStringLiteral("DismissedStreamRoutingConflicts"));
    return r.isValid() ? r.value() : QStringList();
}

void MixerClient::clearStreamRoutingConflictDismissals() {
    call("ClearStreamRoutingConflictDismissals", {});
}

int MixerClient::outputHeadroom() const { return get<int>("OutputHeadroom", 0); }

int MixerClient::effectiveOutputHeadroom() const {
    return get<int>("EffectiveOutputHeadroom", 0);
}

void MixerClient::setOutputHeadroom(int frames) {
    call("SetOutputHeadroom", {frames});
}

void MixerClient::restartWirePlumber() { call("RestartWirePlumber", {}); }

bool MixerClient::realtimeScheduling() const {
    // Defaults to true for the same reason the config key does: an unreachable
    // daemon must not make the checkbox claim real-time is off.
    return get<bool>("RealtimeScheduling", true);
}

void MixerClient::setRealtimeScheduling(bool on) {
    call("SetRealtimeScheduling", {on});
}

bool MixerClient::dspProfiling() const {
    // Defaults to off, which is also the daemon's default: an unreachable
    // daemon must not tick a box claiming a measurement is running.
    return get<bool>("DspProfiling", false);
}

void MixerClient::setDspProfiling(bool on) { call("SetDspProfiling", {on}); }

int MixerClient::shellNoiseSuppressionInputs() const {
    // One, matching the daemon's default: an unreachable daemon must not make
    // the spin box claim the setting is something it is not.
    return get<int>("ShellNoiseSuppressionInputs", 1);
}

void MixerClient::setShellNoiseSuppressionInputs(int count) {
    call("SetShellNoiseSuppressionInputs", {count});
}

bool MixerClient::shellClientPresent() const {
    return get<bool>("ShellClientPresent", false);
}

bool MixerClient::restartDaemon() {
    return QProcess::startDetached(
        QStringLiteral("systemctl"),
        {QStringLiteral("--user"), QStringLiteral("restart"),
         QStringLiteral("wavelined.service")});
}

void MixerClient::setGraphQuantum(int frames) {
    call("SetGraphQuantum", {frames});
}

QStringList MixerClient::graphDiagnostics() const {
    if (!iface_ || !iface_->isValid()) return {};
    QDBusReply<QStringList> r = iface_->call(QStringLiteral("GraphDiagnostics"));
    return r.isValid() ? r.value() : QStringList{};
}

QStringList MixerClient::monitorOutputs() const {
    if (!iface_ || !iface_->isValid()) return {};
    QDBusReply<QStringList> r = iface_->call(QStringLiteral("MonitorOutputs"));
    return r.isValid() ? r.value() : QStringList{};
}

QList<MonitorOutputInfo> MixerClient::monitorOutputStates() const {
    QList<MonitorOutputInfo> out;
    if (!iface_ || !iface_->isValid()) return out;
    QDBusReply<QStringList> r = iface_->call(QStringLiteral("MonitorOutputStates"));
    if (!r.isValid()) return out;
    for (const QString &row : r.value()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.isEmpty()) continue;
        MonitorOutputInfo info;
        info.sink = f[0];
        if (f.size() > 1) info.volume = f[1].toDouble();
        if (f.size() > 2) info.muted = f[2].toInt() != 0;
        if (f.size() > 3) info.connected = f[3].toInt() != 0;
        if (f.size() > 4) info.description = f.mid(4).join(QLatin1Char('\t'));
        if (info.description.isEmpty()) info.description = info.sink;
        out.append(info);
    }
    return out;
}

void MixerClient::setMonitorOutput(const QString &sinkName) {
    setMonitorOutputAt(0, sinkName);
}

void MixerClient::setMonitorOutputAt(int index, const QString &sinkName) {
    call("SetMonitorOutputAt", {index, sinkName});
}

void MixerClient::addMonitorOutput(const QString &sinkName) {
    call("AddMonitorOutput", {sinkName});
}

void MixerClient::removeMonitorOutput(int index) {
    call("RemoveMonitorOutput", {index});
}

void MixerClient::setMonitorOutputVolumeAt(int index, double volume) {
    call("SetMonitorOutputVolumeAt", {index, volume});
}

void MixerClient::setMonitorOutputMutedAt(int index, bool muted) {
    call("SetMonitorOutputMutedAt", {index, muted});
}

void MixerClient::setRoutingEnabled(bool on) { call("SetRoutingEnabled", {on}); }
void MixerClient::setSoftwareMonitor(bool on) { call("SetSoftwareMonitor", {on}); }
void MixerClient::setMicStereo(bool on) { call("SetMicStereo", {on}); }
void MixerClient::setMonitorLevel(double v) { call("SetMonitorLevel", {v}); }
void MixerClient::setMonitorOutputMuted(bool muted) {
    call("SetMonitorOutputMuted", {muted});
}
void MixerClient::setMonitorOutputVolume(double v) {
    call("SetMonitorOutputVolume", {v});
}

void MixerClient::setStreamMixVolume(double v) {
    call("SetStreamMixVolume", {v});
}

void MixerClient::setStreamMixMuted(bool muted) {
    call("SetStreamMixMuted", {muted});
}

void MixerClient::setNoiseIntensity(double v) { call("SetNoiseIntensity", {v}); }
void MixerClient::setChannelEffects(const QString &channelId, const QString &stage,
                                    bool lowCut, int lowCutHz, bool eq, double lowDb,
                                    double midDb, double highDb) {
    call("SetChannelEffects",
         {channelId, stage, lowCut, lowCutHz, eq, lowDb, midDb, highDb});
}
void MixerClient::setChannelProEq(const QString &channelId, const QString &stage,
                                  bool advanced, const QString &bands) {
    call("SetChannelProEq", {channelId, stage, advanced, bands});
}
void MixerClient::setMicDynamics(const MicDynamicsInfo &d) {
    call("SetMicDynamics",
         {d.gate, d.gateThresholdDb, d.gateAttackMs, d.gateReleaseMs, d.compressor,
          d.compThresholdDb, d.compRatio, d.compAttackMs, d.compReleaseMs, d.compKneeDb,
          d.makeupGainDb, d.autoMakeup, d.limiter, d.limitThresholdDb, d.limitAttackMs,
          d.limitReleaseMs});
}
void MixerClient::setChannelDynamics(const QString &channelId, const QString &stage,
                                     const MicDynamicsInfo &d) {
    call("SetChannelDynamics",
         {channelId, stage, d.gate, d.gateThresholdDb, d.gateAttackMs, d.gateReleaseMs,
          d.compressor, d.compThresholdDb, d.compRatio, d.compAttackMs, d.compReleaseMs,
          d.compKneeDb, d.makeupGainDb, d.autoMakeup, d.limiter, d.limitThresholdDb,
          d.limitAttackMs, d.limitReleaseMs});
}

DuckingInfo MixerClient::channelDucking(const QString &channelId) const {
    DuckingInfo d;
    if (!iface_ || !iface_->isValid()) return d;
    QDBusReply<QString> r = iface_->call(QStringLiteral("ChannelDucking"), channelId);
    if (!r.isValid()) return d;
    const QStringList f = r.value().split(QLatin1Char('\t'));
    if (f.size() < 7) return d;
    d.enabled = f[0].toInt() != 0;
    d.intensity = f[1].toDouble();
    // Field 7 arrived with the hold control; a daemon without it keeps the default.
    if (f.size() >= 8) d.holdSec = qBound(0.0, f[7].toDouble(), 10.0);
    const QStringList parts = f[6].split(QLatin1Char('|'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (d.sources.size() >= 6) break;
        DuckingSourceInfo src;
        const int colon = part.indexOf(QLatin1Char(':'));
        if (colon < 0) {
            if (part == QLatin1String("master") || part == QLatin1String("master_mic")) {
                src.kind = QStringLiteral("master_mic");
                src.channelId = QStringLiteral("mic");
            } else {
                src.kind = QStringLiteral("channel_mic");
                src.channelId = part;
            }
        } else {
            src.kind = part.left(colon);
            src.channelId = part.mid(colon + 1);
            if (src.kind == QLatin1String("master"))
                src.kind = QStringLiteral("master_mic");
        }
        if (src.kind == QLatin1String("master_mic") && src.channelId.isEmpty())
            src.channelId = QStringLiteral("mic");
        d.sources.append(src);
    }
    if (d.sources.isEmpty())
        d.sources.append(
            DuckingSourceInfo{QStringLiteral("master_mic"), QStringLiteral("mic")});
    return d;
}

void MixerClient::setChannelDucking(const QString &channelId, bool enabled, double intensity,
                                    const QString &sources, double holdSec) {
    call("SetChannelDucking", {channelId, enabled, intensity, sources, holdSec});
}

LufsLimiterInfo MixerClient::channelLufsLimiter(const QString &channelId) const {
    LufsLimiterInfo info;
    if (!iface_ || !iface_->isValid()) return info;
    QDBusReply<QString> r = iface_->call(QStringLiteral("ChannelLufsLimiter"), channelId);
    if (!r.isValid()) return info;
    const QStringList f = r.value().split(QLatin1Char('\t'));
    if (f.size() < 2) return info;
    info.enabled = f[0].toInt() != 0;
    info.maxLufs = f[1].toDouble();
    return info;
}

void MixerClient::setChannelLufsLimiter(const QString &channelId, bool enabled,
                                        double maxLufs) {
    call("SetChannelLufsLimiter", {channelId, enabled, maxLufs});
}

CreativeFxInfo MixerClient::channelCreativeFx(const QString &channelId,
                                              const QString &stage) const {
    CreativeFxInfo info;
    if (!iface_ || !iface_->isValid()) return info;
    QDBusReply<QString> r =
        iface_->call(QStringLiteral("ChannelCreativeFx"), channelId, stage);
    if (!r.isValid()) return info;
    return fromEngineCreativeFx(waveline::decodeCreativeFx(r.value().toStdString()));
}

void MixerClient::setChannelCreativeFx(const QString &channelId, const QString &stage,
                                       const CreativeFxInfo &info) {
    const QString spec =
        QString::fromStdString(waveline::encodeCreativeFx(toEngineCreativeFx(info)));
    call("SetChannelCreativeFx", {channelId, stage, spec});
}

void MixerClient::setChannelNoiseSuppression(const QString &channelId,
                                             const QString &stage, bool on) {
    call("SetChannelNoiseSuppression", {channelId, stage, on});
}
void MixerClient::setChannelNoiseIntensity(const QString &channelId,
                                           const QString &stage, double v) {
    call("SetChannelNoiseIntensity", {channelId, stage, v});
}
void MixerClient::setChannelEffectsEnabled(const QString &channelId, bool on) {
    call("SetChannelEffectsEnabled", {channelId, on});
}
void MixerClient::setChannelMonitorFx(const QString &channelId, bool on) {
    call("SetChannelMonitorFx", {channelId, on});
}
void MixerClient::setChannelMicSource(const QString &channelId, bool on) {
    call("SetChannelMicSource", {channelId, on});
}
void MixerClient::setChannelMicMonitor(const QString &channelId, bool on) {
    call("SetChannelMicMonitor", {channelId, on});
}
void MixerClient::setChannelMicMuted(const QString &channelId, bool muted) {
    call("SetChannelMicMuted", {channelId, muted});
}
void MixerClient::setSoundSharingAppTarget(uint nodeId, const QString &target) {
    call("SetSoundSharingAppTarget", {nodeId, target});
}
void MixerClient::setSoundSharingAppLevel(uint nodeId, double level) {
    call("SetSoundSharingAppLevel", {nodeId, level});
}
void MixerClient::setAppVolume(uint nodeId, double volume) {
    call("SetAppVolume", {nodeId, volume});
}
void MixerClient::setChannelMicSend(const QString &channelId, double level) {
    call("SetChannelMicSend", {channelId, level});
}
void MixerClient::setChannelEffectSourceMaster(const QString &channelId,
                                               const QString &stage,
                                               const QString &masterId) {
    call("SetChannelEffectSourceMaster", {channelId, stage, masterId});
}
void MixerClient::setMicVolume(const QString &mix, double v) {
    call("SetMicVolume", {mix, v});
}
void MixerClient::setMicMuted(const QString &mix, bool m) {
    call("SetMicMuted", {mix, m});
}
void MixerClient::setHardwareMicMute(bool muted) {
    call("SetHardwareMicMute", {muted});
}
void MixerClient::setMicGainDb(double db) { call("SetMicGainDb", {db}); }
void MixerClient::setSoundSharingEnabled(bool on) {
    call("SetSoundSharingEnabled", {on});
}
void MixerClient::setSoundSharingVolume(const QString &mix, double v) {
    call("SetSoundSharingVolume", {mix, v});
}
void MixerClient::setSoundSharingApp(uint nodeId, bool shared) {
    call("SetSoundSharingApp", {nodeId, shared});
}

QList<MasterBusInfo> MixerClient::masterBuses() const {
    QList<MasterBusInfo> out;
    for (const QString &row : get<QStringList>("MasterBuses")) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 6) continue;
        MasterBusInfo b;
        b.id = f[0];
        b.name = f[1];
        b.captureMatch = f[2];
        b.busType = f.size() >= 4 ? f[3] : QStringLiteral("capture");
        b.primary = f[4].toInt() != 0;
        b.hwConnected = f[5].toInt() != 0;
        // Added after the first six fields; an older daemon simply omits them,
        // which reads as "no label, and nothing known to be missing".
        if (f.size() >= 7) b.deviceLabel = f[6];
        if (f.size() >= 8) b.deviceConnected = f[7].toInt() != 0;
        if (f.size() >= 9) b.latencyUs = f[8].toLongLong();
        out.push_back(b);
    }
    if (out.isEmpty()) {
        MasterBusInfo primary;
        primary.id = QStringLiteral("mic");
        primary.name = deviceBrand();
        primary.primary = true;
        out.push_back(primary);
    }
    return out;
}

QList<CaptureDeviceInfo> MixerClient::captureDevices() const {
    QList<CaptureDeviceInfo> out;
    for (const QString &row : get<QStringList>("CaptureDevices")) {
        const int tab = row.indexOf(QLatin1Char('\t'));
        CaptureDeviceInfo d;
        d.nodeName = tab < 0 ? row : row.left(tab);
        d.description = tab < 0 ? row : row.mid(tab + 1);
        out.push_back(d);
    }
    return out;
}

QList<MidiDeviceInfo> MixerClient::midiDevices() const {
    QList<MidiDeviceInfo> out;
    for (const QString &row : get<QStringList>("MidiDevices")) {
        const int tab = row.indexOf(QLatin1Char('\t'));
        MidiDeviceInfo d;
        d.nodeName = tab < 0 ? row : row.left(tab);
        d.description = tab < 0 ? row : row.mid(tab + 1);
        out.push_back(d);
    }
    return out;
}

void MixerClient::playTunerReference(double hz, int ms) {
    call("PlayTunerReference", {hz, ms});
}

QList<TunerSourceInfo> MixerClient::tunerSources() const {
    QList<TunerSourceInfo> out;
    for (const QString &row : get<QStringList>("TunerSources")) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 3) continue;
        out.push_back(TunerSourceInfo{f[0], f[1], f[2]});
    }
    return out;
}

QString MixerClient::startTuner(const QString &kind, const QString &source) {
    if (!iface_ || !iface_->isValid()) return tr("wavelined is not running");
    QDBusReply<QString> r = iface_->call(QStringLiteral("TunerStart"), kind, source);
    if (!r.isValid()) return r.error().message();
    return r.value();
}

void MixerClient::stopTuner() { call("TunerStop", {}); }

TunerReadingInfo MixerClient::tunerReading() const {
    TunerReadingInfo out;
    const QStringList f = get<QString>("TunerReading").split(QLatin1Char('\t'));
    if (f.size() < 5) return out;
    out.frequencyHz = f[0].toDouble();
    out.confidence = f[1].toDouble();
    out.level = f[2].toDouble();
    out.midiNote = f[3].toInt();
    out.bendCents = f[4].toDouble();
    return out;
}

QString MixerClient::addMasterBus(const QString &name) {
    return addMasterBusEx(name, QStringLiteral("capture"), {});
}

QString MixerClient::addMasterBusEx(const QString &name, const QString &busType,
                                    const QString &deviceMatch) {
    if (!iface_ || !iface_->isValid()) return QString();
    QDBusReply<QString> r =
        iface_->call(QStringLiteral("AddMasterBusEx"), name, busType, deviceMatch);
    if (r.isValid()) return r.value();
    return QString();
}

bool MixerClient::removeMasterBus(const QString &id) {
    if (!iface_ || !iface_->isValid()) return false;
    const QDBusMessage reply = iface_->callWithArgumentList(
        QDBus::Block, QLatin1String("RemoveMasterBus"), {id});
    if (reply.type() == QDBusMessage::ErrorMessage) return false;
    pollState();
    for (const MasterBusInfo &b : masterBuses()) {
        if (b.id == id) return false;
    }
    return true;
}

bool MixerClient::rebuildMasterCapture(const QString &id) {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> reply = iface_->call(QStringLiteral("RebuildMasterCapture"), id);
    if (!reply.isValid()) return false;
    pollState();
    return reply.value();
}

void MixerClient::setMasterCaptureDevice(const QString &id, const QString &nodeMatch) {
    call("SetMasterCaptureDevice", {id, nodeMatch});
}

void MixerClient::setMasterMidiPort(const QString &id, const QString &nodeMatch) {
    call("SetMasterMidiPort", {id, nodeMatch});
}

QStringList MixerClient::masterSoundfonts(const QString &id) const {
    if (!iface_ || !iface_->isValid()) return {};
    QDBusReply<QStringList> reply = iface_->call(QStringLiteral("MasterSoundfonts"), id);
    return reply.isValid() ? reply.value() : QStringList{};
}

void MixerClient::addMasterSoundfont(const QString &id, const QString &path) {
    call("AddMasterSoundfont", {id, path});
}

void MixerClient::removeMasterSoundfont(const QString &id, const QString &path) {
    call("RemoveMasterSoundfont", {id, path});
}

void MixerClient::setMasterSoundfont(const QString &id, const QString &path) {
    call("SetMasterSoundfont", {id, path});
}

void MixerClient::setMasterName(const QString &id, const QString &name) {
    call("SetMasterName", {id, name});
}

void MixerClient::setChannelName(const QString &channelId, const QString &name) {
    call("SetChannelName", {channelId, name});
}

QHash<QString, MixerClient::CardAppearance> MixerClient::cardAppearances() const {
    QHash<QString, CardAppearance> out;
    for (const QString &row : get<QStringList>("CardAppearances")) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 3) continue;
        CardAppearance a;
        a.color = f[1];
        a.icon = f[2];
        out.insert(f[0], a);
    }
    return out;
}

void MixerClient::setCardAppearance(const QString &key, const QString &color,
                                    const QString &icon) {
    call("SetCardAppearance", {key, color, icon});
}

void MixerClient::setChannelMasterMic(const QString &channelId,
                                      const QString &masterId) {
    call("SetChannelMasterMic", {channelId, masterId});
}

QString MixerClient::channelMasterMic(const QString &channelId) const {
    if (!iface_ || !iface_->isValid()) return QStringLiteral("mic");
    QDBusReply<QString> r =
        iface_->call(QStringLiteral("ChannelMasterMic"), channelId);
    return r.isValid() ? r.value() : QStringLiteral("mic");
}

void MixerClient::setChannelMasterMics(const QString &channelId,
                                       const QStringList &masterIds) {
    call("SetChannelMasterMics", {channelId, masterIds});
}

QStringList MixerClient::channelMasterMics(const QString &channelId) const {
    if (!iface_ || !iface_->isValid()) return {QStringLiteral("mic")};
    QDBusReply<QStringList> r =
        iface_->call(QStringLiteral("ChannelMasterMics"), channelId);
    // An unanswered call must not read as "one device": the caller uses this to
    // rebuild the picker, and a wrong answer would drop rows the user set.
    if (!r.isValid() || r.value().isEmpty()) return {};
    return r.value();
}

bool MixerClient::hasHardwareControlsFor(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("HasHardwareControlsFor"), masterId);
    return r.isValid() && r.value();
}

bool MixerClient::masterDeviceConnected(const QString &masterId) const {
    if (!iface_ || !iface_->isValid())
        return masterId == QLatin1String("mic") && get<bool>("DeviceConnected");
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("MasterDeviceConnected"), masterId);
    return r.isValid() ? r.value() : false;
}

QString MixerClient::masterDeviceFirmware(const QString &masterId) const {
    if (!iface_ || !iface_->isValid())
        return masterId == QLatin1String("mic") ? get<QString>("DeviceFirmware")
                                                : QString();
    QDBusReply<QString> r =
        iface_->call(QStringLiteral("MasterDeviceFirmware"), masterId);
    return r.isValid() ? r.value() : QString();
}

double MixerClient::masterMicVolume(const QString &masterId,
                                    const QString &mix) const {
    if (!iface_ || !iface_->isValid()) return 1.0;
    QDBusReply<double> r =
        iface_->call(QStringLiteral("MasterMicVolume"), masterId, mix);
    return r.isValid() ? r.value() : 1.0;
}

bool MixerClient::masterMicMixMuted(const QString &masterId,
                                    const QString &mix) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("MasterMicMixMuted"), masterId, mix);
    return r.isValid() && r.value();
}

bool MixerClient::masterMicEffectsEnabled(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return true;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("MasterMicEffectsEnabled"), masterId);
    return r.isValid() ? r.value() : true;
}

bool MixerClient::masterMicMonitorFx(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("MasterMicMonitorFx"), masterId);
    return r.isValid() && r.value();
}

bool MixerClient::masterMicStereo(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return true;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("MasterMicStereo"), masterId);
    return r.isValid() ? r.value() : true;
}

double MixerClient::masterMicInputVolume(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return 1.0;
    QDBusReply<double> r =
        iface_->call(QStringLiteral("MasterMicInputVolume"), masterId);
    return r.isValid() ? r.value() : 1.0;
}

bool MixerClient::masterMicInputMuted(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("MasterMicInputMuted"), masterId);
    return r.isValid() && r.value();
}

bool MixerClient::masterNoiseSuppression(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("MasterNoiseSuppression"), masterId);
    return r.isValid() && r.value();
}

double MixerClient::masterNoiseIntensity(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return 1.0;
    QDBusReply<double> r =
        iface_->call(QStringLiteral("MasterNoiseIntensity"), masterId);
    return r.isValid() ? r.value() : 1.0;
}

bool MixerClient::masterDeEsser(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("MasterDeEsser"), masterId);
    return r.isValid() && r.value();
}

void MixerClient::setMasterDeEsser(const QString &masterId, bool on) {
    call("SetMasterDeEsser", {masterId, on});
}

double MixerClient::masterDeEsserIntensity(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return 0.5;
    QDBusReply<double> r =
        iface_->call(QStringLiteral("MasterDeEsserIntensity"), masterId);
    return r.isValid() ? r.value() : 0.5;
}

void MixerClient::setMasterDeEsserIntensity(const QString &masterId, double value) {
    call("SetMasterDeEsserIntensity", {masterId, value});
}

bool MixerClient::channelDeEsser(const QString &channelId,
                                 const QString &stage) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("ChannelDeEsser"), channelId, stage);
    return r.isValid() && r.value();
}

void MixerClient::setChannelDeEsser(const QString &channelId, const QString &stage,
                                    bool on) {
    call("SetChannelDeEsser", {channelId, stage, on});
}

double MixerClient::channelDeEsserIntensity(const QString &channelId,
                                            const QString &stage) const {
    if (!iface_ || !iface_->isValid()) return 0.5;
    QDBusReply<double> r =
        iface_->call(QStringLiteral("ChannelDeEsserIntensity"), channelId, stage);
    return r.isValid() ? r.value() : 0.5;
}

void MixerClient::setChannelDeEsserIntensity(const QString &channelId,
                                             const QString &stage, double value) {
    call("SetChannelDeEsserIntensity", {channelId, stage, value});
}

bool MixerClient::masterSoftwareMonitor(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("MasterSoftwareMonitor"), masterId);
    return r.isValid() && r.value();
}

ChannelFxInfo MixerClient::masterChannelEffects(const QString &masterId,
                                                const QString &stage) const {
    ChannelFxInfo fx;
    if (!iface_ || !iface_->isValid()) return fx;
    QDBusReply<QString> r =
        iface_->call(QStringLiteral("MasterChannelEffects"), masterId, stage);
    if (!r.isValid()) return fx;
    return parseFxReply(r.value());
}

MicDynamicsInfo MixerClient::masterMicDynamics(const QString &masterId) const {
    MicDynamicsInfo d;
    if (!iface_ || !iface_->isValid()) return d;
    QDBusReply<QString> r =
        iface_->call(QStringLiteral("MasterMicDynamics"), masterId);
    if (!r.isValid()) return d;
    const QStringList f = r.value().split(QLatin1Char('\t'));
    if (f.size() < 16) return d;
    d.gate = f[0].toInt() != 0;
    d.gateThresholdDb = f[1].toDouble();
    d.gateAttackMs = f[2].toDouble();
    d.gateReleaseMs = f[3].toDouble();
    d.compressor = f[4].toInt() != 0;
    d.compThresholdDb = f[5].toDouble();
    d.compRatio = f[6].toDouble();
    d.compAttackMs = f[7].toDouble();
    d.compReleaseMs = f[8].toDouble();
    d.compKneeDb = f[9].toDouble();
    d.makeupGainDb = f[10].toDouble();
    d.autoMakeup = f[11].toInt() != 0;
    d.limiter = f[12].toInt() != 0;
    d.limitThresholdDb = f[13].toDouble();
    d.limitAttackMs = f[14].toDouble();
    d.limitReleaseMs = f[15].toDouble();
    return d;
}

bool MixerClient::masterClipguard(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("MasterClipguard"), masterId);
    return r.isValid() && r.value();
}

int MixerClient::masterHardwareMonitor(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return 0;
    QDBusReply<int> r =
        iface_->call(QStringLiteral("MasterHardwareMonitor"), masterId);
    return r.isValid() ? r.value() : 0;
}

bool MixerClient::masterMicMuted(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("MasterMicMuted"), masterId);
    return r.isValid() && r.value();
}

double MixerClient::masterMicGainDb(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return 0.0;
    QDBusReply<double> r =
        iface_->call(QStringLiteral("MasterMicGainDb"), masterId);
    return r.isValid() ? r.value() : 0.0;
}

double MixerClient::masterHeadphoneVolumeDb(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return 0.0;
    QDBusReply<double> r =
        iface_->call(QStringLiteral("MasterHeadphoneVolumeDb"), masterId);
    return r.isValid() ? r.value() : 0.0;
}

bool MixerClient::masterHeadphoneMuted(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r =
        iface_->call(QStringLiteral("MasterHeadphoneMuted"), masterId);
    return r.isValid() && r.value();
}

void MixerClient::setMasterMicVolume(const QString &masterId, const QString &mix,
                                     double v) {
    call("SetMasterMicVolume", {masterId, mix, v});
}

void MixerClient::setMasterMicMixMuted(const QString &masterId, const QString &mix,
                                      bool m) {
    call("SetMasterMicMixMuted", {masterId, mix, m});
}

void MixerClient::setMasterMicEffectsEnabled(const QString &masterId, bool on) {
    call("SetMasterMicEffectsEnabled", {masterId, on});
}

void MixerClient::setMasterMicMonitorFx(const QString &masterId, bool on) {
    call("SetMasterMicMonitorFx", {masterId, on});
}

void MixerClient::setMasterMicStereo(const QString &masterId, bool on) {
    call("SetMasterMicStereo", {masterId, on});
}

void MixerClient::setMasterMicInputVolume(const QString &masterId, double volume) {
    call("SetMasterMicInputVolume", {masterId, volume});
}

void MixerClient::setMasterMicInputMuted(const QString &masterId, bool muted) {
    call("SetMasterMicInputMuted", {masterId, muted});
}

void MixerClient::setMasterNoiseSuppression(const QString &masterId, bool on) {
    call("SetMasterNoiseSuppression", {masterId, on});
}

void MixerClient::setMasterNoiseIntensity(const QString &masterId, double v) {
    call("SetMasterNoiseIntensity", {masterId, v});
}

void MixerClient::setMasterSoftwareMonitor(const QString &masterId, bool on) {
    call("SetMasterSoftwareMonitor", {masterId, on});
}

void MixerClient::setMasterChannelEffects(const QString &masterId,
                                          const QString &stage, bool lowCut,
                                          int lowCutHz, bool eq, double lowDb,
                                          double midDb, double highDb) {
    call("SetMasterChannelEffects",
         {masterId, stage, lowCut, lowCutHz, eq, lowDb, midDb, highDb});
}

void MixerClient::setMasterProEq(const QString &masterId, const QString &stage,
                                 bool advanced, const QString &bands) {
    call("SetMasterProEq", {masterId, stage, advanced, bands});
}

void MixerClient::setMasterMicDynamics(const QString &masterId,
                                       const MicDynamicsInfo &d) {
    call("SetMasterMicDynamics",
         {masterId, d.gate, d.gateThresholdDb, d.gateAttackMs, d.gateReleaseMs,
          d.compressor, d.compThresholdDb, d.compRatio, d.compAttackMs,
          d.compReleaseMs, d.compKneeDb, d.makeupGainDb, d.autoMakeup, d.limiter,
          d.limitThresholdDb, d.limitAttackMs, d.limitReleaseMs});
}

CreativeFxInfo MixerClient::masterCreativeFx(const QString &masterId) const {
    CreativeFxInfo info;
    if (!iface_ || !iface_->isValid()) return info;
    QDBusReply<QString> r = iface_->call(QStringLiteral("MasterCreativeFx"), masterId);
    if (!r.isValid()) return info;
    return fromEngineCreativeFx(waveline::decodeCreativeFx(r.value().toStdString()));
}

void MixerClient::setMasterCreativeFx(const QString &masterId, const CreativeFxInfo &info) {
    const QString spec =
        QString::fromStdString(waveline::encodeCreativeFx(toEngineCreativeFx(info)));
    call("SetMasterCreativeFx", {masterId, spec});
}

CreativeFxInfo MixerClient::masterRackCreativeFx(const QString &masterId) const {
    CreativeFxInfo info;
    if (!iface_ || !iface_->isValid()) return info;
    QDBusReply<QString> r = iface_->call(QStringLiteral("MasterRackCreativeFx"), masterId);
    if (!r.isValid()) return info;
    return fromEngineCreativeFx(waveline::decodeCreativeFx(r.value().toStdString()));
}

void MixerClient::setMasterRackCreativeFx(const QString &masterId, const CreativeFxInfo &info) {
    const QString spec =
        QString::fromStdString(waveline::encodeCreativeFx(toEngineCreativeFx(info)));
    call("SetMasterRackCreativeFx", {masterId, spec});
}

bool MixerClient::masterRackMode(const QString &masterId) const {
    if (!iface_ || !iface_->isValid()) return false;
    QDBusReply<bool> r = iface_->call(QStringLiteral("MasterRackMode"), masterId);
    return r.isValid() && r.value();
}

void MixerClient::setMasterRackMode(const QString &masterId, bool on) {
    call("SetMasterRackMode", {masterId, on});
}

void MixerClient::setMasterClipguard(const QString &masterId, bool on) {
    call("SetMasterClipguard", {masterId, on});
}

void MixerClient::setMasterHardwareMonitor(const QString &masterId, int percent) {
    call("SetMasterHardwareMonitor", {masterId, percent});
}

void MixerClient::setMasterHardwareMicMute(const QString &masterId, bool muted) {
    call("SetMasterHardwareMicMute", {masterId, muted});
}

void MixerClient::setMasterMicGainDb(const QString &masterId, double db) {
    call("SetMasterMicGainDb", {masterId, db});
}

void MixerClient::setMasterHeadphoneVolumeDb(const QString &masterId, double db) {
    call("SetMasterHeadphoneVolumeDb", {masterId, db});
}

void MixerClient::setMasterHeadphoneMuted(const QString &masterId, bool muted) {
    call("SetMasterHeadphoneMuted", {masterId, muted});
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Client side of org.waveline.Mixer.
//
// The GUI holds no audio state of its own and touches neither USB nor
// PipeWire: everything goes through the daemon, so closing the window leaves
// the routing running. This class exists to keep QDBus boilerplate out of the
// widgets.

#pragma once

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <array>
#include <functional>

class QDBusInterface;

struct ChannelInfo {
    QString id;
    QString name;
    double streamVolume = 1.0;
    double monitorVolume = 1.0;
    bool streamMuted = false;
    bool monitorMuted = false;
};

struct OutputInfo {
    QString name;
    QString description;
};

struct MonitorOutputInfo {
    QString sink;
    QString description;
    double volume = 1.0;
    bool muted = false;
    bool connected = true;
};

struct AppInfo {
    uint nodeId = 0;
    QString name;
    QString channelId;   // empty when we have not routed it
    double volume = 1.0; // playback level, 0..1.5 (default 1.0)
};

// One Soundboard sound. Mirrors MixerService::SoundboardSounds()'s row
// field for field. Where it plays is not part of this -- see
// SoundboardSettingsInfo, one board-wide setting rather than per sound.
struct SoundboardSoundInfo {
    QString id;
    QString name;
    double volume = 1.0;        // 0..2, the clip's own baked-in level
    int trimStartMs = 0;
    int trimEndMs = 0;          // 0 = plays to the end
    double durationMs = -1.0;   // -1 while the daemon has not decoded it yet
    QString file;               // the daemon's own stored copy
    // Comma-separated peak magnitudes (0..1) for a mini waveform, empty
    // while the sound has not decoded yet.
    QString peaks;
};

// Where every Soundboard sound plays and shares. One set of settings for the
// whole board -- mirrors MixerService::SoundboardSettings().
struct SoundboardSettingsInfo {
    QString channelId = QStringLiteral("sfx");
    QString shareTarget;        // "" = not shared, else a channel id or "mic"
    double shareVolume = 1.0;   // 0..1
    double localVolume = 1.0;   // 0..1.5
};

// A stream another program keeps taking off its channel. sinkName is the
// node.name of where it ends up and is the key everything else uses: it is
// what the daemon reports, what "don't warn me again" silences, and -- for
// every tool that does this -- what identifies the program responsible.
struct StreamConflictInfo {
    QString appName;
    QString channelId;
    QString sinkName;
    QString sinkLabel;
};

struct ChannelFxInfo {
    bool lowCut = false;
    int lowCutHz = 80;
    bool eq = false;
    double lowDb = 0.0;
    double midDb = 0.0;
    double highDb = 0.0;
    // The parametric EQ. Carried as the wire spec rather than unpacked: only
    // the Pro EQ panel decodes it, and everything else here -- the refresh
    // signatures above all -- only ever needs to know whether it changed.
    bool eqAdvanced = false;
    QString proEqBands;
};

struct MicDynamicsInfo {
    bool gate = false;
    double gateThresholdDb = -50.0;
    double gateAttackMs = 3.0;
    double gateReleaseMs = 300.0;
    bool compressor = false;
    double compThresholdDb = -24.0;
    double compRatio = 4.0;
    double compAttackMs = 5.0;
    double compReleaseMs = 150.0;
    double compKneeDb = 6.0;
    double makeupGainDb = 0.0;
    bool autoMakeup = true;
    bool limiter = true;
    double limitThresholdDb = -3.0;
    double limitAttackMs = 1.0;
    double limitReleaseMs = 50.0;
};

struct DuckingSourceInfo {
    QString kind = QStringLiteral("master_mic");
    QString channelId;
};

struct DuckingInfo {
    bool enabled = false;
    double intensity = 0.75;
    double holdSec = 3.0;
    QList<DuckingSourceInfo> sources;
};

struct LufsLimiterInfo {
    bool enabled = false;
    double maxLufs = -18.0;
};

// The Creative FX pedalboard, mirroring engine/creativefx.h's
// CreativeFxSettings field for field (see mixerclient.cpp for the
// conversion, which travels as the wire format from
// engine/creativefxspec.h -- the same convention SetChannelProEq uses for
// its band spec).
struct BitcrusherInfo {
    bool enabled = false;
    double bitDepth = 12.0;
    double sampleRateReduction = 0.0;
    double mix = 1.0;
};

struct OverdriveInfo {
    bool enabled = false;
    double drive = 0.5;
    double tone = 0.5;
    double outputDb = 0.0;
    double mix = 1.0;
};

struct ChorusInfo {
    bool enabled = false;
    double rateHz = 0.8;
    double depthMs = 4.0;
    double feedback = 0.0;
    double mix = 0.5;
};

struct FlangerInfo {
    bool enabled = false;
    double rateHz = 0.25;
    double depthMs = 2.0;
    double feedback = 0.5;
    double mix = 0.5;
};

struct PhaserInfo {
    bool enabled = false;
    double rateHz = 0.5;
    double depth = 0.8;
    double feedback = 0.3;
    double mix = 0.5;
};

struct TremoloInfo {
    bool enabled = false;
    double rateHz = 5.0;
    double depth = 0.5;
    int shape = 0;
    double mix = 1.0;
};

struct DelayFxInfo {
    bool enabled = false;
    double timeMs = 250.0;
    double feedback = 0.35;
    double mix = 0.4;
    double damping = 0.0;
    bool pingPong = false;
};

struct ReverbInfo {
    bool enabled = false;
    double size = 0.5;
    double damping = 0.0;
    double predelayMs = 0.0;
    double mix = 0.35;
};

// An amp-style tone stack, not a parametric EQ -- see engine/creativefx.h's
// CreativeEqSettings, which this mirrors field for field.
struct CreativeEqInfo {
    bool enabled = false;
    double gain = 0.0;
    double bass = 0.0;
    double mid = 0.0;
    double treble = 0.0;
    double presence = 0.0;
    double master = 0.0;
};

struct RingModulatorInfo {
    bool enabled = false;
    double frequencyHz = 200.0;
    double fineTuneHz = 0.0;
    double mix = 0.5;
};

struct EnvelopeFilterInfo {
    bool enabled = false;
    double sensitivity = 0.5;
    double attackMs = 5.0;
    double releaseMs = 120.0;
    double resonance = 0.3;
    double mix = 1.0;
};

struct PitchShifterInfo {
    bool enabled = false;
    double semitones = 0.0;
    double detuneCents = 0.0;
    double mix = 0.5;
};

struct ReverseDelayInfo {
    bool enabled = false;
    double timeMs = 300.0;
    double feedback = 0.3;
    double smoothing = 0.5;
    double mix = 0.4;
};

struct TapeSaturatorInfo {
    bool enabled = false;
    double drive = 0.3;
    double flutter = 0.2;
    double age = 0.3;
    double mix = 1.0;
};

struct CreativeFxInfo {
    BitcrusherInfo bitcrusher;
    OverdriveInfo overdrive;
    ChorusInfo chorus;
    FlangerInfo flanger;
    PhaserInfo phaser;
    TremoloInfo tremolo;
    DelayFxInfo delay;
    ReverbInfo reverb;
    CreativeEqInfo eq;
    RingModulatorInfo ringMod;
    EnvelopeFilterInfo envFilter;
    PitchShifterInfo pitch;
    ReverseDelayInfo reverseDelay;
    TapeSaturatorInfo tapeSat;
    // Processing sequence, front to back, as CreativeFxStageIndex values
    // (0=Bitcrusher..7=Reverb, 8=Eq, 9=RingMod, 10=EnvFilter, 11=Pitch,
    // 12=ReverseDelay, 13=TapeSat) -- what the Virtual Rack's drag-to-
    // reorder edits. A disabled effect is a no-op wherever it sits, so this
    // is safe to leave at the identity order when nothing cares about it.
    std::array<int, 14> order = {10, 11, 0, 1, 8, 9, 2, 3, 4, 5, 6, 12, 7, 13};
    // How many of the front entries of `order` the Rack currently shows a
    // module for, independent of whether each one is powered on -- -1 means
    // "not recorded" (an older spec, or this blob's a tab's, which has no
    // rack UI); see engine/creativefx.h's CreativeFxSettings::presentCount.
    int presentCount = -1;
};

// Round-trips a CreativeFxInfo through the same wire format
// MixerClient::setMasterCreativeFx() and friends already send over D-Bus
// (see engine/creativefxspec.h), without a live daemon connection being
// involved -- what ui/rackpresetstore.h uses to keep a saved preset as one
// opaque string.
QString encodeCreativeFxSpec(const CreativeFxInfo &info);
CreativeFxInfo decodeCreativeFxSpec(const QString &spec);

struct MasterBusInfo {
    QString id;
    QString name;
    QString captureMatch;
    QString busType = QStringLiteral("capture");
    bool primary = false;
    bool hwConnected = false;
    // What captureMatch is called, resolved by the daemon so it reads the same
    // whether the device is plugged in or not. Empty for a bus that follows
    // whatever the default input happens to be.
    QString deviceLabel;
    // Is that device here right now? A bus that follows the default input is
    // never reported as disconnected: it has no particular device to lose.
    bool deviceConnected = true;
    // How far behind real time this input runs, in microseconds. Negative when
    // there is nothing to report -- a MIDI bus, or a device PipeWire has not
    // described yet -- which is why it is not an unsigned count of zero.
    // -1 = not reported yet. kLatencyHidden = the device processes audio
    // internally, so there is no host-side figure worth showing; the UI says
    // "N/A" rather than quoting a capture-side number that would rank a slow
    // microphone above a fast one.
    static constexpr qint64 kLatencyHidden = -2;
    qint64 latencyUs = -1;
};

// The Web Companion's server, as one answer. Assembled from a single D-Bus
// call: the panel wants all of it together, and asking four times would let the
// port and the running flag disagree with each other on screen.
struct CompanionInfo {
    bool running = false;
    bool autoStart = false;
    int port = 8787;
    int clients = 0;
    // "http://192.168.1.20:8787", one per address the machine answers on.
    // Empty while the server is stopped.
    QStringList addresses;
};

struct CaptureDeviceInfo {
    QString nodeName;
    QString description;
};

struct MidiDeviceInfo {
    QString nodeName;
    QString description;
};

struct TunerSourceInfo {
    QString kind;    // "audio" or "midi"
    QString id;      // node name, or a "node|port" MIDI match
    QString label;
};

struct TunerReadingInfo {
    double frequencyHz = 0.0;
    double confidence = 0.0;
    double level = 0.0;
    int midiNote = -1;
    double bendCents = 0.0;
};

class MixerClient : public QObject {
    Q_OBJECT

public:
    explicit MixerClient(QObject *parent = nullptr);

    bool available() const { return available_; }
    QString lastError() const;

    // Served from the last slow poll, so the several places that need it in
    // one refresh share a single round trip.
    const QList<ChannelInfo> &channels() const { return channelCache_; }
    QList<OutputInfo> outputs() const;
    QList<AppInfo> apps() const;

    // ---- soundboard ---------------------------------------------------
    QList<SoundboardSoundInfo> soundboardSounds() const;
    QStringList soundboardPlayingIds() const;
    // Sound id -> 0..1 through its trimmed range, one entry per id currently
    // playing. Absent means not currently playing.
    QHash<QString, double> soundboardProgress() const;
    // Where every sound plays and shares, board-wide.
    SoundboardSettingsInfo soundboardSettings() const;
    void setSoundboardChannel(const QString &channelId);
    void setSoundboardShareTarget(const QString &target);
    void setSoundboardShareVolume(double volume);
    void setSoundboardLocalVolume(double volume);
    // Empty string on failure (see lastError()); otherwise the new sound's
    // 4-digit id.
    QString addSoundboardSound(const QString &name, const QString &sourcePath,
                               int trimStartMs, int trimEndMs, double volume);
    // A non-empty newSourcePath re-imports the audio, replacing the stored
    // file; pass an empty string to change only name/trim/volume.
    bool updateSoundboardSound(const QString &id, const QString &name,
                               const QString &newSourcePath, int trimStartMs,
                               int trimEndMs, double volume);
    void removeSoundboardSound(const QString &id);
    void reorderSoundboardSounds(const QStringList &idsInOrder);
    // Async, unlike every other soundboard call here: PlaySoundboardSound
    // does not reply until the daemon has finished linking the new voice's
    // PipeWire ports, which is a real wait (SoundboardEngine spins up a
    // filter and polls the registry for it, at least one ~25ms sleep and
    // often more) rather than an ordinary property read -- a blocking
    // QDBusReply call for this one specifically was holding the UI thread
    // for that whole stretch, which is what the visible stutter on every
    // play press actually was. `onResult` is called once the reply lands
    // with an empty string on success or the daemon's error otherwise --
    // shown to the user rather than silently doing nothing, since a
    // soundboard button that fails quietly looks identical to one nobody
    // pressed.
    void playSoundboardSound(const QString &id, std::function<void(QString)> onResult);
    void stopSoundboardSound(const QString &id);
    void stopAllSoundboardSounds();
    // "durationMs\tpeak0,peak1,..." for a file not yet added, so the trim
    // dialog can draw a waveform before Save writes anything. Empty on
    // failure.
    QString analyzeSoundboardSource(const QString &path) const;
    void previewSoundboardTrim(const QString &path, int trimStartMs, int trimEndMs,
                               double volume);
    void stopSoundboardPreview();

    QStringList profiles() const;
    QString activeProfile() const;

    // Blocking, unlike the fire-and-forget profile slots below: the Manage
    // Profiles panel has to tell the user whether it worked, and a rename that
    // clashed has to leave the old name on screen rather than a stale new one.
    bool renameProfile(const QString &from, const QString &to);
    // The profile as a JSON document to write to a file; empty on failure.
    QString exportProfile(const QString &name) const;
    // Stores a document under `name`, replacing any profile of that name.
    bool importProfile(const QString &name, const QString &json);
    // Whether the mixer still matches the profile it was loaded from. False for
    // a name that is not a profile, so an empty active profile reads as "these
    // settings are not saved anywhere" -- which is exactly what it means.
    bool profileMatchesLive(const QString &name) const;

    bool deviceConnected() const;
    QString deviceFirmware() const;

    // Which microphone Waveline was installed for. Fixed for the life of the
    // daemon -- the profile is chosen at install time -- so these are fetched
    // once per connection and served from a cache afterwards rather than round
    // tripped from paint code sixty times a second.
    //
    // deviceBrand() is what goes in front of names in the UI ("Wave:3", or
    // "Waveline"); hasHardwareControls() is false unless the microphone has a
    // vendor protocol, and the whole hardware panel is built or not built off it.
    QString deviceBrand() const;
    bool hasHardwareControls() const;
    bool clipguard() const;
    int hardwareMonitor() const;
    bool micMuted() const;
    double micGainDb() const;
    double headphoneVolumeDb() const;
    bool headphoneMuted() const;
    bool soundSharingEnabled() const;
    double soundSharingVolume(const QString &mix) const;
    QStringList soundSharingApps() const;
    // "id\tlabel" rows for microphones an application can be shared into.
    QStringList soundSharingTargets() const;
    double soundSharingAppLevel(uint nodeId) const;
    bool noiseSuppression() const;
    bool micEffectsEnabled() const;
    bool micMonitorFx() const;
    double noiseInputLevel() const;
    double noiseOutputLevel() const;
    double speechProbability() const;
    QString monitorOutput() const;
    QStringList monitorOutputs() const;
    QList<MonitorOutputInfo> monitorOutputStates() const;
    bool routingEnabled() const;
    bool softwareMonitor() const;
    bool micStereo() const;
    double monitorLevel() const;
    double monitorOutputVolume() const;
    bool monitorOutputMuted() const;
    double streamMixVolume() const;
    bool streamMixMuted() const;
    double noiseIntensity() const;

    // ---- graph clock -------------------------------------------------------
    // Frames per PipeWire scheduling cycle: the latency every path in the graph
    // pays before any device buffering. 0 from graphQuantum() means the user
    // has pinned nothing and the graph runs on its configured default;
    // effectiveGraphQuantum() is what it is actually running at, which is 0
    // only while the daemon has not been told yet.
    int graphQuantum() const;
    int effectiveGraphQuantum() const;
    void setGraphQuantum(int frames);

    // ALSA headroom for non-USB outputs. Unlike the quantum this does not apply
    // until WirePlumber is restarted, so effectiveOutputHeadroom() is what the
    // devices are actually open with: equal to outputHeadroom() once it has
    // taken, -1 while the devices disagree about it.
    int outputHeadroom() const;
    int effectiveOutputHeadroom() const;
    void setOutputHeadroom(int frames);
    void restartWirePlumber();

    // Whether audio threads are allowed to be real-time. What is stored, not
    // what is running: a context keeps the scheduling it was created with, so
    // setting this only takes effect once the daemon builds its graph again.
    // restartDaemon() is that step, kept separate so the window can ask first.
    bool realtimeScheduling() const;
    void setRealtimeScheduling(bool on);
    // Restarts wavelined. Run from here rather than from the daemon on purpose:
    // a detached systemctl started by the daemon lives in the daemon's own
    // cgroup and is killed with it mid-restart. The mixer is in its own.
    bool restartDaemon();

    // Per-stage DSP timing and missed-cycle counting. Applies the moment it is
    // set -- no restart, unlike the switch above -- and turning it on resets
    // the counters the diagnostics rows are built from.
    bool dspProfiling() const;
    void setDspProfiling(bool on);

    // How many input devices get a noise-suppression switch in a desktop
    // shell's microphone panel, and whether such a shell is running at all.
    // See MixerService::ShellInputs for the whole of what that surface is.
    int shellNoiseSuppressionInputs() const;
    void setShellNoiseSuppressionInputs(int count);
    bool shellClientPresent() const;

    QList<StreamConflictInfo> streamRoutingConflicts() const;
    void dismissStreamRoutingConflict(const QString &sinkName);
    QStringList dismissedStreamRoutingConflicts() const;
    void clearStreamRoutingConflictDismissals();
    // "key\tvalue\tdetail" rows for the diagnostics window.
    QStringList graphDiagnostics() const;

    // The noise suppression model in use, e.g. "rnnoise".
    // ---- web companion ----
    CompanionInfo companion() const;
    // Both return an empty string on success, otherwise why not. Not slots:
    // the panel has to put its own control back when the daemon refuses, and a
    // fire-and-forget call gives it nothing to decide that with.
    QString companionStart();
    QString setCompanionPort(int port);
    void companionStop();
    void setCompanionAutoStart(bool on);

    QString noiseEngine() const;
    // One entry per selectable engine. `available` is false when the engine
    // cannot run on this machine -- DeepFilterNet is loaded at runtime and is
    // often simply not installed -- and `reason` then says what is missing.
    struct NoiseEngineInfo {
        QString id;
        QString label;
        bool available = false;
        QString reason;
    };
    QList<NoiseEngineInfo> noiseEngines() const;
    // Returns an empty string on success, otherwise why the switch failed. Not
    // a slot: callers need the answer, because a failure leaves the previous
    // engine running and the selector has to be put back.
    QString setNoiseEngine(const QString &id);
    ChannelFxInfo channelEffects(const QString &channelId,
                                   const QString &stage = QStringLiteral("input")) const;
    MicDynamicsInfo micDynamics() const;
    bool channelNoiseSuppression(const QString &channelId,
                                 const QString &stage = QStringLiteral("input")) const;
    double channelNoiseIntensity(const QString &channelId,
                                 const QString &stage = QStringLiteral("input")) const;
    // Master bypass for the channel's whole chain; leaves its settings alone.
    bool channelEffectsEnabled(const QString &channelId) const;
    // Monitor mix fed from the FX chain (audible effects) rather than dry.
    bool channelMonitorFx(const QString &channelId) const;
    // This channel publishes its own microphone as a recording device.
    // `ok` distinguishes "the daemon said no" from "the daemon did not
    // answer", which for a control that mirrors this value is the difference
    // between showing the truth and throwing the user's switch for them.
    bool channelMicSource(const QString &channelId, bool *ok = nullptr) const;
    // Third input effect-source mode: publish each input device's own processed
    // output rather than running this channel's mic chain. `ok` as above.
    bool channelMicUseDeviceFx(const QString &channelId, bool *ok = nullptr) const;
    void setChannelMicUseDeviceFx(const QString &channelId, bool on);
    // Software monitor of that published microphone in the Monitor mix.
    bool channelMicMonitor(const QString &channelId) const;
    bool channelMicMuted(const QString &channelId) const;
    double channelMicSend(const QString &channelId) const;
    // false = unique channel effects, true = any master effect source is active.
    bool channelEffectSourceMaster(const QString &channelId,
                                   const QString &stage) const;
    // Resolved master bus id ("mic", "master-1", …), or empty for unique effects.
    QString channelEffectSourceMasterId(const QString &channelId,
                                        const QString &stage) const;
    // The microphone as an ordinary input: its own level and mute in each mix.
    double micVolume(const QString &mix) const;
    bool micMixMuted(const QString &mix) const;
    // The input device's own level and mute, 0..1. What stands in for a preamp
    // on a microphone with no vendor protocol.
    double micInputVolume() const;
    bool micInputMuted() const;

    // Multi-master buses. Primary id is always "mic".
    QList<MasterBusInfo> masterBuses() const;
    QList<CaptureDeviceInfo> captureDevices() const;
    QList<MidiDeviceInfo> midiDevices() const;
    QString addMasterBus(const QString &name);
    QString addMasterBusEx(const QString &name, const QString &busType,
                           const QString &deviceMatch);
    bool removeMasterBus(const QString &id);
    bool rebuildMasterCapture(const QString &id);
    void setMasterCaptureDevice(const QString &id, const QString &nodeMatch);
    void setMasterMidiPort(const QString &id, const QString &nodeMatch);
    QStringList masterSoundfonts(const QString &id) const;
    void addMasterSoundfont(const QString &id, const QString &path);
    void removeMasterSoundfont(const QString &id, const QString &path);
    void setMasterSoundfont(const QString &id, const QString &path);
    void setMasterName(const QString &id, const QString &name);
    void setChannelName(const QString &channelId, const QString &name);
    // Card key ("channel:game", "master:mic") -> "#rrggbb" colour and icon
    // name, either of which may be empty for "follow the theme".
    struct CardAppearance {
        QString color;
        QString icon;
    };
    QHash<QString, CardAppearance> cardAppearances() const;
    void setCardAppearance(const QString &key, const QString &color,
                           const QString &icon);
    void setChannelMasterMic(const QString &channelId, const QString &masterId);
    QString channelMasterMic(const QString &channelId) const;
    void setChannelMasterMics(const QString &channelId, const QStringList &masterIds);
    // Empty when the daemon did not answer -- callers must not treat that as
    // "no input devices".
    QStringList channelMasterMics(const QString &channelId) const;
    bool hasHardwareControlsFor(const QString &masterId) const;
    bool masterDeviceConnected(const QString &masterId) const;
    QString masterDeviceFirmware(const QString &masterId) const;

    double masterMicVolume(const QString &masterId, const QString &mix) const;
    bool masterMicMixMuted(const QString &masterId, const QString &mix) const;
    bool masterMicEffectsEnabled(const QString &masterId) const;
    // De-esser: a switch and an amount, 0..1. Input devices carry their own;
    // channels carry one per stage ("input" = the channel's microphone,
    // "output" = its app audio), with channelId "mic" addressing the primary
    // device's microphone and the shared App Audio template.
    bool masterDeEsser(const QString &masterId) const;
    void setMasterDeEsser(const QString &masterId, bool on);
    double masterDeEsserIntensity(const QString &masterId) const;
    void setMasterDeEsserIntensity(const QString &masterId, double value);
    bool channelDeEsser(const QString &channelId, const QString &stage) const;
    void setChannelDeEsser(const QString &channelId, const QString &stage, bool on);
    double channelDeEsserIntensity(const QString &channelId,
                                   const QString &stage) const;
    void setChannelDeEsserIntensity(const QString &channelId, const QString &stage,
                                    double value);

    bool masterMicMonitorFx(const QString &masterId) const;
    bool masterMicStereo(const QString &masterId) const;
    double masterMicInputVolume(const QString &masterId) const;
    bool masterMicInputMuted(const QString &masterId) const;
    bool masterNoiseSuppression(const QString &masterId) const;
    double masterNoiseIntensity(const QString &masterId) const;
    bool masterSoftwareMonitor(const QString &masterId) const;
    ChannelFxInfo masterChannelEffects(const QString &masterId,
                                       const QString &stage) const;
    MicDynamicsInfo masterMicDynamics(const QString &masterId) const;
    bool masterClipguard(const QString &masterId) const;
    int masterHardwareMonitor(const QString &masterId) const;
    bool masterMicMuted(const QString &masterId) const;
    double masterMicGainDb(const QString &masterId) const;
    double masterHeadphoneVolumeDb(const QString &masterId) const;
    bool masterHeadphoneMuted(const QString &masterId) const;

    // Signal actually flowing, keyed by channel id plus "monitor-mix",
    // "stream-mix", "mic-in", "mic-out" and "speech". Peak amplitude, 0..1.
    // Served from the last fast poll rather than fetched, so reading it is
    // free and every meter in the window sees the same instant.
    const QHash<QString, double> &levels() const { return levelCache_; }

    // Stops both polls while the window is not on screen.
    void setPollingEnabled(bool on);

    // ---- instrument tuner -------------------------------------------------
    // Not folded into the state poll: the tuner is only running while its
    // window is open, and that window keeps its own faster timer rather than
    // making every other client pay for a reading nobody is looking at.
    // Plays the note a string should be at, in the Monitor mix.
    void playTunerReference(double hz, int ms);
    QList<TunerSourceInfo> tunerSources() const;
    // Empty string on success, otherwise the daemon's explanation.
    QString startTuner(const QString &kind, const QString &source);
    void stopTuner();
    TunerReadingInfo tunerReading() const;

public slots:
    void setChannelVolume(const QString &id, const QString &mix, double v);
    void setChannelMuted(const QString &id, const QString &mix, bool m);
    void setClipguard(bool on);
    void setHardwareMonitor(int percent);
    void setNoiseSuppression(bool on);
    void setMicEffectsEnabled(bool on);
    void setMicMonitorFx(bool on);
    void setMonitorOutput(const QString &sinkName);
    void setMonitorOutputAt(int index, const QString &sinkName);
    void addMonitorOutput(const QString &sinkName);
    void removeMonitorOutput(int index);
    void setMonitorOutputVolumeAt(int index, double volume);
    void setMonitorOutputMutedAt(int index, bool muted);
    void setRoutingEnabled(bool on);
    void setSoftwareMonitor(bool on);
    void setMicStereo(bool on);
    void setMonitorLevel(double v);
    void setMonitorOutputVolume(double v);
    void setMonitorOutputMuted(bool muted);
    void setStreamMixVolume(double v);
    void setStreamMixMuted(bool muted);
    void setNoiseIntensity(double v);
    void setChannelEffects(const QString &channelId, const QString &stage, bool lowCut,
                           int lowCutHz, bool eq, double lowDb, double midDb,
                           double highDb);
    void setChannelProEq(const QString &channelId, const QString &stage, bool advanced,
                         const QString &bands);
    void setMicDynamics(const MicDynamicsInfo &d);
    MicDynamicsInfo channelDynamics(const QString &channelId,
                                    const QString &stage) const;
    void setChannelDynamics(const QString &channelId, const QString &stage,
                            const MicDynamicsInfo &d);
    DuckingInfo channelDucking(const QString &channelId) const;
    void setChannelDucking(const QString &channelId, bool enabled, double intensity,
                           const QString &sources, double holdSec);
    LufsLimiterInfo channelLufsLimiter(const QString &channelId) const;
    void setChannelLufsLimiter(const QString &channelId, bool enabled, double maxLufs);
    CreativeFxInfo channelCreativeFx(const QString &channelId, const QString &stage) const;
    void setChannelCreativeFx(const QString &channelId, const QString &stage,
                              const CreativeFxInfo &info);
    void setChannelNoiseSuppression(const QString &channelId, const QString &stage,
                                    bool on);
    void setChannelNoiseIntensity(const QString &channelId, const QString &stage,
                                   double v);
    void setChannelEffectsEnabled(const QString &channelId, bool on);
    void setChannelMonitorFx(const QString &channelId, bool on);
    void setChannelMicSource(const QString &channelId, bool on);
    void setChannelMicMonitor(const QString &channelId, bool on);
    void setChannelMicMuted(const QString &channelId, bool muted);
    void setChannelMicSend(const QString &channelId, double level);
    void setChannelEffectSourceMaster(const QString &channelId, const QString &stage,
                                      const QString &masterId);
    void setMicVolume(const QString &mix, double v);
    void setMicMuted(const QString &mix, bool m);
    void setHardwareMicMute(bool muted);
    void setMicGainDb(double db);
    void setMicInputVolume(double volume);
    void setMicInputMuted(bool muted);
    void setMasterMicVolume(const QString &masterId, const QString &mix, double v);
    void setMasterMicMixMuted(const QString &masterId, const QString &mix, bool m);
    void setMasterMicEffectsEnabled(const QString &masterId, bool on);
    void setMasterMicMonitorFx(const QString &masterId, bool on);
    void setMasterMicStereo(const QString &masterId, bool on);
    void setMasterMicInputVolume(const QString &masterId, double volume);
    void setMasterMicInputMuted(const QString &masterId, bool muted);
    void setMasterNoiseSuppression(const QString &masterId, bool on);
    void setMasterNoiseIntensity(const QString &masterId, double v);
    void setMasterSoftwareMonitor(const QString &masterId, bool on);
    void setMasterChannelEffects(const QString &masterId, const QString &stage,
                                 bool lowCut, int lowCutHz, bool eq, double lowDb,
                                 double midDb, double highDb);
    void setMasterProEq(const QString &masterId, const QString &stage, bool advanced,
                        const QString &bands);
    void setMasterMicDynamics(const QString &masterId, const MicDynamicsInfo &d);
    CreativeFxInfo masterCreativeFx(const QString &masterId) const;
    void setMasterCreativeFx(const QString &masterId, const CreativeFxInfo &info);
    // The Virtual Rack's own Creative FX configuration -- a separate blob
    // from masterCreativeFx above, only applied to the mic input chain when
    // masterRackMode() is on.
    CreativeFxInfo masterRackCreativeFx(const QString &masterId) const;
    void setMasterRackCreativeFx(const QString &masterId, const CreativeFxInfo &info);
    bool masterRackMode(const QString &masterId) const;
    void setMasterRackMode(const QString &masterId, bool on);
    void setMasterClipguard(const QString &masterId, bool on);
    void setMasterHardwareMonitor(const QString &masterId, int percent);
    void setMasterHardwareMicMute(const QString &masterId, bool muted);
    void setMasterMicGainDb(const QString &masterId, double db);
    void setMasterHeadphoneVolumeDb(const QString &masterId, double db);
    void setMasterHeadphoneMuted(const QString &masterId, bool muted);
    void setHeadphoneVolumeDb(double db);
    void setHeadphoneMuted(bool muted);
    void setSoundSharingEnabled(bool on);
    void setSoundSharingVolume(const QString &mix, double v);
    void setSoundSharingApp(uint nodeId, bool shared);
    void setSoundSharingAppTarget(uint nodeId, const QString &target);
    void setSoundSharingAppLevel(uint nodeId, double level);
    void setAppVolume(uint nodeId, double volume);
    void moveApp(uint nodeId, const QString &channelId);
    void loadProfile(const QString &name);
    void saveProfile(const QString &name);
    void deleteProfile(const QString &name);
    void refresh();

signals:
    // The daemon changed something, or the connection came and went.
    void changed();
    // New meter data. Separate from changed() because it fires ~25x as often
    // and only the meters care.
    void levelsChanged();
    void availabilityChanged(bool available);

private:
    template <typename T>
    T get(const char *method, const T &fallback = {}) const;
    void call(const char *method, const QVariantList &args);
    void probe();
    void pollLevels();
    void pollState();
    QHash<QString, double> fetchLevels() const;
    QList<ChannelInfo> fetchChannels() const;

    // Filled on the first read after a (re)connection, dropped when the daemon
    // goes away so a restarted daemon with a different profile is picked up.
    void fetchProfile() const;
    void readLocalProfile() const;
    mutable bool profileValid_ = false;
    mutable QString deviceBrand_ = QStringLiteral("Waveline");
    mutable bool hasHardwareControls_ = false;

    QDBusInterface *iface_ = nullptr;
    QTimer poll_;        // full state: everything a person can change
    QTimer levelPoll_;   // meters only, one round trip
    QTimer reconnect_;
    QHash<QString, double> levelCache_;
    QList<ChannelInfo> channelCache_;
    bool available_ = false;
};

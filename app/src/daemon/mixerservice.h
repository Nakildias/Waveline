// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The D-Bus face of the mixer: org.waveline.Mixer on the session bus.
//
// The daemon owns the PipeWire graph, the noise filter, the app router and the
// USB device; the GUI is only a client. That split is deliberate -- closing the
// window must not tear down someone's audio routing mid-stream.
//
// Every method takes and returns plain D-Bus types. Custom marshalled structs
// would be tidier in C++ and much worse to debug: this way the whole API is
// reachable from gdbus with no client at all, which is how it gets tested.

#pragma once

#include <QDBusContext>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <map>
#include <memory>

#include "companionserver.h"
#include "configstore.h"
#include "device/deviceprofile.h"
#include "device/wave3device.h"
#include "engine/alsadelay.h"
#include "engine/approuter.h"
#include "engine/dynamicsfilter.h"
#include "engine/effectsfilter.h"
#include "engine/gainfilter.h"
#include "engine/levelprobe.h"
#include "engine/lufslimiter.h"
#include "engine/mixergraph.h"
#include "engine/pwengine.h"
#include "engine/soundboardengine.h"
#include "engine/soundsharerouter.h"
#include "engine/tuner.h"

class MixerService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.waveline.Mixer")

public:
    explicit MixerService(QObject *parent = nullptr);
    ~MixerService() override;

    bool start(QString &error);

    // Answers from EffectiveOutputHeadroom() that are not a frame count.
    // Out here rather than beside it because moc rejects anything that is
    // not a signal or a slot inside a slots block.
    static constexpr int kHeadroomMixed = -1;
    static constexpr int kHeadroomUnknown = -2;

    // Mutes the real output devices this mixer is feeding, and remembers which
    // ones it muted. Called on the way out: while the daemon is gone its
    // loopbacks are gone with it, so whatever else is playing reaches those
    // devices at *their* volume rather than at the level the mixer was holding
    // them to -- which, for headphones fed from a monitor mix sitting at 17%,
    // is a very unpleasant surprise. Restored by start().
    void muteOutputsForShutdown();

public slots:
    // ---- channels -------------------------------------------------------
    QStringList ChannelIds() const;
    // Every channel's whole state in one call, as
    // "id\tname\tstreamVol\tmonitorVol\tstreamMuted\tmonitorMuted" rows.
    // The per-field getters below still exist and are what you want from a
    // script; this is for the GUI, which needs all of it several times a
    // second and was spending 26 round trips per refresh to assemble it.
    QStringList Channels() const;
    QString ChannelName(const QString &id) const;
    // Display name only. The channel's PipeWire sink keeps the name its
    // applications are routed by; empty restores the built-in title.
    void SetChannelName(const QString &id, const QString &name);

    // ---- routing conflicts ----------------------------------------------
    // Streams another program keeps taking off their channel, as
    // "appName\tchannelId\tsinkName\tsinkLabel" rows, one per offending sink.
    //
    // Reported rather than fought. Two programs writing the same target.object
    // key in a loop is a permanent stream of moves and an audible one, so the
    // router still gives up after its four attempts -- this is only so that it
    // stops giving up silently, which is what made this cost an evening to
    // work out the first time.
    QStringList StreamRoutingConflicts() const;
    // Silences one offending sink for good, by node.name. Machine-wide and
    // saved beside the profiles: which other audio software is installed is
    // not something a profile should carry.
    void DismissStreamRoutingConflict(const QString &sinkName);
    QStringList DismissedStreamRoutingConflicts() const;
    void ClearStreamRoutingConflictDismissals();

    // ---- card appearance ------------------------------------------------
    // What the user has chosen for each card, as "key\tcolour\ticon" rows.
    // Keys are "channel:<id>" and "master:<id>"; an absent key means the card
    // follows the theme. Colours are "#rrggbb", icons are names.
    QStringList CardAppearances() const;
    void SetCardAppearance(const QString &key, const QString &color,
                           const QString &icon);
    double ChannelVolume(const QString &id, const QString &mix) const;
    bool ChannelMuted(const QString &id, const QString &mix) const;
    void SetChannelVolume(const QString &id, const QString &mix, double volume);
    void SetChannelMuted(const QString &id, const QString &mix, bool muted);

    // The microphone behaves as a channel present in both mixes.
    void SetMicVolume(const QString &mix, double volume);
    void SetMicMuted(const QString &mix, bool muted);
    // Read back, so the GUI can show the microphone as an ordinary input card.
    // The graph does not keep these -- a loopback's volume is write-only from
    // here -- so they are mirrored alongside the rest of the profile.
    double MicVolume(const QString &mix) const;
    bool MicMixMuted(const QString &mix) const;

    // Hearing yourself through the software Monitor mix.
    bool SoftwareMonitor() const;
    void SetSoftwareMonitor(bool on);
    // How loudly you hear yourself in the Monitor mix, 0..1.
    double MonitorLevel() const;
    void SetMonitorLevel(double volume);
    // Master level of the whole Monitor mix on its way to the output, 0..1.
    // Separate from MonitorLevel: that one is the microphone's level *within*
    // the mix, this one turns everything you hear up or down together.
    double MonitorOutputVolume() const;
    void SetMonitorOutputVolume(double volume);
    // Master mute for the Monitor mix. Separate from the per-channel monitor
    // mutes: this silences everything on its way to the output at once.
    bool MonitorOutputMuted() const;
    void SetMonitorOutputMuted(bool muted);
    // Centre the mono microphone across both channels of the mixes.
    bool MicStereo() const;
    void SetMicStereo(bool on);

    // The input device's own gain and mute -- the same control a volume applet
    // shows for a microphone. This is what stands in for a preamp on hardware
    // with no vendor protocol, and it is a real device mute rather than a fader
    // pulled to zero, so nothing downstream can route around it.
    //
    // 0..1 linear. Kept here rather than read back from PipeWire, for the same
    // reason the rest of the mixer's state is: the config is authoritative and
    // is reapplied whenever the graph is rewired or the microphone changes.
    double MicInputVolume() const;
    void SetMicInputVolume(double volume);
    bool MicInputMuted() const;
    void SetMicInputMuted(bool muted);

    // ---- desktop shell integration -----------------------------------------
    //
    // A panel applet -- GlassBar's Sound and Microphone modules are the ones
    // this was written against -- needs things this API can already answer, but
    // needs them *together* and several times a second while its panel is open.
    // Assembled from the per-field getters that is one round trip per field per
    // device; here it is one per call.
    //
    // Nothing in the daemon depends on a shell being present. These are
    // ordinary methods on the same bus as everything else, reachable from
    // gdbus, and a machine with no panel simply never calls them. The reverse
    // holds too: a panel finds this by asking the bus whether
    // org.waveline.Mixer is there, and falls back to WirePlumber when it is
    // not. Neither program needs the other installed.

    // One row per input device, in MasterBuses() order:
    //   id, name, deviceLabel, connected, inputVolume, inputMuted,
    //   noiseSuppression, noiseIntensity, level, showNoiseSuppression
    //
    // `level` is the post-processing peak, 0..1, the same figure Levels()
    // reports under "<id>-out" -- carried here so a meter costs no extra call.
    // `showNoiseSuppression` is this device's index against
    // ShellNoiseSuppressionInputs(); the daemon resolves that policy rather
    // than making every client re-derive it.
    QStringList ShellInputs() const;

    // One row per Monitor output, in the fan-out order:
    //   sink, description, volume, muted, online, level
    //
    // Everything but `level` is what MonitorOutputStates() already answers;
    // this exists for the level, and carries the rest so a panel drawing a
    // meter beside a fader does not need two calls to draw one row.
    //
    // `level` is the Monitor bus peak scaled by that output's own fader and
    // zeroed when it is muted. There is no separate meter per output and there
    // should not be: the fan-out is one mix sent to several devices, so what
    // differs between them is exactly the fader and the mute. This is the same
    // arithmetic the mixer's own output rows do.
    QStringList ShellOutputs() const;

    // Applications actually recording from a microphone right now:
    //   nodeId, appName, binary, sourceLabel
    //
    // "Actually" is doing real work in that sentence. A shell that asks
    // PipeWire directly sees Waveline's own capture streams -- the mixer holds
    // one open per input device for as long as it runs -- and reports the
    // microphone as permanently in use by Waveline, which is both useless and
    // alarming. What a person wants to know is which *other* program is
    // listening, so this excludes every stream this process owns and every
    // stream reading a sink's monitor rather than a microphone, and reports
    // what is left.
    QStringList MicrophoneConsumers() const;

    // How many input devices get a noise-suppression switch in a shell panel.
    // See ShellSettings::noiseSuppressionInputs.
    int ShellNoiseSuppressionInputs() const;
    void SetShellNoiseSuppressionInputs(int count);

    // Whether a desktop shell that speaks this API is on the bus right now.
    // Informational: nothing here behaves differently either way, and the
    // daemon works exactly the same with no shell installed.
    bool ShellClientPresent() const;

    // ---- multi-master buses -----------------------------------------------
    // "id\tname\tcaptureMatch\tbusType\tprimary\thwConnected\tdeviceLabel\t
    // deviceConnected\tlatencyUs" rows. latencyUs is -1 when there is nothing
    // to report (MIDI, or a device PipeWire has not described yet), and -2
    // (kLatencyHidden) for a device that processes audio internally, where no
    // measurable figure would be the truth.
    QStringList MasterBuses() const;
    // "nodeName\tdescription" for usable Audio/Source capture nodes.
    QStringList CaptureDevices() const;

    // ---- graph clock -------------------------------------------------------
    //
    // Frames per PipeWire scheduling cycle. This is charged to every path in
    // the graph on top of whatever the device itself buffers, so it is half of
    // what latency means in practice and the only half a user can move without
    // editing files.
    //
    // 0 is "not pinned": the graph runs on whatever the config files configured
    // and Waveline reports it rather than imposing one. Setting a value writes
    // clock.force-quantum, which applies to the running graph immediately --
    // no restart and no reinstall, which is what makes this a dropdown rather
    // than a paragraph telling people which file to edit.
    //
    // It is NOT free, and the cost is not optional: every capture hop is
    // rebuilt afterwards. A quantum change can leave an ALSA capture resampler
    // in a permanent resync loop -- audibly robotic until the node is reopened
    // -- and node.lock-quantum does not prevent it, because forced changes go
    // through it by design. See SetGraphQuantum's implementation.
    int GraphQuantum() const;
    void SetGraphQuantum(int frames);
    // What the quantum is right now, as opposed to what we asked for: 0 until
    // PipeWire has answered. Distinguishing them matters because the answer to
    // "why is this 21 ms" is sometimes "because your request never landed".
    int EffectiveGraphQuantum() const;

    // Frames of ALSA headroom for non-USB outputs, 0 for "write no rule".
    //
    // The other half of latency, and the half that is a workaround: a device
    // that is not the graph clock resamples onto it and underruns without slack
    // when the machine is busy. Unlike SetGraphQuantum this does NOT apply to
    // the running graph -- headroom is fixed when a device is opened -- so it
    // writes a WirePlumber rule and leaves it to the caller to decide when to
    // pay for the restart that reads it. See daemon/wpheadroom.h.
    int OutputHeadroom() const;
    void SetOutputHeadroom(int frames);
    // What a non-USB output is actually running with, read back off a live
    // node. This is how the mixer can say "restart pending" honestly instead of
    // showing the stored number and implying it took effect.
    //
    // kHeadroomMixed when the devices disagree (one opened before a rule change
    // and one after), kHeadroomUnknown when there is nothing to read: no PCI
    // output on the machine, or none bound yet.
    int EffectiveOutputHeadroom() const;
    // Restarts WirePlumber so the rule above is read. Every stream on the
    // machine stops for a moment, so this is never called implicitly.
    void RestartWirePlumber();

    // Whether the audio threads are allowed to be real-time. Stored machine
    // policy, not a live reading: GraphDiagnostics() is where the answer to
    // "and did they actually get it" lives, and the two disagree on a stock
    // system where rtkit hands out its first two dozen grants and refuses the
    // rest.
    //
    // Setting it persists and takes effect on the next daemon start, because a
    // PipeWire context decides its scheduling when it is created and does not
    // reconsider -- so the whole graph has to be built again. The setter
    // deliberately does NOT restart anything: the mixer asks first, and a
    // rebuild that interrupts audio is not something to do inside a property
    // write. See engine/rtsched.h.
    bool RealtimeScheduling() const;
    void SetRealtimeScheduling(bool on);

    // Time every DSP stage in the graph and count the cycles PipeWire says it
    // missed. Unlike the switch above this one applies instantly and rebuilds
    // nothing: it is a flag the audio threads read, not a property of the
    // contexts they run in.
    //
    // Turning it on resets the counters, so the figures always mean "since you
    // asked". Off by default -- the cost is two clock reads per stage per
    // cycle, which is small and is still not worth paying on a machine where
    // nobody is looking. See engine/dspprobe.h.
    bool DspProfiling() const;
    void SetDspProfiling(bool on);

    // Human-readable lines for the diagnostics view: "key\tvalue\tdetail".
    //
    // This exists because the entire latency investigation behind it was a
    // multi-day exercise that any one of these lines would have ended in a
    // glance -- above all "driver", which silently changes when unrelated
    // hardware is plugged in and is the reason latency felt random.
    QStringList GraphDiagnostics() const;
    // "nodeName\tdescription" for usable PipeWire MIDI input nodes.
    QStringList MidiDevices() const;
    QString AddMasterBus(const QString &name);
    QString AddMasterBusEx(const QString &name, const QString &busType,
                           const QString &deviceMatch);
    void RemoveMasterBus(const QString &id);
    // Tear down and recreate this master's ALSA→DSP hop (fixes robotic capture).
    bool RebuildMasterCapture(const QString &id);
    void SetMasterCaptureDevice(const QString &id, const QString &nodeMatch);
    void SetMasterMidiPort(const QString &id, const QString &nodeMatch);
    QStringList MasterSoundfonts(const QString &id) const;
    void AddMasterSoundfont(const QString &id, const QString &path);
    void RemoveMasterSoundfont(const QString &id, const QString &path);
    void SetMasterSoundfont(const QString &id, const QString &path);
    void SetMasterName(const QString &id, const QString &name);
    // First entry of ChannelMasterMics; kept for clients that predate the list.
    QString ChannelMasterMic(const QString &channelId) const;
    void SetChannelMasterMic(const QString &channelId, const QString &masterId);
    // Every input device feeding this channel's published microphone, in the
    // order the user arranged them. Never empty.
    QStringList ChannelMasterMics(const QString &channelId) const;
    void SetChannelMasterMics(const QString &channelId, const QStringList &masterIds);
    // Publish each input device's own processed output instead of running this
    // channel's mic chain. Exclusive with an input effect-source device.
    bool ChannelMicUseDeviceFx(const QString &channelId) const;
    void SetChannelMicUseDeviceFx(const QString &channelId, bool on);
    bool HasHardwareControlsFor(const QString &masterId) const;
    bool MasterDeviceConnected(const QString &masterId) const;
    // The firmware version of the device behind one input bus. DeviceFirmware()
    // above is this with masterId "mic" -- it predates multiple input devices
    // and is kept because clients call it; everything with a bus id wants this.
    QString MasterDeviceFirmware(const QString &masterId) const;
    double MasterMicVolume(const QString &masterId, const QString &mix) const;
    void SetMasterMicVolume(const QString &masterId, const QString &mix, double volume);
    bool MasterMicMixMuted(const QString &masterId, const QString &mix) const;
    void SetMasterMicMixMuted(const QString &masterId, const QString &mix, bool muted);
    bool MasterMicEffectsEnabled(const QString &masterId) const;
    void SetMasterMicEffectsEnabled(const QString &masterId, bool on);
    bool MasterMicMonitorFx(const QString &masterId) const;
    void SetMasterMicMonitorFx(const QString &masterId, bool on);
    bool MasterMicStereo(const QString &masterId) const;
    void SetMasterMicStereo(const QString &masterId, bool on);
    double MasterMicInputVolume(const QString &masterId) const;
    void SetMasterMicInputVolume(const QString &masterId, double volume);
    bool MasterMicInputMuted(const QString &masterId) const;
    void SetMasterMicInputMuted(const QString &masterId, bool muted);
    // De-esser, as its own switch and amount rather than more arguments on
    // Set*Dynamics: that call replaces the whole dynamics block, so a de-esser
    // carried in it would be reset by every compressor tweak.
    bool MasterDeEsser(const QString &masterId) const;
    void SetMasterDeEsser(const QString &masterId, bool on);
    double MasterDeEsserIntensity(const QString &masterId) const;
    void SetMasterDeEsserIntensity(const QString &masterId, double value);
    // Channels, by stage: "input" is the channel's own microphone, "output" is
    // its app audio. channelId "mic" addresses the primary input device's
    // microphone and the shared App Audio template respectively, the same
    // convention ChannelNoiseSuppression uses.
    bool ChannelDeEsser(const QString &channelId, const QString &stage) const;
    void SetChannelDeEsser(const QString &channelId, const QString &stage, bool on);
    double ChannelDeEsserIntensity(const QString &channelId,
                                   const QString &stage) const;
    void SetChannelDeEsserIntensity(const QString &channelId, const QString &stage,
                                    double value);

    bool MasterNoiseSuppression(const QString &masterId) const;
    void SetMasterNoiseSuppression(const QString &masterId, bool on);
    double MasterNoiseIntensity(const QString &masterId) const;
    void SetMasterNoiseIntensity(const QString &masterId, double value);
    bool MasterSoftwareMonitor(const QString &masterId) const;
    void SetMasterSoftwareMonitor(const QString &masterId, bool on);
    QString MasterChannelEffects(const QString &masterId, const QString &stage) const;
    void SetMasterChannelEffects(const QString &masterId, const QString &stage,
                                 bool lowCut, int lowCutHz, bool eq, double lowDb,
                                 double midDb, double highDb);
    // See SetChannelProEq.
    void SetMasterProEq(const QString &masterId, const QString &stage, bool advanced,
                        const QString &bands);
    QString MasterMicDynamics(const QString &masterId) const;
    void SetMasterMicDynamics(const QString &masterId, bool gate, double gateThresholdDb,
                              double gateAttackMs, double gateReleaseMs, bool compressor,
                              double compThresholdDb, double compRatio, double compAttackMs,
                              double compReleaseMs, double compKneeDb, double makeupGainDb,
                              bool autoMakeup, bool limiter, double limitThresholdDb,
                              double limitAttackMs, double limitReleaseMs);
    // The Creative FX pedalboard. `spec` is the wire format described in
    // engine/creativefxspec.h -- one block per effect (bitcrusher,
    // overdrive, chorus, flanger, phaser, tremolo, delay, reverb, in that
    // order), joined by ';'. An empty or short spec falls back to that
    // effect's default (off) settings, same convention as SetChannelProEq.
    QString MasterCreativeFx(const QString &masterId) const;
    void SetMasterCreativeFx(const QString &masterId, const QString &spec);
    // The Virtual Rack's own Creative FX configuration for this device --
    // same wire format as MasterCreativeFx, but a separate blob (see
    // MasterBusState::rackCreativeFx). Only takes effect on the mic input
    // chain when MasterRackMode is on.
    QString MasterRackCreativeFx(const QString &masterId) const;
    void SetMasterRackCreativeFx(const QString &masterId, const QString &spec);
    // Whether the mic input chain's effective Creative FX comes from the
    // Rack's own configuration (true) or the Microphone > Creative tab's
    // (false, the default).
    bool MasterRackMode(const QString &masterId) const;
    void SetMasterRackMode(const QString &masterId, bool on);
    bool MasterClipguard(const QString &masterId) const;
    void SetMasterClipguard(const QString &masterId, bool on);
    int MasterHardwareMonitor(const QString &masterId) const;
    void SetMasterHardwareMonitor(const QString &masterId, int percent);
    bool MasterMicMuted(const QString &masterId) const;
    void SetMasterHardwareMicMute(const QString &masterId, bool muted);
    double MasterMicGainDb(const QString &masterId) const;
    void SetMasterMicGainDb(const QString &masterId, double db);
    double MasterHeadphoneVolumeDb(const QString &masterId) const;
    void SetMasterHeadphoneVolumeDb(const QString &masterId, double db);
    bool MasterHeadphoneMuted(const QString &masterId) const;
    void SetMasterHeadphoneMuted(const QString &masterId, bool muted);

    // ---- which microphone this was installed for ------------------------
    // Read once at startup from ~/.config/waveline/profile.conf and constant
    // for the life of the daemon: the profile is chosen when the software is
    // installed, not renegotiated when a device is unplugged. Presence is a
    // separate question, and DeviceConnected() below is the one that answers it.
    QString DeviceProfileId() const;
    // What to put in front of names in the UI: "Wave:3", or "Waveline".
    QString DeviceBrand() const;
    // False when this microphone has no vendor protocol. Everything in the
    // next section is then inert, and the GUI drops its hardware panel rather
    // than showing dead controls.
    bool HasHardwareControls() const;

    // ---- vendor hardware, on microphones that have any -------------------
    bool DeviceConnected() const;
    QString DeviceFirmware() const;
    bool Clipguard() const;
    void SetClipguard(bool on);
    int HardwareMonitor() const;          // percent of your own voice
    void SetHardwareMonitor(int percent);
    bool MicMuted() const;
    void SetHardwareMicMute(bool muted);
    double MicGainDb() const;
    void SetMicGainDb(double db);
    double HeadphoneVolumeDb() const;
    // The headphone jack on the microphone, -60..0 dB. Restored on replug: the
    // firmware forgets it whenever the device loses bus power.
    void SetHeadphoneVolumeDb(double db);
    // Mutes the headphone jack on the microphone, via the vendor config block.
    bool HeadphoneMuted() const;
    void SetHeadphoneMuted(bool muted);

    // ---- sound sharing --------------------------------------------------
    bool SoundSharingEnabled() const;
    void SetSoundSharingEnabled(bool on);
    double SoundSharingVolume(const QString &mix) const;
    void SetSoundSharingVolume(const QString &mix, double volume);
    bool SoundSharingMuted(const QString &mix) const;
    void SetSoundSharingMuted(const QString &mix, bool muted);
    // "nodeId\tapplication\tshared" rows, shared is 0 or 1.
    QStringList SoundSharingApps() const;
    void SetSoundSharingApp(uint nodeId, bool shared);
    // Where an application's audio joins a microphone: "" for not shared, a
    // channel id, or "mic" for the master. The app keeps playing wherever it
    // already was -- this adds it to the microphone, it does not move it.
    void SetSoundSharingAppTarget(uint nodeId, const QString &target);
    // "id\tlabel" rows for the targets that currently publish a microphone.
    QStringList SoundSharingTargets() const;
    // How loud a shared application is inside the microphone, 0..1. Its own
    // playback is untouched.
    double SoundSharingAppLevel(uint nodeId) const;
    void SetSoundSharingAppLevel(uint nodeId, double level);

    // ---- noise suppression ----------------------------------------------
    bool NoiseSuppression() const;
    void SetNoiseSuppression(bool on);
    // Master bypass for the microphone's whole effects chain (NC + EQ).
    bool MicEffectsEnabled() const;
    void SetMicEffectsEnabled(bool on);
    // Feed the Monitor mix from the processed chain instead of the dry tap.
    bool MicMonitorFx() const;
    void SetMicMonitorFx(bool on);
    // 0..1, how much of the denoised signal is used. See NoiseFilter.
    double NoiseIntensity() const;
    void SetNoiseIntensity(double value);
    double NoiseInputLevel() const;
    double NoiseOutputLevel() const;
    double SpeechProbability() const;

    // Which suppression model runs: "rnnoise" or "deepfilternet". One setting
    // for every NC filter in the graph -- see MixerGraph::setNoiseEngine.
    QString NoiseEngine() const;
    // Returns an empty string on success, or a message explaining why the
    // engine could not be selected. DeepFilterNet is loaded at runtime and is
    // frequently not installed, so this is an ordinary outcome rather than an
    // error worth raising on the bus; the previous engine keeps running.
    QString SetNoiseEngine(const QString &engine);
    // "id\tlabel\tavailable\treason" rows, one per engine, for the UI to
    // populate its selector with. Reporting availability from the daemon means
    // the GUI does not have to duplicate the library and model lookup.
    QStringList NoiseEngines() const;

    // Per-channel EQ, low-cut and optional noise suppression.
    // stage is "input" or "output". Tab-separated fx fields:
    // lowCut, lowCutHz, eq, lowDb, midDb, highDb, eqAdvanced, proEqBands.
    // Use channelId "mic" for global microphone EQ (after RNNoise).
    QString ChannelEffects(const QString &channelId, const QString &stage) const;
    void SetChannelEffects(const QString &channelId, const QString &stage,
                           bool lowCut, int lowCutHz, bool eq, double lowDb,
                           double midDb, double highDb);
    // The parametric ("Pro") EQ, set separately from the three-band one so that
    // touching either leaves the other alone. `advanced` chooses which of the
    // two the `eq` switch turns on; `bands` is the ten-band spec described in
    // engine/eqspec.h -- "on,type,freqTenthHz,gainTenthDb,qHundredths" per
    // band, joined by ';'. An empty or short spec falls back to the flat
    // default layout.
    void SetChannelProEq(const QString &channelId, const QString &stage, bool advanced,
                         const QString &bands);
    // Tab-separated dynamics fields for the input-device path.
    QString MicDynamics() const;
    void SetMicDynamics(bool gate, double gateThresholdDb, double gateAttackMs,
                        double gateReleaseMs, bool compressor, double compThresholdDb,
                        double compRatio, double compAttackMs, double compReleaseMs,
                        double compKneeDb, double makeupGainDb, bool autoMakeup,
                        bool limiter, double limitThresholdDb, double limitAttackMs,
                        double limitReleaseMs);
    QString ChannelDynamics(const QString &channelId, const QString &stage) const;
    void SetChannelDynamics(const QString &channelId, const QString &stage, bool gate,
                            double gateThresholdDb, double gateAttackMs,
                            double gateReleaseMs, bool compressor, double compThresholdDb,
                            double compRatio, double compAttackMs, double compReleaseMs,
                            double compKneeDb, double makeupGainDb, bool autoMakeup,
                            bool limiter, double limitThresholdDb, double limitAttackMs,
                            double limitReleaseMs);
    // App-audio sidechain ducking. Tab-separated:
    // enabled, intensity, thresholdDb, depthDb, attackMs, releaseMs, sources,
    // holdSec
    // Sources: master_mic|channel_mic:voice|channel_audio:game
    QString ChannelDucking(const QString &channelId) const;
    // holdSec (0..10) is how long the sidechain must stay quiet before the
    // duck releases.
    void SetChannelDucking(const QString &channelId, bool enabled, double intensity,
                           const QString &sources, double holdSec);
    // App-audio LUFS loudness limiter. Tab-separated: enabled, maxLufs
    QString ChannelLufsLimiter(const QString &channelId) const;
    void SetChannelLufsLimiter(const QString &channelId, bool enabled, double maxLufs);
    // See SetMasterCreativeFx for the wire format.
    QString ChannelCreativeFx(const QString &channelId, const QString &stage) const;
    void SetChannelCreativeFx(const QString &channelId, const QString &stage,
                              const QString &spec);
    bool ChannelNoiseSuppression(const QString &channelId,
                                 const QString &stage) const;
    void SetChannelNoiseSuppression(const QString &channelId, const QString &stage,
                                    bool on);
    double ChannelNoiseIntensity(const QString &channelId,
                                 const QString &stage) const;
    void SetChannelNoiseIntensity(const QString &channelId, const QString &stage,
                                   double value);
    // Master bypass for one channel's whole effects chain. Toggling it leaves
    // every per-stage setting untouched, so the chain comes back exactly as it
    // was rather than needing to be dialled in again.
    bool ChannelEffectsEnabled(const QString &channelId) const;
    void SetChannelEffectsEnabled(const QString &channelId, bool on);
    // Diagnostic: feed the Monitor mix from the FX chain instead of the sink's
    // dry monitor, so the effects are audible on headphones. Triggers a rewire.
    bool ChannelMonitorFx(const QString &channelId) const;
    void SetChannelMonitorFx(const QString &channelId, bool on);
    // Publishes "<brand> <name> Microphone" as a recording device, fed from the
    // raw hardware mic through this channel's input NC/EQ -- independent of the
    // input device's own effects. Triggers a rewire.
    bool ChannelMicSource(const QString &channelId) const;
    void SetChannelMicSource(const QString &channelId, bool on);
    // Software monitor of this channel's published microphone in the Monitor
    // mix. Only available while ChannelMicSource is on.
    bool ChannelMicMonitor(const QString &channelId) const;
    void SetChannelMicMonitor(const QString &channelId, bool on);
    // Gain and mute of this channel's own published microphone -- what the
    // application recording from "<brand> <name> Microphone" hears. Does not
    // touch the input devices or the mixes.
    double ChannelMicSend(const QString &channelId) const;
    void SetChannelMicSend(const QString &channelId, double level);
    bool ChannelMicMuted(const QString &channelId) const;
    void SetChannelMicMuted(const QString &channelId, bool muted);
    // Per-stage effect source: false = unique channel effects, true = master.
    bool ChannelEffectSourceMaster(const QString &channelId,
                                   const QString &stage) const;
    // Resolved master bus id, or empty when using unique channel effects.
    QString ChannelEffectSourceMasterId(const QString &channelId,
                                        const QString &stage) const;
    void SetChannelEffectSourceMaster(const QString &channelId, const QString &stage,
                                      const QString &masterId);

    // Signal actually flowing, as "key\tpeak" rows. Keys are channel ids plus
    // "monitor-mix" and "stream-mix". Peak amplitude, 0..1.
    QStringList Levels() const;

    // ---- instrument tuner --------------------------------------------------
    // Pitch detection lives here rather than in the GUI because the GUI links
    // no PipeWire and so cannot open a capture stream. It is deliberately not
    // part of the mixer graph: the tuner taps a device directly and touches
    // nothing that is being routed, so opening it cannot disturb a stream in
    // progress.
    //
    // "kind\tid\tlabel" rows, kind being "audio" or "midi".
    QStringList TunerSources() const;
    // Empty string on success, otherwise why not. Calling it again with a
    // different source switches over; only one tap exists at a time.
    QString TunerStart(const QString &kind, const QString &source);
    void TunerStop();
    bool TunerActive() const;
    // What is being heard right now, as
    // "frequencyHz\tconfidence\tlevel\tmidiNote\tbendCents". A single string
    // because the tuner window polls this ~30 times a second and five getters
    // would be five round trips per needle movement.
    QString TunerReading() const;
    // Plays the note at `hz` for `ms` into the Monitor mix, so the string can
    // be heard as well as read. Nothing happens when no tuner is running.
    void PlayTunerReference(double hz, int ms);

    // ---- routing ---------------------------------------------------------
    // "nodeId\tapplication\tchannel" per entry, so no custom type is needed.
    QStringList Apps() const;
    void MoveApp(uint nodeId, const QString &channelId);
    // Playback volume of an application stream, 0..1.5 (100% default).
    double AppVolume(uint nodeId) const;
    void SetAppVolume(uint nodeId, double volume);
    bool RoutingEnabled() const;
    void SetRoutingEnabled(bool on);

    // ---- soundboard -------------------------------------------------------
    // One row per sound, in play/display order:
    // "id\tname\tvolume\ttrimStartMs\ttrimEndMs\tdurationMs\tfile"
    // durationMs is -1 until the sound has decoded (see loadSoundboardBuffers,
    // which runs eagerly at startup and after every add/replace, so this is
    // ordinarily immediate).
    QStringList SoundboardSounds() const;
    // Where every sound plays and shares: "channelId\tshareTarget\t
    // shareVolume\tlocalVolume" -- one setting for the whole board, not per
    // sound, so a Stream Deck mashing buttons never has to think about
    // where each one happens to be routed. See SoundboardState.
    QString SoundboardSettings() const;
    void SetSoundboardChannel(const QString &channelId);
    // Where sounds join a microphone -- same convention as
    // SetSoundSharingAppTarget: "" for not shared, a channel id that
    // publishes a microphone, or "mic" for the primary input device's.
    void SetSoundboardShareTarget(const QString &target);
    void SetSoundboardShareVolume(double volume);
    void SetSoundboardLocalVolume(double volume);
    // Decodes `sourcePath` (.wav or .mp3), copies it into the daemon's own
    // soundboard directory, and adds it under a fresh 4-digit id -- the
    // wavelined-cli `--soundboard-play`/`--soundboard-stop` argument. Returns
    // the new id, or an empty string on failure (see LastError()).
    QString AddSoundboardSound(const QString &name, const QString &sourcePath,
                               int trimStartMs, int trimEndMs, double volume);
    // Renames/re-trims/re-levels an existing sound in place, keeping its id.
    // A non-empty newSourcePath re-imports from that file, replacing the
    // stored copy (for "re-record"/"pick a different file" in the edit
    // dialog); empty keeps the file that is already stored.
    bool UpdateSoundboardSound(const QString &id, const QString &name,
                               const QString &newSourcePath, int trimStartMs,
                               int trimEndMs, double volume);
    void RemoveSoundboardSound(const QString &id);
    // Rewrites the whole play/display order wholesale, the same way the
    // Virtual Rack's drag-to-reorder does. Any id this daemon does not
    // recognise is ignored; any sound not named is kept, appended at the end.
    void ReorderSoundboardSounds(const QStringList &idsInOrder);
    // Starts one more instance playing; sounds can overlap themselves and
    // each other. Empty string on success, otherwise why not. The reply is
    // delayed (QDBusContext::setDelayedReply) until the new voice's PipeWire
    // ports are actually linked, on a worker thread rather than this one --
    // see the .cpp for why: that linking wait is real (~100ms, not
    // instant), and answering it synchronously from here would leave the
    // whole daemon unable to answer any other D-Bus call, from any client,
    // for the duration.
    QString PlaySoundboardSound(const QString &id);
    void StopSoundboardSound(const QString &id);
    void StopAllSoundboardSounds();
    // Sound ids with at least one instance still playing right now, for the
    // panel to highlight and for a Stream Deck "is it playing" query.
    QStringList SoundboardPlayingIds() const;
    // "id\tprogress" rows, one per id SoundboardPlayingIds() would report,
    // progress 0..1 through its trimmed range -- what the panel draws its
    // mini waveform's playhead from. A separate call rather than folded into
    // SoundboardPlayingIds() itself: that one is also the plain "is it
    // playing" check onPlayRequested() and the CLI use, and both want a
    // bare id list rather than a row format to parse.
    QStringList SoundboardProgress() const;
    // Decodes `path` without adding it, for the trim editor: "durationMs\t
    // peak0,peak1,...". 400 peaks span the whole file. Empty string on
    // failure (see LastError()).
    QString AnalyzeSoundboardSource(const QString &path);
    // Plays `path` once, trimmed and leveled exactly as given, through the
    // System channel with no sharing -- a private audition, deliberately not
    // the board's own channel/sharing settings, so trying out a trim before
    // Save never plays into a microphone. Replaces any preview already playing.
    void PreviewSoundboardTrim(const QString &path, int trimStartMs, int trimEndMs,
                               double volume);
    void StopSoundboardPreview();

    // ---- output selection -------------------------------------------------
    // "node.name\tdescription" per entry.
    QStringList Outputs() const;
    QStringList MonitorOutputs() const;
    // Tab-separated per row: sink, volume, muted (0/1), online (0/1),
    // description. Index order is the Monitor fan-out order, which is what
    // "output 1", "output 2" mean everywhere else.
    QStringList MonitorOutputStates() const;
    QString MonitorOutput() const;
    void SetMonitorOutput(const QString &sinkName);
    void SetMonitorOutputAt(int index, const QString &sinkName);
    void AddMonitorOutput(const QString &sinkName);
    void RemoveMonitorOutput(int index);
    void SetMonitorOutputVolumeAt(int index, double volume);
    void SetMonitorOutputMutedAt(int index, bool muted);
    double StreamMixVolume() const;
    void SetStreamMixVolume(double volume);
    bool StreamMixMuted() const;
    void SetStreamMixMuted(bool muted);

    // ---- web companion ----------------------------------------------------
    // The tablet controller's server. Running is live state and autostart is a
    // setting, and the two are kept apart deliberately: stopping the server for
    // an evening must not mean turning the feature off for good.
    bool CompanionRunning() const;
    // The port it is on, or the one it would use -- so the panel has a number
    // to show whether or not the server is up.
    int CompanionPort() const;
    // Empty on success, otherwise why it could not bind. Changing the port
    // while the server is running moves it, which drops the clients on the old
    // one; nothing else can put them back.
    QString SetCompanionPort(int port);
    bool CompanionAutoStart() const;
    void SetCompanionAutoStart(bool on);
    QString CompanionStart();
    void CompanionStop();
    // "running\tport\tclients" plus one address per row after it, so the panel
    // gets the whole picture in a single round trip.
    QStringList CompanionStatus() const;

    // ---- profiles ---------------------------------------------------------
    QStringList Profiles() const;
    QString ActiveProfile() const;
    bool LoadProfile(const QString &name);
    void SaveProfile(const QString &name);
    bool DeleteProfile(const QString &name);
    bool RenameProfile(const QString &from, const QString &to);
    // Whether the live mixer still matches what is stored under `name`. False
    // when there is no such profile. Levels are compared with a tolerance: the
    // graph's own loopback gains drift in the last decimal place, and a
    // "you have unsaved changes" warning that is always true teaches people to
    // click through it.
    bool ProfileMatchesLive(const QString &name) const;
    // A profile as a self-contained JSON document, ready to be written to a
    // file. Empty when there is no such profile.
    //
    // The daemon hands back text rather than writing the file itself: it has no
    // display and no business running a file chooser, and the GUI is the only
    // side that knows where the user wants it. It also keeps the whole thing
    // reachable from gdbus, which is how the rest of this API is tested.
    QString ExportProfile(const QString &name) const;
    // Stores a document from ExportProfile() -- or a bare profile object --
    // under `name`, replacing any profile already called that. Does not change
    // which profile is active: importing is filing something away, not
    // switching to it. False when the text is not a profile at all.
    bool ImportProfile(const QString &name, const QString &json);
    QString ConfigPath() const;
    // Persist right now. Settings are also written automatically, debounced.
    void Save();
    // Stop any pending debounced write and persist immediately (shutdown path).
    void flushPendingSave();

    QString LastError() const { return lastError_; }

signals:
    // Emitted when anything above may have changed. Clients re-read what they
    // care about rather than the daemon shipping a large state blob.
    void Changed();

    // A recording stream started or stopped somewhere on the machine.
    //
    // Separate from Changed() because nothing about the *mixer* changed when it
    // fires, and because it fires on a completely different schedule: opening a
    // browser tab with a microphone permission is not a mixer edit. A shell
    // showing a "microphone in use" indicator wants this one and would
    // otherwise have to poll MicrophoneConsumers() forever to get it.
    void MicrophoneConsumersChanged();

private:
    struct MasterHwSlot {
        waveline::Device dev;
        waveline::State state;
        waveline::DeviceInfo info;
        bool connected = false;
        int settleTicks = 0;
        // Polls to skip before trying this device again, and how many attempts
        // in a row have failed. A device that has stopped answering costs a full
        // USB timeout per transfer, on the thread that also serves D-Bus, so
        // polling it at the full 250ms rate starves everything else.
        int backoffTicks = 0;
        int failStreak = 0;
        // Polls to wait after the usbfs node opens before the first control
        // transfer, and whether the identity read still owes us a round trip.
        // See kHwOpenGateTicks.
        int openGateTicks = 0;
        bool needInfo = false;
    };

    MasterBusState *masterBusState(Profile &p, const QString &id);
    const MasterBusState *masterBusState(const Profile &p, const QString &id) const;
    // Puts back the devices muteOutputsForShutdown() muted last time.
    void restoreOutputsMutedAtShutdown();
    void applyMasterBuses();
    // Starts the observation window that watches whether routing stuck.
    void scheduleRouteVerify();
    // Re-applies every routing decision after the session manager was replaced.
    // See PwEngine::setOnSessionManagerRestarted: a WirePlumber restart empties
    // the metadata all of them are written to, without adding or removing a
    // single node, so nothing else in here notices.
    void reapplyStreamRouting();
    // Everything that has to happen when hardware leaves, independent of how
    // we found out. Shared by the registry's removal event and the sweep below.
    void handleHardwareNodeGone(const waveline::PwNode &n);
private slots:
    // Resume from sleep, from logind's PrepareForSleep. A private slot on
    // purpose: main.cpp exports this object with ExportAllSlots, which takes
    // every *public* slot onto org.waveline.Mixer, and waking the machine is
    // not a mixer method.
    void onPrepareForSleep(bool goingToSleep);
    // An unplugged card whose PipeWire node is still in the graph will sit
    // there running and replay its last buffer for as long as anything is
    // linked to it. Nothing reports it, because no node was removed -- so this
    // checks /proc/asound instead of waiting to be told.
    void sweepDeadHardwareNodes();
private:
    // The other half of surviving a session manager restart. A new WirePlumber
    // rebuilds the graph to its own policy and destroys the links it did not
    // make -- which is every link between waveline's own nodes -- so each
    // channel sink is left feeding nothing at all. No node is added or removed
    // while that happens, so the registry callbacks are silent and the mixer
    // looks healthy: meters move, the monitor loopbacks are still attached to
    // the hardware, and not one sample reaches them. This is what made the
    // machine go quiet until wavelined itself was restarted.
    void healMixWiringAfterSessionRestart();
    // Counts one pass' worth of streams that did not stay put.
    void recordRouteBounces();
    // End of the ladder: promotes anything that bounced more than once into
    // routeConflicts_ and logs it.
    void settleRouteConflicts();

    void applyMasterFx(const QString &masterId);
    void applyMasterInputVolume(const QString &masterId);
    // Re-push Monitor/Stream mix levels after paths were recreated (rewire/rebuild).
    void applyMasterMixLevels(const QString &masterId);
    bool masterHasWave3Hw(const QString &masterId) const;
    // Readable name for the device a bus is configured to capture from, whether
    // or not it is plugged in right now. Present devices give their own
    // description; an absent one is named from its node name, so the picker can
    // keep showing the device the bus was set up with instead of falling back
    // to whatever else is connected. Callers pass the device lists they already
    // hold; MasterBuses() builds them once for every bus.
    // Which stored stage a (channelId, stage) pair addresses. Null when the
    // pair is the primary input device's microphone, which lives on the master
    // bus rather than in channelEffects.
    ChannelFxStageState *deEsserStage(const QString &channelId, const QString &stage);
    QString masterDeviceLabel(const MasterBusState &m, const QStringList &captures,
                              const QStringList &midis,
                              bool *connected = nullptr) const;
    // Records what each bus's device calls itself while it is plugged in, so
    // the name survives unplugging it. Returns whether anything changed.
    bool rememberMasterDeviceLabels();
    // Capture-node name -> measured capture delay in microseconds. Absent
    // means "not measured", which is not the same as zero and must never be
    // rendered as it.
    //
    // The figure is the median of recent readings of ALSA's own `delay` for
    // that PCM (engine/alsadelay.h), not anything derived from a setting. It
    // is capture-side: the device's own internal processing is not in it and
    // cannot be, so whatever shows it has to say so.
    QHash<QString, qint64> captureLatencies() const;
    // What MasterBuses() puts in the latency column for a device that does its
    // own processing. Distinct from -1 ("nothing to report yet") because the
    // two mean opposite things to a reader: -1 is a number that has not
    // arrived, this is a number that does not exist and never will.
    static constexpr qint64 kLatencyHidden = -2;
    // Points the probe at the PCMs behind the capture nodes that exist now, and
    // takes one reading of each. Driven from the same 4 Hz timer as the
    // hardware poll: two small procfs reads per device, and a rolling median
    // needs samples over time rather than a burst when someone asks.
    void sampleCaptureDelays();
    QString effectiveMasterCaptureMatch(const QString &masterId,
                                        const QString &configMatch) const;
    void pollMasterHardware(const QString &masterId);
    void restoreMasterHardwareState(const QString &masterId, waveline::State &fresh);
    QString nextMasterId() const;
    MasterHwSlot *masterHwSlot(const QString &masterId);
    const MasterHwSlot *masterHwSlot(const QString &masterId) const;
    void markMasterDisconnected(const QString &masterId);
    bool isCaptureDeviceNode(const std::string &name) const;
    bool isMidiDeviceNode(const waveline::PwNode &n) const;
    QString pickUnusedCaptureDevice() const;
    QString pickUnusedMidiDevice() const;

    void pollHardware();
    // Polls (250 ms each) for which saved hardware settings keep being
    // re-asserted after the device appears. A replug races WirePlumber, which
    // applies its own stored volume to the freshly registered ALSA card, so a
    // single write at first sight loses. Ten seconds is comfortably longer than
    // that takes to settle, and every write is comparison-guarded.
    static constexpr int kHwSettleTicks = 40;
    // Polls (250 ms each) to leave the device alone after its usbfs node first
    // opens. Opening that node moves nothing on the bus; the control transfers
    // do, and they are not serialised against the ones snd-usb-audio itself is
    // making while it enumerates the card -- usbfs traffic does not take
    // chip->mutex. On this firmware a control transfer arriving mid-probe is
    // how the device stops answering: every transfer after it times out, the
    // driver's next usb_set_interface() returns -ETIMEDOUT with chip->mutex
    // held, and the card is wedged until its port is power-cycled. The kernel
    // quirk paces the driver's own transfers (CTL_MSG_DELAY_1M, IFACE_DELAY)
    // and cannot see ours. Three seconds clears the probe burst; the cost of
    // being wrong in this direction is only that the hardware panel populates a
    // moment later.
    static constexpr int kHwOpenGateTicks = 12;
    bool monitorMasterMuted_ = false;
    // Re-picks the microphone when the user changes their default input, and
    // rewires if it changed. Always runs on this object's own thread; the
    // engine callback that triggers it does not.
    // Pushes the stored input gain and mute onto whichever hardware node the
    // graph is currently using. Cheap, and idempotent, so it is simply called
    // again after anything that could have changed which node that is.
    void applyMicInputVolume();  // primary master; calls applyMasterInputVolume("mic")
    void followDefaultSource();
    void updateStreamRouting();
    void scheduleRouteRetry();
    void scheduleRouteAll();
    static waveline::Mix parseMix(const QString &mix);
    static waveline::FxStage parseFxStage(const QString &stage);
    static waveline::ChannelFxSettings toFxSettings(const ChannelFxState &s);
    static ChannelFxState fromFxSettings(const waveline::ChannelFxSettings &s);
    static waveline::DynamicsSettings toDynamicsSettings(const DynamicsState &s);
    static DynamicsState fromDynamicsSettings(const waveline::DynamicsSettings &s);
    static waveline::DuckingSettings toDuckingSettings(const DuckingState &s);
    static waveline::LufsLimiterSettings toLufsLimiterSettings(const LufsLimiterState &s);
    static waveline::CreativeFxSettings toCreativeFxSettings(const CreativeFxState &s);
    static DuckingState fromDuckingSettings(const waveline::DuckingSettings &s);
    // Pushes the stored settings for one channel into its filters, or neutral
    // ones when the channel is bypassed. The config stays authoritative either
    // way -- the filters are derived state, never read back.
    void applyChannelFx(const QString &channelId);
    void applyMicFx();
    void applyMasterOutputFx();
    // Links every shared application into the microphone it was assigned to.
    void applySoundShareTargets();
    void applyAppVolume(uint nodeId);
    void applyAppVolumes();
    double appVolumeForName(const QString &app) const;

    // One application's own gain stage: a sink it plays into, and the loopback
    // that carries it to its channel and holds the level. Only apps whose
    // volume has actually been moved off 100% get one; see ensureAppGainStage.
    struct AppGainStage {
        std::string sinkName;    // waveline-app-<key>
        std::string pathHandle;  // app-gain-<key>
        QString channelId;       // the channel the loopback currently feeds
    };
    // Keyed by application display name, like Profile::appVolumes. Stages live
    // until the profile changes, not until the app quits: a browser drops and
    // recreates its stream every time playback pauses, and rebuilding the
    // stage around that would be exactly the churn this design avoids.
    QHash<QString, AppGainStage> appGainStages_;

    bool ensureAppGainStage(const QString &app);
    // Points every one of an app's live streams at its stage sink.
    void retargetAppToStage(const QString &app);
    // The channel an app's streams resolve to right now, or empty.
    QString appStageChannel(const QString &app) const;
    // Tears down every stage and rebuilds the ones the current profile asks
    // for. Profiles carry app volumes, so switching one has to move the graph.
    void rebuildAppGainStages();
    // Link or relink one application's microphone feed without touching the
    // rest of the graph.
    void applySoundShareTargetForApp(const QString &app);
    // The link half of the above, with no teardown: existing links survive, so
    // it is safe to retry while a restarting stream settles.
    void relinkSoundShareForApp(const QString &app);
    bool linkSoundShareNode(const waveline::PwNode &node, std::string &error);
    // Same node id *and* name. PipeWire recycles ids, so an id alone can name
    // something else entirely by the time a retry runs.
    bool streamStillPresent(const waveline::PwNode &node) const;
    // One gain stage per shared application. A plain port-to-port link has no
    // volume, so without this the only choice is full blast or nothing.
    waveline::GainFilter *shareGain(const QString &app);
    std::map<QString, std::unique_ptr<waveline::GainFilter>> shareGains_;
    QString appNameForNode(uint nodeId) const;
    // What loading a stored profile actually does to the mixer.
    //
    // A profile does not decide which input devices exist. It used to: loading
    // one replaced the whole bus list, so switching to a setup saved when you
    // owned one microphone tore down the other four, and tearing down a bus
    // means a full rewireGraph() -- every link in the graph dropped and rebuilt
    // around an 800 ms settle, which is audible as the mixer going silent for
    // about a second.
    //
    // So presence is live state and settings are profile state. A bus the
    // profile has never heard of is carried across untouched; a bus both sides
    // know about takes the profile's settings. Nothing is ever removed. In the
    // common case -- same devices, different levels and effects -- that leaves
    // the graph structurally identical and the switch costs a handful of volume
    // writes instead of a rebuild.
    struct ProfileLoadPlan {
        Profile profile;
        // Buses the profile has and the mixer does not. Creating a bus is
        // genuinely structural; this is the one case that still rewires.
        QStringList addedMasters;
        // Buses that keep their nodes but have to re-open a device, because the
        // profile points them somewhere else. Handled per bus, the same way the
        // device picker does it, so the other buses keep playing.
        QStringList recapturedMasters;
        // The settings whose own D-Bus setters do a targeted relink rather than
        // just writing a value -- a monitor path fed from the FX chain instead
        // of the dry sink, a channel published as a recording device. Applying
        // them is a handful of links on one path each, which is why the fast
        // path can afford to do exactly the ones that changed and nothing else.
        QStringList monitorPathChannels;
        QStringList micSourceChannels;
        QStringList monitorPathMasters;
        // A bus was carried across, or added. The merged result is written back
        // to the profile so the next load of it has nothing left to reconcile.
        bool migrated = false;
    };
    ProfileLoadPlan planProfileLoad(const Profile &stored) const;
    // Points one bus at the device its state now names, without touching the
    // rest of the graph.
    void relinkMasterDevice(const QString &masterId);
    // Runs the per-path relinks a plan asked for. Each one is what the matching
    // setter does on its own, so nothing here is a new way of changing the
    // graph -- only the same changes, driven from a profile.
    void applyProfileFixups(const ProfileLoadPlan &plan);
    // Push mix levels again, a few times, over the next half second. Cheap
    // (volume writes only) and worth it: applyProfile() can recreate a monitor
    // output's loopback, and a loopback comes up at unity until it is told
    // otherwise -- which on a monitor mix sitting at 17% is a very unpleasant
    // surprise.
    void reassertMixLevels();

    // Live state as a Profile, without touching anything stored.
    Profile snapshot() const;
    // Copies live state into the active profile, and back.
    void captureToProfile();
    void applyProfile();
    // The old global monitor-master gain folded into the per-output levels, so
    // each device's own slider is authoritative. Shared because two callers
    // need the same answer: applyProfile(), which does it for real, and the
    // has-this-drifted check, which has to compare live state against what
    // loading would produce rather than against what is sitting on disk.
    static void foldMonitorMaster(Profile &p);
    // Set by the last applyProfile(): did it have to build a node that is not
    // wired into anything yet. Only a rewire can finish the job, so a profile
    // load that turns on a channel filter for the first time cannot take the
    // fast path.
    bool applyCreatedNodes_ = false;
    void scheduleSave();
    void scheduleRewire();
    // Re-push every Monitor output's stored level after its loopback was
    // recreated. Needed wherever a real output device comes and goes --
    // notably a WirePlumber restart -- because a fresh loopback plays at
    // unity until a level reaches the node the registry now resolves.
    void scheduleMonitorLevelReassert();
    // ALSA namehints for Audacity / Wine — mirrors live published sources.
    void syncAlsaAliases();
    // Empty ids = every master (cold start). Otherwise only those masters.
    // False when there was nothing to settle -- no graph yet, or no master bus
    // to rebuild. Callers that must not lose the rebuild (the startup quantum
    // assert) retry on that; everyone else ignores it.
    bool scheduleCaptureSettle(const QStringList &masterIds = {});
    // Capture devices that wireMicPaths() skipped as absent but that are in the
    // registry by the time the rewire finishes. Run once on a successful
    // rewire, just as hotplug is armed.
    void wireCaptureDevicesThatAppeared();
    void rebuildCaptureHops();
    void finishMasterCaptureRebuild(const QString &masterId);
    void finishNewMidiMaster(const QString &masterId);
    void finishAllPendingMidi();
    QStringList mastersForCaptureNode(const QString &nodeName) const;
    void healDisconnectedMidiInputs();
    // Logs the PipeWire server's descriptor usage when it is near its limit.
    // Past that limit the server cannot create nodes, and the graph fails in
    // ways that look like anything but a resource problem, so say so plainly.
    void warnIfDescriptorsExhausted();
    // Logs any path PipeWire is not scheduling, and why it probably happened.
    void reportStalledPaths(int atMs);
    // The tuner's MIDI tap is linked by hand -- see Tuner::midiNodeName() for
    // why -- which means it is also this class's job to put the link back
    // after anything that clears manual links.
    bool linkTunerMidi(const QString &source, std::string &error);
    void relinkTunerMidi();

    void rewireGraph();
    void rewireChannelMonitorPath(const QString &channelId);
    void rewireMicMonitorPath();
    void syncMasterMeters(const QString &masterId = {});

    waveline::DeviceProfile profile_;
    waveline::PwEngine engine_;
    std::unique_ptr<waveline::MixerGraph> graph_;
    std::unique_ptr<waveline::AppRouter> router_;
    std::unique_ptr<waveline::SoundShareRouter> soundShare_;
    std::unique_ptr<waveline::SoundboardEngine> soundboard_;
    // Decoded audio for every stored sound, keyed by id. Loaded eagerly (see
    // loadSoundboardBuffers()) rather than on first play: soundboard clips
    // are short, decoding all of them costs nothing worth mentioning, and a
    // button that has to decode before it can make a sound is a button with
    // an audible delay the first time and no delay ever again -- worse than
    // either always fast or always slow.
    QHash<QString, std::shared_ptr<waveline::SoundboardBuffer>> soundboardBuffers_;
    void loadSoundboardBuffers();
    QString soundboardDataDir() const;
    SoundboardSoundState *findSoundboardSound(const QString &id);
    const SoundboardSoundState *findSoundboardSound(const QString &id) const;
    QTimer soundboardReapTimer_;
    std::unique_ptr<waveline::LevelProbe> meters_;
    // Created on the first TunerStart and torn down on TunerStop, so a daemon
    // nobody is tuning against holds no capture stream open.
    std::unique_ptr<waveline::Tuner> tuner_;
    // Always constructed, listening only when asked to. Holding it either way
    // means CompanionStart() is a call on an object rather than a construction,
    // so the autostart path and the button do exactly the same thing.
    std::unique_ptr<CompanionServer> companion_;
    std::map<QString, MasterHwSlot> masterHw_;

    // Capture latency, measured. Mutable because the const D-Bus getters are
    // where a client's question arrives, and answering one must not require
    // the service to be non-const -- but the sampling that fills it is driven
    // by the timer, not by being asked, so a client polling faster or slower
    // cannot change the number it gets.
    mutable waveline::AlsaDelayProbe delayProbe_;

    // Coalesces MicrophoneConsumersChanged(). Streams appear and disappear in
    // bursts -- a browser opening a call creates several within a few hundred
    // milliseconds -- and a signal per node would make a shell rebuild its
    // panel five times for one event.
    QTimer micConsumerTimer_;
    void scheduleMicConsumerSignal();
    // Last set of node ids reported by MicrophoneConsumers(), so the debounce
    // stays quiet when a burst nets out to no change at all.
    QSet<uint> lastMicConsumers_;
    // Watches for a desktop shell on the bus. Held so ShellClientPresent() has
    // an answer without a blocking round trip on every call.
    std::unique_ptr<QDBusServiceWatcher> shellWatcher_;
    bool shellPresent_ = false;

    QTimer hwTimer_;
    // Watches for hardware that left without PipeWire saying so. Slow on
    // purpose: it reads /proc once a second and only ever matters in the
    // seconds after an unplug. See sweepDeadHardwareNodes().
    QTimer deadCardTimer_;
    // Nodes already dealt with by that sweep, so it acts once per unplug
    // rather than every tick. Keyed by node id: a replug is a new id.
    QSet<uint32_t> deadCardNodes_;
    QTimer saveTimer_;
    QTimer rewireTimer_;
    QTimer monitorLevelTimer_;
    int monitorLevelPasses_ = 0;
    QTimer midiWireTimer_;
    QTimer captureSettleTimer_;
    QSet<QString> pendingSettleMasters_;
    QSet<QString> captureHealOnAppear_;
    QStringList settleQueue_;
    QStringList settleBatch_;
    int settlePass_ = 0;
    bool captureHotplugArmed_ = false;
    QTimer routeRetryTimer_;
    int routeRetryLeft_ = 0;
    QTimer routeVerifyTimer_;
    int routeVerifyLeft_ = 0;
    // How many of the observation window's passes found a stream somewhere other
    // than the channel we had just put it on, keyed by node id.
    //
    // One pass is not evidence: a user moving a stream in pavucontrol, an
    // application reconnecting, or a link that has not settled all look the
    // same from here, and warning about any of those would be crying wolf.
    // Something that takes the stream back on pass after pass, every 400 ms,
    // is another program routing audio on a policy of its own -- and nothing
    // a person does by hand looks like that.
    QHash<uint32_t, int> routeBounces_;
    QHash<uint32_t, waveline::StreamContention> routeBounceDetail_;
    // Survives the ladder: what the mixer is told about, keyed by the sink the
    // streams are being taken to.
    QHash<QString, waveline::StreamContention> routeConflicts_;
    int rewireAttempts_ = 0;
    // Consecutive midiWireTimer_ ticks where a MIDI bus's audio chain would not
    // activate. Drives the chain rebuild in that timer.
    int midiWireStalls_ = 0;
    ConfigStore config_;
    bool routing_ = false;
    bool soundSharingEnabled_ = false;
    double soundShareStreamLevel_ = 1.0;
    double soundShareMonitorLevel_ = 1.0;
    bool soundShareStreamMuted_ = false;
    bool soundShareMonitorMuted_ = false;
    double micMonitorLevel_ = 1.0;
    double micStreamLevel_ = 1.0;
    bool micStreamMuted_ = false;
    bool micMonitorMuted_ = false;
    QString lastError_;
};

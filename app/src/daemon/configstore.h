// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Persistence for the mixer: ~/.config/waveline/config.json
//
// JSON rather than QSettings' INI, because the routing rules are a list of
// pairs and profiles are nested -- both awkward in INI and both things a user
// might reasonably want to edit by hand.
//
// Writes are atomic (temp file + rename). A mixer config is small and rewritten
// on every volume change, so a half-written file after a crash or a full disk
// is a real possibility, and losing the whole configuration to it would be a
// poor trade for a few saved milliseconds.

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct ChannelState {
    double streamVolume = 1.0;
    double monitorVolume = 1.0;
    bool streamMuted = false;
    bool monitorMuted = false;
};

struct RuleState {
    QString pattern;
    QString channel;
};

struct SoundSharingState {
    bool enabled = false;
    double streamVolume = 1.0;
    double monitorVolume = 1.0;
    bool streamMuted = false;
    bool monitorMuted = false;
    // application.name substrings, matched case-insensitively
    QStringList apps;
    // application.name -> where its audio joins a microphone: a channel id, or
    // "mic" for the master. Absent means not shared, which is the default.
    QHash<QString, QString> appTargets;
    // application.name -> how loud it is *in the microphone*, 0..1. Separate
    // from its own playback volume (Profile::appVolumes), which is the point:
    // a soundboard can sit quietly in the call while staying loud in your
    // headphones.
    QHash<QString, double> appGains;
};

struct DynamicsState {
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

struct DuckingSourceState {
    // master_mic, channel_mic, or channel_audio
    QString kind = QStringLiteral("master_mic");
    QString channelId;
};

struct DuckingState {
    bool enabled = false;
    QVector<DuckingSourceState> sources;
    double intensity = 0.75;
    double thresholdDb = -32.0;
    double depthDb = -18.0;
    double attackMs = 100.0;
    double releaseMs = 450.0;
    // Seconds of silence from the sidechain before the duck releases, 0..10.
    double holdSec = 3.0;
};

struct LufsLimiterState {
    bool enabled = false;
    double maxLufs = -18.0;
};

struct CreativeFxState {
    // The Creative FX pedalboard's wire format, from
    // engine/creativefxspec.h's encodeCreativeFx() -- fourteen effects times
    // four-to-seven parameters each is too much to keep as individual fields
    // here, so it is stored opaque and unpacked only where it is actually
    // used, the same way ChannelFxState::proEqBands stores the Pro EQ.
    // Empty means every effect at its default (off); see configstore.cpp's
    // creativeFromJson for the one-time upgrade from the pre-overhaul
    // per-field layout.
    QString spec;
};

struct ChannelFxState {
    bool lowCut = false;
    int lowCutHz = 80;   // 80 or 120
    bool eq = false;
    double lowDb = 0.0;
    double midDb = 0.0;
    double highDb = 0.0;
    // The parametric EQ. `eqAdvanced` picks which of the two curves the `eq`
    // switch turns on; the other one is kept, not discarded, so a user who
    // looks at the advanced panel and goes back still has their three bands.
    // Stored in the wire format from engine/eqspec.h rather than unpacked:
    // profiles are copied, compared and diffed all over this file, and one
    // opaque string does all of that for free.
    bool eqAdvanced = false;
    QString proEqBands;   // empty = the flat default layout
};

struct ChannelFxStageState {
    ChannelFxState fx;
    DynamicsState dynamics;
    CreativeFxState creativeFx;
    bool noiseSuppression = false;
    double noiseIntensity = 1.0;
    // De-esser. Kept beside noise suppression rather than inside DynamicsState
    // because it is set through a switch and a slider of its own, and the
    // dynamics setter replaces that whole block in one call -- a de-esser
    // stored in there would be wiped every time the compressor was touched.
    bool deEsser = false;
    double deEsserIntensity = 0.5;
};

struct MasterBusState {
    QString id = QStringLiteral("mic");
    // "capture" for microphones, "midi" for FluidSynth instruments.
    QString busType = QStringLiteral("capture");
    QString name;
    QString captureMatch;
    QString midiPortMatch;
    // What that device calls itself, remembered from the last time it was
    // plugged in. A node's description only exists while the node does, and a
    // picker that forgets it the moment the device is unplugged has nothing
    // left to show but the node name.
    QString deviceLabel;
    QString soundfontPath;
    QStringList soundfontPaths;
    bool nameCustom = false;
    ChannelState mix;
    ChannelFxState micFx;
    DynamicsState micDynamics;
    CreativeFxState micCreativeFx;
    // The Virtual Rack's own Creative FX configuration for this device --
    // deliberately separate from micCreativeFx above rather than a mirror of
    // it, so Rack Mode can swap which one actually drives the mic input
    // chain without either overwriting the other. See rackMode below.
    CreativeFxState rackCreativeFx;
    // When true, the mic input chain's effective Creative FX settings come
    // from rackCreativeFx instead of micCreativeFx -- the Rack window's
    // "Rack Mode" switch. Off means the Microphone > Creative tab drives it,
    // same as before this existed.
    bool rackMode = false;
    bool micEffectsEnabled = true;
    // Monitor fed from the FX chain rather than the dry tap: what you hear is
    // what the mixes get. AddMasterBusEx() sets this explicitly too, so the
    // default a new device arrives with does not depend on this line.
    bool micMonitorFx = true;
    bool noiseSuppression = true;
    double noiseIntensity = 1.0;
    bool deEsser = false;
    double deEsserIntensity = 0.5;
    // Software monitor of this master's microphone in the Monitor mix.
    bool softwareMonitor = false;
    bool micStereo = true;
    double micInputVolume = 1.0;
    bool micInputMuted = false;
    int hardwareMonitor = -1;
    double micGainDb = -1.0;
    int hwClipguard = -1;
    int hwMicMuted = -1;
    double hwHpVolumeDb = 1.0;
    int hwHpMuted = -1;
};

struct ChannelEffectsState {
    ChannelFxStageState input;
    ChannelFxStageState output;
    // Which master buses supply capture for this channel's mic chain. More than
    // one is allowed: they sum into a single published recording device, which
    // is how two microphones (or a mic and a MIDI instrument) end up on one
    // channel. Never empty -- the first entry is the one legacy profiles and the
    // single-value D-Bus calls see.
    QStringList masterMicIds{QStringLiteral("mic")};

    QString primaryMasterMicId() const {
        return masterMicIds.isEmpty() ? QStringLiteral("mic") : masterMicIds.first();
    }
    // Gain and mute of this channel's own published microphone. Stored under
    // new keys because the old micSend meant something else entirely (a send
    // into the Stream mix) and its saved values would read as "silent" here.
    double micGain = 1.0;
    bool micMuted = false;
    // Master bypass for the whole channel chain. The per-stage settings above
    // stay exactly as the user left them while this is false -- turning it back
    // on restores the chain rather than rebuilding it from defaults.
    bool effectsEnabled = true;
    // Send the FX chain's output to the Monitor mix as well as the Stream mix,
    // so the headphones hear the effects. On by default: a channel whose
    // effects you cannot hear is a channel you cannot set up.
    bool monitorFx = true;
    // Publish this channel's mic chain as a recording device. Off by default.
    bool micSource = false;
    // Software monitor of that published microphone in the Monitor mix.
    bool micMonitor = false;
    // Sidechain ducking on app audio (output chain only).
    DuckingState ducking;
    // LUFS loudness limiter on app audio (output chain only).
    LufsLimiterState lufsLimiter;
    // When true, this stage inherits the master channel's effects instead of
    // the per-channel settings above. Unique settings are kept in the profile.
    bool inputUseMasterEffects = false;
    bool outputUseMasterEffects = false;
    // Stable master bus id ("mic", "master-1", …) for effect-source inheritance.
    // Empty means unique per-channel effects. Invalid ids resolve to "mic".
    QString inputEffectSourceMasterId;
    QString outputEffectSourceMasterId;
    // Third input-stage mode: take each input device's own processed output and
    // sum that into the published microphone, instead of running this channel's
    // mic chain at all. The point is several devices with *different* effects on
    // one recording device -- copying one device's settings (the field above)
    // cannot express that, because the channel has only one chain.
    bool inputUseDeviceFx = false;
};

// How one card looks, when the user has said. Keyed by card: "channel:game",
// "master:master-1". Empty fields mean "whatever the theme picks", so a card
// the user has never touched carries no state at all and follows the palette
// if that ever changes.
//
// The icon is a name, not a file: bundled icons live in the mixer's resources
// and the user's own SVGs in ~/.config/waveline/icons, both looked up by stem.
// Storing a path would break the moment the file moved.
struct CardAppearanceState {
    QString color;   // "#rrggbb"
    QString icon;    // icon name, e.g. "piano"
};

// One saved configuration. "Profile" in the Wave Link sense: a whole mixer
// setup you can switch between.
struct MonitorOutputState {
    QString sink;
    QString description;
    double volume = 1.0;
    bool muted = false;
};

struct Profile {
    QHash<QString, ChannelState> channels;
    ChannelState mic;
    QList<MonitorOutputState> monitorOutputs;
    bool noiseSuppression = true;
    bool routingEnabled = true;
    bool softwareMonitor = true;
    double monitorLevel = 1.0;    // the microphone's level in the Monitor mix
    double monitorMaster = 1.0;   // the whole Monitor mix on its way out
    // Master mute for the Monitor mix. Stored separately from monitorMaster so
    // unmuting returns to the level you had rather than to full scale.
    bool monitorMasterMuted = false;
    // Master level and mute on the Stream mix virtual device itself.
    double streamMixVolume = 1.0;
    bool streamMixMuted = false;
    double noiseIntensity = 1.0;
    // Which noise suppression model runs, for every NC filter in the graph.
    // "rnnoise" or "deepfilternet"; see engine/denoiser.h.
    //
    // DeepFilterNet is the default because it is markedly better on the steady
    // broadband noise people actually complain about -- fans, keyboards,
    // traffic -- and install.sh builds it. Choosing it here is safe even when
    // it is absent: NoiseFilter::start() falls back to RNNoise rather than
    // failing, so the graph always comes up.
    QString noiseEngine = QStringLiteral("deepfilternet");
    // Default on: a mono microphone left unremixed is audible in one ear only,
    // which is a defect for almost everyone.
    bool micStereo = true;
    // The input device's own gain and mute, for microphones with no vendor
    // protocol -- where the capture node's volume is the only preamp control
    // there is. Applied to whatever hardware node the graph is using, so it
    // follows the microphone when the default input changes.
    double micInputVolume = 1.0;
    bool micInputMuted = false;
    // Settings that live in the Wave:3 itself. All use a "-1 means we have
    // never seen this device, leave it as it is" convention, so a fresh install
    // does not stamp defaults over however the user had the hardware set up.
    //
    // These are restored whenever the device reappears -- replug, resume, or a
    // reboot -- because the vendor config block does not reliably survive
    // losing bus power, and a Wave:3 that comes back with its monitor mix at
    // zero and its mute ring off is both surprising and, for a live mic, worse
    // than surprising.
    int hardwareMonitor = -1;   // -1 = leave the device alone
    double micGainDb = -1.0;    // -1 = leave hardware gain alone on load
    int hwClipguard = -1;       // -1 unknown, 0 off, 1 on
    int hwMicMuted = -1;        // -1 unknown, 0 unmuted, 1 muted
    // Headphone jack level in dB. The real range is -60..0, so any positive
    // value means "never seen a device"; 1.0 is the unset marker.
    double hwHpVolumeDb = 1.0;
    int hwHpMuted = -1;         // -1 unknown, 0 unmuted, 1 muted
    SoundSharingState soundSharing;
    ChannelFxState micFx;
    DynamicsState micDynamics;
    CreativeFxState micCreativeFx;
    // Master bypass for the microphone's whole effects chain (NC + EQ). The
    // per-stage settings above stay exactly as the user left them while this
    // is false -- turning it back on restores the chain rather than rebuilding
    // it from defaults.
    bool micEffectsEnabled = true;
    // When software monitoring is on: feed the Monitor mix from the processed
    // chain instead of the dry tap, so headphones hear the effects. On by
    // default -- hearing the chain is how you tell it is doing anything.
    bool micMonitorFx = true;
    // App-audio processing template for channels that use Input Device Effects
    // on the App Audio tab. Not wired as its own graph path — channels mirror it.
    ChannelFxStageState masterOutput;
    DuckingState masterOutputDucking;
    LufsLimiterState masterOutputLufsLimiter;
    QHash<QString, ChannelEffectsState> channelEffects;
    QList<MasterBusState> masterBuses;
    QList<RuleState> rules;     // empty = use the built-in defaults
    // Stable app identity -> channel. Set when the user picks a channel in the
    // Apps tab; survives restarts and new PipeWire node ids.
    QHash<QString, QString> appChannels;
    // application.name -> playback volume on the stream node, 0..1.5.
    // Absent means 1.0 (100%). Separate from soundSharing.appGains (mic share).
    QHash<QString, double> appVolumes;
    // Output sinks this daemon muted on its way out, so the next start can put
    // them back. Written to disk rather than kept in memory because the whole
    // point is to survive the daemon not being there.
    QStringList sinksMutedAtStop;
    // Card key -> colour and icon the user chose. See CardAppearanceState.
    QHash<QString, CardAppearanceState> cardAppearance;
    // Channel id -> the title the user typed. Display only: the PipeWire sink
    // keeps the name applications already route by, so renaming "Game" cannot
    // strand the applications sitting in it.
    QHash<QString, QString> channelNames;
};

// The Web Companion's server: a phone or tablet driving this mixer over the
// local network.
//
// Deliberately outside Profile. A profile is a mixer setup -- levels, effects,
// routing -- and switching one is something a companion client does *to itself*
// mid-session. If the port lived in there, loading a profile saved on another
// machine would move the server out from under every tablet already pointed at
// it, and the client asking for the switch would be the first to be cut off.
// The graph clock: how much audio moves per PipeWire scheduling tick, and so
// how much latency every path in the graph is charged before any device
// buffering is counted.
//
// Deliberately outside Profile, for the same reason CompanionSettings is and
// then one more. A profile is a mixer setup -- levels, effects, routing -- and
// people move between them mid-session; this is a property of the machine and
// what it can keep up with. Putting it in a profile would mean loading a
// profile saved on a fast desktop would drop a laptop's whole graph to a
// quantum it cannot service, and would make switching profiles a graph-wide
// audio event. Profiles carry device settings, never device policy.
struct AudioSettings {
    // Frames per graph cycle, written to PipeWire's clock.force-quantum.
    //
    // 0 means "leave it alone", and is the default on purpose: it is the only
    // value that does not take a global setting away from a user who set it
    // themselves. With 0 the graph runs on whatever the config files say --
    // 512 once 50-waveline-clock.conf is installed -- and Waveline reports
    // that number rather than imposing one.
    int graphQuantum = 0;
    // Frames of api.alsa.headroom for non-USB output devices, 0 for "do not
    // write a rule at all".
    //
    // Machine policy like graphQuantum above, and for the same reason: it
    // describes what this computer can keep up with under load, not what a
    // profile wants. Unlike graphQuantum it does not apply until WirePlumber
    // restarts, because headroom is fixed when a device is opened.
    //
    // 512 frames (10.7 ms at 48 kHz) by default: enough slack to keep a PCI
    // output from tearing under load, and small enough that monitoring still
    // feels immediate.
    int outputHeadroom = 512;
    // Whether the audio threads may be real-time (SCHED_RR) at all.
    //
    // Machine policy again, and the same reasoning: whether this computer wants
    // a hundred real-time threads on it is not a property of a mixing profile.
    //
    // On by default because it is what the graph is designed around -- see
    // engine/rtsched.h for why ordinary threads miss a 2.66 ms deadline. Off is
    // for the machine where that trade is wrong: real-time audio threads
    // starving something else that matters more, a shared or virtualised host,
    // or simply working out whether Waveline is what is stuttering.
    //
    // Read once, before the first PipeWire context exists, and turned into the
    // module.rt context property from there. Changing it cannot affect contexts
    // that already exist, which is why the mixer restarts the daemon to apply
    // it rather than pretending the switch is live.
    bool realtime = true;
};

// One Soundboard sound. Machine-wide (like CompanionSettings below) rather
// than part of Profile: a soundboard is a library of clips, not a mixer
// setting, and switching profiles must not make sounds disappear or their
// 4-digit ids (the CLI's `--soundboard-play`/`--soundboard-stop` argument)
// change under a Stream Deck button that already has one bound to it.
struct SoundboardSoundState {
    QString id;      // stable 4-digit string, e.g. "4821"
    QString name;
    // Absolute path to the daemon's own copy of the source file (see
    // MixerService::AddSoundboardSound), not the path it was imported from --
    // the original may move, get renamed, or live on removable media.
    QString file;
    double volume = 1.0;   // 0..2, the clip's own baked-in level
    int trimStartMs = 0;
    int trimEndMs = 0;     // 0 = play to the end
    // A small, comma-separated set of peak magnitudes (0..1) spanning the
    // whole file, computed once at add/replace time (see
    // MixerService::soundboardPeaksFor) so the panel can draw a mini
    // waveform per sound without decoding anything itself -- the GUI links
    // no audio, and re-decoding on every poll would not be free even if it
    // did.
    QString peaks;
};

struct SoundboardState {
    // Display/playback order is list order; ReorderSoundboardSounds rewrites
    // it wholesale, the same way the Virtual Rack's drag-to-reorder does.
    QList<SoundboardSoundState> sounds;

    // Where every sound plays and shares -- one setting for the whole board,
    // not per sound: a soundboard button is meant to be mashed without first
    // checking where it happens to be routed, so "where sounds go" is a
    // property of the board, set once at the top of the panel. "sfx" (the
    // built-in SFX channel) rather than "system": a soundboard clip is sound
    // effects almost by definition, and system is a poor default for
    // something meant to be routed deliberately.
    QString channelId = QStringLiteral("sfx");
    // Where sounds join a microphone: "" for not shared, a channel id that
    // publishes one, or "mic" for the primary input device's -- same
    // convention as SoundSharingState::appTargets.
    QString shareTarget;
    double shareVolume = 1.0;  // 0..1, "audio sharing" slider
    double localVolume = 1.0;  // 0..1.5, "what I hear" slider
};

struct CompanionSettings {
    // Start the server when the daemon does. Separate from whether it happens
    // to be running now: stopping it for an evening must not silently turn the
    // feature off for good.
    bool autoStart = false;
    // 1024..65535. Above the privileged range because wavelined is a user unit
    // and binding below 1024 is not something it can do or should want to.
    int port = 8787;
};

// Warnings the user has told us not to repeat. Machine-wide for the same
// reason AudioSettings is: which other audio software is installed on this
// machine is not a property of a mixing profile, and loading a profile saved
// elsewhere must not bring back a warning already dismissed here.
struct DiagnosticsSettings {
    // node.name of each sink whose stream-stealing the user has accepted.
    QStringList dismissedRoutingSinks;
    // Time every DSP stage and count the cycles it missed. Off by default and
    // remembered across restarts: it is cheap, but it is a measurement nobody
    // is reading unless they asked for it, and an investigation that survives
    // a `systemctl restart wavelined` is the whole point of persisting it.
    // See engine/dspprobe.h.
    bool dspProfiling = false;
};

// What a desktop shell's audio applet is allowed to show for this mixer.
//
// Machine-wide for the same reason AudioSettings is: which shell is installed
// is a property of the desktop, not of a mixing profile, and loading a profile
// exported from a machine running a different panel must not change what this
// one shows.
struct ShellSettings {
    // How many input devices, in MasterBuses() order, get a noise-suppression
    // switch in the shell's microphone panel.
    //
    // One by default. Every input device has its own noise suppression and all
    // of them are reachable in the mixer; this is only about how many are worth
    // a switch in a panel that has to stay glanceable. Raising it to 4 puts the
    // first four there, which is what a multi-microphone setup wants and what
    // a single-microphone one would find noise.
    //
    // 0 hides the switch entirely; anything above the number of input devices
    // simply means "all of them".
    int noiseSuppressionInputs = 1;
};

// Old auto-generated titles that must not stay pinned as nameCustom.
bool isLegacyAutoInputName(const QString &name);
void clearLegacyAutoInputNames(Profile &p);

class ConfigStore {
public:
    ConfigStore();

    // Missing or unreadable config is not an error: first run is the common
    // case and must produce a working default rather than a failure.
    bool load();
    bool save() const;

    QString path() const { return path_; }
    QString lastError() const { return lastError_; }

    QStringList profileNames() const;
    // Which named profile was last loaded. Informational only -- editing the
    // mixer does not write back into it.
    QString activeProfile() const { return active_; }

    // The working state. Autosaved continuously and restored at startup.
    //
    // Kept separate from the named profiles on purpose: when live state *was*
    // the active profile, every volume change after a "save as" silently
    // overwrote the profile you had just created, so switching back to it
    // returned whatever you had done since.
    Profile &live() { return live_; }
    const Profile &live() const { return live_; }

    // Machine-wide settings, saved beside the profiles rather than inside them.
    CompanionSettings &companion() { return companion_; }
    const CompanionSettings &companion() const { return companion_; }
    AudioSettings &audio() { return audio_; }
    const AudioSettings &audio() const { return audio_; }
    DiagnosticsSettings &diagnostics() { return diagnostics_; }
    const DiagnosticsSettings &diagnostics() const { return diagnostics_; }
    ShellSettings &shell() { return shell_; }
    const ShellSettings &shell() const { return shell_; }
    SoundboardState &soundboard() { return soundboard_; }
    const SoundboardState &soundboard() const { return soundboard_; }

    // Explicit snapshots.
    void saveAs(const QString &name, const Profile &p);
    // Copies a snapshot out and marks it active, so the label always names the
    // profile actually loaded rather than the last one saved.
    bool loadInto(const QString &name, Profile &out);
    bool remove(const QString &name);

    bool contains(const QString &name) const { return profiles_.contains(name); }
    // Marks a profile as the one live state came from, without copying it in.
    // Loading reconciles the stored profile against the input devices that
    // exist right now, so what lands in live() is not what loadInto() would
    // have handed over -- but it is still that profile that is loaded.
    void setActive(const QString &name);
    // Read-only peek at a stored snapshot; null when there is no such profile.
    // Export needs the profile *without* the side effects loadInto() has --
    // writing a file must not change which profile the mixer says it is on.
    const Profile *find(const QString &name) const;
    // Adds or replaces a snapshot, leaving the active profile alone. This is
    // what import is: a file arriving on disk does not mean the mixer you are
    // listening to has changed, and marking it active would claim it had.
    void put(const QString &name, const Profile &p);
    // Fails when `from` is missing, `to` is already taken, or either is empty.
    // Carries the active marker across, so renaming the profile you are on does
    // not leave the header saying you are on nothing.
    bool rename(const QString &from, const QString &to);

    // Public because import and export are the same serialisation the config
    // file uses -- a profile written to a file has to be readable by the same
    // code that reads it back, and duplicating it is how the two drift apart.
    static QJsonObject toJson(const Profile &p);
    static Profile fromJson(const QJsonObject &o);

private:
    QString path_;
    QString active_;
    Profile live_;
    CompanionSettings companion_;
    AudioSettings audio_;
    DiagnosticsSettings diagnostics_;
    ShellSettings shell_;
    SoundboardState soundboard_;
    QHash<QString, Profile> profiles_;
    mutable QString lastError_;
};

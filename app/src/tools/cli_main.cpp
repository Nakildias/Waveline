// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// wavelined-cli -- a command-line front end for org.waveline.Mixer, built for
// Stream Decks and keybinds rather than a terminal session. Every action is
// one flag plus positional arguments, so one button press is one command
// line with nothing to script around it: no subcommands to chain, no state
// to hold between invocations. See ../../../commands.md for the full list.
//
// A pure D-Bus client, same as the GUI: this links neither PipeWire nor the
// USB device layer, so it cannot fight wavelined for anything. It reuses
// engine/creativefxspec.h's encode/decode for the Creative FX pedalboard
// rather than reimplementing that wire format -- both are header-only and
// carry no Qt or PipeWire dependency, so including them here costs nothing
// and cannot drift from what the daemon actually parses.

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <cstdio>
#include <cstdlib>
#include <optional>

#include "engine/creativefx.h"
#include "engine/creativefxspec.h"

namespace {

// ============================================================== plumbing

[[noreturn]] void errorExit(const QString &msg) {
    std::fprintf(stderr, "wavelined-cli: %s\n", qPrintable(msg));
    std::exit(1);
}

QDBusMessage call(QDBusInterface &iface, const char *method,
                   const QVariantList &args = {}) {
    QDBusMessage reply =
        iface.callWithArgumentList(QDBus::Block, QString::fromLatin1(method), args);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        std::fprintf(stderr, "wavelined-cli: %s: %s\n", method,
                     qPrintable(reply.errorMessage()));
        std::exit(2);
    }
    return reply;
}

QVariant call1(QDBusInterface &iface, const char *method,
               const QVariantList &args = {}) {
    return call(iface, method, args).arguments().value(0);
}

QStringList getList(QDBusInterface &iface, const char *m, const QVariantList &a = {}) {
    return call1(iface, m, a).toStringList();
}
QString getStr(QDBusInterface &iface, const char *m, const QVariantList &a = {}) {
    return call1(iface, m, a).toString();
}
double getDouble(QDBusInterface &iface, const char *m, const QVariantList &a = {}) {
    return call1(iface, m, a).toDouble();
}
bool getBool(QDBusInterface &iface, const char *m, const QVariantList &a = {}) {
    return call1(iface, m, a).toBool();
}

const char *onOff(bool b) { return b ? "on" : "off"; }

int clampPct(int v) { return std::max(0, std::min(100, v)); }
double pctToUnit(int pct) { return clampPct(pct) / 100.0; }
int unitToPct(double u) { return int(std::lround(u * 100.0)); }

int toIntOrExit(const QString &s, const QString &what) {
    bool ok = false;
    int v = s.toInt(&ok);
    if (!ok) errorExit(QStringLiteral("'%1' is not a number (%2)").arg(s, what));
    return v;
}

QString requireMix(const QString &s) {
    if (s == QLatin1String("monitor") || s == QLatin1String("stream")) return s;
    errorExit(QStringLiteral("mix must be 'monitor' or 'stream', got '%1'").arg(s));
}

// ============================================================== targeting
//
// Every command that touches an input device or a channel takes one token:
// input:<n-or-id> or channel:<id>. "input1"/"input_2"/"input:mic" and
// "channel_voice"/"channel:voice" all parse the same way -- the separator is
// cosmetic. This is the one bit of the grammar every other flag shares, so
// getting it wrong once here is better than every command inventing its own
// rules for what a target string looks like.

enum class TargetKind { Input, Channel };
struct Target {
    TargetKind kind;
    QString rest;  // the part after "input"/"channel", not yet resolved to an id
};

std::optional<Target> parseTargetToken(const QString &tok) {
    static const QRegularExpression re(QStringLiteral("^(input|channel)[:_]?(.+)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(tok);
    if (!m.hasMatch()) return std::nullopt;
    const QString kind = m.captured(1).toLower();
    return Target{kind == QLatin1String("input") ? TargetKind::Input : TargetKind::Channel,
                 m.captured(2)};
}

Target requireTarget(const QString &tok) {
    auto t = parseTargetToken(tok);
    if (!t)
        errorExit(QStringLiteral("'%1' is not a target -- use input:<n> or channel:<id> "
                                 "(e.g. input:1, channel:voice)")
                      .arg(tok));
    return *t;
}

QString resolveInputId(QDBusInterface &iface, const QString &rest) {
    const QStringList buses = getList(iface, "MasterBuses");
    bool isNum = false;
    const int idx = rest.toInt(&isNum);
    if (isNum) {
        if (idx < 1 || idx > buses.size())
            errorExit(QStringLiteral("no such input: input:%1 (there %2 %3 input%4 -- run "
                                     "--list to see them)")
                          .arg(rest)
                          .arg(buses.size() == 1 ? "is" : "are")
                          .arg(buses.size())
                          .arg(buses.size() == 1 ? "" : "s"));
        return buses[idx - 1].split(QLatin1Char('\t')).value(0);
    }
    for (const QString &row : buses) {
        const QString id = row.split(QLatin1Char('\t')).value(0);
        if (id.compare(rest, Qt::CaseInsensitive) == 0) return id;
    }
    errorExit(QStringLiteral("unknown input '%1' -- run --list to see input ids").arg(rest));
}

QString resolveChannelId(QDBusInterface &iface, const QString &rest) {
    const QStringList chans = getList(iface, "Channels");
    for (const QString &row : chans) {
        const QString id = row.split(QLatin1Char('\t')).value(0);
        if (id.compare(rest, Qt::CaseInsensitive) == 0) return id;
    }
    for (const QString &row : chans) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.value(1).compare(rest, Qt::CaseInsensitive) == 0) return f.value(0);
    }
    errorExit(QStringLiteral("unknown channel '%1' -- run --list to see channel ids").arg(rest));
}

// ============================================================ tab formats
//
// Every *Dynamics and *Effects getter on org.waveline.Mixer hands back a
// tab-separated row whose fields are exactly the matching setter's
// parameters, in order (see mixerservice.h). Toggling one field means:
// read the row, flip one value, call the setter with everything else passed
// straight through unchanged.

struct DynFields {
    bool gate;
    double gateThr, gateAtk, gateRel;
    bool comp;
    double compThr, compRatio, compAtk, compRel, compKnee, makeup;
    bool autoMakeup;
    bool limiter;
    double limThr, limAtk, limRel;
};

DynFields parseDynamics(const QString &s) {
    const QStringList f = s.split(QLatin1Char('\t'));
    if (f.size() < 16) errorExit(QStringLiteral("daemon returned malformed dynamics state"));
    auto b = [&](int i) { return f[i].trimmed() != QLatin1String("0"); };
    auto d = [&](int i) { return f[i].toDouble(); };
    return DynFields{b(0),  d(1),  d(2),  d(3),  b(4),  d(5),  d(6),
                     d(7),  d(8),  d(9),  d(10), b(11), b(12), d(13),
                     d(14), d(15)};
}

QVariantList dynArgs(const DynFields &f) {
    return {f.gate,  f.gateThr, f.gateAtk, f.gateRel, f.comp,      f.compThr, f.compRatio,
            f.compAtk, f.compRel, f.compKnee, f.makeup, f.autoMakeup, f.limiter,
            f.limThr, f.limAtk, f.limRel};
}

struct EasyFx {
    bool lowCut;
    int lowCutHz;
    bool eq;
    double lowDb, midDb, highDb;
};

EasyFx parseEasyFx(const QString &s) {
    const QStringList f = s.split(QLatin1Char('\t'));
    if (f.size() < 6) errorExit(QStringLiteral("daemon returned malformed effects state"));
    return EasyFx{f[0].trimmed() != QLatin1String("0"), f[1].toInt(),
                 f[2].trimmed() != QLatin1String("0"),  f[3].toDouble(),
                 f[4].toDouble(),                       f[5].toDouble()};
}

QVariantList easyFxArgs(const EasyFx &f) {
    return {f.lowCut, f.lowCutHz, f.eq, f.lowDb, f.midDb, f.highDb};
}

// ---- Creative FX pedalboard: bitcrusher .. tape-saturator, plus the rack's
// own tone-stack EQ (named "creative-eq" here to keep it apart from the
// ordinary 3-band/Pro EQ that "eq" already means on the main chain).

bool *creativeFxFlag(waveline::CreativeFxSettings &s, const QString &name) {
    if (name == QLatin1String("bitcrusher")) return &s.bitcrusher.enabled;
    if (name == QLatin1String("overdrive")) return &s.overdrive.enabled;
    if (name == QLatin1String("chorus")) return &s.chorus.enabled;
    if (name == QLatin1String("flanger")) return &s.flanger.enabled;
    if (name == QLatin1String("phaser")) return &s.phaser.enabled;
    if (name == QLatin1String("tremolo")) return &s.tremolo.enabled;
    if (name == QLatin1String("delay")) return &s.delay.enabled;
    if (name == QLatin1String("reverb")) return &s.reverb.enabled;
    if (name == QLatin1String("creative-eq")) return &s.eq.enabled;
    if (name == QLatin1String("ringmod")) return &s.ringMod.enabled;
    if (name == QLatin1String("envelope-filter")) return &s.envFilter.enabled;
    if (name == QLatin1String("pitch-shifter")) return &s.pitch.enabled;
    if (name == QLatin1String("reverse-delay")) return &s.reverseDelay.enabled;
    if (name == QLatin1String("tape-saturator")) return &s.tapeSat.enabled;
    return nullptr;
}

const char *kEffectNames =
    "noise-suppression, de-esser, gate, compressor, limiter, eq, lowcut, "
    "bitcrusher, overdrive, chorus, flanger, phaser, tremolo, delay, reverb, "
    "creative-eq, ringmod, envelope-filter, pitch-shifter, reverse-delay, "
    "tape-saturator (and, on a channel's app-audio side, ducking, lufs-limiter)";

[[noreturn]] void unknownEffect(const QString &name) {
    errorExit(QStringLiteral("unknown effect '%1' -- valid names: %2").arg(name, kEffectNames));
}

// Reads spec via getMethod(idArgs), flips `effect`'s enabled flag, writes it
// back via setMethod(idArgs..., newSpec). Exits with unknownEffect() if the
// name is not a Creative FX module -- callers only reach this after every
// other known module name has already been ruled out.
bool toggleCreativeFx(QDBusInterface &iface, const char *getMethod, const char *setMethod,
                      const QVariantList &idArgs, const QString &effect) {
    const QString spec = getStr(iface, getMethod, idArgs);
    waveline::CreativeFxSettings s = waveline::decodeCreativeFx(spec.toStdString());
    bool *flag = creativeFxFlag(s, effect);
    if (!flag) unknownEffect(effect);
    *flag = !*flag;
    QVariantList args = idArgs;
    args << QString::fromStdString(waveline::encodeCreativeFx(s));
    call(iface, setMethod, args);
    return *flag;
}

// ============================================================ effect toggles

QString toggleMasterEffectModule(QDBusInterface &iface, const QString &masterId,
                                 const QString &effect) {
    if (effect == QLatin1String("noise-suppression")) {
        const bool cur = getBool(iface, "MasterNoiseSuppression", {masterId});
        call(iface, "SetMasterNoiseSuppression", {masterId, !cur});
        return onOff(!cur);
    }
    if (effect == QLatin1String("de-esser")) {
        const bool cur = getBool(iface, "MasterDeEsser", {masterId});
        call(iface, "SetMasterDeEsser", {masterId, !cur});
        return onOff(!cur);
    }
    if (effect == QLatin1String("gate") || effect == QLatin1String("compressor") ||
        effect == QLatin1String("limiter")) {
        DynFields f = parseDynamics(getStr(iface, "MasterMicDynamics", {masterId}));
        bool *t = effect == QLatin1String("gate")   ? &f.gate
                  : effect == QLatin1String("compressor") ? &f.comp
                                                          : &f.limiter;
        *t = !*t;
        QVariantList args{masterId};
        args += dynArgs(f);
        call(iface, "SetMasterMicDynamics", args);
        return onOff(*t);
    }
    if (effect == QLatin1String("eq") || effect == QLatin1String("lowcut")) {
        EasyFx f = parseEasyFx(
            getStr(iface, "MasterChannelEffects", {masterId, QStringLiteral("input")}));
        bool *t = effect == QLatin1String("eq") ? &f.eq : &f.lowCut;
        *t = !*t;
        QVariantList args{masterId, QStringLiteral("input")};
        args += easyFxArgs(f);
        call(iface, "SetMasterChannelEffects", args);
        return onOff(*t);
    }
    const bool now = toggleCreativeFx(iface, "MasterCreativeFx", "SetMasterCreativeFx",
                                      {masterId}, effect);
    return onOff(now);
}

QString toggleChannelEffectModule(QDBusInterface &iface, const QString &channelId,
                                  const QString &stage, const QString &effect) {
    if (effect == QLatin1String("noise-suppression")) {
        const bool cur = getBool(iface, "ChannelNoiseSuppression", {channelId, stage});
        call(iface, "SetChannelNoiseSuppression", {channelId, stage, !cur});
        return onOff(!cur);
    }
    if (effect == QLatin1String("de-esser")) {
        const bool cur = getBool(iface, "ChannelDeEsser", {channelId, stage});
        call(iface, "SetChannelDeEsser", {channelId, stage, !cur});
        return onOff(!cur);
    }
    if (effect == QLatin1String("gate") || effect == QLatin1String("compressor") ||
        effect == QLatin1String("limiter")) {
        DynFields f = parseDynamics(getStr(iface, "ChannelDynamics", {channelId, stage}));
        bool *t = effect == QLatin1String("gate")   ? &f.gate
                  : effect == QLatin1String("compressor") ? &f.comp
                                                          : &f.limiter;
        *t = !*t;
        QVariantList args{channelId, stage};
        args += dynArgs(f);
        call(iface, "SetChannelDynamics", args);
        return onOff(*t);
    }
    if (effect == QLatin1String("eq") || effect == QLatin1String("lowcut")) {
        EasyFx f = parseEasyFx(getStr(iface, "ChannelEffects", {channelId, stage}));
        bool *t = effect == QLatin1String("eq") ? &f.eq : &f.lowCut;
        *t = !*t;
        QVariantList args{channelId, stage};
        args += easyFxArgs(f);
        call(iface, "SetChannelEffects", args);
        return onOff(*t);
    }
    if (effect == QLatin1String("ducking")) {
        if (stage != QLatin1String("output"))
            errorExit(QStringLiteral("ducking only exists on the app-audio side"));
        const QStringList f = getStr(iface, "ChannelDucking", {channelId}).split(QLatin1Char('\t'));
        if (f.size() < 8) errorExit(QStringLiteral("daemon returned malformed ducking state"));
        const bool now = f[0].trimmed() == QLatin1String("0");
        call(iface, "SetChannelDucking", {channelId, now, f[1].toDouble(), f[6], f[7].toDouble()});
        return onOff(now);
    }
    if (effect == QLatin1String("lufs-limiter")) {
        if (stage != QLatin1String("output"))
            errorExit(QStringLiteral("lufs-limiter only exists on the app-audio side"));
        const QStringList f =
            getStr(iface, "ChannelLufsLimiter", {channelId}).split(QLatin1Char('\t'));
        const bool now = f.value(0).trimmed() == QLatin1String("0");
        call(iface, "SetChannelLufsLimiter", {channelId, now, f.value(1).toDouble()});
        return onOff(now);
    }
    const bool now = toggleCreativeFx(iface, "ChannelCreativeFx", "SetChannelCreativeFx",
                                      {channelId, stage}, effect);
    return onOff(now);
}

// ================================================================== usage

void printUsage() {
    std::puts(R"(wavelined-cli -- command-line control for wavelined (org.waveline.Mixer)

Targets:
  input:<n-or-id>       an input device, by 1-based position (input:1) or id (input:mic)
  channel:<id>          a channel, by id or display name (channel:voice, channel:game)

General:
  --list                                     list inputs, channels, outputs and their state
  --help                                     this text

Mute and volume (input: hardware gain/gate when mix is omitted, else that mix):
  --toggle-mute <target> [monitor|stream]
  --set-volume <target> <0-100> [monitor|stream]
  --offset-volume <target> <+-N> [monitor|stream]

A channel's own published microphone:
  --toggle-channel-mic <channel>
  --set-channel-mic <channel> <0-100>
  --offset-channel-mic <channel> <+-N>
  --toggle-channel-mic-monitor <channel>

Input device extras:
  --toggle-monitor <input>                   hear yourself (software monitor)
  --toggle-rack <input>                      Virtual Rack on/off for this input's mic chain

Effects (see commands.md for the full effect name list):
  --toggle-effects microphone|app-audio <target> [effect]
      no effect  -> bypass the whole chain
      an effect  -> toggle just that module (noise-suppression, gate, eq, ...)
  --toggle-rack-effect <input> <effect>      toggle one module inside the input's Rack

Soundboard:
  --soundboard-list                          list every sound, its id and where it's set to play
  --soundboard-play <id>                     play a sound (its 4-digit id, e.g. 4821)
  --soundboard-stop <id>                     stop every instance of a sound currently playing
  --soundboard-stop-all                      stop everything the soundboard is playing

Global output mixes:
  --toggle-output-monitor-mix <n>
  --set-output-monitor-mix <n> <0-100>
  --offset-output-monitor-mix <n> <+-N>
  --toggle-output-monitor-master
  --set-output-monitor-master <0-100>
  --offset-output-monitor-master <+-N>
  --toggle-output-stream-mix
  --set-output-stream-mix <0-100>
  --offset-output-stream-mix <+-N>
)");
}

// =================================================================== list

void cmdList(QDBusInterface &iface) {
    const QStringList buses = getList(iface, "MasterBuses");
    std::puts("Inputs:");
    for (int i = 0; i < buses.size(); ++i) {
        const QStringList f = buses[i].split(QLatin1Char('\t'));
        const QString id = f.value(0), name = f.value(1);
        const bool connected = f.value(7) == QLatin1String("1");
        const int gainPct = unitToPct(getDouble(iface, "MasterMicInputVolume", {id}));
        std::printf("  input:%d  id=%-10s name=%-20s connected=%-3s gain=%d%%\n", i + 1,
                    qPrintable(id), qPrintable(name), connected ? "yes" : "no", gainPct);
    }

    const QStringList chans = getList(iface, "Channels");
    std::puts("Channels:");
    for (const QString &row : chans) {
        const QStringList f = row.split(QLatin1Char('\t'));
        const QString id = f.value(0), name = f.value(1);
        std::printf("  channel:%-10s name=%-20s monitor=%3d%% stream=%3d%%\n", qPrintable(id),
                    qPrintable(name), unitToPct(f.value(3).toDouble()),
                    unitToPct(f.value(2).toDouble()));
    }

    const QStringList outs = getList(iface, "MonitorOutputStates");
    std::puts("Monitor outputs:");
    for (int i = 0; i < outs.size(); ++i) {
        const QStringList f = outs[i].split(QLatin1Char('\t'));
        std::printf("  output:%d  %s  volume=%d%% muted=%s\n", i + 1,
                    qPrintable(f.value(4, f.value(0))), unitToPct(f.value(1).toDouble()),
                    f.value(2) == QLatin1String("1") ? "yes" : "no");
    }

    std::printf("Stream mix: volume=%d%% muted=%s\n",
                unitToPct(getDouble(iface, "StreamMixVolume")),
                getBool(iface, "StreamMixMuted") ? "yes" : "no");
    std::printf("Monitor mix master: volume=%d%% muted=%s\n",
                unitToPct(getDouble(iface, "MonitorOutputVolume")),
                getBool(iface, "MonitorOutputMuted") ? "yes" : "no");
}

// =========================================================== mute / volume

void cmdToggleMute(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty())
        errorExit(QStringLiteral("usage: --toggle-mute <target> [monitor|stream]"));
    const Target t = requireTarget(a[0]);
    const QString mix = a.value(1);

    if (t.kind == TargetKind::Input) {
        const QString id = resolveInputId(iface, t.rest);
        if (mix.isEmpty()) {
            const bool cur = getBool(iface, "MasterMicInputMuted", {id});
            call(iface, "SetMasterMicInputMuted", {id, !cur});
            std::printf("input:%s hardware mute -> %s\n", qPrintable(id), onOff(!cur));
        } else {
            requireMix(mix);
            const bool cur = getBool(iface, "MasterMicMixMuted", {id, mix});
            call(iface, "SetMasterMicMixMuted", {id, mix, !cur});
            std::printf("input:%s %s mix mute -> %s\n", qPrintable(id), qPrintable(mix),
                        onOff(!cur));
        }
        return;
    }

    const QString id = resolveChannelId(iface, t.rest);
    if (mix.isEmpty())
        errorExit(QStringLiteral(
            "channel mute needs a mix: --toggle-mute channel:%1 monitor|stream").arg(t.rest));
    requireMix(mix);
    const bool cur = getBool(iface, "ChannelMuted", {id, mix});
    call(iface, "SetChannelMuted", {id, mix, !cur});
    std::printf("channel:%s %s mix mute -> %s\n", qPrintable(id), qPrintable(mix), onOff(!cur));
}

// Shared by --set-volume and --offset-volume: applies newPct to whatever
// (target, mix) resolves to and prints the confirmation line.
void applyVolume(QDBusInterface &iface, const Target &t, const QString &mix, int newPct) {
    newPct = clampPct(newPct);
    if (t.kind == TargetKind::Input) {
        const QString id = resolveInputId(iface, t.rest);
        if (mix.isEmpty()) {
            call(iface, "SetMasterMicInputVolume", {id, pctToUnit(newPct)});
            std::printf("input:%s device gain -> %d%%\n", qPrintable(id), newPct);
        } else {
            call(iface, "SetMasterMicVolume", {id, mix, pctToUnit(newPct)});
            std::printf("input:%s %s mix level -> %d%%\n", qPrintable(id), qPrintable(mix),
                        newPct);
        }
        return;
    }
    if (mix.isEmpty())
        errorExit(QStringLiteral("channel volume needs a mix: monitor|stream"));
    const QString id = resolveChannelId(iface, t.rest);
    call(iface, "SetChannelVolume", {id, mix, pctToUnit(newPct)});
    std::printf("channel:%s %s mix level -> %d%%\n", qPrintable(id), qPrintable(mix), newPct);
}

int currentVolumePct(QDBusInterface &iface, const Target &t, const QString &mix) {
    if (t.kind == TargetKind::Input) {
        const QString id = resolveInputId(iface, t.rest);
        return unitToPct(mix.isEmpty() ? getDouble(iface, "MasterMicInputVolume", {id})
                                       : getDouble(iface, "MasterMicVolume", {id, mix}));
    }
    if (mix.isEmpty())
        errorExit(QStringLiteral("channel volume needs a mix: monitor|stream"));
    const QString id = resolveChannelId(iface, t.rest);
    return unitToPct(getDouble(iface, "ChannelVolume", {id, mix}));
}

void cmdSetVolume(QDBusInterface &iface, const QStringList &a) {
    if (a.size() < 2)
        errorExit(QStringLiteral("usage: --set-volume <target> <0-100> [monitor|stream]"));
    const Target t = requireTarget(a[0]);
    const int pct = toIntOrExit(a[1], QStringLiteral("volume percent"));
    const QString mix = a.value(2);
    if (!mix.isEmpty()) requireMix(mix);
    applyVolume(iface, t, mix, pct);
}

void cmdOffsetVolume(QDBusInterface &iface, const QStringList &a) {
    if (a.size() < 2)
        errorExit(QStringLiteral("usage: --offset-volume <target> <+-N> [monitor|stream]"));
    const Target t = requireTarget(a[0]);
    const int delta = toIntOrExit(a[1], QStringLiteral("offset"));
    const QString mix = a.value(2);
    if (!mix.isEmpty()) requireMix(mix);
    applyVolume(iface, t, mix, currentVolumePct(iface, t, mix) + delta);
}

// ===================================================== channel's own mic

void cmdToggleChannelMic(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty()) errorExit(QStringLiteral("usage: --toggle-channel-mic <channel>"));
    const QString id = resolveChannelId(iface, requireTarget(a[0]).rest);
    const bool cur = getBool(iface, "ChannelMicMuted", {id});
    call(iface, "SetChannelMicMuted", {id, !cur});
    std::printf("channel:%s mic mute -> %s\n", qPrintable(id), onOff(!cur));
}

void cmdSetChannelMic(QDBusInterface &iface, const QStringList &a) {
    if (a.size() < 2)
        errorExit(QStringLiteral("usage: --set-channel-mic <channel> <0-100>"));
    const QString id = resolveChannelId(iface, requireTarget(a[0]).rest);
    const int pct = clampPct(toIntOrExit(a[1], QStringLiteral("volume percent")));
    call(iface, "SetChannelMicSend", {id, pctToUnit(pct)});
    std::printf("channel:%s mic level -> %d%%\n", qPrintable(id), pct);
}

void cmdOffsetChannelMic(QDBusInterface &iface, const QStringList &a) {
    if (a.size() < 2)
        errorExit(QStringLiteral("usage: --offset-channel-mic <channel> <+-N>"));
    const QString id = resolveChannelId(iface, requireTarget(a[0]).rest);
    const int delta = toIntOrExit(a[1], QStringLiteral("offset"));
    const int pct = clampPct(unitToPct(getDouble(iface, "ChannelMicSend", {id})) + delta);
    call(iface, "SetChannelMicSend", {id, pctToUnit(pct)});
    std::printf("channel:%s mic level -> %d%%\n", qPrintable(id), pct);
}

void cmdToggleChannelMicMonitor(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty())
        errorExit(QStringLiteral("usage: --toggle-channel-mic-monitor <channel>"));
    const QString id = resolveChannelId(iface, requireTarget(a[0]).rest);
    const bool cur = getBool(iface, "ChannelMicMonitor", {id});
    call(iface, "SetChannelMicMonitor", {id, !cur});
    std::printf("channel:%s mic monitor -> %s\n", qPrintable(id), onOff(!cur));
}

// ==================================================== input device extras

void cmdToggleMonitor(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty()) errorExit(QStringLiteral("usage: --toggle-monitor <input>"));
    const QString id = resolveInputId(iface, requireTarget(a[0]).rest);
    const bool cur = getBool(iface, "MasterSoftwareMonitor", {id});
    call(iface, "SetMasterSoftwareMonitor", {id, !cur});
    std::printf("input:%s software monitor -> %s\n", qPrintable(id), onOff(!cur));
}

void cmdToggleRack(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty()) errorExit(QStringLiteral("usage: --toggle-rack <input>"));
    const QString id = resolveInputId(iface, requireTarget(a[0]).rest);
    const bool cur = getBool(iface, "MasterRackMode", {id});
    call(iface, "SetMasterRackMode", {id, !cur});
    std::printf("input:%s rack mode -> %s\n", qPrintable(id), onOff(!cur));
}

// ============================================================ effects

void cmdToggleEffects(QDBusInterface &iface, const QStringList &a) {
    if (a.size() < 2)
        errorExit(QStringLiteral(
            "usage: --toggle-effects microphone|app-audio <target> [effect]"));
    const QString side = a[0];
    if (side != QLatin1String("microphone") && side != QLatin1String("app-audio"))
        errorExit(QStringLiteral("side must be 'microphone' or 'app-audio', got '%1'").arg(side));
    const Target t = requireTarget(a[1]);
    const QString effect = a.value(2);

    if (t.kind == TargetKind::Input) {
        if (side != QLatin1String("microphone"))
            errorExit(QStringLiteral(
                "input devices have no app-audio chain -- app-audio effects live on channels"));
        const QString id = resolveInputId(iface, t.rest);
        if (effect.isEmpty()) {
            const bool cur = getBool(iface, "MasterMicEffectsEnabled", {id});
            call(iface, "SetMasterMicEffectsEnabled", {id, !cur});
            std::printf("input:%s effects chain -> %s\n", qPrintable(id), onOff(!cur));
        } else {
            const QString now = toggleMasterEffectModule(iface, id, effect);
            std::printf("input:%s %s -> %s\n", qPrintable(id), qPrintable(effect),
                        qPrintable(now));
        }
        return;
    }

    const QString id = resolveChannelId(iface, t.rest);
    const QString stage = side == QLatin1String("microphone") ? QStringLiteral("input")
                                                               : QStringLiteral("output");
    if (effect.isEmpty()) {
        const bool cur = getBool(iface, "ChannelEffectsEnabled", {id});
        call(iface, "SetChannelEffectsEnabled", {id, !cur});
        std::printf(
            "channel:%s effects chain -> %s  (note: this one switch covers both the "
            "microphone and app-audio sides of the channel)\n",
            qPrintable(id), onOff(!cur));
    } else {
        const QString now = toggleChannelEffectModule(iface, id, stage, effect);
        std::printf("channel:%s (%s) %s -> %s\n", qPrintable(id), qPrintable(side),
                    qPrintable(effect), qPrintable(now));
    }
}

void cmdToggleRackEffect(QDBusInterface &iface, const QStringList &a) {
    if (a.size() < 2)
        errorExit(QStringLiteral("usage: --toggle-rack-effect <input> <effect>"));
    const Target t = requireTarget(a[0]);
    if (t.kind != TargetKind::Input)
        errorExit(QStringLiteral("the Virtual Rack exists on input devices, not channels"));
    const QString id = resolveInputId(iface, t.rest);
    const bool now = toggleCreativeFx(iface, "MasterRackCreativeFx", "SetMasterRackCreativeFx",
                                      {id}, a[1]);
    std::printf("input:%s rack %s -> %s\n", qPrintable(id), qPrintable(a[1]), onOff(now));
}

// ======================================================== output mixes

void cmdToggleOutputMonitorMix(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty()) errorExit(QStringLiteral("usage: --toggle-output-monitor-mix <n>"));
    const int n = toIntOrExit(a[0], QStringLiteral("output number"));
    const QStringList rows = getList(iface, "MonitorOutputStates");
    if (n < 1 || n > rows.size())
        errorExit(QStringLiteral("no such output: %1 (there are %2)").arg(n).arg(rows.size()));
    const bool cur = rows[n - 1].split(QLatin1Char('\t')).value(2) == QLatin1String("1");
    call(iface, "SetMonitorOutputMutedAt", {n - 1, !cur});
    std::printf("output:%d mute -> %s\n", n, onOff(!cur));
}

void cmdSetOutputMonitorMix(QDBusInterface &iface, const QStringList &a) {
    if (a.size() < 2)
        errorExit(QStringLiteral("usage: --set-output-monitor-mix <n> <0-100>"));
    const int n = toIntOrExit(a[0], QStringLiteral("output number"));
    const QStringList rows = getList(iface, "MonitorOutputStates");
    if (n < 1 || n > rows.size())
        errorExit(QStringLiteral("no such output: %1 (there are %2)").arg(n).arg(rows.size()));
    const int pct = clampPct(toIntOrExit(a[1], QStringLiteral("volume percent")));
    call(iface, "SetMonitorOutputVolumeAt", {n - 1, pctToUnit(pct)});
    std::printf("output:%d volume -> %d%%\n", n, pct);
}

void cmdOffsetOutputMonitorMix(QDBusInterface &iface, const QStringList &a) {
    if (a.size() < 2)
        errorExit(QStringLiteral("usage: --offset-output-monitor-mix <n> <+-N>"));
    const int n = toIntOrExit(a[0], QStringLiteral("output number"));
    const QStringList rows = getList(iface, "MonitorOutputStates");
    if (n < 1 || n > rows.size())
        errorExit(QStringLiteral("no such output: %1 (there are %2)").arg(n).arg(rows.size()));
    const int delta = toIntOrExit(a[1], QStringLiteral("offset"));
    const int cur = unitToPct(rows[n - 1].split(QLatin1Char('\t')).value(1).toDouble());
    const int pct = clampPct(cur + delta);
    call(iface, "SetMonitorOutputVolumeAt", {n - 1, pctToUnit(pct)});
    std::printf("output:%d volume -> %d%%\n", n, pct);
}

void cmdToggleOutputMonitorMaster(QDBusInterface &iface) {
    const bool cur = getBool(iface, "MonitorOutputMuted");
    call(iface, "SetMonitorOutputMuted", {!cur});
    std::printf("monitor mix master mute -> %s\n", onOff(!cur));
}

void cmdSetOutputMonitorMaster(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty()) errorExit(QStringLiteral("usage: --set-output-monitor-master <0-100>"));
    const int pct = clampPct(toIntOrExit(a[0], QStringLiteral("volume percent")));
    call(iface, "SetMonitorOutputVolume", {pctToUnit(pct)});
    std::printf("monitor mix master volume -> %d%%\n", pct);
}

void cmdOffsetOutputMonitorMaster(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty()) errorExit(QStringLiteral("usage: --offset-output-monitor-master <+-N>"));
    const int delta = toIntOrExit(a[0], QStringLiteral("offset"));
    const int pct = clampPct(unitToPct(getDouble(iface, "MonitorOutputVolume")) + delta);
    call(iface, "SetMonitorOutputVolume", {pctToUnit(pct)});
    std::printf("monitor mix master volume -> %d%%\n", pct);
}

// ============================================================== soundboard

void cmdSoundboardList(QDBusInterface &iface) {
    // Settings: "channelId\tshareTarget\tshareVolume\tlocalVolume" -- one
    // board-wide setting, not per sound; see MixerService::SoundboardSettings.
    const QStringList settings = getStr(iface, "SoundboardSettings").split(QLatin1Char('\t'));
    std::printf("Soundboard -- channel=%s share=%s\n",
                qPrintable(settings.value(0)),
                qPrintable(settings.value(1).isEmpty() ? QStringLiteral("(none)")
                                                       : settings.value(1)));

    // "id\tname\tvolume\ttrimStartMs\ttrimEndMs\tdurationMs\tfile" per row.
    const QStringList rows = getList(iface, "SoundboardSounds");
    const QStringList playing = getList(iface, "SoundboardPlayingIds");
    if (rows.isEmpty()) {
        std::puts("  (no sounds -- add some from the Soundboard panel)");
        return;
    }
    for (const QString &row : rows) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 6) continue;
        const QString id = f.value(0);
        const QString name = f.value(1);
        const double durationMs = f.value(5).toDouble();
        const bool isPlaying = playing.contains(id);
        std::printf("  %s  %-24s dur=%.1fs%s\n", qPrintable(id), qPrintable(name),
                    durationMs / 1000.0, isPlaying ? "  [playing]" : "");
    }
}

void cmdSoundboardPlay(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty()) errorExit(QStringLiteral("usage: --soundboard-play <id>"));
    const QString err = getStr(iface, "PlaySoundboardSound", {a[0]});
    if (!err.isEmpty()) errorExit(QStringLiteral("soundboard %1: %2").arg(a[0], err));
    std::printf("soundboard:%s -> playing\n", qPrintable(a[0]));
}

void cmdSoundboardStop(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty()) errorExit(QStringLiteral("usage: --soundboard-stop <id>"));
    call(iface, "StopSoundboardSound", {a[0]});
    std::printf("soundboard:%s -> stopped\n", qPrintable(a[0]));
}

void cmdSoundboardStopAll(QDBusInterface &iface) {
    call(iface, "StopAllSoundboardSounds");
    std::puts("soundboard -> all stopped");
}

void cmdToggleOutputStreamMix(QDBusInterface &iface) {
    const bool cur = getBool(iface, "StreamMixMuted");
    call(iface, "SetStreamMixMuted", {!cur});
    std::printf("stream mix mute -> %s\n", onOff(!cur));
}

void cmdSetOutputStreamMix(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty()) errorExit(QStringLiteral("usage: --set-output-stream-mix <0-100>"));
    const int pct = clampPct(toIntOrExit(a[0], QStringLiteral("volume percent")));
    call(iface, "SetStreamMixVolume", {pctToUnit(pct)});
    std::printf("stream mix volume -> %d%%\n", pct);
}

void cmdOffsetOutputStreamMix(QDBusInterface &iface, const QStringList &a) {
    if (a.isEmpty()) errorExit(QStringLiteral("usage: --offset-output-stream-mix <+-N>"));
    const int delta = toIntOrExit(a[0], QStringLiteral("offset"));
    const int pct = clampPct(unitToPct(getDouble(iface, "StreamMixVolume")) + delta);
    call(iface, "SetStreamMixVolume", {pctToUnit(pct)});
    std::printf("stream mix volume -> %d%%\n", pct);
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments();
    args.removeFirst();

    if (args.isEmpty() || args[0] == QLatin1String("--help") || args[0] == QLatin1String("-h")) {
        printUsage();
        return 0;
    }

    const QString action = args.takeFirst();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        errorExit(QStringLiteral("no session D-Bus connection"));
    QDBusInterface iface(QStringLiteral("org.waveline.Mixer"),
                        QStringLiteral("/org/waveline/Mixer"),
                        QStringLiteral("org.waveline.Mixer"), bus);
    if (!iface.isValid()) {
        std::fprintf(stderr,
                     "wavelined-cli: wavelined is not running (org.waveline.Mixer is not on "
                     "the session bus)\n  start it with: systemctl --user start wavelined\n");
        return 2;
    }

    if (action == QLatin1String("--list")) {
        cmdList(iface);
    } else if (action == QLatin1String("--toggle-mute")) {
        cmdToggleMute(iface, args);
    } else if (action == QLatin1String("--set-volume")) {
        cmdSetVolume(iface, args);
    } else if (action == QLatin1String("--offset-volume")) {
        cmdOffsetVolume(iface, args);
    } else if (action == QLatin1String("--toggle-channel-mic")) {
        cmdToggleChannelMic(iface, args);
    } else if (action == QLatin1String("--set-channel-mic")) {
        cmdSetChannelMic(iface, args);
    } else if (action == QLatin1String("--offset-channel-mic")) {
        cmdOffsetChannelMic(iface, args);
    } else if (action == QLatin1String("--toggle-channel-mic-monitor")) {
        cmdToggleChannelMicMonitor(iface, args);
    } else if (action == QLatin1String("--toggle-monitor")) {
        cmdToggleMonitor(iface, args);
    } else if (action == QLatin1String("--toggle-rack")) {
        cmdToggleRack(iface, args);
    } else if (action == QLatin1String("--toggle-effects")) {
        cmdToggleEffects(iface, args);
    } else if (action == QLatin1String("--toggle-rack-effect")) {
        cmdToggleRackEffect(iface, args);
    } else if (action == QLatin1String("--soundboard-list")) {
        cmdSoundboardList(iface);
    } else if (action == QLatin1String("--soundboard-play")) {
        cmdSoundboardPlay(iface, args);
    } else if (action == QLatin1String("--soundboard-stop")) {
        cmdSoundboardStop(iface, args);
    } else if (action == QLatin1String("--soundboard-stop-all")) {
        cmdSoundboardStopAll(iface);
    } else if (action == QLatin1String("--toggle-output-monitor-mix")) {
        cmdToggleOutputMonitorMix(iface, args);
    } else if (action == QLatin1String("--set-output-monitor-mix")) {
        cmdSetOutputMonitorMix(iface, args);
    } else if (action == QLatin1String("--offset-output-monitor-mix")) {
        cmdOffsetOutputMonitorMix(iface, args);
    } else if (action == QLatin1String("--toggle-output-monitor-master")) {
        cmdToggleOutputMonitorMaster(iface);
    } else if (action == QLatin1String("--set-output-monitor-master")) {
        cmdSetOutputMonitorMaster(iface, args);
    } else if (action == QLatin1String("--offset-output-monitor-master")) {
        cmdOffsetOutputMonitorMaster(iface, args);
    } else if (action == QLatin1String("--toggle-output-stream-mix")) {
        cmdToggleOutputStreamMix(iface);
    } else if (action == QLatin1String("--set-output-stream-mix")) {
        cmdSetOutputStreamMix(iface, args);
    } else if (action == QLatin1String("--offset-output-stream-mix")) {
        cmdOffsetOutputStreamMix(iface, args);
    } else {
        errorExit(QStringLiteral("unknown flag '%1' -- run --help for the list").arg(action));
    }

    return 0;
}

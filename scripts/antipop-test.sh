#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
#
# Anti-Pop test: is the click gone when an application stream is wired and
# unwired?
#
#   ./scripts/antipop-test.sh                 # measure with Anti-Pop off, then on
#   ./scripts/antipop-test.sh --channel game  # test a different channel
#   ./scripts/antipop-test.sh --fade 300      # a longer fade
#   ./scripts/antipop-test.sh --keep          # leave the recordings behind
#
# How it works, and why it can decide the question rather than just play sounds
# at you:
#
# The test tone is a sine that *starts at its peak*. A stream carrying it goes
# from digital silence to +0.7 full scale in one sample, which is the worst case
# for the artefact being tested and is exactly what a soundboard hit or a game
# starting mid-effect does.
#
# The channel's own sink monitor is recorded throughout, so what is measured is
# what the mix actually received -- no microphone, no effects, nothing else in
# the way. Three transitions are timed separately, because Waveline can do
# something about two of them and nothing about the third:
#
#   stream start    an application starts playing        -- faded
#   channel change  Waveline moves it to another channel -- faded, both ways
#   stream stop     the application stops playing        -- NOT faded
#
# The last one is not a bug and no setting will change it. The application
# decides when its final sample is written and then drops the stream; nothing in
# Waveline gets to run in between, so there is no moment at which a fade could
# start. Only a look-ahead delay on every channel could hide it, and that would
# cost latency on all app audio to soften one edge. The test reports it so the
# number is not mistaken for a regression.
#
# The measurement is the largest sample-to-sample jump near each transition,
# against the largest jump inside the steady tone. A tone has a slew rate of its
# own, so 1.0 means "this transition is no sharper than the tone itself" -- no
# click. Anti-Pop off scores 20-30 on this tone.
#
# Auto Routing and the microphone's stream-mix mute are saved, changed and put
# back: the router would move the test stream out from under the recorder, and a
# live microphone would be measured as part of the signal. Anything else playing
# into the channel under test *will* corrupt the result, so stop other audio on
# that channel first.

set -uo pipefail

CHANNEL=system
OTHER=music
REPS=4
FADE=150
KEEP=0
RATE=48000
TONE_HZ=300
AMPLITUDE=0.7
# Set from --fade below: every burst needs a steady stretch between its fade in
# and its fade out for the tone's own slew rate to be measured against.
BURST_MS=0
LONG_MS=0
GAP_MS=0

die() { echo "antipop-test: $*" >&2; exit 1; }

usage() {
	sed -n '4,48p' "$0" | sed 's/^# \?//'
	exit 0
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--channel) CHANNEL="${2:?}"; shift 2 ;;
		--other)   OTHER="${2:?}"; shift 2 ;;
		--reps)    REPS="${2:?}"; shift 2 ;;
		--fade)    FADE="${2:?}"; shift 2 ;;
		--keep)    KEEP=1; shift ;;
		-h|--help) usage ;;
		*) die "unknown option: $1 (try --help)" ;;
	esac
done
[[ "$CHANNEL" != "$OTHER" ]] || die "--channel and --other must differ"
[[ "$FADE" =~ ^[0-9]+$ ]] || die "--fade takes milliseconds"

BURST_MS=$((FADE * 2 + 600))
GAP_MS=$((FADE + 400))
# Long enough to survive: settle, find the stream, move away, move back, and a
# steady stretch after each of those.
MOVE_WAIT_MS=$((FADE * 2 + 900))
LONG_MS=$((2000 + MOVE_WAIT_MS * 3))

for tool in pw-play pw-record gdbus python3; do
	command -v "$tool" >/dev/null 2>&1 || die "$tool is not installed"
done
python3 -c 'import numpy' 2>/dev/null || die "python3 numpy is not installed"

# ------------------------------------------------------------------ the daemon

mix() {
	local method="$1"; shift
	gdbus call --session -d org.waveline.Mixer -o /org/waveline/Mixer \
		-m "org.waveline.Mixer.$method" "$@" 2>&1
}

# gdbus takes GVariant literals, not dbus-send's "boolean:true" type prefixes.
# Getting that wrong fails the call rather than the value, which is silent and
# looks exactly like a feature that does not work -- so calls that matter go
# through mixw, which checks that something came back.
mixw() {
	local out
	out="$(mix "$@")"
	[[ "$out" == \(* ]] || die "$1 failed: $out"
}

# gdbus prints results as a tuple: "(true,)", "(150,)", "(['a', 'b'],)".
unwrap() { sed -e 's/^(//' -e "s/,\{0,1\})\$//" -e "s/^'//" -e "s/'\$//"; }

out="$(mix ChannelIds)"
[[ "$out" == \(* ]] || die "wavelined is not answering on the session bus ($out)"
[[ "$out" == *"'$CHANNEL'"* ]] || die "no channel called '$CHANNEL'"
[[ "$out" == *"'$OTHER'"* ]] || die "no channel called '$OTHER'"
out="$(mix AntiPopEnabled)"
[[ "$out" == \(* ]] || die "this wavelined has no Anti-Pop -- rebuild and restart it"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/waveline-antipop.XXXXXX")" || die "cannot make a work directory"

SAVED_ANTIPOP="$(mix AntiPopEnabled | unwrap)"
SAVED_FADE="$(mix AntiPopFadeMs | unwrap)"
SAVED_ROUTING="$(mix RoutingEnabled | unwrap)"
SAVED_MICMUTE="$(mix MicMixMuted "'stream'" | unwrap)"

cleanup() {
	trap - EXIT INT TERM
	[[ -n "${RECPID:-}" ]] && kill "$RECPID" 2>/dev/null
	[[ -n "${PLAYPID:-}" ]] && kill "$PLAYPID" 2>/dev/null
	mix SetAntiPopEnabled "$SAVED_ANTIPOP" >/dev/null
	mix SetAntiPopFadeMs "$SAVED_FADE" >/dev/null
	mix SetRoutingEnabled "$SAVED_ROUTING" >/dev/null
	mix SetMicMuted "'stream'" "$SAVED_MICMUTE" >/dev/null
	if [[ $KEEP -eq 1 ]]; then
		echo "recordings left in $WORK"
	else
		rm -rf "$WORK"
	fi
}
trap cleanup EXIT INT TERM

mixw SetRoutingEnabled false
mixw SetMicMuted "'stream'" true
mixw SetAntiPopFadeMs "$FADE"

SINK="waveline-ch-$CHANNEL"

# --------------------------------------------------------------------- the tone

# Starts at the peak of the sine and ends wherever it ends: the discontinuity at
# both edges is the whole point, so there is deliberately no envelope on it.
python3 - "$WORK" "$RATE" "$TONE_HZ" "$AMPLITUDE" "$BURST_MS" "$LONG_MS" <<'PY' || die "could not generate the test tone"
import sys
import numpy as np

work, rate, hz, amp = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])


def tone(ms):
    n = int(rate * float(ms) / 1000.0)
    t = np.arange(n) / rate
    # Phase 0 of a cosine: sample zero is the peak, so the stream steps straight
    # to full amplitude with no ramp of its own.
    mono = (amp * np.cos(2.0 * np.pi * hz * t)).astype('<f4')
    return np.column_stack([mono, mono]).astype('<f4')


tone(sys.argv[5]).tofile(f"{work}/burst.raw")
tone(sys.argv[6]).tofile(f"{work}/long.raw")
PY

record() {  # $1 = output file
	pw-record --raw --format f32 --rate "$RATE" --channels 2 \
		--target "$SINK" -P stream.capture.sink=true \
		-P 'application.name=waveline-antipop-rec' "$1" >/dev/null 2>&1 &
	RECPID=$!
	# The recorder's link has to be up before anything plays, or the first
	# transition is missing from the recording rather than clean.
	sleep 1.0
}

stop_record() {
	kill "$RECPID" 2>/dev/null
	wait "$RECPID" 2>/dev/null
	RECPID=
}

play() {  # one burst, blocking
	pw-play --raw --format f32 --rate "$RATE" --channels 2 \
		--target "$SINK" -P 'application.name=waveline-antipop' \
		"$WORK/burst.raw" >/dev/null 2>&1
}

play_long() {  # the long tone, in the background, so it can be moved mid-play
	pw-play --raw --format f32 --rate "$RATE" --channels 2 \
		--target "$SINK" -P 'application.name=waveline-antipop' \
		"$WORK/long.raw" >/dev/null 2>&1 &
	PLAYPID=$!
}

# The node id of the test stream, so it can be moved between channels.
test_stream_id() {
	mix Apps | tr ',' '\n' | grep -i 'waveline-antipop' | head -1 |
		sed -e "s/^ *'//" -e "s/'.*\$//" -e 's/\\t.*$//'
}

# --------------------------------------------------------------------- one pass

run_pass() {  # $1 = "off"|"on"
	local mode="$1" i id
	mixw SetAntiPopEnabled "$([[ $mode == on ]] && echo true || echo false)"
	sleep 0.3

	# Pass 1: streams appearing and going away.
	record "$WORK/$mode-startstop.raw"
	for ((i = 0; i < REPS; i++)); do
		play
		sleep "$(python3 -c "print($GAP_MS/1000)")"
	done
	sleep 0.3
	stop_record

	# Pass 2: one stream, moved to another channel and back while it plays. In
	# a recording of this channel that is the tone leaving and returning, which
	# is exactly the unwire/rewire Anti-Pop is supposed to smooth.
	record "$WORK/$mode-move.raw"
	play_long
	local wait_s
	wait_s="$(python3 -c "print($MOVE_WAIT_MS/1000)")"
	sleep 1.2
	id="$(test_stream_id)"
	if [[ -n "$id" && "$id" != 0 ]]; then
		mixw MoveApp "uint32 $id" "'$OTHER'"
		sleep "$wait_s"
		mixw MoveApp "uint32 $id" "'$CHANNEL'"
		sleep "$wait_s"
	else
		echo "  WARNING: could not find the test stream; the channel-change part is missing"
	fi
	kill "$PLAYPID" 2>/dev/null
	wait "$PLAYPID" 2>/dev/null
	PLAYPID=
	sleep 0.3
	stop_record

	python3 "$WORK/analyse.py" "$WORK" "$mode" "$RATE" "$FADE"
}

# ------------------------------------------------------------------- the ruler

cat >"$WORK/analyse.py" <<'PY'
import json
import sys

import numpy as np

work, mode, rate, fade_ms = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
HOP = rate // 1000          # one envelope point per millisecond


def load(name):
    x = np.fromfile(f"{work}/{mode}-{name}.raw", dtype='<f4')
    if x.size < rate // 5:
        raise SystemExit(f"{mode}/{name}: recording is empty -- nothing reached the channel")
    return x.reshape(-1, 2).mean(axis=1)


def regions(x):
    """Millisecond spans where the tone is present, plus its sustained level."""
    frames = x.size // HOP
    env = np.abs(x[:frames * HOP]).reshape(frames, HOP).max(axis=1)
    # A 1 ms window does not always contain a peak of a 300 Hz tone, so the raw
    # envelope ripples by several dB and a "98% of sustain" threshold would
    # trigger all over the steady part. A sliding max over one period flattens
    # it without blunting the edges by more than that period.
    period = max(1, int(round(1000.0 / 300.0)) + 2)
    env = np.max(np.stack([np.roll(env, k) for k in range(period)]), axis=0)
    env[:period] = 0.0
    peak = float(env.max())
    if peak < 1e-3:
        raise SystemExit(f"{mode}: nothing audible was recorded (peak {peak:.5f})")
    sustain = float(np.median(env[env > peak * 0.5]))
    on = env > sustain * 0.02
    edges = np.diff(on.astype(np.int8))
    starts = list(np.flatnonzero(edges == 1) + 1)
    ends = list(np.flatnonzero(edges == -1) + 1)
    if on[0]:
        starts.insert(0, 0)
    if on[-1]:
        ends.append(len(on) - 1)
    spans = [(s, e) for s, e in zip(starts, ends) if e - s > 100]
    return env, sustain, spans


def window(span, extra=80):
    """Half-width to look either side of a transition.

    Wide enough to contain the whole fade, and never wider than half the burst
    it belongs to -- otherwise a long --fade makes the window swallow the burst's
    *other* edge, and the sharp end of a stream gets reported as the start.
    """
    return int(min(fade_ms + extra, span / 2))


def slew_near(d, ms, half):
    """Largest sample-to-sample jump within `half` ms of a transition."""
    a, b = max(0, (ms - half) * HOP), min(d.size, (ms + half) * HOP)
    return float(d[a:b].max()) if b > a else 0.0


def steady_slew(d, spans):
    """The tone's own slew, taken clear of every fade."""
    keep = np.zeros(d.size, dtype=bool)
    for s, e in spans:
        pad = int(min(fade_ms + 60, (e - s) * 0.4) * HOP)
        a, b = s * HOP + pad, e * HOP - pad
        if b > a:
            keep[a:b] = True
    if not keep.any():
        raise SystemExit(f"{mode}: bursts are too short for a {fade_ms:.0f} ms fade")
    return float(d[keep].max())


def fade_span(env, sustain, ms, half):
    """Milliseconds the level takes to cross 2% .. 98% of the sustained level."""
    a, b = max(0, ms - half), min(env.size, ms + half)
    seg = env[a:b]
    inside = np.flatnonzero((seg >= sustain * 0.02) & (seg <= sustain * 0.98))
    return float(inside[-1] - inside[0] + 1) if inside.size else 0.0


result = {}

# --- streams appearing and going away
x = load('startstop')
env, sustain, spans = regions(x)
d = np.abs(np.diff(x))
base = steady_slew(d, spans)
result['tone_slew'] = base
result['bursts'] = len(spans)
result['start'] = max(slew_near(d, s, window(e - s)) for s, e in spans)
result['stop'] = max(slew_near(d, e, window(e - s)) for s, e in spans)
result['start_ms'] = float(np.median(
    [fade_span(env, sustain, s, window(e - s)) for s, e in spans]))

# --- one stream moved to another channel and back
x = load('move')
env, sustain, spans = regions(x)
d = np.abs(np.diff(x))
if len(spans) >= 2:
    # [ appears .. moved away ] [ moved back .. ends ]
    out_half = window(spans[0][1] - spans[0][0])
    in_half = window(spans[1][1] - spans[1][0])
    result['move_out'] = slew_near(d, spans[0][1], out_half)
    result['move_in'] = slew_near(d, spans[1][0], in_half)
    result['move_out_ms'] = fade_span(env, sustain, spans[0][1], out_half)
    result['move_in_ms'] = fade_span(env, sustain, spans[1][0], in_half)
else:
    result['move_out'] = result['move_in'] = float('nan')
    result['move_out_ms'] = result['move_in_ms'] = float('nan')

def ratio(v):
    return v / base if base > 0 else float('inf')


print(f"  tone slew        {base:.4f} per sample (the tone's own; 1.00x)")
print(f"  bursts found     {result['bursts']}")
print(f"  stream start     {result['start']:.4f}  ({ratio(result['start']):5.1f}x)"
      f"   fade {result['start_ms']:.0f} ms")
print(f"  stream stop      {result['stop']:.4f}  ({ratio(result['stop']):5.1f}x)"
      f"   -- not fadeable, see the note at the top")
print(f"  channel out      {result['move_out']:.4f}  ({ratio(result['move_out']):5.1f}x)"
      f"   fade {result['move_out_ms']:.0f} ms")
print(f"  channel in       {result['move_in']:.4f}  ({ratio(result['move_in']):5.1f}x)"
      f"   fade {result['move_in_ms']:.0f} ms")

with open(f"{work}/{mode}.json", 'w') as fh:
    json.dump(result, fh)
PY

# ---------------------------------------------------------------------- the run

# PipeWire applies a node volume once per quantum, so a fade is a staircase of
# fadeMs/quantum steps and the click that is left over is the size of one step.
# That floor is a property of the graph, not of Waveline, so the verdict is
# measured against it rather than against a number pulled out of the air.
QUANTUM="$(pw-metadata -n settings 2>/dev/null |
	sed -n "s/.*key:'clock.force-quantum' value:'\([0-9]*\)'.*/\1/p" | tail -1)"
[[ -n "$QUANTUM" && "$QUANTUM" != 0 ]] || QUANTUM="$(pw-metadata -n settings 2>/dev/null |
	sed -n "s/.*key:'clock.quantum' value:'\([0-9]*\)'.*/\1/p" | tail -1)"
[[ -n "$QUANTUM" ]] || QUANTUM=1024

echo "Waveline Anti-Pop test"
echo "  channel      : $CHANNEL  (moved to $OTHER and back)"
echo "  tone         : ${TONE_HZ} Hz, starting at full amplitude"
echo "  bursts       : $REPS per pass"
echo "  fade length  : $FADE ms"
echo "  graph quantum: $QUANTUM samples ($(python3 -c "print(f'{$QUANTUM/$RATE*1000:.1f}')") ms)"
echo
echo "Stop anything else playing into '$CHANNEL' -- it would be measured too."
echo

echo "--- Anti-Pop OFF (the baseline: this is the click) -----------------------"
run_pass off || die "the baseline pass recorded nothing usable"
echo
echo "--- Anti-Pop ON ----------------------------------------------------------"
run_pass on || die "the Anti-Pop pass recorded nothing usable"
echo

python3 - "$WORK" "$FADE" "$QUANTUM" "$RATE" "$AMPLITUDE" <<'PY'
import json
import math
import sys

work, fade, quantum, rate, amp = (sys.argv[1], float(sys.argv[2]), float(sys.argv[3]),
                                  float(sys.argv[4]), float(sys.argv[5]))
off = json.load(open(f"{work}/off.json"))
on = json.load(open(f"{work}/on.json"))

# One volume per quantum, evenly spaced by a linear ramp: this is the largest
# step the fade can possibly be made of, and so the click that survives it.
steps = max(1.0, fade / (quantum / rate * 1000.0))
floor = amp / steps


def db(a, b):
    if a <= 0 or b <= 0:
        return "n/a"
    return f"{20 * math.log10(b / a):+.1f} dB"


print("--- verdict --------------------------------------------------------------")
for key, label in (('start', 'stream start '),
                   ('move_out', 'channel out  '),
                   ('move_in', 'channel in   '),
                   ('stop', 'stream stop  ')):
    print(f"  {label} {off[key]:.4f} off  ->  {on[key]:.4f} on   ({db(off[key], on[key])})")
print()
print(f"  a {fade:.0f} ms fade spans {steps:.1f} quanta on this graph, so the smallest")
print(f"  step it can be built from is {floor:.4f} ({db(amp, floor)} against the raw edge).")
print(f"  A longer --fade buys a smaller one.")
print()

# The fade is measured between 2% and 98% of the sustained level, which a linear
# ramp crosses over 96% of its length.
want = fade * 0.96
problems = []
if off['start'] < on['tone_slew'] * 5:
    problems.append("the baseline pass showed no click either -- the test tone never "
                    "reached the channel cleanly, so nothing here is conclusive")
for key, label in (('start', 'stream start'),
                   ('move_out', 'channel change (out)'),
                   ('move_in', 'channel change (in)')):
    # Twice the floor rather than the floor itself: the ramp starts when the
    # stream is announced, and its first audio arrives a few tens of
    # milliseconds later, so an edge can land a step or two into the ramp
    # instead of at the very bottom of it.
    if on[key] > floor * 2.0:
        problems.append(f"{label} left a {on[key]:.4f} step, well above the {floor:.4f} "
                        f"this graph's quantum forces -- the ramp is not doing its job")
# Only where the baseline is reliably a full-scale edge. Moving a stream *away*
# cuts it wherever the waveform happens to be, so its untreated size varies from
# run to run and a relative test on it would be a coin toss.
for key, label in (('start', 'stream start'), ('move_in', 'channel change (in)')):
    if on[key] > off[key] * 0.5:
        problems.append(f"{label} is barely better than with Anti-Pop off "
                        f"({off[key]:.4f} -> {on[key]:.4f})")
for key, label in (('start_ms', 'stream start'),
                   ('move_out_ms', 'channel change (out)'),
                   ('move_in_ms', 'channel change (in)')):
    if on[key] < want * 0.5:
        problems.append(f"{label} faded over {on[key]:.0f} ms, not the "
                        f"~{want:.0f} ms a {fade:.0f} ms fade should measure")

if problems:
    print("FAIL")
    for p in problems:
        print(f"  - {p}")
    sys.exit(1)

print("PASS")
print("  Streams start and change channel without a click: what is left of each")
print("  edge is one quantum-sized step of the ramp, which is the floor for this")
print("  graph. A stream *ending* is unchanged and cannot be -- see the note at")
print("  the top of this script.")
PY

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
#
# How much delay is inside the device itself?
#
#   ./scripts/latency-offset-test.sh --a Webcam --b Wave3
#   ./scripts/latency-offset-test.sh --a hw:6 --b hw:5 --seconds 8
#   ./scripts/latency-offset-test.sh --a Webcam --b Wave3 --keep
#
# Records two capture devices at once straight from ALSA, with PipeWire out of
# the path entirely, while you make one sharp sound in front of both. Reports
# how much later the sound arrives on one than on the other.
#
# ---------------------------------------------------------------- why bother
#
# A microphone can measure fast and sound slow. Everything this project can see
# -- period size, headroom, graph quantum -- describes the delay between the
# device handing audio to the host and the host doing something with it. None
# of it describes what the device did *before* that, and some devices do a
# great deal: a conference camera running its own noise suppression, echo
# cancellation and AGC can spend over a hundred milliseconds on audio before
# the host is ever offered a sample, and by construction the host is never told.
# It is handed a finished stream with no note of how long it took to make.
#
# So a device can sit at 21 ms of host-side buffering, identically to a studio
# microphone next to it, and still be visibly behind its own video. That is not
# a tuning problem and no buffer setting will touch it. This script is how you
# find out which kind of problem you have before spending days on the wrong one.
#
# --------------------------------------------------------- why it is trustworthy
#
# The naive version of this test is wrong, and wrong in a direction that looks
# plausible: start two recorders, clap, compare where the clap lands in each
# file. That measures the device delay *plus* however far apart the two
# recorders happened to start, and two processes starting "at the same time"
# routinely differ by tens of milliseconds -- the same order as the thing being
# measured.
#
# The kernel already knows the answer. Every running PCM publishes trigger_time
# in /proc/asound/card<N>/pcm<D>c/sub<S>/status: the monotonic instant that
# stream began, on a clock shared by every card in the machine. Read both and
# the start skew is not estimated, it is subtracted.
#
#   clap enters both microphones at absolute time T
#   device A delays its audio internally by Da, and started at trigger_a
#     => the clap lands in file A at position  Pa = T + Da - trigger_a
#   likewise                                   Pb = T + Db - trigger_b
#
#   so  Da - Db = (Pa - Pb) + (trigger_a - trigger_b)
#
# Note what is absent: buffering. Period size and headroom change when a sample
# can be *read*, not where it sits in the stream, so they cancel out of this
# measurement completely. What is left is signal-path delay -- the device's own
# processing -- which is exactly the quantity in question and the only one that
# cannot be tuned from the host.
#
# ------------------------------------------------------------ reading the result
#
# Near zero          the two devices are equally direct. Any delay you can hear
#                    is in the graph, and period size, headroom and graph
#                    quantum are worth chasing.
#
# Tens of ms or more that much delay is inside the slower device, before Linux
#                    ever sees it. No setting in this project will reduce it.
#                    The honest response is to say so and stop tuning.
#
# The number is a *difference*, not an absolute: it says how much later A is
# than B, and it is only as good as your reference. Compare a suspect device
# against the most direct microphone you own -- a plain USB or XLR mic doing no
# processing -- and the difference is very nearly the suspect's own delay.
set -euo pipefail

DEV_A=""
DEV_B=""
SECONDS_REC=6
KEEP=0
WORK=""

die()  { printf '\033[31merror\033[0m  %s\n' "$*" >&2; exit 1; }
say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
ok()   { printf '  \033[32m*\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*"; }

usage() {
	sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; s/^set -euo.*//'
	exit 0
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--a)       DEV_A="${2:-}"; shift 2 ;;
		--b)       DEV_B="${2:-}"; shift 2 ;;
		--seconds) SECONDS_REC="${2:-}"; shift 2 ;;
		--keep)    KEEP=1; shift ;;
		-h|--help) usage ;;
		*) die "unknown argument: $1 (try --help)" ;;
	esac
done

[[ -n "$DEV_A" && -n "$DEV_B" ]] || die "need --a and --b (try --help)"
command -v arecord >/dev/null || die "arecord not found (install alsa-utils)"
python3 -c 'import numpy' 2>/dev/null || die "python3 with numpy is required"

# ------------------------------------------------------------ resolve a device
#
# Accepts "hw:6", a bare card index, or any substring of the card's id or name
# as shown by /proc/asound/cards -- so --a Webcam and --a hw:6 are the same
# thing, and you do not have to look the number up first.
resolve_card() {
	local want="$1"
	if [[ "$want" =~ ^hw:([0-9]+)$ ]]; then echo "${BASH_REMATCH[1]}"; return; fi
	if [[ "$want" =~ ^[0-9]+$ ]]; then echo "$want"; return; fi
	want="${want#hw:}"   # "hw:Wave3" -- ALSA's own by-name form
	# /proc/asound/cards is two lines per card: an index/id/name line, then an
	# indented long description. Match the substring against either, carrying
	# the index down from the first line to the second.
	local n=-1 line
	while IFS= read -r line; do
		[[ "$line" =~ ^[[:space:]]*([0-9]+)[[:space:]]\[ ]] && n="${BASH_REMATCH[1]}"
		[[ $n -ge 0 && "$line" == *"$want"* ]] && { echo "$n"; return; }
	done < /proc/asound/cards
	die "no card matches '$1' -- see /proc/asound/cards"
}

# The capture PCM to use on a card. Almost always pcm0c; scanning rather than
# assuming keeps this working on cards that put capture somewhere else.
first_capture_pcm() {
	local card="$1" d
	for d in /proc/asound/card"$card"/pcm*c; do
		[[ -d "$d" ]] || continue
		basename "$d" | sed 's/^pcm\([0-9]\+\)c$/\1/'
		return
	done
	die "card $card has no capture PCM"
}

CARD_A="$(resolve_card "$DEV_A")"; PCM_A="$(first_capture_pcm "$CARD_A")"
CARD_B="$(resolve_card "$DEV_B")"; PCM_B="$(first_capture_pcm "$CARD_B")"
[[ "$CARD_A" != "$CARD_B" ]] || die "--a and --b resolved to the same card ($CARD_A)"

card_label() { sed -n "s/^ *$1 \[\([^]]*\)\].*/\1/p" /proc/asound/cards | tr -d ' '; }
LABEL_A="$(card_label "$CARD_A")"
LABEL_B="$(card_label "$CARD_B")"

say "Devices"
ok "A: hw:$CARD_A ($LABEL_A), capture pcm$PCM_A"
ok "B: hw:$CARD_B ($LABEL_B), capture pcm$PCM_B"

# ------------------------------------------------------- get PipeWire out of the way
#
# Our own profiles set node.always-process and suspend-timeout 0 precisely so
# these devices are never released, so "wait for PipeWire to let go" is not a
# thing that happens. The stack comes down for the duration and goes back up in
# the trap -- including on Ctrl-C, which is the run most likely to be
# interrupted.
STACK_DOWN=0
restore() {
	local rc=$?
	if [[ $STACK_DOWN -eq 1 ]]; then
		say "Restoring the audio stack"
		systemctl --user start pipewire.socket pipewire-pulse.socket >/dev/null 2>&1 || true
		systemctl --user start pipewire.service pipewire-pulse.service >/dev/null 2>&1 || true
		systemctl --user start wireplumber.service >/dev/null 2>&1 || true
		systemctl --user restart wavelined.service >/dev/null 2>&1 || true
		ok "audio is back"
	fi
	if [[ -n "$WORK" && $KEEP -eq 0 ]]; then rm -rf "$WORK"; fi
	if [[ -n "$WORK" && $KEEP -eq 1 ]]; then printf '\n  recordings kept in %s\n' "$WORK"; fi
	exit $rc
}
trap restore EXIT INT TERM

say "Stopping PipeWire"
warn "audio will be unavailable for about $((SECONDS_REC + 8)) seconds"
systemctl --user stop wavelined.service >/dev/null 2>&1 || true
systemctl --user stop wireplumber.service pipewire-pulse.service \
	pipewire-pulse.socket pipewire.service pipewire.socket >/dev/null 2>&1 || true
STACK_DOWN=1
sleep 2
ok "stopped"

WORK="$(mktemp -d)"
A_WAV="$WORK/a.wav"
B_WAV="$WORK/b.wav"

# Native rate for each device: hw: does no rate conversion, so asking for a rate
# the hardware does not have fails outright. The C922 runs its ADC at 32 kHz
# while everything else here is 48 -- the analysis resamples, this does not.
native_rate() {
	local card="$1" cand
	for cand in 48000 44100 32000 16000; do
		if arecord -D "hw:$card" -f S16_LE -c 1 -r "$cand" -d 1 -t raw >/dev/null 2>&1; then
			echo "$cand"; return
		fi
	done
	die "could not find a working rate for hw:$card"
}

say "Probing rates"
RATE_A="$(native_rate "$CARD_A")"; ok "hw:$CARD_A at $RATE_A Hz"
RATE_B="$(native_rate "$CARD_B")"; ok "hw:$CARD_B at $RATE_B Hz"

say "Recording"
printf '  Both devices are live. Make ONE sharp sound in front of both --\n'
printf '  a single hand clap, close and equidistant, is ideal.\n\n'

arecord -D "hw:$CARD_A" -f S16_LE -c 1 -r "$RATE_A" -d "$SECONDS_REC" "$A_WAV" >/dev/null 2>&1 &
PID_A=$!
arecord -D "hw:$CARD_B" -f S16_LE -c 1 -r "$RATE_B" -d "$SECONDS_REC" "$B_WAV" >/dev/null 2>&1 &
PID_B=$!

# The kernel's own answer to "when did each of these actually start", which is
# the whole reason this test can be trusted. Read as soon as both report
# RUNNING and before either finishes.
read_trigger() {
	local card="$1" pcm="$2" f t
	f="/proc/asound/card$card/pcm${pcm}c/sub0/status"
	for _ in $(seq 1 200); do
		if [[ -r "$f" ]] && grep -q '^state: RUNNING' "$f" 2>/dev/null; then
			t="$(sed -n 's/^trigger_time: *//p' "$f")"
			[[ -n "$t" ]] && { echo "$t"; return; }
		fi
		sleep 0.02
	done
	echo ""
}
TRIG_A="$(read_trigger "$CARD_A" "$PCM_A")"
TRIG_B="$(read_trigger "$CARD_B" "$PCM_B")"

for i in $(seq "$SECONDS_REC" -1 1); do printf '\r  recording... %ds ' "$i"; sleep 1; done
printf '\r  recording... done   \n'
wait "$PID_A" "$PID_B" 2>/dev/null || true

[[ -s "$A_WAV" && -s "$B_WAV" ]] || die "one of the recordings is empty -- was the device busy?"

if [[ -z "$TRIG_A" || -z "$TRIG_B" ]]; then
	warn "could not read trigger_time for both streams"
	warn "the result below is NOT corrected for recorder start skew, which can"
	warn "be tens of ms -- treat it as indicative only"
	TRIG_A="${TRIG_A:-0}"
	TRIG_B="${TRIG_B:-0}"
	SKEW_KNOWN=0
else
	SKEW_KNOWN=1
	ok "trigger_time A = $TRIG_A"
	ok "trigger_time B = $TRIG_B"
fi

say "Analysis"
LABEL_A="$LABEL_A" LABEL_B="$LABEL_B" \
A_WAV="$A_WAV" B_WAV="$B_WAV" TRIG_A="$TRIG_A" TRIG_B="$TRIG_B" \
SKEW_KNOWN="$SKEW_KNOWN" python3 <<'PY'
import os, sys, wave
import numpy as np

def load(path):
    with wave.open(path, 'rb') as w:
        rate = w.getframerate()
        n = w.getnframes()
        raw = w.readframes(n)
    x = np.frombuffer(raw, dtype='<i2').astype(np.float64)
    return x, rate

def envelope(x, rate, target=48000):
    # Resample to a common rate so two devices running at different rates can
    # be compared at all. Linear is plenty: the feature being located is a
    # transient tens of samples wide, not a waveform being reproduced.
    if rate != target:
        n_out = int(len(x) * target / rate)
        x = np.interp(np.arange(n_out) * (rate / target), np.arange(len(x)), x)
    # First difference kills DC and low-frequency room rumble and emphasises
    # the attack, which is the only part of a clap that is localised in time.
    d = np.abs(np.diff(x, prepend=x[:1]))
    # ~1 ms smoothing: enough to make the envelope single-peaked, short enough
    # not to move the peak.
    k = max(1, target // 1000)
    return np.convolve(d, np.ones(k) / k, mode='same'), target

a, ra = load(os.environ['A_WAV'])
b, rb = load(os.environ['B_WAV'])
ea, R = envelope(a, ra)
eb, _ = envelope(b, rb)

# Ignore the first 250 ms of each: a freshly started capture can open with a
# burst of nonsense that is louder than the clap and is not a sound.
guard = R // 4
if len(ea) <= guard or len(eb) <= guard:
    sys.exit("recordings are too short to analyse")
ea[:guard] = 0
eb[:guard] = 0

pa = int(np.argmax(ea))
pb = int(np.argmax(eb))

def snr(e, p):
    peak = e[p]
    floor = np.median(e[e > 0]) if np.any(e > 0) else 0.0
    return peak / floor if floor > 0 else float('inf')

sa, sb = snr(ea, pa), snr(eb, pb)

# Refine with a cross-correlation over a window around the two peaks. The peak
# positions alone are already good to about a millisecond; this pins it tighter
# and, more usefully, gives a correlation score that says whether the two
# transients are actually the same event.
half = R // 4                      # +/- 250 ms
lo_a, hi_a = max(0, pa - half), min(len(ea), pa + half)
lo_b, hi_b = max(0, pb - half), min(len(eb), pb + half)
wa = ea[lo_a:hi_a] - ea[lo_a:hi_a].mean()
wb = eb[lo_b:hi_b] - eb[lo_b:hi_b].mean()
n = min(len(wa), len(wb))
wa, wb = wa[:n], wb[:n]
corr = np.correlate(wa, wb, mode='full')
lag = int(np.argmax(corr)) - (n - 1)
denom = np.sqrt((wa @ wa) * (wb @ wb))
quality = float(corr.max() / denom) if denom > 0 else 0.0

# Pa - Pb, in seconds, measured from each stream's own first sample.
pos_a = pa / R
pos_b = pb / R
refined = (lo_a - lo_b + lag) / R
# Prefer the refined figure when the two transients correlate well; fall back
# to the raw peak difference when they do not, and say which was used.
use_refined = quality >= 0.5
delta_pos = refined if use_refined else (pos_a - pos_b)

skew_known = os.environ['SKEW_KNOWN'] == '1'
trig_a = float(os.environ['TRIG_A'])
trig_b = float(os.environ['TRIG_B'])
skew = (trig_a - trig_b) if skew_known else 0.0

# Da - Db = (Pa - Pb) + (trigger_a - trigger_b)
offset_ms = (delta_pos + skew) * 1000.0

la, lb = os.environ['LABEL_A'], os.environ['LABEL_B']

print(f"  clap found in A at {pos_a*1000:8.1f} ms   (peak/floor {sa:6.1f})")
print(f"  clap found in B at {pos_b*1000:8.1f} ms   (peak/floor {sb:6.1f})")
print(f"  raw difference     {(pos_a-pos_b)*1000:8.1f} ms")
if use_refined:
    print(f"  correlated         {refined*1000:8.1f} ms   (quality {quality:.2f})")
else:
    print(f"  correlation was poor (quality {quality:.2f}) -- using raw peaks")
if skew_known:
    print(f"  start skew         {skew*1000:8.1f} ms   (from ALSA trigger_time)")
else:
    print(f"  start skew          UNKNOWN            (NOT corrected)")
print()
print(f"  \033[1m{la} is {offset_ms:+.1f} ms relative to {lb}\033[0m")
print()

weak = min(sa, sb) < 8
if weak:
    print("  The transient was weak in at least one recording (peak/floor below 8).")
    print("  Clap closer, louder, and equidistant from both devices, and run again.")
    print()

mag = abs(offset_ms)
if weak:
    pass
elif mag < 8:
    print(f"  VERDICT: no meaningful device-internal difference ({mag:.1f} ms).")
    print("  Both devices hand audio to the host equally promptly. Any delay you")
    print("  can hear is in the graph -- period size, headroom and graph quantum")
    print("  are the things worth chasing.")
elif mag < 30:
    print(f"  VERDICT: a small internal difference ({mag:.1f} ms).")
    print("  Real, but the same order as host-side buffering. Worth tuning the")
    print("  graph first and re-running this afterwards.")
else:
    slower = la if offset_ms > 0 else lb
    print(f"  VERDICT: {mag:.1f} ms of delay is inside {slower}.")
    print("  That is signal-path delay, before Linux is offered a sample. No")
    print("  period size, headroom or quantum setting will reduce it, and no")
    print("  latency figure this project displays includes it. Say so and stop")
    print("  tuning this device.")
print()
print("  Run this two or three times. A device's internal delay is a constant;")
print("  a number that moves between runs is a clap that was not equidistant.")
PY

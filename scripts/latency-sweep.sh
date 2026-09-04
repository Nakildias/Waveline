#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
#
# What does a buffer setting actually cost, and what does it actually buy?
#
#   ./scripts/latency-sweep.sh --device Wave3
#   ./scripts/latency-sweep.sh --device Rocksmith --headroom unset,256,512 --soak 20m
#   ./scripts/latency-sweep.sh --device Webcam --period 128,256,512 --soak 5m --out c922.md
#
# For one capture device, sets api.alsa.period-size and api.alsa.headroom to
# each combination you ask for, restarts the session, and measures what came
# out. Prints a markdown table you can paste next to the setting in the
# device's profile.
#
# ------------------------------------------------------------------ why it exists
#
# Every buffer number in this project was arrived at the same way: someone had
# a dropout, raised a value until the dropout stopped, and wrote
# "TESTED 0/20 Failures" next to it. That is a real test of one thing and no
# test at all of the other -- none of those settings was ever timed, and one of
# them (the Rocksmith's headroom) turned out to make the guitar adapter the
# slowest input on the machine, 11 ms behind devices nobody had tuned.
#
# It is also not a strong test of reliability, because the fault these settings
# guard against is a *start-up race*, not a steady-state one. With too little
# buffered capture data at the instant the follower resampler starts, the
# stream comes up wrong and stays wrong for as long as it is open. Twenty plug
# cycles that happened to win the race prove very little. A long soak that
# watches for the stream restarting proves rather more.
#
# So this measures both ends at once: what the setting costs in milliseconds,
# and whether the stream survives being left alone.
#
# ------------------------------------------------------------------ what it reports
#
# delay        Median of `delay` from /proc/asound/card<N>/pcm<D>c/sub<S>/status,
#              converted at the device's own rate. This is the capture-side
#              latency, straight from the kernel. It swings by a full period
#              across a cycle as the buffer fills and drains, so the median is
#              the number that means anything and the min/max are shown to
#              prove the swing is the shape it should be.
#
# predicted    headroom + period, at the device's rate. Included as a check on
#              the model, NOT as a result -- and the model is already known to
#              be incomplete. It matched measured delay within ~2 ms on every
#              device on the run these figures came from, and then
#              stopped matching on the same machine after a graph
#              reconfiguration, with no setting changed and nothing replugged:
#              the Wave:3 went from 21.9 ms to 11.6 ms, and the C922's delay
#              went *down* when its period doubled. What survives is that
#              effective headroom dominates and the rest moves with graph
#              state. So read this column as "how far off is the simple model
#              today", and never quote it as a device's latency.
#
# effective    The headroom the node actually got, which is not always the one
# headroom     that was asked for. PipeWire treats USB devices as batch devices
#              and adds a period to a requested headroom -- the Rocksmith asks
#              for 512 and runs at 1024. An unset headroom lands equal to the
#              period. This column is the one that explains the delay column,
#              and it is why "just set headroom to what it already is" makes a
#              device slower rather than confirming it.
#
# fill         Peak `avail_max` as a percentage of the ALSA buffer. How close
#              the device came to overrunning across the whole soak. Low is
#              slack; approaching 100% is a device about to drop audio.
#
# restarts     Times the capture stream was torn down and restarted during the
#              soak, counted by watching trigger_time change. This is the
#              dropout counter, and a non-zero value fails the row regardless
#              of how good its latency looks.
#
# NOTE ON XRUNS: there is deliberately no xrun column. The usual sources do not
# work on every machine -- pw-top's profiler reports nothing at all on the
# development machine (every field reads "???"), CONFIG_SND_PCM_XRUN_DEBUG is
# off in stock kernels so there is no xrun_debug file, and PipeWire's default
# log level does not record them. Printing a column that silently reads zero
# because nothing was counting is worse than not having it. `fill` and
# `restarts` are measured from /proc, which is always there, and between them
# they catch what an xrun count would have told you.
set -euo pipefail

DEVICE=""
PERIODS="256,512,1024"
HEADROOMS="unset"
SOAK="60"
OUT=""
SAMPLES=40

die()  { printf '\033[31merror\033[0m  %s\n' "$*" >&2; exit 1; }
say()  { printf '\n\033[1m%s\033[0m\n' "$*"; }
ok()   { printf '  \033[32m*\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*"; }

usage() { sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; s/^set -euo.*//'; exit 0; }

while [[ $# -gt 0 ]]; do
	case "$1" in
		--device)   DEVICE="${2:-}"; shift 2 ;;
		--period)   PERIODS="${2:-}"; shift 2 ;;
		--headroom) HEADROOMS="${2:-}"; shift 2 ;;
		--soak)     SOAK="${2:-}"; shift 2 ;;
		--out)      OUT="${2:-}"; shift 2 ;;
		-h|--help)  usage ;;
		*) die "unknown argument: $1 (try --help)" ;;
	esac
done

[[ -n "$DEVICE" ]] || die "need --device (try --help)"
command -v pw-dump >/dev/null || die "pw-dump not found"
command -v systemctl >/dev/null || die "systemctl not found"

# 20m / 90s / 90
parse_duration() {
	local v="$1"
	case "$v" in
		*m) echo $(( ${v%m} * 60 )) ;;
		*s) echo "${v%s}" ;;
		*)  echo "$v" ;;
	esac
}
SOAK_S="$(parse_duration "$SOAK")"
[[ "$SOAK_S" =~ ^[0-9]+$ && "$SOAK_S" -gt 0 ]] || die "bad --soak: $SOAK"

WP_DIR="$HOME/.config/wireplumber/wireplumber.conf.d"
# 99- so it overrides the device's own 51- profile for the duration.
SWEEP_CONF="$WP_DIR/99-waveline-sweep.conf"

# ------------------------------------------------------------------- find the node
#
# Matched on the running graph rather than on a profile, so this works for a
# device Waveline has no profile for -- which is most of the point, since a
# device with no profile is exactly the one whose numbers nobody knows.
find_node() {
	pw-dump 2>/dev/null | python3 -c '
import json, re, sys, os

# Punctuation-insensitive matching, so the name a human would type finds the
# device. The same microphone is "Wave3" in /proc/asound/cards, "Wave_3" in its
# PipeWire node name and "Wave:3" in its ALSA card name, and none of those is
# a substring of the others.
def norm(s):
    return re.sub(r"[^a-z0-9]", "", str(s).lower())

want = norm(os.environ["DEVICE"])

# ALSA short card ids ("Wave3", "Webcam", "Adapter") by index -- the names the
# user actually sees, and the ones PipeWire does not carry in its props.
ids = {}
try:
    with open("/proc/asound/cards") as f:
        for line in f:
            m = re.match(r"\s*(\d+)\s*\[([^]]*)\]", line)
            if m: ids[m.group(1)] = m.group(2).strip()
except OSError:
    pass

matches = []
for o in json.load(sys.stdin):
    if o.get("type") != "PipeWire:Interface:Node": continue
    p = o.get("info", {}).get("props", {})
    if p.get("media.class") != "Audio/Source": continue
    name = p.get("node.name", "")
    if not name.startswith("alsa_input."): continue
    card = str(p.get("api.alsa.pcm.card", ""))
    label = ids.get(card, card)
    # A bare number is a card index and nothing else. Allowing it to fall
    # through to the substring search below makes "3" match the Sony pad on
    # card 4, whose USB path is ...78:00.4-1.3 -- a plausible-looking answer
    # to a question the user did not ask.
    if want.isdigit():
        if want == norm(card):
            matches.append((name, card, label))
        continue
    hay = norm(" ".join(str(p.get(k, "")) for k in
                        ("node.name", "node.description", "api.alsa.card.name",
                         "api.alsa.card.longname")) + " " + label)
    if want and want in hay:
        matches.append((name, card, label))

if len(matches) > 1:
    sys.stderr.write("AMBIGUOUS\n")
    for name, card, label in matches:
        sys.stderr.write(f"  card {card} [{label}]  {name}\n")
    sys.exit(3)
if matches:
    name, card, _ = matches[0]
    print(name, card, sep="\t")
'
}

MATCH=""
if ! MATCH="$(find_node)"; then
	# Exit 3 is "more than one device matched"; the candidates are already on
	# stderr. Anything else is a real failure.
	[[ $? -eq 3 ]] && die "'$DEVICE' matches more than one device (listed above) -- be more specific"
fi
IFS=$'\t' read -r NODE_NAME CARD <<< "$MATCH"
[[ -n "${NODE_NAME:-}" ]] || die "no running capture node matches '$DEVICE' -- is it plugged in and is PipeWire up?"
[[ -n "${CARD:-}" ]] || die "matched $NODE_NAME but it reports no api.alsa.pcm.card"

PCM=""
for d in /proc/asound/card"$CARD"/pcm*c; do
	[[ -d "$d" ]] || continue
	PCM="$(basename "$d" | sed 's/^pcm\([0-9]\+\)c$/\1/')"
	break
done
[[ -n "$PCM" ]] || die "card $CARD has no capture PCM"
STATUS="/proc/asound/card$CARD/pcm${PCM}c/sub0/status"
HWPARAMS="/proc/asound/card$CARD/pcm${PCM}c/sub0/hw_params"

say "Device"
ok "node   $NODE_NAME"
ok "card   $CARD, capture pcm$PCM"
ok "status $STATUS"

# --------------------------------------------------------------------- restore
HAD_WAVELINED=0
systemctl --user is-active --quiet wavelined.service && HAD_WAVELINED=1

restore() {
	local rc=$?
	say "Restoring"
	rm -f "$SWEEP_CONF"
	systemctl --user restart wireplumber.service >/dev/null 2>&1 || true
	sleep 3
	if [[ $HAD_WAVELINED -eq 1 ]]; then
		systemctl --user start wavelined.service >/dev/null 2>&1 || true
	fi
	ok "removed $SWEEP_CONF and restarted the session"
	exit $rc
}
trap restore EXIT INT TERM

# wavelined reacts to a session restart by rebuilding its graph, and its retry
# ladder is up to eight full teardowns. Doing that once per sweep row would
# take longer than the soaks and would put the mixer's own filters in the path
# of a measurement that is meant to be of the bare ALSA device.
if [[ $HAD_WAVELINED -eq 1 ]]; then
	say "Stopping wavelined for the duration"
	systemctl --user stop wavelined.service >/dev/null 2>&1 || true
	ok "stopped (restarted automatically at the end)"
fi

mkdir -p "$WP_DIR"

# ------------------------------------------------------------------ apply a combo
write_conf() {
	local period="$1" headroom="$2"
	{
		printf '# Written by scripts/latency-sweep.sh -- temporary, safe to delete.\n'
		printf 'monitor.alsa.rules = [\n'
		printf '  {\n'
		printf '    matches = [\n'
		printf '      { node.name = "%s" }\n' "$NODE_NAME"
		printf '    ]\n'
		printf '    actions = {\n'
		printf '      update-props = {\n'
		printf '        session.suspend-timeout-seconds = 0\n'
		printf '        node.pause-on-idle              = false\n'
		printf '        node.always-process             = true\n'
		printf '        api.alsa.period-size            = %s\n' "$period"
		[[ "$headroom" != "unset" ]] && printf '        api.alsa.headroom               = %s\n' "$headroom"
		printf '      }\n'
		printf '    }\n'
		printf '  }\n'
		printf ']\n'
	} > "$SWEEP_CONF"
}

# Effective props, read back off the running node. Requested is what we asked
# for; this is what we got, and they are routinely different.
read_effective() {
	NODE_NAME="$NODE_NAME" pw-dump 2>/dev/null | python3 -c '
import json, sys, os
want = os.environ["NODE_NAME"]
for o in json.load(sys.stdin):
    if o.get("type") != "PipeWire:Interface:Node": continue
    p = o.get("info", {}).get("props", {})
    if p.get("node.name") != want: continue
    print(p.get("api.alsa.headroom", "?"), p.get("api.alsa.period-size", "?"), sep="\t")
    break
'
}

wait_for_node() {
	for _ in $(seq 1 60); do
		if [[ -r "$STATUS" ]] && grep -q '^state: RUNNING' "$STATUS" 2>/dev/null; then
			# One more beat so the props settle before they are read back.
			sleep 1
			return 0
		fi
		sleep 1
	done
	return 1
}

# ----------------------------------------------------------------------- soak
#
# Samples the kernel's own view for the whole soak: the delay figure, how close
# avail_max came to the buffer size, and whether trigger_time ever moved (which
# means the stream was torn down and restarted underneath us).
soak() {
	local secs="$1"
	SECS="$secs" STATUS="$STATUS" HWPARAMS="$HWPARAMS" SAMPLES="$SAMPLES" python3 -c '
import os, re, time, statistics, sys

status  = os.environ["STATUS"]
hwp     = os.environ["HWPARAMS"]
secs    = int(os.environ["SECS"])
target  = int(os.environ["SAMPLES"])

def read(path):
    try:
        with open(path) as f: return f.read()
    except OSError:
        return ""

def field(txt, key):
    m = re.search(rf"^{re.escape(key)}\s*:\s*(.+)$", txt, re.M)
    return m.group(1).strip() if m else None

hw = read(hwp)
rate = buf = period = None
if hw:
    r = field(hw, "rate")
    if r: rate = int(r.split()[0])
    b = field(hw, "buffer_size")
    if b: buf = int(b)
    p = field(hw, "period_size")
    if p: period = int(p)

delays, fills = [], []
restarts = 0
not_running = 0
first_trigger = None
last_trigger = None

# Spread the samples across the whole soak rather than taking them in a burst:
# a burst measures one moment, and the thing being watched is whether the
# device stays well-behaved when left alone.
deadline = time.monotonic() + secs
interval = max(0.05, secs / max(target, 1))
while time.monotonic() < deadline:
    txt = read(status)
    if not txt:
        not_running += 1
    else:
        state = field(txt, "state")
        if state != "RUNNING":
            not_running += 1
        else:
            trig = field(txt, "trigger_time")
            if trig is not None:
                if first_trigger is None:
                    first_trigger = trig
                elif trig != last_trigger and trig != first_trigger:
                    restarts += 1
                last_trigger = trig
            d = field(txt, "delay")
            a = field(txt, "avail_max")
            # The sc0710 reports delay 0 while claiming RUNNING. Zero is not a
            # latency, it is a status file that does not work, and averaging it
            # in would quietly halve every figure on the row.
            if d is not None and int(d) > 0:
                delays.append(int(d))
            if a is not None and buf:
                fills.append(int(a) / buf * 100.0)
    time.sleep(interval)

def out(k, v): print(f"{k}\t{v}")

out("rate", rate or 0)
out("period_hw", period or 0)
out("buffer_hw", buf or 0)
out("n", len(delays))
if delays and rate:
    out("med_ms",  statistics.median(delays) / rate * 1000.0)
    out("min_ms",  min(delays) / rate * 1000.0)
    out("max_ms",  max(delays) / rate * 1000.0)
else:
    out("med_ms", -1); out("min_ms", -1); out("max_ms", -1)
out("fill", max(fills) if fills else -1)
out("restarts", restarts)
out("stalled", not_running)
'
}

# ------------------------------------------------------------------------ run
IFS=',' read -ra PERIOD_LIST <<< "$PERIODS"
IFS=',' read -ra HEADROOM_LIST <<< "$HEADROOMS"

TOTAL=$(( ${#PERIOD_LIST[@]} * ${#HEADROOM_LIST[@]} ))
EST=$(( TOTAL * (SOAK_S + 12) ))
say "Sweep"
ok "$TOTAL combinations, ${SOAK_S}s soak each"
ok "estimated run time: $((EST / 60))m $((EST % 60))s"
warn "audio restarts between every combination -- do not rely on this machine"

ROWS=()
for period in "${PERIOD_LIST[@]}"; do
	for headroom in "${HEADROOM_LIST[@]}"; do
		say "period $period, headroom $headroom"
		write_conf "$period" "$headroom"
		systemctl --user restart wireplumber.service >/dev/null 2>&1 || true
		if ! wait_for_node; then
			warn "node did not come back RUNNING -- recording as a failure"
			ROWS+=("$period|$headroom|-|-|-|-|-|-|did not start")
			continue
		fi

		eff_headroom="?"; eff_period="?"
		if read -r eff_headroom eff_period < <(read_effective); then :; fi
		ok "requested headroom $headroom -> effective ${eff_headroom}"

		printf '  soaking for %ss...\n' "$SOAK_S"
		declare -A M=()
		while IFS=$'\t' read -r k v; do M["$k"]="$v"; done < <(soak "$SOAK_S")

		rate="${M[rate]:-0}"
		med="${M[med_ms]:--1}"; mn="${M[min_ms]:--1}"; mx="${M[max_ms]:--1}"
		fill="${M[fill]:--1}"; restarts="${M[restarts]:-0}"; stalled="${M[stalled]:-0}"

		# predicted = effective headroom + period, at the device's own rate.
		pred="-"
		if [[ "$eff_headroom" =~ ^[0-9]+$ && "$eff_period" =~ ^[0-9]+$ && "$rate" -gt 0 ]]; then
			pred="$(python3 -c "print(f'{($eff_headroom + $eff_period) / $rate * 1000:.1f}')")"
		fi

		note=""
		[[ "$restarts" -gt 0 ]] && note="FAILED: $restarts restart(s)"
		[[ "$stalled" -gt 0 && -z "$note" ]] && note="stream not RUNNING for $stalled samples"
		[[ "${M[n]:-0}" -eq 0 && -z "$note" ]] && note="no usable delay readings"

		fmt() { [[ "$1" == "-1" || "$1" == "-" ]] && echo "-" || python3 -c "print(f'{$1:.1f}')"; }
		ok "median $(fmt "$med") ms (predicted $pred), peak fill $(fmt "$fill")%, restarts $restarts"

		ROWS+=("$period|$headroom|$eff_headroom|$pred|$(fmt "$med")|$(fmt "$mn")|$(fmt "$mx")|$(fmt "$fill")|$restarts|$note")
	done
done

# --------------------------------------------------------------------- report
render() {
	printf '# Latency sweep: %s\n\n' "$NODE_NAME"
	printf 'card %s, capture pcm%s. Soak %ss per combination, %s samples spread across it.\n' \
	       "$CARD" "$PCM" "$SOAK_S" "$SAMPLES"
	printf 'Measured %s.\n\n' "$(date -Iseconds)"
	printf '| period | headroom asked | headroom effective | predicted | **median** | min | max | peak fill | restarts | |\n'
	printf '|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|\n'
	local r
	for r in "${ROWS[@]}"; do
		IFS='|' read -r p h eh pr md mn mx fl rs nt <<< "$r"
		printf '| %s | %s | %s | %s ms | **%s ms** | %s | %s | %s%% | %s | %s |\n' \
		       "$p" "$h" "$eh" "$pr" "$md" "$mn" "$mx" "$fl" "$rs" "$nt"
	done
	printf '\n'
	printf 'Reading it:\n\n'
	printf -- '- A row with any restarts is not a candidate, whatever its latency.\n'
	printf -- '- `predicted` is effective headroom + period. If measured stops tracking it,\n'
	printf    '  the model behind every buffer decision in this project is wrong and that is\n'
	printf    '  the finding, not the latency numbers.\n'
	printf -- '- Compare `headroom asked` against `headroom effective`. Where they differ, the\n'
	printf    '  device is being treated as a batch device and a period was added to the\n'
	printf    '  request. Setting headroom to the value a device already has therefore makes\n'
	printf    '  it slower, and is the mistake this whole exercise started from.\n'
	printf -- '- Pick the lowest-latency row with zero restarts and comfortable peak fill,\n'
	printf    '  then write it into the profile with these numbers beside it.\n'
}

say "Result"
render
if [[ -n "$OUT" ]]; then
	render > "$OUT"
	ok "written to $OUT"
fi

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
# Collect everything needed to diagnose a Waveline problem, into one file.
#
#   ./scripts/collect-report.sh              -> ./waveline-report-<date>.txt
#   ./scripts/collect-report.sh /tmp/out.txt -> that path
#
# Read-only. Runs as a normal user, needs no sudo, changes nothing, and
# starts nothing. Anything unavailable is reported as unavailable rather
# than silently skipped -- a missing section is itself a clue.
#
# The device serial number, your home directory, your username and your
# hostname are redacted wherever they appear, including inside PipeWire node
# names, which embed the serial. Check the output before posting it anyway; it
# is a plain text file and that is the point.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/waveline-report-$(date -u '+%Y%m%d-%H%M%S').txt}"
VID_PID="0fd9:0070"

# ------------------------------------------------------------------ helpers
# Locate the device before anything else; most sections key off it.
CARD=""
for f in /proc/asound/card*/usbid; do
	[[ -r "$f" ]] && [[ "$(cat "$f" 2>/dev/null)" == "$VID_PID" ]] \
	  && CARD="$(basename "$(dirname "$f")")"
done
CARD_NUM="${CARD#card}"

# sysfs path of the USB device, e.g. /sys/bus/usb/devices/1-4
USBDEV=""
for d in /sys/bus/usb/devices/*/; do
	[[ -r "$d/idVendor" && -r "$d/idProduct" ]] || continue
	[[ "$(cat "$d/idVendor" 2>/dev/null):$(cat "$d/idProduct" 2>/dev/null)" == "$VID_PID" ]] \
	  && USBDEV="${d%/}"
done

SERIAL=""
[[ -n "$USBDEV" && -r "$USBDEV/serial" ]] && SERIAL="$(cat "$USBDEV/serial" 2>/dev/null)"

# Redact identifying strings everywhere. PipeWire node names embed the serial
# (so does `pactl info`'s default sink/source), so filtering the iSerial line
# alone is not enough. This is applied once to the entire report rather than
# per section, because a per-section filter is one forgotten call away from
# leaking -- which it did once, via `pactl info`.
#
# The home directory, username and hostname go too. This report is written to
# be pasted into a public issue, and half its sections (`ls -l ~/.local/...`,
# systemd unit paths, journal lines) carry all three without anyone noticing.
#
# Escape the values before they become sed patterns: none of a serial, a
# username or a hostname is supposed to contain a regex metacharacter, but the
# home directory contains slashes by definition and "supposed to" is not a
# guarantee worth betting a mangled report on.
re_escape() { printf '%s' "$1" | sed -e 's/[][\.*^$|]/\\&/g'; }

REDACT_ARGS=()
[[ -n "$SERIAL" ]] \
  && REDACT_ARGS+=(-e "s|$(re_escape "$SERIAL")|<SERIAL-REDACTED>|g")

# Longest first: with the home directory replaced already, the username inside
# it is gone before the username rule runs, so nothing is substituted twice.
REPORT_HOME="${HOME:-}"
[[ -n "$REPORT_HOME" && "$REPORT_HOME" != "/" ]] \
  && REDACT_ARGS+=(-e "s|$(re_escape "$REPORT_HOME")|<HOME>|g")

# Word-anchored, and only from three characters up. A two-letter username like
# "ed" or "pi" appears inside ordinary words all over a PipeWire dump, and a
# report shredded into <user> fragments is worse than one naming its author.
REPORT_USER="$(id -un 2>/dev/null || true)"
[[ ${#REPORT_USER} -ge 3 ]] \
  && REDACT_ARGS+=(-e "s|\b$(re_escape "$REPORT_USER")\b|<user>|g")

# `uname -n` rather than `hostname`: the hostname binary is not installed on
# every distribution any more (Arch, among others), and coreutils always is.
REPORT_HOST="$(uname -n 2>/dev/null || true)"
[[ ${#REPORT_HOST} -ge 3 && "$REPORT_HOST" != "localhost" ]] \
  && REDACT_ARGS+=(-e "s|\b$(re_escape "$REPORT_HOST")\b|<host>|g")

redact() {
	if [[ ${#REDACT_ARGS[@]} -gt 0 ]]; then
		sed "${REDACT_ARGS[@]}"
	else
		cat
	fi
}

sec() { printf '\n\n===== %s =====\n' "$*"; }

# Run a command into the report, or say why it produced nothing.
run() {
	local label="$1"; shift
	printf '\n--- %s ---\n' "$label"
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "(not available: $1 is not installed)"
		return
	fi
	local out
	out="$("$@" 2>&1)"
	if [[ -z "$out" ]]; then
		echo "(no output)"
	else
		printf '%s\n' "$out"
	fi
}

# Dump a file into the report, or say it is missing.
cat_file() {
	printf '\n--- %s ---\n' "$1"
	if [[ -r "$1" ]]; then
		cat "$1" 2>&1
	else
		echo "(not readable or does not exist)"
	fi
}

# ------------------------------------------------------------------- report
{
printf 'Waveline diagnostic report\n'
printf 'generated %s\n' "$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
printf '%s\n' 'https://github.com/Nakildias/Waveline'

sec "SUMMARY"
printf 'distro          : %s\n' "$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
printf 'kernel          : %s\n' "$(uname -r)"
printf 'arch            : %s\n' "$(uname -m)"
printf 'device present  : %s\n' "$([[ -n "$USBDEV" ]] && echo "yes ($VID_PID)" || echo "NO")"
printf 'usb sysfs path  : %s\n' "${USBDEV:-not found}"
printf 'alsa card       : %s\n' "${CARD:-not found}"
printf 'serial          : %s\n' "$([[ -n "$SERIAL" ]] && echo "<redacted>" || echo "unknown")"
# bcdDevice is BCD in sysfs ("0122"); show it the way lsusb does ("1.22").
BCD="$(cat "$USBDEV/bcdDevice" 2>/dev/null)"
if [[ "$BCD" =~ ^[0-9]{4}$ ]]; then
	# Base-10 forced: "01" must not be read as octal.
	printf 'firmware        : %s.%s\n' "$((10#${BCD:0:2}))" "${BCD:2:2}"
else
	printf 'firmware        : %s\n' "${BCD:-unknown}"
fi

# The three fixes this repo installs, and whether each is actually in place.
printf '\n-- installed fixes --\n'
MODFILE="$(modinfo -F filename snd-usb-audio 2>/dev/null)"
case "$MODFILE" in
	*updates/dkms*|*extra*) printf 'kernel patch    : ACTIVE (%s)\n' "$MODFILE" ;;
	"")                     printf 'kernel patch    : unknown (modinfo failed)\n' ;;
	*)                      printf 'kernel patch    : not active, stock module (%s)\n' "$MODFILE" ;;
esac
WPC="$HOME/.config/wireplumber/wireplumber.conf.d/51-waveline-wave3.conf"
printf 'mic fix         : %s\n' "$([[ -f "$WPC" ]] && echo "installed ($WPC)" || echo "NOT installed")"
# Waveline's graph policy, installed on every machine. Both are worth having in
# a bug report: without the driver policy a capture device can be clocking the
# whole graph, which changes every latency figure below and is the single most
# common reason two machines with the same hardware behave differently.
CLK="$HOME/.config/pipewire/pipewire.conf.d/50-waveline-clock.conf"
DRV="$HOME/.config/wireplumber/wireplumber.conf.d/50-waveline-driver-policy.conf"
printf 'clock policy    : %s\n' "$([[ -f "$CLK" ]] && echo "installed ($CLK)" || echo "NOT installed")"
printf 'driver policy   : %s\n' "$([[ -f "$DRV" ]] && echo "installed ($DRV)" || echo "NOT installed")"
# The superseded global quantum lock. Only reported when it is still on disk,
# where it would pin an 85 ms floor on every device and explain a lot.
OLDQ="$HOME/.config/pipewire/pipewire.conf.d/51-waveline-wave3-quantum.conf"
[[ -f "$OLDQ" ]] && printf 'quantum fix     : STALE, superseded ($OLDQ) -- remove it\n'

printf '\n-- graph clock --\n'
pw-metadata -n settings 2>/dev/null | grep -E 'clock\.' \
  || printf 'pw-metadata unavailable\n'

printf '\n-- graph driver --\n'
# Which node is clocking the graph, and every capture device's measured delay.
# Both are the first things to look at when two machines with identical
# hardware behave differently: a capture device winning the driver role
# re-clocks everything downstream of it, and nothing else in the system says so.
python3 - <<'PYEOF' 2>/dev/null || printf 'could not determine the graph driver\n'
import json, os, re, subprocess, sys

try:
    objs = json.loads(subprocess.run(["pw-dump"], capture_output=True,
                                     text=True, timeout=10).stdout)
except Exception:
    sys.exit(1)

nodes, drivers = {}, set()
for o in objs:
    if o.get("type") != "PipeWire:Interface:Node":
        continue
    p = o.get("info", {}).get("props", {})
    nodes[o["id"]] = p
    d = p.get("node.driver-id")
    if d:
        drivers.add(int(d))

if not drivers:
    print("no node is driving the graph")
for d in sorted(drivers):
    p = nodes.get(d, {})
    name = p.get("node.name", "?")
    note = ("INPUT -- an input is the clock; is 50-waveline-driver-policy.conf "
            "installed?" if name.startswith("alsa_input.") else "output, as it should be")
    print("driver %s: %s  [%s]" % (d, name, note))

print()
print("capture devices (delay straight from ALSA):")
ids = {}
try:
    for line in open("/proc/asound/cards"):
        m = re.match(r"\s*(\d+)\s*\[([^]]*)\]", line)
        if m:
            ids[m.group(1)] = m.group(2).strip()
except OSError:
    pass

for nid, p in sorted(nodes.items()):
    if p.get("media.class") != "Audio/Source":
        continue
    card = str(p.get("api.alsa.pcm.card", ""))
    if not card:
        continue
    base = "/proc/asound/card%s/pcm0c/sub0" % card
    try:
        st = open(base + "/status").read()
        hw = open(base + "/hw_params").read()
    except OSError:
        continue
    def f(txt, key):
        m = re.search(r"^%s\s*:\s*(\S+)" % key, txt, re.M)
        return m.group(1) if m else None
    state = f(st, "state")
    delay = f(st, "delay")
    rate = f(hw, "rate")
    buf = f(hw, "buffer_size")
    ms = "-"
    # Same plausibility bound the daemon applies. The sc0710 capture card
    # reports delay 0 while claiming RUNNING, and has been seen returning a
    # value larger than the buffer it lives in -- neither is a latency, and a
    # bug report full of 9.6e16 ms helps nobody.
    if state == "RUNNING" and delay and rate:
        d = int(delay)
        limit = int(buf) if buf else int(rate)
        if d <= 0 or d > limit:
            ms = "implausible (%s frames, buffer %s) -- status file is unreliable" % (
                delay, buf or "?")
        else:
            ms = "%.1f ms" % (d / int(rate) * 1000)
    print("  card %-2s %-10s %-9s %8s  hr=%-5s per=%-5s drv=%s %s" % (
        card, ids.get(card, "?"), state or "?", ms,
        p.get("api.alsa.headroom", "-"), p.get("api.alsa.period-size", "-"),
        p.get("node.driver-id", "self"), p.get("node.name", "")))
PYEOF

UDR=""
for p in /etc/udev/rules.d/60-waveline-wave3.rules /usr/lib/udev/rules.d/60-waveline-wave3.rules; do
	[[ -f "$p" ]] && UDR="$p"
done
printf 'udev rule       : %s\n' "${UDR:-NOT installed}"

if [[ -z "$USBDEV" ]]; then
	printf '\n!! The Wave:3 was not found on USB. If it was working before, note\n'
	printf '!! that a wedged device survives a reboot -- VBUS stays powered.\n'
	printf '!! Unplug it for 30 seconds before drawing conclusions.\n'
fi

sec "USB DEVICE"
run "lsusb (all)" lsusb
run "lsusb -t (topology)" lsusb -t
printf '\n--- lsusb -v -d %s ---\n' "$VID_PID"
if command -v lsusb >/dev/null 2>&1; then
	# Verbose descriptors need no privileges for most fields; some are
	# root-only and simply come back blank.
	lsusb -v -d "$VID_PID" 2>&1
else
	echo "(not available: usbutils is not installed)"
fi

printf '\n--- interface driver bindings ---\n'
if [[ -n "$USBDEV" ]]; then
	for i in "$USBDEV":*; do
		[[ -d "$i" ]] || continue
		drv="$(basename "$(readlink -f "$i/driver" 2>/dev/null)" 2>/dev/null)"
		[[ "$drv" == "driver" || -z "$drv" ]] && drv="(none)"
		printf '%-12s class=%s subclass=%s driver=%s\n' \
			"$(basename "$i")" \
			"$(cat "$i/bInterfaceClass" 2>/dev/null)" \
			"$(cat "$i/bInterfaceSubClass" 2>/dev/null)" \
			"$drv"
	done
	printf '\nNote: interfaces 3 (vendor) and 4 (DFU) are expected to show\n'
	printf 'driver=(none). Interface 3 having a driver would mean something\n'
	printf 'else is managing the device. See docs/protocol.md.\n'
else
	echo "(device not present)"
fi

printf '\n--- usbfs node permissions ---\n'
if [[ -n "$USBDEV" && -r "$USBDEV/busnum" && -r "$USBDEV/devnum" ]]; then
	NODE="$(printf '/dev/bus/usb/%03d/%03d' "$(cat "$USBDEV/busnum")" "$(cat "$USBDEV/devnum")")"
	ls -l "$NODE" 2>&1
	getfacl -p "$NODE" 2>/dev/null | grep -v '^#' | grep -v '^$' || true
	printf '(writable by me: %s)\n' "$([[ -w "$NODE" ]] && echo yes || echo "no -- vendor control needs the udev rule")"
else
	echo "(device not present)"
fi

printf '\n--- power / runtime pm ---\n'
if [[ -n "$USBDEV" ]]; then
	for p in power/control power/runtime_status power/autosuspend_delay_ms; do
		printf '%-32s %s\n' "$p" "$(cat "$USBDEV/$p" 2>/dev/null || echo '(n/a)')"
	done
else
	echo "(device not present)"
fi

sec "ALSA"
cat_file /proc/asound/cards
if [[ -n "$CARD" ]]; then
	printf '\n--- /proc/asound/%s/stream0 ---\n' "$CARD"
	cat "/proc/asound/$CARD/stream0" 2>&1
	printf '\nExpected on a healthy device: both directions list\n'
	printf 'Rates: 48000, 96000 / Bits: 24, and both channel maps\n'
	printf '(FL FR for playback, MONO for capture). Compare with\n'
	printf 'reference/stock-stream0.txt.\n'

	run "amixer contents" amixer -c "$CARD_NUM" contents
	for s in pcm0p pcm0c; do
		cat_file "/proc/asound/$CARD/$s/sub0/hw_params"
		cat_file "/proc/asound/$CARD/$s/sub0/status"
	done
else
	echo "(no Wave:3 ALSA card -- the sections below will be empty)"
fi
run "aplay -l" aplay -l
run "arecord -l" arecord -l

sec "PIPEWIRE / WIREPLUMBER"
run "pactl info" pactl info
printf '\n--- pactl list cards ---\n'
if command -v pactl >/dev/null 2>&1; then
	pactl list cards 2>&1
else
	echo "(not available: pactl is not installed)"
fi
printf '\n--- waveline sinks and sources ---\n'
if command -v pactl >/dev/null 2>&1; then
	{ pactl list sinks; pactl list sources; } 2>&1 \
	  | grep -iA30 'elgato\|wave'
else
	echo "(not available: pactl is not installed)"
fi
printf '\n--- wpctl status ---\n'
if command -v wpctl >/dev/null 2>&1; then
	wpctl status 2>&1
else
	echo "(not available: wpctl is not installed)"
fi

printf '\n--- effective quantum settings ---\n'
if command -v pw-metadata >/dev/null 2>&1; then
	pw-metadata -n settings 2>&1 | grep -i quantum || echo "(no quantum keys reported)"
else
	echo "(not available: pw-metadata is not installed)"
fi

printf '\n--- this repo'"'"'s config files, as installed ---\n'
for f in "$WPC" "$PWC"; do
	printf '\n>> %s\n' "$f"
	[[ -f "$f" ]] && cat "$f" || echo "(not installed)"
done

printf '\n--- other pipewire/wireplumber drop-ins (conflicts live here) ---\n'
for d in "$HOME/.config/pipewire/pipewire.conf.d" \
         "$HOME/.config/wireplumber/wireplumber.conf.d" \
         /etc/pipewire/pipewire.conf.d /etc/wireplumber/wireplumber.conf.d \
         /usr/share/wireplumber/wireplumber.conf.d; do
	[[ -d "$d" ]] && ls -l "$d" 2>/dev/null | sed "s|^|$d: |"
done

sec "KERNEL"
run "lsmod (sound + usb)" bash -c "lsmod | grep -E 'snd|usb' || true"
run "modinfo snd-usb-audio" modinfo snd-usb-audio
run "dkms status" dkms status

printf '\n--- kernel log: Wave:3 and USB audio errors ---\n'
if command -v journalctl >/dev/null 2>&1; then
	LOG="$(journalctl -k --no-pager -b 2>&1)"
	if printf '%s' "$LOG" | grep -q 'No journal files\|Operation not permitted\|not.*permission'; then
		echo "(journal not readable as this user -- add yourself to the"
		echo " 'systemd-journal' group, or re-run with sudo for this section)"
	else
		printf '%s\n' "$LOG" \
		  | grep -iE 'wave|elgato|snd.usb|usb_set_interface|cannot set freq|-110|xhci.*fail' \
		  | tail -60
		printf '\n(matches above; empty is good -- "-110" or "cannot set freq"\n'
		printf 'means the lockup the DKMS patch addresses.)\n'
	fi
else
	echo "(not available: journalctl is not installed)"
fi

sec "END OF REPORT"
} 2>&1 | redact > "$OUT"

# ------------------------------------------------------------------- finish
chmod 600 "$OUT"
echo "wrote $OUT  ($(du -h "$OUT" | cut -f1))"
# Verify rather than assume. Each rule above can be defeated by output this
# script has not seen -- a serial split across a line wrap, a username the
# three-character floor skipped -- and finding that out here is much cheaper
# than finding it out in a public issue thread.
LEAKED=0
check_redacted() {
	local label="$1" value="$2"
	[[ -n "$value" ]] || return 0
	if grep -qF -- "$value" "$OUT"; then
		echo "WARNING: your $label still appears in the report -- remove it by hand" >&2
		LEAKED=1
	fi
}
check_redacted "device serial number" "$SERIAL"
check_redacted "home directory" "$REPORT_HOME"
[[ ${#REPORT_USER} -ge 3 ]] && check_redacted "username" "$REPORT_USER"
[[ ${#REPORT_HOST} -ge 3 && "$REPORT_HOST" != "localhost" ]] \
  && check_redacted "hostname" "$REPORT_HOST"
[[ $LEAKED -eq 0 ]] && echo "redacted: serial number, home directory, username, hostname"

echo "review it before posting; it describes your audio setup in detail"

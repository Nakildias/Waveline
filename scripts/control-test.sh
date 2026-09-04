#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
# Control test: does capture work with the STOCK in-tree driver?
#
# Answers the one question I cannot answer without root -- whether the quirk
# broke capture, or whether capture was already broken on this machine.
#
#   sudo ./scripts/control-test.sh
#
# Leaves the DKMS module loaded again at the end.
set -uo pipefail

[[ $EUID -eq 0 ]] || { echo "run as root" >&2; exit 1; }
RUSER="${SUDO_USER:-$USER}"; UD="/run/user/$(id -u "$RUSER")"
# Verify the stop actually worked before drawing conclusions.
OUT="/tmp/wave3-control"; mkdir -p "$OUT"; chmod 777 "$OUT"
# DKMS moves the in-tree module aside on install; prefer its saved original.
# Locate the Wave:3 card rather than assuming card2.
CARD=""
for f in /proc/asound/card*/usbid; do
	[[ -r "$f" ]] && [[ "$(cat "$f")" == "0fd9:0070" ]] && CARD="$(basename "$(dirname "$f")")"
done
[[ -n "$CARD" ]] || { echo "Wave:3 ALSA card not found" >&2; exit 1; }
STOCK="/var/lib/dkms/snd-usb-audio-waveline/original_module/$(uname -r)/x86_64/snd-usb-audio.ko.zst"
[[ -f "$STOCK" ]] || STOCK="/lib/modules/$(uname -r)/kernel/sound/usb/snd-usb-audio.ko.zst"
[[ -f "$STOCK" ]] || { echo "stock module not found" >&2; exit 1; }
# Must be runuser + env: `sudo -u user VAR=val` does not set the env, and
# systemctl --user also needs DBUS_SESSION_BUS_ADDRESS or it silently targets
# root's (nonexistent) session and leaves PipeWire holding the module.
# Errors are NOT swallowed here -- a failed stop invalidates the whole test.
usvc() {
	runuser -u "$RUSER" -- env "XDG_RUNTIME_DIR=$UD" \
		"DBUS_SESSION_BUS_ADDRESS=unix:path=$UD/bus" systemctl --user "$@"
}
UNITS="wireplumber pipewire-pulse.service pipewire-pulse.socket pipewire.service pipewire.socket"

cap() { # $1 = label
	local f="$OUT/$1.wav"
	rm -f "$f"
	timeout 15 arecord -D hw:Wave3 -f S24_3LE -c1 -r48000 -d 3 "$f" 2>"$OUT/$1.err"
	local rc=$? sz=0
	[[ -f "$f" ]] && sz=$(stat -c%s "$f")
	echo "    exit=$rc  bytes=$sz  $( [[ $sz -gt 44 ]] && echo 'DATA CAPTURED' || echo 'NO DATA')"
	sed 's/^/      /' "$OUT/$1.err"
	python3 - "$f" 2>/dev/null <<'EOF'
import sys
d=open(sys.argv[1],'rb').read()[44:]
s=[int.from_bytes(d[i:i+3],'little',signed=True) for i in range(0,len(d)-2,3)]
if s: print(f"      frames={len(s)} peak={max(abs(x) for x in s)} nonzero={sum(1 for x in s if x)}")
EOF
}

echo "==> stopping audio stack"
usvc stop $UNITS; sleep 2
if [[ "$(cat /sys/module/snd_usb_audio/refcnt 2>/dev/null || echo 0)" != "0" ]]; then
	echo "!! snd_usb_audio still in use -- the module swap would silently not happen." >&2
	fuser -v /dev/snd/* 2>&1 | sed 's/^/   /' >&2
	exit 1
fi

MARK=$(date '+%Y-%m-%d %H:%M:%S')

echo
echo "=== A: PATCHED module (DKMS) ==="
modinfo -F filename snd-usb-audio | sed 's/^/    /'
cap patched

echo
echo "=== B: STOCK in-tree module ==="
modprobe -r snd-usb-audio 2>&1 | sed 's/^/    /'
modprobe snd-usbmidi-lib 2>/dev/null
if insmod "$STOCK" 2>&1 | sed 's/^/    /'; then :; fi
sleep 3
if [[ -e /proc/asound/$CARD/stream0 ]]; then
	echo "    --- stock stream0 (capture) ---"
	sed -n '/^Capture:/,$p' /proc/asound/$CARD/stream0 | sed 's/^/    /'
fi
cap stock

echo
echo "=== kernel log since $MARK ==="
journalctl -k --since "$MARK" --no-pager 2>/dev/null | grep -iE "usb|audio|snd" | tail -40

echo
echo "==> restoring patched module"
modprobe -r snd-usb-audio 2>/dev/null
modprobe snd-usb-audio
sleep 2
modinfo -F filename snd-usb-audio | sed 's/^/    /'
usvc start pipewire.socket pipewire-pulse.socket; usvc start wireplumber
echo "==> done. Artifacts in $OUT"

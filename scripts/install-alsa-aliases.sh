#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
#
# Ensure ~/.config/alsa/asoundrc includes waveline.conf. The PCM list itself
# is written by wavelined whenever input devices / published mics change.
#
# Usage:
#   install-alsa-aliases.sh install [HOME]
#   install-alsa-aliases.sh remove  [HOME]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/data/alsa/waveline.conf"
ACTION="${1:-install}"
HOME_N="${2:-${HOME:-}}"

[[ -n "$HOME_N" && -d "$HOME_N" ]] || {
	echo "install-alsa-aliases: home directory required" >&2
	exit 1
}

ALSAD="$HOME_N/.config/alsa"
CONF="$ALSAD/waveline.conf"
ASOUNDRC="$ALSAD/asoundrc"
INCLUDE_LINE="<$CONF>"
MARKER="# Waveline ALSA device aliases"

ensure_include() {
	mkdir -p "$ALSAD"
	if [[ -f "$ASOUNDRC" ]] && grep -Fq "$CONF" "$ASOUNDRC"; then
		return 0
	fi
	local tmp
	tmp="$(mktemp)"
	{
		echo "$MARKER"
		echo "$INCLUDE_LINE"
		echo
		[[ -f "$ASOUNDRC" ]] && cat "$ASOUNDRC"
	} >"$tmp"
	mv "$tmp" "$ASOUNDRC"
	chmod 644 "$ASOUNDRC"
}

remove_include() {
	[[ -f "$ASOUNDRC" ]] || return 0
	local tmp
	tmp="$(mktemp)"
	awk -v conf="$CONF" -v marker="$MARKER" '
		$0 == marker { next }
		index($0, conf) { next }
		{ print }
	' "$ASOUNDRC" >"$tmp"
	awk 'NR==1 && /^$/ { next } { print }' "$tmp" >"$ASOUNDRC"
	rm -f "$tmp"
	if [[ ! -s "$ASOUNDRC" ]]; then
		rm -f "$ASOUNDRC"
	fi
}

case "$ACTION" in
install)
	[[ -f "$SRC" ]] || {
		echo "install-alsa-aliases: missing $SRC" >&2
		exit 1
	}
	mkdir -p "$ALSAD"
	# Do not clobber a live daemon-generated list on reinstall.
	if [[ ! -f "$CONF" ]]; then
		install -m644 "$SRC" "$CONF"
	fi
	ensure_include
	echo "ALSA include ready: ${CONF/#$HOME_N/\$HOME} (PCMs filled by wavelined)"
	;;
remove)
	rm -f "$CONF"
	remove_include
	rmdir "$ALSAD" 2>/dev/null || true
	echo "Removed ALSA aliases from ${ALSAD/#$HOME_N/\$HOME}"
	;;
*)
	echo "usage: $0 install|remove [HOME]" >&2
	exit 1
	;;
esac

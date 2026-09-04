# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
# Waveline device profiles -- shared by install.sh and uninstall.sh.
#
# Sourced, never executed:
#
#   source scripts/lib/profiles.sh
#   profile_load "$(profile_resolve wave3)"   # fills the PROFILE_* variables
#   profile_detect                            # ids of profiles that are plugged in
#
# Layout: devices/<brand>/<category>/<id>/device.conf  (plus devices/generic/).
# See devices/README.md.

# Every key a profile may set. Cleared before each load so a key one profile
# omits can never be inherited from the profile loaded before it -- which would
# quietly install the previous device's udev rule for this one.
WAVELINE_PROFILE_KEYS=(
	PROFILE_ID PROFILE_LABEL BRAND USB_IDS PCI_IDS ALSA_NODE_MATCH
	HARDWARE_CONTROLS HARDWARE_VENDOR_ID HARDWARE_PRODUCT_ID
	KERNEL_PATCH WIREPLUMBER_CONF PIPEWIRE_CONF UDEV_RULES SYNC_SERVICE
)

# All non-generic profile directories (devices/<brand>/<category>/<id>), then
# generic last: it is the fallback and should never win a match against a real
# device.
profile_dirs() {
	local conf d
	while IFS= read -r -d '' conf; do
		d="$(dirname "$conf")"
		printf '%s\n' "$d"
	done < <(find "$WAVELINE_ROOT/devices" \
		-mindepth 4 -maxdepth 4 -type f -name device.conf -print0 2>/dev/null \
		| sort -z)
	[[ -f "$WAVELINE_ROOT/devices/generic/device.conf" ]] \
	  && printf '%s\n' "$WAVELINE_ROOT/devices/generic"
	return 0
}

# Absolute path of the profile directory for a device id (directory basename /
# PROFILE_ID), or failure if unknown.
profile_resolve() {
	local id="$1" d
	[[ -n "$id" ]] || return 1
	if [[ "$id" == "generic" ]]; then
		[[ -f "$WAVELINE_ROOT/devices/generic/device.conf" ]] || return 1
		printf '%s\n' "$WAVELINE_ROOT/devices/generic"
		return 0
	fi
	while read -r d; do
		[[ "$(basename "$d")" == "$id" ]] || continue
		printf '%s\n' "$d"
		return 0
	done < <(profile_dirs)
	return 1
}

# Reads one profile into the PROFILE_* variables. PROFILE_DIR is set too, since
# every path key is relative to it.
profile_load() {
	local dir="$1" k
	for k in "${WAVELINE_PROFILE_KEYS[@]}"; do unset "$k"; done
	# shellcheck disable=SC1091
	source "$dir/device.conf"
	for k in "${WAVELINE_PROFILE_KEYS[@]}"; do
		[[ -v "$k" ]] || printf -v "$k" '%s' ''
	done
	PROFILE_DIR="$dir"
	# BRAND is what the UI puts in front of everything, so it must never be
	# empty -- an unbranded "Stream Mix" is not identifiable in a device list.
	[[ -n "$BRAND" ]] || BRAND="Waveline"
}

# Resolves one of a profile's path keys to an absolute path, or nothing when
# the profile does not set it. Keeps the "is it set, and does it exist" test in
# one place rather than at all eight call sites.
profile_file() {
	local rel="$1"
	[[ -n "$rel" ]] || return 1
	[[ -f "$PROFILE_DIR/$rel" ]] || return 1
	printf '%s\n' "$PROFILE_DIR/$rel"
}

# True when any of this profile's USB_IDS or PCI_IDS is present on the bus.
# Falls back to /sys when lsusb/lspci are missing.
profile_present() {
	local id vid pid
	if [[ -n "$USB_IDS" ]]; then
		for id in $USB_IDS; do
			vid="${id%%:*}"; pid="${id##*:}"
			if command -v lsusb >/dev/null 2>&1; then
				lsusb -d "$id" >/dev/null 2>&1 && return 0
			fi
			local f
			for f in /sys/bus/usb/devices/*/idVendor; do
				[[ -r "$f" ]] || continue
				[[ "$(cat "$f")" == "$vid" ]] || continue
				[[ "$(cat "${f%idVendor}idProduct" 2>/dev/null)" == "$pid" ]] \
				  && return 0
			done
		done
	fi
	if [[ -n "$PCI_IDS" ]]; then
		for id in $PCI_IDS; do
			vid="${id%%:*}"; pid="${id##*:}"
			vid="${vid#0x}"; pid="${pid#0x}"
			vid="$(printf '%s' "$vid" | tr 'A-F' 'a-f')"
			pid="$(printf '%s' "$pid" | tr 'A-F' 'a-f')"
			if command -v lspci >/dev/null 2>&1; then
				lspci -nn -d "${vid}:${pid}" 2>/dev/null | grep -q . && return 0
			fi
			local f
			for f in /sys/bus/pci/devices/*/vendor; do
				[[ -r "$f" ]] || continue
				local sv sp
				sv="$(cat "$f" 2>/dev/null | tr -d '[:space:]' | tr 'A-F' 'a-f')"
				sp="$(cat "${f%vendor}device" 2>/dev/null | tr -d '[:space:]' | tr 'A-F' 'a-f')"
				sv="${sv#0x}"; sp="${sp#0x}"
				# Zero-pad to 4 hex digits for comparison.
				sv="$(printf '%04x' "0x$sv" 2>/dev/null || printf '%s' "$sv")"
				sp="$(printf '%04x' "0x$sp" 2>/dev/null || printf '%s' "$sp")"
				[[ "$sv" == "$(printf '%04x' "0x$vid" 2>/dev/null || printf '%s' "$vid")" ]] || continue
				[[ "$sp" == "$(printf '%04x' "0x$pid" 2>/dev/null || printf '%s' "$pid")" ]] \
				  && return 0
			done
		done
	fi
	return 1
}

# Ids of every profile whose device is plugged in right now, one per line.
# Empty output means nothing recognised -- the caller falls back to generic.
profile_detect() {
	local dir
	while read -r dir; do
		[[ -n "$dir" ]] || continue
		( profile_load "$dir"; profile_present ) && basename "$dir"
	done < <(profile_dirs)
	return 0
}

# The ALSA card index for the currently loaded profile's device, if it has one.
profile_alsa_card() {
	local id f
	[[ -n "$USB_IDS" ]] || return 1
	for f in /proc/asound/card*/usbid; do
		[[ -r "$f" ]] || continue
		for id in $USB_IDS; do
			[[ "$(cat "$f")" == "$id" ]] \
			  && { basename "$(dirname "$f")"; return 0; }
		done
	done
	return 1
}

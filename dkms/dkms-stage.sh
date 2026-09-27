#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
# DKMS PRE_BUILD hook: stage a patched sound/usb/ for the kernel being built.
#
#   dkms-stage.sh <kernel release>
#
# The module is not a self-contained driver: it is the kernel's own sound/usb
# with a profile's patch applied, so the sources have to come from the kernel
# being built for, not from whichever kernel install.sh happened to run on.
# Shipping one snapshot and letting DKMS rebuild it against newer headers is
# what used to make an automatic rebuild unsafe. Re-staging per kernel is what
# makes it safe: every patch asserts its own anchors, a kernel that moved the
# code fails here, DKMS installs nothing for it, and the stock driver is used.
#
# Runs from the DKMS build directory, which is a copy of this package's source
# tree. The profiles to patch for were recorded by install.sh in ./profiles --
# they cannot be detected here, because the device need not be plugged in when
# a kernel update runs.
set -euo pipefail

KREL="${1:?usage: dkms-stage.sh <kernel release>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PROFILES="$(tr '\n' ' ' < "$HERE/profiles" 2>/dev/null || true)"
[[ -n "${PROFILES// /}" ]] || {
	echo "!! no profiles recorded in $HERE/profiles -- re-run install.sh" >&2
	exit 1
}

# Upstream tarballs are shared by every kernel this package is built for, and
# kept between runs so a reinstall of the same kernel does not fetch again.
# Anything not used for a month is from a kernel long since replaced.
CACHE="${WAVELINE_CACHE:-/var/cache/waveline}"
mkdir -p "$CACHE"
find "$CACHE" -maxdepth 1 -name 'linux-*.tar.xz' -mtime +30 -delete 2>/dev/null || true

WAVELINE_PROFILES="$PROFILES" WAVELINE_SRC="$HERE/src" WAVELINE_CACHE="$CACHE" \
	bash "$HERE/scripts/prepare-src.sh" "$KREL"

# Mark the tarball as used, for the pruning above.
touch "$CACHE"/linux-*.tar.xz 2>/dev/null || true

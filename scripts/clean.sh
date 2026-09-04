#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
# Remove generated artifacts from the working tree.
#
#   ./scripts/clean.sh          remove src/ (staged kernel sources + build output)
#   ./scripts/clean.sh --all    also remove .build/ (the cached kernel tarball)
#
# Nothing here is needed at runtime. An installed module lives in
# /var/lib/dkms and /usr/src, not in this directory, so cleaning does not
# uninstall anything. `install.sh` regenerates whatever it needs.
#
# Both directories are in .gitignore already; this is about disk space, not
# about what reaches GitHub.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Refuse to run anywhere that does not look like this repo.
[[ -f "$ROOT/install.sh" && -d "$ROOT/devices" ]] \
  || { echo "not the Waveline tree; refusing" >&2; exit 1; }
cd "$ROOT" || exit 1

human() { du -sh "$1" 2>/dev/null | cut -f1; }

FREED=0
# Python bytecode from scripts/waveline-hw. Harmless, but generated and does
# not belong in the tree.
for d in scripts/__pycache__; do
	[[ -d "$d" ]] && { rm -rf "${ROOT:?}/$d"; echo "  removing $d/"; FREED=1; }
done
# The FluidSynth library install.sh copies out of the distribution package.
# Regenerated on the next install; removing it here means a stale copy cannot
# outlive a FluidSynth upgrade.
for f in app/lib/libfluidsynth.so app/lib/libfluidsynth.so.*; do
	[[ -f "$ROOT/$f" ]] && { rm -f "${ROOT:?}/$f"; echo "  removing $f"; FREED=1; }
done

for d in src src-orig; do
	if [[ -d "$d" ]]; then
		echo "  removing $d/ ($(human "$d"))"
		rm -rf "${ROOT:?}/$d"
		FREED=1
	fi
done

if [[ "${1:-}" == "--all" ]]; then
	if [[ -d .build ]]; then
		echo "  removing .build/ ($(human .build)) -- the kernel tarball will be"
		echo "  re-downloaded on the next install"
		rm -rf "${ROOT:?}/.build"
		FREED=1
	fi
else
	[[ -d .build ]] && echo "  keeping .build/ ($(human .build)) -- cached kernel tarball;"
	[[ -d .build ]] && echo "  use --all to remove it too (costs a re-download later)"
fi

[[ $FREED -eq 0 ]] && echo "  already clean"
echo "  tree is now $(human .)"

if command -v dkms >/dev/null && dkms status -m snd-usb-audio-waveline 2>/dev/null | grep -q installed; then
	echo "  note: the installed module is untouched --"
	dkms status -m snd-usb-audio-waveline | sed 's/^/        /'
fi

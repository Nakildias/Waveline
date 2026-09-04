#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
# Stage a standalone, patched copy of sound/usb/ that builds out of tree.
#
#   ./scripts/prepare-src.sh [kernel-release]
#
# Defaults to the running kernel. Produces ./src/ ready for
#   make -C /usr/lib/modules/<rel>/build M=$PWD/src modules
#
# Which patches get applied is decided by device profiles, not by this script:
# every profile in devices/ that sets KERNEL_PATCH and whose device is plugged
# in contributes one. Override the detection with
#
#   WAVELINE_PROFILES="wave3" ./scripts/prepare-src.sh
#
# and stage the patches for those profiles whether or not the hardware is here
# -- which is how you build for a device that is currently unplugged, and how
# install.sh passes down what it already detected.
#
# With no profile contributing a patch this stages a pristine sound/usb/ and
# says so. That is a legitimate outcome, not a failure: most microphones need
# no kernel change at all.
set -euo pipefail

KREL="${1:-$(uname -r)}"
KVER="${KREL%%-*}"                       # 7.1.5-arch1-1 -> 7.1.5
KMAJ="${KVER%%.*}"                       # -> 7
[[ "$KVER" == *.*.0 ]] && KVER="${KVER%.0}"   # kernel.org ships x.y, not x.y.0

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WAVELINE_ROOT="$ROOT"
BUILDDIR="$ROOT/.build"
SRC="$ROOT/src"
KBUILD="/usr/lib/modules/$KREL/build"
TARBALL="linux-$KVER.tar.xz"
KURL="https://cdn.kernel.org/pub/linux/kernel/v${KMAJ}.x"
URL="$KURL/$TARBALL"
SUMS_URL="$KURL/sha256sums.asc"

# shellcheck source=lib/profiles.sh
source "$ROOT/scripts/lib/profiles.sh"

echo "==> target kernel $KREL (upstream $KVER)"

# Staging the sources needs curl, tar and python3 -- not a kernel build
# directory. It is only checked here so the "build with:" line at the end can
# be right, and so a missing one is noticed now rather than after the download.
# Not fatal: on an atomic distro the sources are staged in one environment and
# compiled in another, and install.sh has already decided the compile is
# possible before calling this.
if [[ ! -d "$KBUILD" ]]; then
	for alt in "/lib/modules/$KREL/build" $(ls -d /usr/src/kernels/*/ 2>/dev/null); do
		[[ -d "$alt" ]] && { KBUILD="$alt"; break; }
	done
fi
if [[ ! -d "$KBUILD" ]]; then
	echo "!! no kernel build directory for $KREL. Install the headers first:" >&2
	echo "     Arch:   sudo pacman -S --needed linux-headers dkms" >&2
	echo "     Fedora: sudo dnf install kernel-devel-$KREL" >&2
	echo "   Continuing anyway -- staging the sources needs no headers." >&2
fi

# Profiles to patch for: told to us, or worked out from what is plugged in.
PROFILES="${WAVELINE_PROFILES:-}"
[[ -n "$PROFILES" ]] || PROFILES="$(profile_detect | tr '\n' ' ')"

# Check a tarball against kernel.org's published sha256sums.asc.
#
# What this buys: a truncated or corrupted download, a CDN edge serving a stale
# or wrong file, and a cached tarball in .build/ that something on this machine
# has since altered are all caught before its contents are compiled into a
# kernel module and loaded as root. Given that this tree becomes ring-0 code,
# "the download probably went fine" is not a good enough answer.
#
# What it does not buy: sha256sums.asc comes from the same origin as the
# tarball, so a compromised kernel.org would simply publish a matching hash.
# Defending against that means verifying the PGP signature on the sums file
# (it is clearsigned, hence the .asc) against the kernel.org release keys --
# which needs a keyring this script has no business installing on your machine.
# If you want that guarantee, do it by hand:
#
#     gpg --locate-keys torvalds@kernel.org gregkh@kernel.org
#     curl -fLO "$SUMS_URL" && gpg --verify sha256sums.asc
#
# A missing or unreachable sums file is a warning, not an error: kernel.org has
# been reachable enough to serve the tarball at this point, but an air-gapped
# mirror or a hand-placed file in .build/ is a legitimate way to work.
verify_tarball() {
	local file="$1" want got
	command -v sha256sum >/dev/null || {
		echo "!! sha256sum is not installed; cannot verify $TARBALL" >&2
		return 0
	}
	want="$(curl -fsSL "$SUMS_URL" 2>/dev/null \
	        | awk -v f="$TARBALL" '$2 == f { print $1; exit }')"
	if [[ -z "$want" ]]; then
		echo "!! could not fetch a checksum for $TARBALL from kernel.org;" >&2
		echo "   continuing unverified." >&2
		return 0
	fi
	got="$(sha256sum "$file" | cut -d' ' -f1)"
	if [[ "$got" != "$want" ]]; then
		echo "!! checksum mismatch for $TARBALL" >&2
		echo "   expected $want" >&2
		echo "   got      $got" >&2
		echo "   Refusing to build a kernel module from it. Delete it and re-run:" >&2
		echo "     rm $file" >&2
		return 1
	fi
	echo "==> sha256 verified against kernel.org"
	return 0
}

mkdir -p "$BUILDDIR"
if [[ ! -f "$BUILDDIR/$TARBALL" ]]; then
	echo "==> downloading $URL"
	# Downloaded to .part and only renamed once verified, so an interrupted or
	# rejected download cannot be mistaken for a cached one on the next run.
	curl -fL --progress-bar -o "$BUILDDIR/$TARBALL.part" "$URL"
	if ! verify_tarball "$BUILDDIR/$TARBALL.part"; then
		rm -f "$BUILDDIR/$TARBALL.part"
		exit 1
	fi
	mv "$BUILDDIR/$TARBALL.part" "$BUILDDIR/$TARBALL"
else
	# Cached from an earlier run. Re-checked rather than trusted: .build/ is an
	# ordinary directory in the user's home that anything can write to.
	verify_tarball "$BUILDDIR/$TARBALL" || exit 1
fi

echo "==> extracting sound/usb"
rm -rf "$SRC"
mkdir -p "$SRC"
# Top level of sound/usb only: the subdirectories are separate modules.
tar -xJf "$BUILDDIR/$TARBALL" -C "$SRC" --strip-components=3 \
	--wildcards --no-wildcards-match-slash \
	"linux-$KVER/sound/usb/*.c" \
	"linux-$KVER/sound/usb/*.h" \
	"linux-$KVER/sound/usb/Makefile"

# Drop the "descend into subdirectory" rules; those build unrelated modules
# (line6, caiaq, 6fire, usx2y, qcom, ...) that we did not extract.
sed -i -E '/\+=.*\/[[:space:]]*$/d' "$SRC/Makefile"

# Applied in the order profile_dirs lists them, so a tree built from the same
# set of profiles is byte-identical from run to run. Each script asserts its own
# anchors and exits non-zero if the kernel moved the code out from under it;
# set -e then aborts the whole stage rather than leaving a half-patched tree.
APPLIED=0
for id in $PROFILES; do
	dir="$(profile_resolve "$id")"
	[[ -n "$dir" ]] || continue
	[[ -f "$dir/device.conf" ]] || { echo "!! unknown profile: $id" >&2; exit 1; }
	profile_load "$dir"
	patch_script="$(profile_file "$KERNEL_PATCH")" || continue
	echo "==> applying $PROFILE_LABEL kernel patch"
	python3 "$patch_script" "$SRC"
	APPLIED=$((APPLIED + 1))
done

if [[ $APPLIED -eq 0 ]]; then
	echo "==> no device profile needs a kernel patch -- staged tree is stock"
fi

echo "==> applying kernel API compatibility patches"
python3 "$ROOT/scripts/patches/bitfield-compat.py" "$SRC"

echo "==> ready: $SRC"
echo "    build with: make -C $KBUILD M=$SRC modules"

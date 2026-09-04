#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
# Waveline -- installer for Arch, Fedora and Debian/Ubuntu.
# https://github.com/Nakildias/Waveline
#
#   sudo ./install.sh
#   sudo ./install.sh --app-only    # rebuild and install waveline-mixer only
#   sudo ./install.sh --help
#
# Waveline is a Wave Link-style mixer for any microphone: channels, separate
# stream and monitor mixes, noise suppression, per-application routing. None of
# that is device-specific and all of it is installed on every machine.
#
# What IS device-specific -- a kernel quirk, a WirePlumber workaround, a vendor
# USB protocol -- lives in a profile under devices/. This script matches every
# profile's USB IDs against what is actually plugged in and installs only the
# parts belonging to profiles that matched. Nothing else gets another device's
# workarounds. See devices/README.md.
#
# Seven parts:
#
#   1. kernel patch   per matched profile, via DKMS. The Elgato Wave:3 needs one
#                     (it fixes the usb_set_interface -110 lockup); most
#                     microphones need none, and then this step does nothing.
#   2. wireplumber    per matched profile
#   3. pipewire       per matched profile
#   4. real-time      permission for the audio threads to outrank ordinary work,
#                     on every machine. Without it PipeWire has to ask rtkit,
#                     which grants 25 threads per user and leaves the rest of a
#                     mixer graph to be starved by a busy CPU.
#   5. hardware CLI   waveline-hw, for controls ALSA does not expose. Installed
#                     only when a matched profile has a vendor protocol.
#   6. mixer          wavelined + waveline-mixer. Built from source, so it is
#                     skipped when the toolchain is absent.
#   7. DeepFilterNet  the good noise suppression engine, and the default.
#      (opt)          Built from upstream Rust sources because no distribution
#                     packages libDF's C API. Skipped without cargo, or with
#                     WAVELINE_SKIP_DFN=1; the mixer then falls back to RNNoise.
#
# Parts 2-7 are pure userspace and work on any kernel. Part 1 needs kernel
# headers matching the *running* kernel; if that is not possible the script says
# so and still installs the rest.
#
# The mixer daemon IS enabled and started, at the very end of the run, so that
# `waveline-mixer` works straight afterwards with nothing else to type. It
# creates virtual sinks and routes audio, so if you would rather decide that
# yourself:
#
#   sudo WAVELINE_NO_AUTOSTART=1 ./install.sh   # install, do not start
#   systemctl --user disable --now wavelined    # undo it later
#
# To install a device profile's parts without that device plugged in:
#
#   sudo WAVELINE_PROFILES="wave3" ./install.sh
#
# Graph latency is no longer an install-time decision. Two files are installed
# on every machine -- data/pipewire/50-waveline-clock.conf pins the sample rate
# and sets a deterministic 512-frame cycle, and
# data/wireplumber/50-waveline-driver-policy.conf stops a capture device from
# becoming the graph's clock -- and the cycle itself is a control in the mixer
# that applies instantly. The old Pro Audio quantum fix (~85 ms on everything,
# installed by answering a prompt, removed by re-running this script) is now
# the "Pro Audio (85 ms)" entry in that control.
#
# Part 6 is the slow one: it downloads a Rust crate graph and compiles it, which
# takes a few minutes and around 1 GB of scratch space (removed afterwards).
# WAVELINE_SKIP_DFN=1 skips it entirely.
#
# Atomic / image-based distros (Silverblue, Kinoite, Bazzite, Bluefin, Aurora,
# SteamOS, openSUSE MicroOS) are supported and detected automatically. Nothing
# is layered onto the image and no reboot is needed: the toolchain comes from a
# rootless podman container built from the host's own base release, and the
# kernel patch -- when a detected microphone needs one at all -- is carried in
# /var by scripts/waveline-kmod instead of DKMS. Everything else already
# installs into $HOME and /etc, which are writable there. See ATOMIC-SUPPORT.md.
# Deliberately `set -uo pipefail` and NOT `set -e`.
#
# This script installs eight loosely coupled things, most of them optional, and
# it checks the status of each one explicitly. Under `set -e` a single failed
# optional step -- no cargo for DeepFilterNet, no kernel headers for the DKMS
# module, a package manager that could not reach its mirror -- would abandon the
# run partway through and leave a half-configured machine behind, which is a
# worse outcome than finishing and reporting what did not happen. `-u` and
# `pipefail` stay on, because an unset variable here is always a bug.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WAVELINE_ROOT="$ROOT"
# NB: do NOT call these NAME/VERSION -- /etc/os-release defines those
# and sourcing it below would silently clobber them (NAME="Arch Linux").
DKMS_NAME="snd-usb-audio-waveline"; DKMS_VER="1.0"
KREL="$(uname -r)"
FAILED_KERNEL=0
NEED_REBOOT=0

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
ok()   { printf '    \033[32mok\033[0m   %s\n' "$*"; }
warn() { printf '    \033[33mwarn\033[0m %s\n' "$*"; }
die()  { printf '\n\033[31mxx   %s\033[0m\n' "$*" >&2; exit 1; }

APP_ONLY=0
KERNEL_ONLY=0
for arg in "$@"; do
	case "$arg" in
		--app-only) APP_ONLY=1 ;;
		--kernel-only) KERNEL_ONLY=1 ;;
		--help|-h)
			cat <<'EOF'
Waveline installer

Usage:
  sudo ./install.sh                 Full install (kernel, audio stack, mixer, …)
  sudo ./install.sh --app-only      Rebuild and install waveline-mixer only
  sudo ./install.sh --kernel-only   Rebuild and install the kernel patch only
  sudo ./install.sh --help          Show this help

The --app-only path skips kernel patches, WirePlumber/PipeWire drop-ins,
DeepFilterNet, and daemon restarts. It rebuilds the mixer GUI and copies it
to ~/.local/bin/waveline-mixer. Restart the open mixer window to pick up
changes (the daemon is left running).

The --kernel-only path does nothing but rebuild the patched snd-usb-audio for
the running kernel and install it. This is what to run after a kernel update
on an atomic distro, where the module is carried in /var and does not survive
one; `sudo waveline-kmod rebuild` is the same thing by another name.

Environment variables (full install):
  WAVELINE_PROFILES="wave3"   Install a profile without the device plugged in
  WAVELINE_NO_AUTOSTART=1     Install without enabling/starting wavelined
  WAVELINE_PRO_AUDIO=1        Superseded: Pro Audio is the mixer's Latency setting
  WAVELINE_SKIP_DFN=1         Skip building DeepFilterNet
  WAVELINE_SKIP_FLUIDSYNTH=1  Do not install or stage FluidSynth (no MIDI
                              instruments; leaves app/lib/ untouched)

Atomic / image-based distros (Bazzite, SteamOS, Silverblue, Kinoite, …):
  WAVELINE_BUILD_IMAGE=…      Base image for the container build environment
  WAVELINE_NO_CONTAINER=1     Never containerise; fail instead
  WAVELINE_STEAMOS_UNLOCK=1   SteamOS only: build natively by unlocking /usr
                              (see ATOMIC-SUPPORT.md before using this)

Revert a full install:  sudo ./uninstall.sh
EOF
			exit 0
			;;
		*) die "unknown option: $arg (try --help)" ;;
	esac
done
[[ $APP_ONLY -eq 1 && $KERNEL_ONLY -eq 1 ]] \
  && die "--app-only and --kernel-only are mutually exclusive"

# shellcheck source=scripts/lib/profiles.sh
source "$ROOT/scripts/lib/profiles.sh"
# shellcheck source=scripts/lib/atomic.sh
source "$ROOT/scripts/lib/atomic.sh"

# --------------------------------------------------------------- preflight
[[ $EUID -eq 0 ]] || die "run as root:  sudo ./install.sh"
RUSER="${SUDO_USER:-}"
[[ -n "$RUSER" && "$RUSER" != "root" ]] \
  || die "run via sudo from your normal user account (sudo ./install.sh), not as root"
UID_N="$(id -u "$RUSER")"; UD="/run/user/$UID_N"
HOME_N="$(getent passwd "$RUSER" | cut -d: -f6)"
[[ -d "$HOME_N" ]] || die "cannot resolve home directory for $RUSER"
runu() { runuser -u "$RUSER" -- env XDG_RUNTIME_DIR="$UD" \
         DBUS_SESSION_BUS_ADDRESS="unix:path=$UD/bus" "$@"; }

BIND="$HOME_N/.local/bin"
# Bundled runtime libraries, when the host image does not ship one the mixer
# needs. Empty and unused on an ordinary distro -- see the atomic section below.
RUNTIMED="$HOME_N/.local/lib/waveline/runtime"

# Read os-release in subshells so it cannot clobber our variables.
OS_ID="$(. /etc/os-release 2>/dev/null; echo "${ID:-}")"
OS_LIKE="$(. /etc/os-release 2>/dev/null; echo "${ID_LIKE:-}")"
OS_PRETTY="$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-}")"
case "$OS_ID $OS_LIKE" in
	*arch*)            FAM=arch    ;;
	*fedora*|*rhel*)   FAM=fedora  ;;
	*debian*|*ubuntu*) FAM=debian  ;;
	*)                 FAM=unknown ;;
esac

mixer_deps_ok() {
	local tool lib
	for tool in cmake g++ pkg-config; do
		command -v "$tool" >/dev/null || { warn "missing $tool"; return 1; }
	done
	for lib in Qt6Widgets Qt6DBus Qt6Svg libpipewire-0.3 rnnoise; do
		pkg-config --exists "$lib" 2>/dev/null || { warn "missing $lib (pkg-config)"; return 1; }
	done
	return 0
}

# Same question, asked of whatever is actually going to do the compiling. In a
# container the answer is decided by the image recipe, but ask anyway: a package
# rename upstream should surface here and not as a wall of CMake errors.
build_deps_ok() {
	if [[ "$BUILDENV" == "host" ]]; then mixer_deps_ok; return $?; fi
	buildenv_ready || { warn "${BUILDENV_ERR:-build environment unavailable}"; return 1; }
	buildenv_run "$ROOT" sh -c '
		for t in cmake g++ pkg-config; do command -v $t >/dev/null || exit 1; done
		for l in Qt6Widgets Qt6DBus Qt6Svg libpipewire-0.3 rnnoise; do
			pkg-config --exists $l || exit 1
		done' >/dev/null 2>&1 \
	  || { warn "the build container is missing a development package"; return 1; }
	return 0
}

deps_hint() {
	case "$FAM" in
	arch)   warn "    sudo pacman -S --needed cmake qt6-base qt6-svg qt6-websockets pipewire rnnoise base-devel" ;;
	fedora) warn "    sudo dnf install cmake qt6-qtbase-devel qt6-qtsvg-devel qt6-qtwebsockets-devel pipewire-devel rnnoise-devel" ;;
	debian) warn "    sudo apt install cmake qt6-base-dev libqt6svg6-dev libqt6websockets6-dev libpipewire-0.3-dev librnnoise-dev" ;;
	*)      warn "    cmake, Qt6 (Widgets + DBus + Svg + WebSockets), libpipewire-0.3, rnnoise" ;;
	esac
}

# ------------------------------------------------------- atomic / image-based
# Everything above is the same everywhere. This decides where the compiler comes
# from, and it is the whole of what makes an atomic distro different: /usr is a
# read-only image, so the toolchain cannot be installed into it and DKMS has
# nowhere to put a module. Both have answers that touch neither.
atomic_detect

# On a mutable system there is nothing to decide: build on the host, exactly as
# every previous release of this script did.
HOST_PKGS=1          # may the system package manager be used?
if [[ $ATOMIC -eq 0 ]]; then
	BUILDENV="host"; BUILDENV_READY=1
else
	HOST_PKGS=0
fi

# SteamOS is the one atomic system with a supported, reversible way back to a
# writable /usr, and using it is strictly better than a container: the binaries
# then link against the very libraries they will run against. It is opt-in
# because a SteamOS update replaces the image and takes those packages with it,
# and because it is the user's system to unlock, not ours.
STEAMOS_RELOCK=0
steamos_unlock() {
	command -v steamos-readonly >/dev/null 2>&1 || return 1
	steamos-readonly disable >/dev/null 2>&1 || return 1
	STEAMOS_RELOCK=1
	# A Steam Deck ships without pacman's keyring populated.
	pacman-key --init >/dev/null 2>&1
	pacman-key --populate archlinux holo >/dev/null 2>&1
	return 0
}
steamos_relock() {
	[[ $STEAMOS_RELOCK -eq 1 ]] || return 0
	steamos-readonly enable >/dev/null 2>&1 \
	  && ok "/usr locked read-only again" \
	  || warn "could not re-enable steamos-readonly -- run: sudo steamos-readonly enable"
	STEAMOS_RELOCK=0
}
trap steamos_relock EXIT

if [[ $ATOMIC -eq 1 ]]; then
	say "Atomic / image-based system"
	ok "$ATOMIC_LABEL"
	echo  "         /usr is a read-only image, so nothing is installed into it and"
	echo  "         nothing is layered onto your deployment. No reboot is needed."

	if [[ "$ATOMIC_KIND" == "steamos" && "${WAVELINE_STEAMOS_UNLOCK:-0}" == "1" ]]; then
		if steamos_unlock; then
			ok "steamos-readonly disabled for this run (re-enabled at the end)"
			ATOMIC_USR_RW=1
			# With /usr writable this is an ordinary Arch install for the rest
			# of the run: pacman works, the compiler is the host's, and what it
			# produces links against the exact libraries it will run against.
			HOST_PKGS=1
			BUILDENV="host"; BUILDENV_READY=1
			warn "a SteamOS update replaces the image and will remove the packages"
			warn "  installed below. Waveline itself lives in your home directory and"
			warn "  survives; re-run this script if the mixer stops building."
		else
			warn "could not disable steamos-readonly; falling back to a container build"
		fi
	fi

	if [[ $HOST_PKGS -eq 1 ]]; then
		:   # SteamOS, unlocked above -- handled by the normal package step
	elif [[ "${WAVELINE_NO_CONTAINER:-0}" == "1" ]]; then
		if mixer_deps_ok >/dev/null 2>&1; then
			BUILDENV="host"; BUILDENV_READY=1
			ok "build environment: host toolchain (WAVELINE_NO_CONTAINER=1)"
		else
			die "WAVELINE_NO_CONTAINER=1 but this system has no usable toolchain.
     Either drop that variable, or add the development packages to the image:
     $(atomic_layer_cmd "$(buildenv_packages)")"
		fi
	else
		buildenv_init mixer_deps_ok
		ok "build environment: $(buildenv_describe)"
		if [[ "$BUILDENV" == "none" ]]; then
			warn "no way to compile anything on this system."
			warn "  Install podman (it is present by default on every atomic distro"
			warn "  this script knows about) and re-run. The userspace drop-ins and"
			warn "  udev rules below are installed either way."
		elif [[ "$BUILDENV" != "host" ]]; then
			echo  "         The first run builds a $BUILDENV_ENGINE image from"
			echo  "         $BUILDENV_BASE_IMAGE -- the same base and release as this"
			echo  "         system, so what comes out links against the libraries you"
			echo  "         already have. Later runs reuse it. Remove it any time with:"
			echo  "             $BUILDENV_ENGINE rmi \$($BUILDENV_ENGINE images -q localhost/waveline-build)"
		fi
	fi
fi

# A CMake cache remembers absolute paths to the compiler, to Qt and to every
# other package it found. Those paths belong to whichever environment configured
# it: reusing an Arch host's cache inside a Fedora container (or the other way
# round) fails in ways that read like a broken checkout rather than a stale
# cache. So the environment is stamped into the build tree, and a change to it
# throws the tree away.
#
# A tree with no stamp is treated as a host build, which it is: that is every
# build directory that existed before this file learned to containerise, and
# there is no reason to make those people pay for a full rebuild.
BUILD_STAMP="$ROOT/app/build/.waveline-buildenv"
match_build_dir_to_env() {
	local want cur
	want="$BUILDENV|${BUILDENV_IMAGE:-none}"
	if [[ -d "$ROOT/app/build" ]]; then
		cur="$(cat "$BUILD_STAMP" 2>/dev/null || echo 'host|none')"
		if [[ "$cur" != "$want" ]]; then
			warn "build environment changed ($cur -> $want) -- reconfiguring"
			rm -rf "$ROOT/app/build"
		fi
	fi
	install -d -o "$RUSER" -g "$RUSER" "$ROOT/app/build"
	printf '%s\n' "$want" > "$BUILD_STAMP"
	chown "$RUSER:$RUSER" "$BUILD_STAMP" 2>/dev/null
	return 0
}

build_mixer_app_only() {
	say "Mixer application (app only)"
	if ! build_deps_ok; then
		warn "cannot build waveline-mixer -- install development packages and retry"
		[[ $ATOMIC -eq 1 ]] && warn "    (or install podman and let this script containerise the build)"
		deps_hint
		return 1
	fi
	match_build_dir_to_env
	stage_fluidsynth
	# Whether app/lib/libfluidsynth.so.3 exists is decided at configure time, so
	# a cached CMakeCache.txt from before it appeared (or went away) is wrong.
	if [[ ! -f "$ROOT/app/build/CMakeCache.txt" || $FLUIDSYNTH_CHANGED -eq 1 ]]; then
		if ! buildenv_run "$ROOT" cmake -S "$ROOT/app" -B "$ROOT/app/build" \
		     -DCMAKE_BUILD_TYPE=Release >/tmp/waveline-mixer-build.log 2>&1; then
			warn "cmake configure failed -- see /tmp/waveline-mixer-build.log"
			return 1
		fi
	fi
	if buildenv_run "$ROOT" cmake --build "$ROOT/app/build" --target waveline-mixer \
	     >>/tmp/waveline-mixer-build.log 2>&1; then
		ok "waveline-mixer built"
	else
		warn "build failed -- see /tmp/waveline-mixer-build.log"
		return 1
	fi
	stage_runtime_libs "$ROOT/app/build/waveline-mixer" || return 1
	install -d -o "$RUSER" -g "$RUSER" "$BIND"
	install -m755 -o "$RUSER" -g "$RUSER" "$ROOT/app/build/waveline-mixer" "$BIND/waveline-mixer"
	install_runtime_libs
	ok "~/.local/bin/waveline-mixer"
	echo  "         Restart the mixer window to pick up changes."
	return 0
}

# A binary built in a container still has to run on the host. Same base distro
# and same release means the ABI matches; what can still be missing is a library
# the host image simply does not ship -- librnnoise above all, which is a build
# dependency of the mixer and is on almost no desktop image.
#
# So ask the host's own loader what it cannot resolve, take exactly those files
# out of the container, and point the binaries at them with an RPATH. Nothing is
# copied that the host already has, and on a host build this is a no-op.
STAGED_RUNTIME=0
stage_runtime_libs() {
	[[ "$BUILDENV" == "host" ]] && return 0
	local stage="$ROOT/app/build/waveline-runtime"
	# Started empty every time. A library the host has since gained should stop
	# being carried, and a stale copy shadowing a newer system one through the
	# RPATH is exactly the kind of bug nobody would think to look for.
	rm -rf "$stage"
	install -d -o "$RUSER" -g "$RUSER" "$stage"
	if ! buildenv_bundle_libs "$stage" "$@"; then
		warn "the mixer cannot run on this system as built:"
		warn "  ${BUILDENV_ERR:-unknown}"
		case "${BUILDENV_ERR:-}" in
		*libQt6*)
			warn "  This system has no Qt 6 runtime. Waveline will not bundle one:"
			warn "  Qt loads platform plugins out of its own installation and a"
			warn "  copied library without them fails at startup. Add Qt 6 to the"
			warn "  image once and re-run:"
			case "$ATOMIC_BASE" in
			fedora) warn "      $(atomic_layer_cmd 'qt6-qtbase qt6-qtsvg qt6-qtwebsockets')" ;;
			arch)   warn "      $(atomic_layer_cmd 'qt6-base qt6-svg qt6-websockets')" ;;
			*)      warn "      $(atomic_layer_cmd 'the Qt 6 runtime')" ;;
			esac
			;;
		*newer\ than\ this\ system*)
			warn "  The base image is newer than your host. Pin it to your release:"
			warn "      sudo WAVELINE_BUILD_IMAGE=<image:tag> ./install.sh"
			;;
		esac
		return 1
	fi
	# patchelf lives in the build image, so this costs the host nothing. The
	# second entry keeps the bundled-FluidSynth layout in app/lib working.
	buildenv_set_rpath '$ORIGIN/../lib/waveline/runtime:$ORIGIN/../lib' "$@"
	STAGED_RUNTIME=1
	find "$stage" -maxdepth 1 -name '*.so*' -printf '    ok   bundled %f\n' 2>/dev/null
	return 0
}

install_runtime_libs() {
	[[ $STAGED_RUNTIME -eq 1 ]] || return 0
	local stage="$ROOT/app/build/waveline-runtime" f n=0
	# Mirrors the staging directory rather than merging into it, for the same
	# reason it is rebuilt from scratch above.
	rm -rf "$RUNTIMED"
	for f in "$stage"/*.so*; do
		[[ -f "$f" ]] || continue
		[[ $n -eq 0 ]] && install -d -o "$RUSER" -g "$RUSER" "$RUNTIMED"
		install -m755 -o "$RUSER" -g "$RUSER" "$f" "$RUNTIMED/"
		n=$((n + 1))
	done
	# The common case on a well-stocked image: nothing was missing, so there is
	# no bundle directory and nothing to say about one.
	[[ $n -gt 0 ]] || return 0
	ok "~/.local/lib/waveline/runtime/  ($n librar$( [[ $n -eq 1 ]] && echo y || echo ies) this system does not ship)"
	return 0
}

# ------------------------------------------ FluidSynth (MIDI instrument sounds)
#
# wavelined dlopens libfluidsynth at run time, so this whole step is optional:
# without it the mixer loses MIDI instrument playback and nothing else. But when
# app/lib/libfluidsynth.so.3 exists at configure time, app/CMakeLists.txt also
# links wavelined against it directly (WAVELINE_FLUIDSYNTH_LINKED), and that is
# the arrangement this project is actually tested with.
#
# The library itself is not in this repository and never should be. It is
# LGPL-2.1-or-later, so redistributing a binary carries obligations we have no
# way to meet from a git tree, and it is built against one distribution's glibc,
# so a copy taken from the development machine is useless to most people anyway.
# What used to be a manual "cp /usr/lib/libfluidsynth.so.3 app/lib/" is done
# here instead: install the distribution's own package, then copy its library
# into place.
#
# Copied fresh on every run rather than left alone. wavelined's INSTALL_RPATH
# puts $ORIGIN/../lib ahead of the system paths, so a copy left over from before
# a FluidSynth upgrade would silently shadow the new system library -- and that
# failure looks nothing like a stale-library problem when you hit it.
FLUIDSYNTH_LIB="$ROOT/app/lib/libfluidsynth.so.3"

# The SONAME we want is libfluidsynth.so.3 specifically: it is what CMake looks
# for and what the ABI the engine is written against. A system carrying only
# .so.2 is left to the dlopen fallback, which accepts older sonames.
fluidsynth_system_lib() {
	local p
	p="$(ldconfig -p 2>/dev/null | awk '/libfluidsynth\.so\.3 /{print $NF; exit}')"
	[[ -n "$p" && -f "$p" ]] && { printf '%s\n' "$p"; return 0; }
	for p in /usr/lib64/libfluidsynth.so.3 /usr/lib/libfluidsynth.so.3 \
	         /usr/lib/*-linux-gnu/libfluidsynth.so.3 \
	         /usr/local/lib/libfluidsynth.so.3; do
		[[ -f "$p" ]] && { printf '%s\n' "$p"; return 0; }
	done
	return 1
}

# Runtime package only -- the headers are not needed, because nothing here
# compiles against FluidSynth's API. Failures are warnings: this is the one
# optional feature in the mixer build.
fluidsynth_install_pkg() {
	case "$FAM" in
	arch)   pacman -S --needed --noconfirm fluidsynth >/dev/null 2>&1 ;;
	fedora) dnf install -y fluidsynth-libs >/dev/null 2>&1 ;;
	debian) DEBIAN_FRONTEND=noninteractive apt-get install -y \
	          --no-install-recommends libfluidsynth3 >/dev/null 2>&1 ;;
	*)      return 1 ;;
	esac
}

# 1 when the file appeared or disappeared under app/lib, which changes what
# `cmake` decides. The app-only path reuses an existing CMakeCache.txt, so it
# has to know to reconfigure.
FLUIDSYNTH_CHANGED=0

stage_fluidsynth() {
	local had=0 lib
	[[ -f "$FLUIDSYNTH_LIB" ]] && had=1

	if [[ "${WAVELINE_SKIP_FLUIDSYNTH:-0}" == "1" ]]; then
		warn "FluidSynth staging skipped (WAVELINE_SKIP_FLUIDSYNTH=1)"
		return 0
	fi

	lib="$(fluidsynth_system_lib || true)"

	# Nothing is installed on the host of an image-based system -- that is the
	# entire premise of the atomic support -- so there the library is either in
	# the image already or this step does not happen.
	#
	# Announced before it runs. Every other package this script installs prints
	# the command first, and a silent pacman/apt/dnf transaction in the middle
	# of the mixer build is both surprising and impossible to tell apart from
	# "it was already there" when reading the log afterwards.
	if [[ -z "$lib" && $ATOMIC -eq 0 ]]; then
		case "$FAM" in
		arch)   ok "installing FluidSynth (pacman -S fluidsynth)" ;;
		fedora) ok "installing FluidSynth (dnf install fluidsynth-libs)" ;;
		debian) ok "installing FluidSynth (apt-get install libfluidsynth3)" ;;
		esac
		fluidsynth_install_pkg
		lib="$(fluidsynth_system_lib || true)"
	fi

	if [[ -z "$lib" ]]; then
		# Anything already sitting in app/lib is left alone: it was put there by
		# hand, by someone building without this script, and deleting it would
		# be the one thing they did not ask for.
		[[ $had -eq 1 ]] && {
			warn "no FluidSynth package found; keeping the copy already in app/lib/"
			return 0
		}
		warn "no FluidSynth on this system -- MIDI instruments will be unavailable"
		case "$FAM" in
		arch)   warn "    sudo pacman -S fluidsynth        then re-run this script" ;;
		fedora) warn "    sudo dnf install fluidsynth-libs then re-run this script" ;;
		debian) warn "    sudo apt install libfluidsynth3  then re-run this script" ;;
		esac
		[[ $ATOMIC -eq 1 ]] && warn "    $(atomic_layer_cmd fluidsynth)"
		warn "  everything else in the mixer works without it."
		return 0
	fi

	install -d -o "$RUSER" -g "$RUSER" "$ROOT/app/lib"
	if install -m755 -o "$RUSER" -g "$RUSER" "$lib" "$FLUIDSYNTH_LIB"; then
		[[ $had -eq 0 ]] && FLUIDSYNTH_CHANGED=1
		ok "FluidSynth staged from $lib"
	else
		warn "could not copy $lib into app/lib -- MIDI instruments will be unavailable"
	fi
	return 0
}

if [[ $APP_ONLY -eq 1 ]]; then
	say "Waveline -- app-only update"
	ok "user: $RUSER"
	if build_mixer_app_only; then
		say "Done"
		ok "updated: waveline-mixer"
	else
		exit 1
	fi
	exit 0
fi

# Where cargo is, if anywhere. Checked in the *user's* home as well as on root's
# PATH: rustup installs into ~/.cargo/bin, which sudo's PATH does not include,
# so looking only at `command -v cargo` would report "no Rust" to exactly the
# people most likely to have it. Empty when there is genuinely no toolchain.
# In a container it is whatever the image put there, so the search is skipped.
if [[ "$BUILDENV" == "host" ]]; then
	CARGO="$(command -v cargo 2>/dev/null || true)"
	[[ -n "$CARGO" ]] || { [[ -x "$HOME_N/.cargo/bin/cargo" ]] && CARGO="$HOME_N/.cargo/bin/cargo"; }
	# runuser resets PATH to root's, so cargo's own directory has to go back on
	# it for rustup's shims to find rustc.
	[[ -n "$CARGO" ]] \
	  && BUILDENV_HOST_PATH="$(dirname "$CARGO"):$HOME_N/.local/bin:/usr/local/bin:/usr/bin:/bin"
else
	CARGO="cargo"
fi

# ----------------------------------------------------- stand down the daemon
# First thing after preflight, because everything below this line is hostile to
# a daemon that has the microphone open.
#
# wavelined holds the mic: a capture stream, and on a device with a vendor
# protocol a usbfs control endpoint it polls once a second. Rebuilding and
# reinstalling snd-usb-audio underneath that is what wedges a Wave:3 into the
# -110 lockup -- one control transfer lands mid-swap and times out, so does
# every one after it, and the daemon ends up in uninterruptible sleep with the
# device dead until its USB port is power-cycled. The mic is not the only
# casualty: WirePlumber can come back unable to configure the graph at all, and
# then there is no audio on the machine, in or out.
#
# The audio-stack restart further down does stop wavelined, via
# Requires=pipewire.service propagation -- but that is far too late. DKMS has
# already built and installed the module by then. This has to happen here.
#
# Not a behaviour change: the daemon was going down during the install either
# way, and the end of this script starts it again.
say "Stopping the mixer daemon"
if runu systemctl --user is-active wavelined.service >/dev/null 2>&1; then
	runu systemctl --user stop wavelined.service >/dev/null 2>&1
	# Wait for the process to be gone, not merely for systemd to report the
	# unit inactive. A daemon blocked in a USB ioctl takes a moment to unwind,
	# and the whole point of this block is that nothing holds the device when
	# the module is swapped.
	for _ in $(seq 1 20); do
		pgrep -u "$RUSER" -x wavelined >/dev/null 2>&1 || break
		sleep 0.5
	done
	if pgrep -u "$RUSER" -x wavelined >/dev/null 2>&1; then
		warn "wavelined will not stop -- it may already be stuck on the microphone"
		warn "  the module reload below can wedge the device; unplug it if so"
	else
		ok "stopped (started again at the end of this script)"
	fi
else
	ok "not running"
fi

kbuild() {
	[[ -d "/usr/lib/modules/$KREL/build" ]] && { echo "/usr/lib/modules/$KREL/build"; return; }
	[[ -d "/lib/modules/$KREL/build" ]]     && { echo "/lib/modules/$KREL/build"; return; }
}

# Is the snd-usb-audio already in the kernel a different build from the one
# modprobe would load now?
#
# Worth asking because the reload is not free: swapping this module is the one
# operation known to wedge a Wave:3 into the -110 lockup, and a wedged card
# needs its USB port power-cycled -- see the stand-down block above. Re-running
# the installer with nothing new to load used to pay that risk for no reason,
# every time, because the swap was gated only on the build having succeeded.
#
# srcversion is a hash of the module's own sources, so it distinguishes two
# builds of snd-usb-audio without caring which tree they came from. Only
# meaningful on the modprobe path: with waveline-kmod the loaded module is
# deliberately not on modprobe's search path, so modinfo would answer about the
# stock one and this would compare the wrong pair. Callers there use the tool.
#
# Unknown on either side means no basis to skip, so say yes and behave as before.
module_swap_needed() {
	local loaded want
	loaded="$(cat /sys/module/snd_usb_audio/srcversion 2>/dev/null || true)"
	want="$(modinfo -F srcversion snd-usb-audio 2>/dev/null || true)"
	[[ -n "$loaded" && -n "$want" ]] || return 0
	[[ "$loaded" != "$want" ]]
}

# The reload itself, with an honest report. `modprobe -r` fails whenever
# anything still holds any USB audio card -- not just the microphone; any other
# device this one module drives is enough -- and the old `-r ... && modprobe`
# one-liner then skipped the load silently while the line below it announced
# success anyway. Nothing is broken by a refused unload: the module already
# installed on disk is picked up at the next boot, and the verification section
# at the end of this script says so.
reload_snd_usb_audio() {
	if ! module_swap_needed; then
		ok "module unchanged -- not reloading (the swap is what wedges a Wave:3)"
		return 0
	fi
	if modprobe -r snd_usb_audio 2>/dev/null && modprobe snd-usb-audio 2>/dev/null; then
		ok "module reloaded"
		return 0
	fi
	warn "could not reload snd-usb-audio -- something still holds a USB audio card"
	warn "  it is installed; reboot to start using it"
	return 1
}

# ------------------------------------------------------------ distro + pkgs
say "System"
ok "distro: ${OS_PRETTY:-${OS_ID:-unknown}}   (family: $FAM)"
ok "kernel: $KREL"
[[ $ATOMIC -eq 1 ]] && ok "image-based: yes ($ATOMIC_KIND)"

HAD_HEADERS=0; [[ -n "$(kbuild)" ]] && HAD_HEADERS=1

say "Installing build dependencies"
# On an atomic system the host package manager is either absent, or writes to a
# read-only image, or writes somewhere the next system update will overwrite.
# The toolchain comes from the build environment instead -- which on this path
# has already been chosen above -- and the only thing to do here is materialise
# it. Nothing is added to the host.
if [[ $HOST_PKGS -eq 0 ]]; then
	if [[ "$BUILDENV" == "none" ]]; then
		warn "no build environment: skipping. Everything that needs no compiler"
		warn "  is still installed below."
	elif [[ "$BUILDENV" == "host" ]]; then
		ok "host toolchain already present -- nothing to install"
		warn "note: this is an image-based system. Anything you added to /usr by"
		warn "  hand may not survive the next system update; re-run this script if"
		warn "  the mixer stops building afterwards."
	else
		ok "not installing anything on the host -- using $(buildenv_describe)"
		echo  "         Building the image now if this is the first run; that step"
		echo  "         downloads the base image and its development packages once."
		if buildenv_ready 0; then
			ok "build environment ready: $BUILDENV_IMAGE"
		else
			warn "${BUILDENV_ERR:-could not prepare the build environment}"
			BUILDENV="none"
		fi
	fi
else
case "$FAM" in
arch)
	PKGBASE="$(cat "/usr/lib/modules/$KREL/pkgbase" 2>/dev/null || echo linux)"
	PKGS=(dkms "${PKGBASE}-headers" base-devel curl git python tar xz usbutils)
	# `rust` and `rustup` conflict, so only ask for one when neither is
	# already providing cargo. Someone who set up rustup themselves keeps it.
	[[ -n "$CARGO" ]] || PKGS+=(rust)
	ok "pacman -S ${PKGS[*]}"
	# Deliberately NO -y: "pacman -Sy <pkg>" is a partial upgrade and can break
	# an Arch system. If the local DB is too old to find a package, tell the
	# user to run a full -Syu themselves rather than doing it for them.
	if ! pacman -S --needed --noconfirm "${PKGS[@]}"; then
		warn "pacman could not install everything."
		warn "  If a package was 'not found', your package DB is stale --"
		warn "  run:  sudo pacman -Syu     then re-run this script."
	fi
	;;
debian)
	export DEBIAN_FRONTEND=noninteractive
	PKGS=(dkms "linux-headers-$KREL" build-essential curl git python3 xz-utils usbutils)
	[[ -n "$CARGO" ]] || PKGS+=(cargo)
	ok "apt-get install ${PKGS[*]}"
	apt-get update -qq || warn "apt-get update failed"
	apt-get install -y --no-install-recommends "${PKGS[@]}" || {
		warn "no exact headers package for $KREL; trying the generic metapackage"
		apt-get install -y --no-install-recommends dkms build-essential curl \
		  python3 xz-utils usbutils linux-headers-generic || warn "apt install failed"
	}
	;;
fedora)
	PKGS=(dkms "kernel-devel-$KREL" gcc make curl git python3 xz tar usbutils elfutils-libelf-devel)
	[[ -n "$CARGO" ]] || PKGS+=(cargo)
	ok "dnf install ${PKGS[*]}"
	dnf install -y "${PKGS[@]}" || {
		warn "no exact kernel-devel for $KREL; trying the unversioned package"
		dnf install -y dkms kernel-devel gcc make curl python3 xz tar usbutils \
		  elfutils-libelf-devel || warn "dnf install failed"
	}
	;;
*)
	warn "unrecognised distro -- skipping package installation"
	warn "  needed: dkms, kernel headers for $KREL, gcc, make, curl, git, python3,"
	warn "          tar, xz, and cargo/rustc >= 1.70 for DeepFilterNet"
	;;
esac
fi

# Headers that do not match the running kernel mean the kernel was updated:
# usable only after a reboot. Not so on an atomic system, where nothing was just
# installed and the container can fetch headers of its own -- there, a missing
# host kbuild directory is a fact about the image, not a stale install.
if [[ -z "$(kbuild)" ]]; then
	OTHER="$(ls -1d /usr/lib/modules/*/build /lib/modules/*/build 2>/dev/null | head -1)"
	if [[ $ATOMIC -eq 1 ]]; then
		ok "no kernel build directory in the image; the build environment supplies one"
	elif [[ -n "$OTHER" ]]; then
		warn "headers installed, but not for the running kernel ($KREL)"
		warn "  found: $OTHER"
		[[ $HAD_HEADERS -eq 0 ]] && NEED_REBOOT=1
	else
		warn "no kernel headers available for $KREL"
	fi
else
	ok "kernel headers present for $KREL"
fi

# -------------------------------------------------------- device detection
# Everything device-specific below this point is driven by what lands here.
say "Microphone"

DETECTED="${WAVELINE_PROFILES:-}"
if [[ -n "$DETECTED" ]]; then
	ok "profiles forced by WAVELINE_PROFILES: $DETECTED"
	for id in $DETECTED; do
		profile_resolve "$id" >/dev/null \
		  || die "no such device profile: $id"
	done
else
	DETECTED="$(profile_detect | tr '\n' ' ')"
fi

# The profile the *software* is branded for. With two recognised microphones
# plugged in, the first wins and the others still get their fixes installed --
# but only one of them can name the window, and guessing is better than
# refusing to install.
PRIMARY=""
for id in $DETECTED; do
	[[ -n "$PRIMARY" ]] || PRIMARY="$id"
	profile_load "$(profile_resolve "$id")"
	ok "detected: $PROFILE_LABEL  (profile: $id)"
	if CARD="$(profile_alsa_card)"; then ok "  ALSA card: $CARD"; fi
done

if [[ -z "$PRIMARY" ]]; then
	PRIMARY="generic"
	DETECTED="generic"
	warn "no microphone with a device profile is plugged in."
	warn "  Installing Waveline for ordinary microphones: the mixer, the noise"
	warn "  suppression and the routing, and none of anyone else's workarounds."
	warn "  Plug a supported microphone in and re-run to get its fixes too."
elif [[ "$(echo "$DETECTED" | wc -w)" -gt 1 ]]; then
	warn "more than one recognised microphone plugged in."
	warn "  Every matched device's fixes are installed; the app stays generic Waveline."
fi

profile_load "$(profile_resolve generic)"
ok "Waveline will install as: Waveline (any microphone)"

# Verification still targets the first detected device's ALSA node when present.
PRIMARY_CARD=""
PRIMARY_ALSA=""
for id in $DETECTED; do
	if dir="$(profile_resolve "$id")" && [[ -n "$dir" ]]; then
		profile_load "$dir"
		if [[ -n "${ALSA_NODE_MATCH:-}" ]]; then
			PRIMARY_ALSA="$ALSA_NODE_MATCH"
			PRIMARY_CARD="$(profile_alsa_card || true)"
			break
		fi
	fi
done

# True when ANY matched profile wants a thing, which is what decides whether a
# whole step runs at all.
any_profile_has() {
	local key="$1" id
	for id in $DETECTED; do
		( profile_load "$(profile_resolve "$id")"; [[ -n "${!key}" && "${!key}" != "0" ]] ) \
		  && return 0
	done
	return 1
}

profile_in_detected() {
	local want="$1" id
	for id in $DETECTED; do
		[[ "$id" == "$want" ]] && return 0
	done
	return 1
}

# The Pro Audio quantum fix used to be an install-time choice here: a global
# PipeWire drop-in, shipped inside the Wave:3 profile, that pinned the graph
# quantum to 4096 and cost ~85 ms of latency on every device on the machine.
# Getting it back off again meant re-running the installer.
#
# It is now a setting in the mixer -- the Latency control in the header, "Pro
# Audio (85 ms)". It reaches the same quantum through clock.force-quantum,
# which PipeWire applies to the running graph immediately, so it can be turned
# on for a session and off afterwards without touching a file or reinstalling
# anything. WAVELINE_PRO_AUDIO is kept working as a pointer rather than
# silently ignored, because someone following an old note deserves to be told
# where the thing went.
if [[ -n "${WAVELINE_PRO_AUDIO:-}" ]]; then
	case "${WAVELINE_PRO_AUDIO,,}" in
		1|yes|true|y)
			say "Pro Audio"
			ok  "no longer an install option -- it is a setting in the mixer now"
			ok  "  open Waveline and set Latency to \"Pro Audio (85 ms)\""
			ok  "  it applies instantly and can be switched back the same way"
			;;
		0|no|false|n) ;;
		*) die "WAVELINE_PRO_AUDIO must be 0 or 1 (got: ${WAVELINE_PRO_AUDIO})" ;;
	esac
fi

# ------------------------------------------------------------- 1. kernel
#
# Two implementations of the same idea. On a mutable distro the module goes
# through DKMS, which is what everyone expects and what other tooling knows how
# to reason about. On an atomic one DKMS has nowhere to write -- /usr/src and
# /usr/lib/modules are inside the read-only image -- so the module is staged in
# /var and swapped in by a boot unit instead. See scripts/waveline-kmod.
#
# Both produce the same file from the same patched sources; only its address
# and the mechanism that loads it differ.
KMOD_TOOL=/var/lib/waveline/bin/waveline-kmod

install_kmod_tool() {
	install -d -m755 /var/lib/waveline/bin
	install -m755 "$ROOT/scripts/waveline-kmod" "$KMOD_TOOL" || return 1
	mkdir -p /usr/local/bin 2>/dev/null
	# /usr/local is a symlink into /var on ostree systems and writable there;
	# on SteamOS it is inside the read-only image. A symlink when we can, and
	# the absolute path in the unit file either way.
	if ln -sf "$KMOD_TOOL" /usr/local/bin/waveline-kmod 2>/dev/null; then
		ok "/usr/local/bin/waveline-kmod  ->  $KMOD_TOOL"
	else
		ok "$KMOD_TOOL  (/usr/local/bin is read-only, so no symlink)"
	fi
	return 0
}

kernel_patch_atomic() {
	if [[ "$BUILDENV" == "none" ]]; then
		warn "skipped: nothing on this system can compile a kernel module."
		warn "  Install podman and re-run. The microphone still works -- the"
		warn "  stock in-tree driver is used, which is the correct fallback."
		return 1
	fi
	if ! buildenv_ready 1; then
		warn "skipped: ${BUILDENV_ERR:-could not prepare a kernel build environment}"
		return 1
	fi
	# The staging tree may be left over from a run of this script on a mutable
	# system, where it was written as root. The container writes it as the user
	# and cannot clear a root-owned directory, so hand it over first.
	for d in "$ROOT/src" "$ROOT/.build"; do
		[[ -e "$d" ]] && chown -R "$RUSER:$RUSER" "$d" 2>/dev/null
	done
	# Staged inside the build environment rather than on the host: the tree is
	# extracted from an upstream tarball and needs curl, tar and python3, and
	# the container is the one place all three are guaranteed to be present.
	if ! buildenv_run "$ROOT" env WAVELINE_PROFILES="$DETECTED" \
	     bash "$ROOT/scripts/prepare-src.sh" >/tmp/waveline-prepare.log 2>&1; then
		warn "could not stage/patch sources -- see /tmp/waveline-prepare.log"
		return 1
	fi
	ok "kernel sources staged and patched"
	# The kbuild directory is resolved inside the environment: it is either the
	# host's, bind-mounted read-only, or one the image pulled in itself.
	if ! buildenv_run "$ROOT" sh -c '
		kb="/usr/lib/modules/'"$KREL"'/build"
		[ -d "$kb" ] || kb="/lib/modules/'"$KREL"'/build"
		[ -d "$kb" ] || kb="$(ls -d /usr/src/kernels/*/ 2>/dev/null | head -1)"
		[ -n "$kb" ] && [ -d "$kb" ] || { echo "no kernel build directory" >&2; exit 1; }
		echo "using $kb"
		exec make -C "$kb" M='"$ROOT"'/src modules' >/tmp/waveline-build.log 2>&1; then
		warn "build failed -- see /tmp/waveline-build.log"
		warn "  either the patch anchors do not match kernel $KREL, or the build"
		warn "  environment has no headers for it. On an image-based system the"
		warn "  usual cause is the second: the distribution's archive no longer"
		warn "  carries the -devel package matching your running kernel."
		return 1
	fi
	ok "module built"
	install_kmod_tool || { warn "could not install waveline-kmod"; return 1; }
	# waveline-kmod owns everything from here: vermagic check, Secure Boot
	# signing and the boot unit. --no-load because PipeWire still has the sound
	# card open at this point and rmmod would fail on a busy module; the swap
	# happens in the audio-stack restart further down, with the stack stopped.
	if WAVELINE_REPO="$ROOT" WAVELINE_PROFILES="$DETECTED" \
	   bash "$KMOD_TOOL" install --no-load "$ROOT/src/snd-usb-audio.ko"; then
		ok "installed without DKMS, in /var/lib/waveline/modules/$KREL"
		echo  "         A kernel update does not carry this module with it. After one:"
		echo  "             sudo waveline-kmod rebuild"
		echo  "         Until then the stock driver is used and nothing breaks."
		return 0
	fi
	warn "waveline-kmod could not install the module"
	return 1
}

say "1/9  Kernel patch (snd-usb-audio)"
KB="$(kbuild)"
if ! any_profile_has KERNEL_PATCH; then
	ok "not needed: no detected microphone requires a kernel change"
elif [[ $ATOMIC -eq 1 ]]; then
	kernel_patch_atomic || FAILED_KERNEL=1
elif [[ $NEED_REBOOT -eq 1 ]]; then
	warn "skipped: reboot required first (see the end of this run)"
	FAILED_KERNEL=1
elif ! command -v dkms >/dev/null || [[ -z "$KB" ]]; then
	warn "skipped: needs dkms and kernel headers for $KREL"
	FAILED_KERNEL=1
else
	# Invoked through bash on purpose: if the execute bit is ever lost (a zip
	# download, a copied tree, a permissive umask) executing it directly fails
	# with EACCES and the kernel patch is silently skipped -- the one part of
	# this installer whose absence is hardest to notice.
	#
	# WAVELINE_PROFILES is passed explicitly rather than letting prepare-src.sh
	# detect again: it must patch for exactly the set decided above, including
	# a set that was forced on the command line.
	if WAVELINE_PROFILES="$DETECTED" bash "$ROOT/scripts/prepare-src.sh" \
	     >/tmp/waveline-prepare.log 2>&1; then
		ok "kernel sources staged and patched"
		if make -C "$KB" M="$ROOT/src" modules >/tmp/waveline-build.log 2>&1; then
			ok "module built"
			dkms remove -m "$DKMS_NAME" -v "$DKMS_VER" --all >/dev/null 2>&1
			# The pre-rename package, from an install that predates Waveline.
			# Left behind it would keep shipping its own snd-usb-audio and the
			# two would race for the same module path.
			dkms remove -m snd-usb-audio-wave3 -v "$DKMS_VER" --all >/dev/null 2>&1 \
			  && ok "removed the old snd-usb-audio-wave3 DKMS package"
			rm -rf "/usr/src/snd-usb-audio-wave3-$DKMS_VER"
			# A very early install copied sources into /usr/src/Arch Linux-1.0/.
			# The space breaks Arch's DKMS pacman hook: its unquoted
			# `for nv in $(all_nv_from_kver)` splits "Arch Linux/1.0" into
			# Arch and Linux/1.0, so every kernel reinstall tries
			# `dkms install Linux/1.0` and fails.
			dkms remove -m "Arch Linux" -v "$DKMS_VER" --all >/dev/null 2>&1
			rm -rf "/usr/src/Arch Linux-$DKMS_VER"
			DEST="/usr/src/$DKMS_NAME-$DKMS_VER"; rm -rf "$DEST"; mkdir -p "$DEST"
			cp -a "$ROOT/src"/. "$DEST"/; cp -a "$ROOT/dkms/dkms.conf" "$DEST"/
			if dkms add -m "$DKMS_NAME" -v "$DKMS_VER" >/dev/null 2>&1 &&
			   dkms build -m "$DKMS_NAME" -v "$DKMS_VER" -k "$KREL" >/dev/null 2>&1 &&
			   dkms install -m "$DKMS_NAME" -v "$DKMS_VER" -k "$KREL" --force >/dev/null 2>&1; then
				ok "installed via dkms"
			else
				warn "dkms install failed"; FAILED_KERNEL=1
			fi
		else
			warn "build failed -- see /tmp/waveline-build.log"
			warn "  the patch anchors may not match kernel $KREL"
			FAILED_KERNEL=1
		fi
	else
		warn "could not stage/patch sources -- see /tmp/waveline-prepare.log"
		FAILED_KERNEL=1
	fi
fi

# --kernel-only stops here. Everything below is userspace and unaffected by a
# kernel change, so a post-kernel-update rebuild should not rebuild the mixer,
# refetch DeepFilterNet or rewrite anyone's drop-ins. The daemon was stopped at
# the top of this script and has to come back up.
if [[ $KERNEL_ONLY -eq 1 ]]; then
	# The module was staged but not loaded -- PipeWire had the card open. Take
	# the stack down, swap, bring it back, exactly as the full run does.
	if [[ $FAILED_KERNEL -eq 0 ]] && any_profile_has KERNEL_PATCH; then
		say "Loading the patched module"
		runu systemctl --user stop wireplumber pipewire-pulse.service \
		     pipewire-pulse.socket pipewire.service pipewire.socket >/dev/null 2>&1
		sleep 2
		if [[ $ATOMIC -eq 1 && -x "$KMOD_TOOL" ]]; then
			bash "$KMOD_TOOL" load || FAILED_KERNEL=1
		else
			reload_snd_usb_audio
		fi
		runu systemctl --user start pipewire.socket pipewire-pulse.socket >/dev/null 2>&1
		runu systemctl --user start wireplumber >/dev/null 2>&1
		sleep 5
		ok "audio stack restarted"
	fi
	if [[ -x "$BIND/wavelined" ]]; then
		say "Starting the mixer daemon"
		runu systemctl --user restart wavelined.service >/dev/null 2>&1 \
		  && ok "wavelined restarted" || warn "could not restart wavelined"
	fi
	say "Done"
	if [[ $FAILED_KERNEL -eq 1 ]]; then
		warn "the kernel patch was NOT installed; the stock driver is in use."
		exit 1
	fi
	ok "kernel patch installed for $KREL"
	exit 0
fi

# ---------------------------------------------------------- 2+3. userspace
WP="$HOME_N/.config/wireplumber/wireplumber.conf.d"
PW="$HOME_N/.config/pipewire/pipewire.conf.d"

say "2/9  WirePlumber rules"
# Always installed, on every machine, whatever is plugged in: this is Waveline's
# own graph policy, not a workaround for a particular device.
#
# It lowers priority.driver on every capture node so that outputs outrank
# inputs and a webcam can never become the clock for the whole graph. Left to
# WirePlumber's stock numbers, capture outranks playback -- which is how a
# 32 kHz C922 ended up clocking a guitar adapter, a studio microphone and an
# HDMI capture card, and how unplugging that webcam re-clocked everything.
install -d -o "$RUSER" -g "$RUSER" "$WP"
install -m644 -o "$RUSER" -g "$RUSER" \
	"$ROOT/data/wireplumber/50-waveline-driver-policy.conf" "$WP/"
ok "~/.config/wireplumber/wireplumber.conf.d/50-waveline-driver-policy.conf  (all machines)"

if any_profile_has WIREPLUMBER_CONF; then
	for id in $DETECTED; do
		profile_load "$(profile_resolve "$id")"
		f="$(profile_file "$WIREPLUMBER_CONF")" || continue
		install -m644 -o "$RUSER" -g "$RUSER" "$f" "$WP/"
		ok "~/.config/wireplumber/wireplumber.conf.d/$(basename "$f")  ($PROFILE_LABEL)"
	done
else
	ok "no detected microphone needs a WirePlumber rule of its own"
fi

say "3/9  PipeWire drop-ins"
# Also always installed, and the single owner of graph-wide clock policy: the
# sample rate is pinned so no device can drag the graph onto its own, and the
# quantum gets a deterministic default instead of a negotiated one.
#
# Nothing under devices/ may write clock policy any more. It used to: the
# Wave:3 profile shipped 51-waveline-wave3-quantum.conf, a global setting
# living inside one microphone's directory where it applied to every device on
# the machine and could disagree with anything else that touched the same keys.
# The quantum is now a runtime setting in the mixer (the Latency control),
# which is why that file is reaped below rather than replaced.
install -d -o "$RUSER" -g "$RUSER" "$PW"
install -m644 -o "$RUSER" -g "$RUSER" \
	"$ROOT/data/pipewire/50-waveline-clock.conf" "$PW/"
ok "~/.config/pipewire/pipewire.conf.d/50-waveline-clock.conf  (all machines)"
ok "  graph pinned to 48 kHz; default cycle 512 frames (10.7 ms)"
ok "  change it any time from the Latency control in the mixer"

# Superseded and pre-Waveline file names. Left in place, each would apply rules
# a second time under a name this script no longer manages, and uninstalling
# would never remove them.
#
#   51-wave3.conf / 51-wave3-quantum.conf   pre-rename names
#   51-waveline-wave3-quantum.conf          the old global Pro Audio quantum,
#                                           now the mixer's Latency setting
#   51-waveline-rocksmith.conf              renamed to -rocksmith-tone-cable;
#                                           both matched the same node, so
#                                           nothing misbehaved and nothing
#                                           reaped the old one either
for stale in "$WP/51-wave3.conf" "$PW/51-wave3-quantum.conf" \
             "$PW/51-waveline-wave3-quantum.conf" \
             "$WP/51-waveline-rocksmith.conf"; do
	[[ -f "$stale" ]] && rm -f "$stale" \
	  && ok "removed superseded ${stale/#$HOME_N/\~}"
done

# --------------------------------------------- 4. real-time scheduling rights
# The one part of this script that decides whether a small graph cycle is usable
# on a busy machine, and the only one that writes outside $HOME and /etc/udev.
#
# Every filter node in the mixer is its own PipeWire context with its own data
# loop, so a full graph carries around a hundred threads with a hard deadline.
# At the default 512-frame cycle that deadline is 10.7 ms and an ordinary
# SCHED_OTHER thread makes it even under load. At the Ultra Low Latency setting
# it is 2.66 ms and it does not: `stress -c 32` on a 32-core machine was enough
# to put audible clicks into a mix that was clean without it.
#
# PipeWire's module-rt has two routes to real-time scheduling and picks by
# RLIMIT_RTPRIO:
#
#   above zero  pthread_setschedparam() directly. No ceiling on thread count.
#   zero        ask rtkit-daemon over D-Bus. This is the default everywhere.
#
# rtkit does not scale to a graph this size, and its shipped defaults are the
# whole story -- 25 real-time threads per user, 25 requests per 20 s, priority
# capped at 20. Measured on the development machine before this step existed:
# wavelined had 324 threads of which 19 were real-time, the journal had several
# hundred "Reached maximum concurrent threads limit for user, denying request",
# and the threads that lost were whichever filters happened to be built last --
# which is why the same machine glitched differently after every rewire.
#
# So: grant the rlimit. This is exactly what the realtime-privileges package
# does on Arch and what @audio in limits.conf does elsewhere, and a machine that
# already has either one loses nothing by having this too.
#
# Two files, because there are two ways in and they do not share a mechanism:
#
#   limits.d      pam_limits, for login shells and anything started from one
#   user@.service the systemd user manager, which is what actually spawns
#                 wavelined and pipewire, and which pam_limits does not reach
#                 on every distribution
#
# The kernel keeps the safety net either way: sched_rt_runtime_us reserves 5% of
# every second for non-real-time work, so a runaway thread cannot wedge a core.
say "4/9  Real-time scheduling privileges"

RT_PRIO_MAX=95
UID_N="$(id -u "$RUSER" 2>/dev/null || echo "")"

if [[ -z "$UID_N" ]]; then
	warn "cannot resolve a uid for $RUSER -- skipping real-time privileges"
else
	RT_LIMITS="/etc/security/limits.d/99-waveline-realtime.conf"
	RT_DROPIN_D="/etc/systemd/system/user@${UID_N}.service.d"

	# limits.d is pam_limits' directory and it is not always there: a machine
	# whose PAM stack does not include pam_limits (or that has no /etc/security
	# at all) simply has no such path, and the bare redirection below then fails
	# with a shell-level "No such file or directory" before cat ever runs.
	#
	# Created only inside an existing /etc/security. Inventing that whole tree on
	# a system that does not use PAM would mean writing a file nothing will ever
	# read, in a directory that is not ours to create -- and the drop-in below is
	# the one that actually matters, because the systemd user manager is what
	# spawns wavelined.
	RT_LIMITS_OK=1
	if [[ ! -d /etc/security/limits.d ]]; then
		if [[ -d /etc/security ]]; then
			install -d -m755 /etc/security/limits.d 2>/dev/null || RT_LIMITS_OK=0
		else
			RT_LIMITS_OK=0
		fi
	fi

	if [[ $RT_LIMITS_OK -eq 1 ]] && cat > "$RT_LIMITS" <<-EOF 2>/dev/null
		# Installed by Waveline. Removed by uninstall.sh.
		#
		# Lets $RUSER run audio threads at real-time priority, so PipeWire can
		# schedule them itself instead of asking rtkit -- which stops at 25
		# threads per user and leaves most of a mixer graph running at ordinary
		# priority, audible as clicks whenever the machine is busy.
		#
		# rtprio is a ceiling, not a setting: nothing becomes real-time because
		# of this line, and PipeWire asks for well under it. The kernel's
		# sched_rt_runtime_us still reserves 5% of each second for everything
		# else, and a machine that already has realtime-privileges or @audio
		# configured is unchanged by this file.
		$RUSER   -   rtprio      $RT_PRIO_MAX
		$RUSER   -   memlock     unlimited
		$RUSER   -   nice        -19
	EOF
	then
		chmod 644 "$RT_LIMITS"
		ok "$RT_LIMITS"
	elif [[ $RT_LIMITS_OK -eq 0 ]]; then
		# Not a warning. This system has no pam_limits directory, so there is
		# nothing to write and nothing missing: the systemd drop-in below covers
		# wavelined, which is the process that needs the grant.
		ok "no pam_limits directory on this system -- the systemd drop-in covers it"
	else
		warn "could not write $RT_LIMITS"
	fi

	if install -d "$RT_DROPIN_D" 2>/dev/null && cat > "$RT_DROPIN_D/10-waveline-realtime.conf" <<-EOF 2>/dev/null
		# Installed by Waveline. Removed by uninstall.sh.
		#
		# The systemd user manager is what spawns wavelined and pipewire, and a
		# user unit cannot raise itself past the limits of the manager that
		# started it. pam_limits does not reach this manager on every
		# distribution, so the same grant is made here as well.
		[Service]
		LimitRTPRIO=$RT_PRIO_MAX
		LimitMEMLOCK=infinity
		LimitNICE=-19
	EOF
	then
		chmod 644 "$RT_DROPIN_D/10-waveline-realtime.conf"
		systemctl daemon-reload >/dev/null 2>&1
		ok "$RT_DROPIN_D/10-waveline-realtime.conf"
	else
		warn "could not write the user@${UID_N}.service drop-in"
	fi

	# Apply it to the session that is already running, so this does not need a
	# logout to take effect. Children inherit the manager's limits at fork, so
	# raising them on the live manager is enough for the pipewire and wavelined
	# restarts at the end of this script -- they are spawned after this point.
	#
	# Nothing already running is changed, which is why this is not a substitute
	# for the two files above: the next login has to get the same answer.
	UMPID="$(systemctl show -p MainPID --value "user@${UID_N}.service" 2>/dev/null || echo 0)"
	if [[ -n "$UMPID" && "$UMPID" != "0" ]] && command -v prlimit >/dev/null 2>&1 \
	   && prlimit --pid "$UMPID" --rtprio="$RT_PRIO_MAX:$RT_PRIO_MAX" \
	        --memlock=unlimited:unlimited >/dev/null 2>&1; then
		ok "applied to the running session -- no logout needed"
	else
		warn "could not raise the limits of the running session"
		warn "    log out and back in for real-time scheduling to take effect"
	fi

	ok "  the mixer's Audio diagnostics window reports whether it took"
fi

# ------------------------------------------------ 5. JACK compatibility (DAWs)
# A DAW reaches the audio stack through libjack, and which libjack is installed
# decides whether that works at all:
#
#   pipewire-jack   the shim. A JACK client connects to the PipeWire graph that
#                   is already running, sees Waveline's channels as ports, and
#                   there is one audio server on the machine.
#   jack2           the real thing. Ardour starts an actual jackd, which opens
#                   an ALSA device *exclusively* -- a device PipeWire already
#                   owns. Two servers then fight over the same hardware and
#                   everything on the machine crackles, not just the DAW.
#
# The second is installed on a lot of machines by accident: ffmpeg, obs-studio
# and audacity all pull jack2 in as a dependency and nothing anywhere warns that
# it has displaced the shim. The symptom is "my audio breaks when I open
# Ardour", which reads like a DAW bug and is not one -- it is two audio servers.
#
# Both provide libjack.so, so this is decided by which package owns the file,
# not by what is installed alongside what.
#
# WAVELINE_KEEP_JACK2=1 skips the whole step, for a machine deliberately running
# a real jackd.
say "5/9  JACK compatibility (DAW support)"

# Package that owns the libjack the dynamic linker actually finds, or empty.
jack_provider() {
	local lib
	for lib in /usr/lib/libjack.so.0 /usr/lib64/libjack.so.0 /usr/lib/*/libjack.so.0; do
		[[ -e "$lib" ]] || continue
		case "$FAM" in
		arch)   pacman -Qoq "$lib" 2>/dev/null ;;
		fedora) rpm -qf --qf '%{NAME}\n' "$lib" 2>/dev/null ;;
		debian) dpkg -S "$lib" 2>/dev/null | cut -d: -f1 ;;
		esac
		return
	done
}

if [[ -n "${WAVELINE_KEEP_JACK2:-}" ]]; then
	ok "WAVELINE_KEEP_JACK2 set -- leaving the JACK implementation alone"
elif [[ $HOST_PKGS -eq 0 ]]; then
	# Nothing may be added to the host image, and a shim installed into the
	# build container would not be the one a DAW on the host loads.
	warn "image-based system: not changing host packages."
	warn "  If you use a DAW, check that libjack comes from PipeWire and not"
	warn "  from jack2, or the two will fight over the sound card:"
	case "$FAM" in
	fedora) warn "    rpm-ostree install pipewire-jack-audio-connection-kit" ;;
	*)      warn "    install your distribution's pipewire-jack package" ;;
	esac
else
	JACK_OWNER="$(jack_provider)"
	case "$JACK_OWNER" in
	*pipewire*)
		ok "libjack already comes from PipeWire ($JACK_OWNER) -- nothing to do"
		;;
	*)
		if [[ -n "$JACK_OWNER" ]]; then
			ok "libjack currently comes from $JACK_OWNER -- replacing it with the"
			ok "  PipeWire shim so a DAW joins the existing graph"
		else
			ok "no libjack installed -- adding the PipeWire shim so DAWs work"
		fi
		JACK_DONE=0
		case "$FAM" in
		arch)
			# pipewire-jack Provides jack and libjack.so, so everything that
			# depends on jack2 (ardour, audacity, obs-studio, ffmpeg, fluidsynth,
			# portaudio ...) stays satisfied. pacman still will not resolve the
			# conflict unattended, hence the explicit removal -- and -Rdd, because
			# those same dependants would otherwise block it.
			if pacman -S --needed --noconfirm pipewire-jack >/dev/null 2>&1; then
				JACK_DONE=1
			elif pacman -Rdd --noconfirm jack2 >/dev/null 2>&1 &&
			     pacman -S --needed --noconfirm pipewire-jack; then
				JACK_DONE=1
			fi
			;;
		fedora)
			# dnf resolves the obsoletes/conflicts against jack-audio-connection-kit
			# itself, so this needs no removal step.
			dnf install -y pipewire-jack-audio-connection-kit >/dev/null 2>&1 && JACK_DONE=1
			;;
		debian)
			export DEBIAN_FRONTEND=noninteractive
			apt-get install -y --no-install-recommends pipewire-jack >/dev/null 2>&1 && JACK_DONE=1
			;;
		*)
			warn "unrecognised distro -- install your pipewire-jack package by hand"
			;;
		esac

		# Verify against the file, not against the exit status: on a partial
		# upgrade the install can fail *after* the removal, and leaving the
		# machine with no libjack at all is worse than never having touched it.
		JACK_NOW="$(jack_provider)"
		if [[ $JACK_DONE -eq 1 && "$JACK_NOW" == *pipewire* ]]; then
			ok "libjack now provided by $JACK_NOW"
			ok "  log out and back in before starting a DAW"
		elif [[ -z "$JACK_NOW" ]]; then
			warn "libjack is now MISSING -- the install did not complete."
			warn "  Fix this before opening a DAW or anything linking libjack:"
			case "$FAM" in
			arch)   warn "    sudo pacman -Syu && sudo pacman -S pipewire-jack" ;;
			fedora) warn "    sudo dnf install pipewire-jack-audio-connection-kit" ;;
			debian) warn "    sudo apt-get install pipewire-jack" ;;
			esac
		else
			warn "could not replace $JACK_NOW with the PipeWire JACK shim."
			warn "  DAWs will start their own jackd and fight PipeWire for the"
			warn "  sound card. Install the shim by hand when convenient."
		fi
		;;
	esac
fi

# In the mixer: Settings -> Latency -> Graph latency must be "Automatic" for a
# DAW, or clock.force-quantum overrides the buffer size the DAW asks for. The
# Latency tab says so; repeated here because this is the other half of the same
# problem and the two are always hit together.
ok "  DAW checklist: also set Settings -> Latency -> Graph latency to Automatic"

# ------------------------------------------------------------- 6. udev rules
# A profile ships one for either of two unrelated reasons, and neither implies
# the other:
#
#   - a vendor control interface whose usbfs node is root-only for writing, so
#     waveline-hw cannot talk to it (Wave:3). That is HARDWARE_CONTROLS=1.
#   - a plain ALSA control that powers on at a value making the microphone
#     hard to use, and has to be set on every plug because nothing else in the
#     stack reaches it (the Meet 4K's capture preamp). No vendor protocol is
#     involved at all, and HARDWARE_CONTROLS is 0.
#
# So this step is gated on UDEV_RULES itself. It used to sit inside the
# HARDWARE_CONTROLS branch below, where the second kind could never install.
say "6/9  udev rules"

UDEV_APPLY=()
if any_profile_has UDEV_RULES; then
	UDEV_OK=0
	for id in $DETECTED; do
		profile_load "$(profile_resolve "$id")"
		f="$(profile_file "$UDEV_RULES")" || continue
		if install -Dm644 "$f" "/etc/udev/rules.d/$(basename "$f")" 2>/dev/null; then
			ok "/etc/udev/rules.d/$(basename "$f")  ($PROFILE_LABEL)"
			UDEV_OK=1
			UDEV_APPLY+=("$id")
		else
			warn "could not install $(basename "$f") for $PROFILE_LABEL"
			[[ "$HARDWARE_CONTROLS" == "1" ]] \
			  && warn "  waveline-hw will need root"
		fi
	done
	if [[ $UDEV_OK -eq 1 ]]; then
		udevadm control --reload-rules >/dev/null 2>&1
		ok "udev rules reloaded"
		# Apply to hardware that is plugged in now, so the rules take effect
		# without waiting for a replug.
		for id in "${UDEV_APPLY[@]}"; do
			profile_load "$(profile_resolve "$id")"
			for usbid in $USB_IDS; do
				udevadm trigger --subsystem-match=usb \
				  --attr-match=idVendor="${usbid%%:*}" \
				  --attr-match=idProduct="${usbid##*:}" >/dev/null 2>&1
			done
			# A rule that sets a mixer default matches the card's *control*
			# device, which the usb trigger above never reaches -- different
			# subsystem. Trigger that one by card index instead, and only for
			# this device, so no other card's saved ALSA state is disturbed.
			if card="$(profile_alsa_card)"; then
				if [[ -e "/sys/class/sound/controlC${card#card}" ]]; then
					udevadm trigger --action=add \
					  "/sys/class/sound/controlC${card#card}" >/dev/null 2>&1
				fi
			fi
		done
	fi
else
	ok "not needed: no detected microphone ships a udev rule"
fi

# ---------------------------------------------------------- 6. control tools
# Controls ALSA does not expose live in a vendor config block reached over an
# unclaimed USB interface; see docs/protocol.md. Only microphones whose profile
# says HARDWARE_CONTROLS=1 have one, and on the rest this whole step is skipped
# rather than installing a tool with nothing to talk to.
say "7/9  Hardware control tool (waveline-hw)"

LIBD="$HOME_N/.local/lib/waveline"
SYSD="$HOME_N/.config/systemd/user"
install -d -o "$RUSER" -g "$RUSER" "$LIBD" "$BIND" "$SYSD"

if any_profile_has HARDWARE_CONTROLS; then
	# Two kinds of file: the entry point, and one transport module per device
	# that has a vendor protocol. They go in ~/.local/lib/waveline with a
	# symlink on PATH, so the modules do not clutter ~/.local/bin.
	install -m755 -o "$RUSER" -g "$RUSER" "$ROOT/scripts/waveline-hw" "$LIBD/waveline-hw"
	for backend in "$ROOT"/scripts/waveline_*.py; do
		[[ -f "$backend" ]] || continue
		install -m644 -o "$RUSER" -g "$RUSER" "$backend" "$LIBD/"
	done
	ln -sf "../lib/waveline/waveline-hw" "$BIND/waveline-hw"
	chown -h "$RUSER:$RUSER" "$BIND/waveline-hw"
	ok "~/.local/bin/waveline-hw  ->  ~/.local/lib/waveline/waveline-hw"

	# Optional sync service. Installed but NOT enabled: it is the only part of
	# this repository that runs continuously, and that should be the user's
	# choice.
	if any_profile_has SYNC_SERVICE; then
		install -m644 -o "$RUSER" -g "$RUSER" \
		  "$ROOT/systemd/waveline-sync.service" "$SYSD/"
		runu systemctl --user daemon-reload >/dev/null 2>&1
		ok "~/.config/systemd/user/waveline-sync.service  (installed, not enabled)"
		echo  "         Some microphones do not report their own dial and mute pad"
		echo  "         to the kernel, so Linux shows stale values when you change"
		echo  "         things ON the mic. This service polls for that:"
		echo  "             systemctl --user enable --now waveline-sync"
	fi
else
	ok "not needed: no detected microphone has controls ALSA cannot reach"
	echo  "         Gain, mute and headphone volume are ordinary ALSA controls on"
	echo  "         an ordinary microphone -- your volume UI already reaches them."
fi

# ------------------------------------------------------- the device profile
# Written for wavelined and waveline-mixer to read: it is how they know what to
# call themselves and whether to open a USB device at all. Deliberately the
# same format as devices/<brand>/<category>/<id>/device.conf, so it can be read or fixed by hand.
say "Device profile"
CONFD="$HOME_N/.config/waveline"
install -d -o "$RUSER" -g "$RUSER" "$CONFD"
# The other two directories wavelined.service lists in ReadWritePaths. The unit
# tolerates them being absent, but a path it skipped is a path the daemon cannot
# write under ProtectHome=read-only -- and then the ALSA aliases and the
# WirePlumber headroom drop-in silently never get written. ~/.config/alsa is
# otherwise created only by scripts/install-alsa-aliases.sh, which runs inside a
# conditional branch, so on a machine that skips it nothing creates it at all.
install -d -o "$RUSER" -g "$RUSER" \
	"$HOME_N/.config/alsa" "$HOME_N/.config/wireplumber"
# Written for wavelined and waveline-mixer to read. App branding is always the
# generic Waveline mixer; detected device profiles only decide which kernel
# patches, udev rules and waveline-hw backends get installed.
profile_load "$(profile_resolve generic)"
cat > "$CONFD/profile.conf" <<EOF
# Written by install.sh on $(date -u '+%Y-%m-%d %H:%M:%S UTC'). Read by
# wavelined and waveline-mixer at startup. Re-run the installer to regenerate
# it; editing it by hand works too and takes effect at the next daemon restart.
PROFILE_ID="generic"
PROFILE_LABEL="$PROFILE_LABEL"
BRAND="$BRAND"
HARDWARE_CONTROLS="0"
ALSA_NODE_MATCH=""
# Every profile matched at install time. Device-specific fixes were installed
# for these; the running app stays generic unless an input device is assigned one.
DETECTED_PROFILES="$(echo "$DETECTED" | tr -s ' ' | sed 's/ *$//')"
EOF
chown "$RUSER:$RUSER" "$CONFD/profile.conf"
chmod 644 "$CONFD/profile.conf"
ok "~/.config/waveline/profile.conf  (generic app; detected: $(echo "$DETECTED" | tr -s ' '))"

# A binary that is not on PATH is a binary the user cannot find. Probe with the
# user's *login shell*, not bash: someone who sets PATH in .zshrc would other-
# wise get a false warning. rc files may print escape sequences before our
# printf, so match the directory as a substring rather than parsing the output.
SHELL_N="$(getent passwd "$RUSER" | cut -d: -f7)"
[[ -x "$SHELL_N" ]] || SHELL_N=/bin/sh
USER_PATH="$(runuser -u "$RUSER" -- "$SHELL_N" -lc 'printf ":%s:" "$PATH"' 2>/dev/null)"
case "$USER_PATH" in
	*":$HOME_N/.local/bin:"*) ok "~/.local/bin is on your PATH" ;;
	*)
		warn "~/.local/bin is not on your PATH ($(basename "$SHELL_N")), so"
		warn "  'waveline-mixer' will not be found by name. Add this to your"
		warn "  shell rc file, then open a new terminal:"
		warn '      export PATH="$HOME/.local/bin:$PATH"'
		;;
esac

# --------------------------------------------------------------- 7. mixer
# The reason this repository exists now that the fixes are per-device: a mixer
# that works with any microphone. Skipped rather than failing the install when
# its build dependencies are missing.
say "8/9  Mixer application"

MIXER_DEPS_OK=1
if ! build_deps_ok; then
	MIXER_DEPS_OK=0
	warn "skipping the mixer -- it is the main thing here, so this is worth fixing."
	if [[ $ATOMIC -eq 1 && "$BUILDENV" == "none" ]]; then
		warn "  This is an image-based system with no container engine, so there"
		warn "  is nothing here that can compile. Install podman and re-run:"
		warn "      $(atomic_layer_cmd podman)"
	else
		warn "  Install the development packages and re-run:"
		deps_hint
	fi
fi

if [[ $MIXER_DEPS_OK -eq 1 ]]; then
	match_build_dir_to_env
	stage_fluidsynth
	# Built as the user, not as root: the build tree lands in the repo and
	# root-owned object files would be a nuisance to clean up afterwards.
	if buildenv_run "$ROOT" cmake -S "$ROOT/app" -B "$ROOT/app/build" \
	     -DCMAKE_BUILD_TYPE=Release >/tmp/waveline-mixer-build.log 2>&1 &&
	   buildenv_run "$ROOT" cmake --build "$ROOT/app/build" \
	     >>/tmp/waveline-mixer-build.log 2>&1 &&
	   stage_runtime_libs "$ROOT/app/build/wavelined" "$ROOT/app/build/waveline-mixer"; then
		ok "mixer built"
		for b in wavelined waveline-mixer wavelined-cli; do
			install -m755 -o "$RUSER" -g "$RUSER" "$ROOT/app/build/$b" "$BIND/$b"
		done
		install_runtime_libs
		ok "~/.local/bin/wavelined, ~/.local/bin/waveline-mixer, ~/.local/bin/wavelined-cli"

		APPDIR="$HOME_N/.local/share/applications"
		ICONDIR="$HOME_N/.local/share/icons/hicolor/scalable/apps"
		install -d -o "$RUSER" -g "$RUSER" "$APPDIR" "$ICONDIR"
		sed "s|@EXEC@|$BIND/waveline-mixer|g" "$ROOT/data/waveline-mixer.desktop" \
		  > "$APPDIR/waveline-mixer.desktop"
		chown "$RUSER:$RUSER" "$APPDIR/waveline-mixer.desktop"
		chmod 644 "$APPDIR/waveline-mixer.desktop"
		install -m644 -o "$RUSER" -g "$RUSER" \
		  "$ROOT/data/icons/hicolor/scalable/apps/waveline-mixer.svg" "$ICONDIR/"
		ok "~/.local/share/applications/waveline-mixer.desktop"
		ok "~/.local/share/icons/hicolor/scalable/apps/waveline-mixer.svg"
		runu update-desktop-database "$APPDIR" >/dev/null 2>&1 || true
		runu gtk-update-icon-cache -f -t "$HOME_N/.local/share/icons/hicolor" \
		  >/dev/null 2>&1 || true

		install -m644 -o "$RUSER" -g "$RUSER" "$ROOT/systemd/wavelined.service" "$SYSD/"
		runu systemctl --user daemon-reload >/dev/null 2>&1
		ok "~/.config/systemd/user/wavelined.service"

		# File-descriptor headroom for the PipeWire *server*.
		#
		# The server owns a memfd and eventfds for every node, port, link and
		# buffer in the graph. A full Waveline graph is ~170 nodes, and each
		# filter instance is its own client connection, so a stock 1024 soft
		# limit is reached on a busy setup -- measured 1011/1024 with four input
		# devices, two Monitor outputs and a MIDI instrument.
		#
		# Past the limit pw_core_create_object fails with "Too many open files"
		# and the graph degrades in ways that do not look like an fd problem at
		# all: loopbacks instantiate but never get a driver and sit suspended,
		# links are created but stay in "init" and never activate, and the
		# Monitor mix goes silent while its meter still moves. Rebuilding only
		# burns more descriptors.
		#
		# Only the server needs this. wireplumber and pipewire-pulse hold
		# proxies and sit near 50 and 30 descriptors respectively.
		PWDROP="$SYSD/pipewire.service.d"
		install -d -o "$RUSER" -g "$RUSER" "$PWDROP"
		cat > "$PWDROP/waveline-limits.conf" <<-'EOF'
			# Installed by Waveline. See install.sh for why this is needed.
			[Service]
			LimitNOFILE=65536
		EOF
		chown "$RUSER:$RUSER" "$PWDROP/waveline-limits.conf"
		chmod 644 "$PWDROP/waveline-limits.conf"
		runu systemctl --user daemon-reload >/dev/null 2>&1
		ok "~/.config/systemd/user/pipewire.service.d/waveline-limits.conf (fd headroom)"

		# ALSA PCM aliases → Waveline PipeWire nodes (Audacity, Wine, …).
		# Namehints only; pavucontrol / Helvum unchanged.
		if [[ -x "$ROOT/scripts/install-alsa-aliases.sh" ]]; then
			runu bash "$ROOT/scripts/install-alsa-aliases.sh" install "$HOME_N" \
			  >/dev/null
			ok "~/.config/alsa/waveline.conf (ALSA device aliases)"
		fi
		# Enabled and started at the very end of this script rather than here:
		# the audio stack is restarted below, and the microphone verification
		# looks for the *hardware* source -- which the daemon's own virtual
		# sources would match, testing the wrong device.
	else
		MIXER_DEPS_OK=0
		if [[ -x "$ROOT/app/build/waveline-mixer" && $STAGED_RUNTIME -eq 0 \
		      && "$BUILDENV" != "host" ]]; then
			# It compiled; it just cannot run here. stage_runtime_libs has
			# already said why and what to do about it.
			warn "the mixer was built but not installed"
		else
			warn "mixer build failed -- see /tmp/waveline-mixer-build.log"
		fi
	fi
fi

# -------------------------------------------------------- 8. DeepFilterNet
# The better noise suppression engine, and the mixer's default. Built from
# source because no distribution packages libDF's C API -- upstream ships a
# Rust crate and a LADSPA plugin, neither of which exposes df_create() to us.
#
# Everything here is optional. The mixer already works with RNNoise; if this
# step is skipped the engine simply reports itself unavailable and RNNoise is
# used instead. So every failure below is a warn, never a die.
say "9/9  DeepFilterNet noise suppression (optional)"

DFN_LIB="$HOME_N/.local/lib/libdf.so"
DFN_MODELD="$HOME_N/.local/share/waveline/models"
DFN_MODEL="$DFN_MODELD/DeepFilterNet3_onnx.tar.gz"

# Reported at the end, so the summary can tell the truth about which engine is
# actually going to run.
DFN_OK=0

# Carried over from the pre-Waveline layout, so an upgrade does not spend five
# minutes rebuilding a 23 MB library the machine already has.
if [[ ! -s "$DFN_MODEL" && -s "$HOME_N/.local/share/wave3/models/DeepFilterNet3_onnx.tar.gz" ]]; then
	install -d -o "$RUSER" -g "$RUSER" "$DFN_MODELD"
	install -m644 -o "$RUSER" -g "$RUSER" \
	  "$HOME_N/.local/share/wave3/models/DeepFilterNet3_onnx.tar.gz" "$DFN_MODEL" \
	  && ok "migrated the DeepFilterNet model from ~/.local/share/wave3/"
fi

# Re-resolved: the package step above may have just installed it. Not on the
# container path, where cargo is the image's and was never going to be here.
[[ -n "$CARGO" || "$BUILDENV" != "host" ]] \
  || CARGO="$(command -v cargo 2>/dev/null || true)"

# git and cargo are needed wherever the compiling happens, which is not always
# this machine.
build_has() {
	if [[ "$BUILDENV" == "host" ]]; then command -v "$1" >/dev/null 2>&1
	else buildenv_run "$ROOT" sh -c "command -v $1 >/dev/null 2>&1"; fi
}

dfn_cargo_ok() {
	# libDF needs edition 2021 and rust-version 1.70. Debian stable and older
	# Ubuntus ship a cargo too old to build it, and the failure that produces is
	# a wall of trait errors that looks like our bug rather than a stale
	# toolchain, so check the version rather than letting it explode.
	local v
	v="$(buildenv_run "$ROOT" "$CARGO" --version 2>/dev/null | awk '{print $2}')"
	[[ -n "$v" ]] || return 1
	local maj min
	maj="${v%%.*}"; min="${v#*.}"; min="${min%%.*}"
	(( maj > 1 || (maj == 1 && min >= 70) ))
}

if [[ "${WAVELINE_SKIP_DFN:-0}" == "1" ]]; then
	warn "skipped: WAVELINE_SKIP_DFN=1 -- the mixer will use RNNoise"
elif [[ -s "$DFN_LIB" && -s "$DFN_MODEL" ]]; then
	ok "already installed: ~/.local/lib/libdf.so"
	ok "already installed: ~/.local/share/waveline/models/"
	DFN_OK=1
elif [[ $MIXER_DEPS_OK -eq 0 ]]; then
	warn "skipped: the mixer was not built, so there is nothing to use it"
elif ! build_has git; then
	warn "skipped: git is needed to fetch the DeepFilterNet sources"
elif [[ -z "$CARGO" ]]; then
	warn "skipped: cargo (Rust) not found"
	case "$FAM" in
	arch)   warn "    sudo pacman -S rust        then re-run this script" ;;
	fedora) warn "    sudo dnf install cargo     then re-run this script" ;;
	debian) warn "    sudo apt install cargo     then re-run this script" ;;
	*)      warn "    install Rust >= 1.70, then re-run this script" ;;
	esac
	warn "  the mixer works meanwhile; it just uses RNNoise."
elif ! dfn_cargo_ok; then
	warn "skipped: cargo is older than 1.70, which libDF requires"
	warn "  got: $(buildenv_run "$ROOT" "$CARGO" --version 2>/dev/null)"
	warn "  install a current toolchain with rustup, then re-run:"
	warn "      curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
	warn "  the mixer works meanwhile; it just uses RNNoise."
else
	# Built as the user: cargo wants a writable ~/.cargo, and a root-owned
	# registry cache in the user's home would break their later cargo use.
	#
	# Under a container the scratch directory has to be somewhere the container
	# can see, so it goes in the mounted cache rather than /tmp. The cargo
	# registry lives there too and is deliberately kept between runs: a second
	# install should not re-download the crate graph.
	if [[ "$BUILDENV" == "host" ]]; then
		DFN_SRC="$(runuser -u "$RUSER" -- mktemp -d -t waveline-dfn-XXXXXX)"
	else
		DFN_SRC="$BUILDENV_CACHE/dfn-build"
		rm -rf "$DFN_SRC"
		install -d -o "$RUSER" -g "$RUSER" "$DFN_SRC" || DFN_SRC=""
	fi
	# Guarded because every path below is built by concatenation and this runs
	# as root: an empty DFN_SRC would turn the clone target into /DeepFilterNet
	# and the cleanup into `rm -rf /...`.
	if [[ -z "$DFN_SRC" || ! -d "$DFN_SRC" ]]; then
		warn "skipped: could not create a temporary build directory"
		DFN_SRC=""
	else
	echo  "         Fetching and compiling DeepFilterNet. This takes a few"
	echo  "         minutes and about 1 GB of scratch space in $DFN_SRC,"
	echo  "         which is removed afterwards. Log: /tmp/waveline-dfn-build.log"

	if buildenv_run "$DFN_SRC" git clone --depth 1 \
	     https://github.com/Rikorose/DeepFilterNet "$DFN_SRC/DeepFilterNet" \
	     >/tmp/waveline-dfn-build.log 2>&1; then
		ok "sources fetched"
		# --features capi is what exports df_create/df_process_frame; a default
		# build produces a libdf.so with none of the symbols we dlopen.
		if buildenv_run "$DFN_SRC/DeepFilterNet" \
		     "$CARGO" build --release -p deep_filter --features capi \
		     >>/tmp/waveline-dfn-build.log 2>&1; then
			ok "libdf.so built"
			install -d -o "$RUSER" -g "$RUSER" "$HOME_N/.local/lib" "$DFN_MODELD"
			if install -m755 -o "$RUSER" -g "$RUSER" \
			     "$DFN_SRC/DeepFilterNet/target/release/libdf.so" "$DFN_LIB" &&
			   install -m644 -o "$RUSER" -g "$RUSER" \
			     "$DFN_SRC/DeepFilterNet/models/DeepFilterNet3_onnx.tar.gz" \
			     "$DFN_MODEL"; then
				ok "~/.local/lib/libdf.so"
				ok "~/.local/share/waveline/models/DeepFilterNet3_onnx.tar.gz"
				DFN_OK=1
			else
				warn "could not install libdf.so or the model"
			fi
		else
			warn "DeepFilterNet build failed -- see /tmp/waveline-dfn-build.log"
			warn "  the mixer works regardless; it falls back to RNNoise."
		fi
	else
		warn "could not fetch DeepFilterNet -- see /tmp/waveline-dfn-build.log"
		warn "  (no network?) the mixer works regardless, using RNNoise."
	fi
	fi
	# ~1 GB of Rust build tree. Removed whether or not the build worked; the
	# log is what is worth keeping.
	[[ -n "$DFN_SRC" ]] && rm -rf "$DFN_SRC"
fi

if [[ $DFN_OK -eq 1 ]]; then
	echo  "         DeepFilterNet is the default engine. It is markedly better on"
	echo  "         steady noise (fans, keyboards, traffic) but costs 3-4x the CPU"
	echo  "         of RNNoise per enabled filter. Switch engines any time in the"
	echo  "         mixer under Application Settings -> NC engine."
fi

# ------------------------------------------------------------ apply + check
# NOTE: wavelined is already down -- it was stopped at the top of this script,
# before DKMS ran. Stopping pipewire.service would take it down anyway
# (wavelined.service has Requires=pipewire.service and systemd propagates a stop
# across Requires), but relying on that is what let the module be swapped under
# a live daemon. Either way nothing here brings it back, which is why the mixer
# is started explicitly at the end of this script. Re-running the installer used
# to leave a previously working daemon dead, looking for all the world like the
# build had broken.
say "Restarting audio stack"
runu systemctl --user stop wireplumber pipewire-pulse.service pipewire-pulse.socket \
     pipewire.service pipewire.socket >/dev/null 2>&1
sleep 2
if [[ $FAILED_KERNEL -eq 0 ]]; then
	if [[ $ATOMIC -eq 1 && -x "$KMOD_TOOL" ]]; then
		# A plain `modprobe snd-usb-audio` here would load the *stock* module
		# from the image and quietly undo the swap: nothing outside /usr is on
		# modprobe's search path. waveline-kmod does the same reload and puts
		# the patched module back.
		bash "$KMOD_TOOL" load >/dev/null 2>&1
	else
		reload_snd_usb_audio
	fi
fi
runu systemctl --user start pipewire.socket pipewire-pulse.socket >/dev/null 2>&1
runu systemctl --user start wireplumber >/dev/null 2>&1
sleep 6
ok "restarted"

say "Verifying"
if any_profile_has KERNEL_PATCH; then
	if [[ $ATOMIC -eq 1 ]]; then
		# modinfo answers about the module on modprobe's search path, which our
		# out-of-tree copy deliberately is not on. Ask waveline-kmod what is
		# actually in the kernel instead.
		if [[ -x "$KMOD_TOOL" ]] && bash "$KMOD_TOOL" status 2>/dev/null \
		   | grep -q 'patched (Waveline)'; then
			ok "patched module loaded (from /var/lib/waveline/modules/$KREL)"
		elif [[ $FAILED_KERNEL -eq 1 ]]; then
			warn "stock module (kernel patch not installed)"
		else
			warn "stock module still loaded -- see: sudo waveline-kmod status"
		fi
	else
		case "$(modinfo -F filename snd-usb-audio 2>/dev/null)" in
			*updates/dkms*|*extra*) ok "patched module loaded" ;;
			*) if [[ $FAILED_KERNEL -eq 1 ]]; then warn "stock module (kernel patch not installed)"
			   else warn "stock module still loaded -- reboot to pick up the patch"; fi ;;
		esac
	fi
fi
if [[ -n "$PRIMARY_CARD" ]]; then
	CST="$(grep -h '^state' "/proc/asound/$PRIMARY_CARD/pcm0c/sub0/status" 2>/dev/null || echo closed)"
	[[ "$CST" == *RUNNING* ]] && ok "capture stream held open -- this keeps the mic alive" \
	                          || warn "capture not held open yet ($CST)"
fi

# The hardware microphone, not one of ours. Matched on the profile's ALSA node
# name when there is one; otherwise on the default source, because a microphone
# we have no profile for is exactly the one whose name we cannot predict. Our
# own virtual sources are excluded either way -- recording from those would test
# the mixer's plumbing rather than the microphone.
if [[ -n "$PRIMARY_ALSA" ]]; then
	SRC_ID="$(runu pactl list sources short 2>/dev/null \
	          | grep -F "$PRIMARY_ALSA" | grep -v monitor | awk '{print $1}' | head -1)"
else
	SRC_ID="$(runu pactl list sources short 2>/dev/null \
	          | grep -v 'waveline' | grep -v monitor | grep 'alsa_input' \
	          | awk '{print $1}' | head -1)"
fi
if [[ -n "${SRC_ID:-}" ]] && command -v pw-record >/dev/null 2>&1; then
	T="$(mktemp -u /tmp/waveline-verify.XXXX.wav)"
	# timeout must run INSIDE runuser: runu is a shell function, timeout execs binaries.
	runu timeout 8 pw-record --target "$SRC_ID" "$T" >/dev/null 2>&1
	PEAK="$(python3 - "$T" 2>/dev/null <<'EOF'
import sys,struct,os
p=sys.argv[1]
if not os.path.exists(p) or os.path.getsize(p)<=44: print(-1); raise SystemExit
d=open(p,'rb').read(); i=12; fmt=data=None
while i+8<=len(d):
    c=d[i:i+4]; sz=struct.unpack('<I',d[i+4:i+8])[0]
    if c==b'fmt ': fmt=d[i+8:i+8+sz]
    elif c==b'data': data=d[i+8:i+8+sz]; break
    i+=8+sz+(sz&1)
if not data or not fmt: print(-1); raise SystemExit
bits=struct.unpack('<HHIIHH',fmt[:16])[5]; bl=bits//8
print(max((abs(struct.unpack('<h',data[o:o+bl])[0])<<8 if bits==16
           else abs(int.from_bytes(data[o:o+bl],'little',signed=True)))
          for o in range(0,len(data)-bl+1,bl)))
EOF
)"; rm -f "$T"
	if [[ "${PEAK:-0}" -gt 2000 ]]; then ok "microphone produces audio (peak $PEAK)"
	else
		warn "microphone captured silence (peak ${PEAK:-?})"
		warn "  if this device was wedged by an earlier lockup, UNPLUG it for 30s"
		warn "  and re-run -- a reboot does NOT clear it, VBUS stays powered."
	fi
else
	warn "could not test the microphone (no pw-record, or no hardware source)"
fi

# ------------------------------------------------------------ start the mixer
# Deliberately last: the audio stack restart above has to have settled, and the
# microphone check has to run against the *hardware* source rather than the
# virtual ones the daemon publishes.
#
# `restart` rather than `start` so that re-running this script actually picks up
# the binary just built -- an already-running daemon would otherwise keep
# serving the old one, which looks exactly like the build not having worked.
# Keyed on what is actually on disk rather than on whether this run built it:
# re-running the script after a skipped or failed build should still leave a
# previously installed daemon running, which is what the user asked for.
if [[ -x "$BIND/wavelined" && -f "$SYSD/wavelined.service" ]]; then
	say "Starting the mixer daemon"
	# The pre-rename unit, still enabled and still pointing at a binary this
	# script no longer installs. Two daemons cannot both own org.waveline.Mixer,
	# and the loser restarts forever.
	if runu systemctl --user list-unit-files wave3d.service >/dev/null 2>&1; then
		runu systemctl --user disable --now wave3d.service >/dev/null 2>&1 \
		  && ok "stopped and disabled the old wave3d service"
	fi
	if [[ "${WAVELINE_NO_AUTOSTART:-0}" == "1" ]]; then
		warn "skipped: WAVELINE_NO_AUTOSTART=1"
		echo  "         Start it yourself with:  systemctl --user enable --now wavelined"
	else
		runu systemctl --user enable wavelined.service >/dev/null 2>&1 \
		  && ok "enabled at login" || warn "could not enable wavelined"
		runu systemctl --user restart wavelined.service >/dev/null 2>&1
		# Waits for the *bus name*, not just for systemd to have forked
		# something. "active" appears immediately for Type=simple and would
		# still be reported for a daemon that dies a second later; the GUI's
		# "wavelined is not running" is a D-Bus check, so check the same thing.
		# The graph takes a few seconds to build, hence the generous window.
		MIXER_UP=0
		for _ in $(seq 20); do
			if runu busctl --user list --no-pager 2>/dev/null \
			   | grep -q '^org\.waveline\.Mixer[[:space:]]'; then
				MIXER_UP=1; break
			fi
			sleep 1
		done
		if [[ $MIXER_UP -eq 1 ]]; then
			ok "wavelined is running"
			echo  "         It creates the virtual sinks and routes audio, so it stays"
			echo  "         on from now on. To stop that:"
			echo  "             systemctl --user disable --now wavelined"
		else
			warn "wavelined did not stay running"
			warn "  see:  journalctl --user -u wavelined -n 40"
		fi
	fi
fi

say "Done"
if [[ $NEED_REBOOT -eq 1 ]]; then
	printf '\n\033[1;33m    REBOOT REQUIRED\033[0m\n'
	echo  "    Kernel headers were just installed but do not match the running"
	echo  "    kernel ($KREL). Reboot, then re-run:  sudo ./install.sh"
	echo  "    Everything else is already active."
elif [[ $FAILED_KERNEL -eq 1 ]]; then
	warn "kernel patch not installed; everything else IS active."
fi

echo
ok "installed: Waveline (any microphone)"
if [[ -x "$BIND/waveline-mixer" ]]; then
	echo  "        waveline-mixer            the mixer"
	echo  "        Applications menu         Waveline Mixer"
fi
if [[ -x "$BIND/wavelined-cli" ]]; then
	echo  "        wavelined-cli --help      command-line control (Stream Deck / keybinds)"
fi
if [[ -x "$LIBD/waveline-hw" ]]; then
	if runuser -u "$RUSER" -- "$LIBD/waveline-hw" --version >/dev/null 2>&1; then
		ok "waveline-hw works -- try:  waveline-hw --status"
		echo  "        waveline-hw --clipguard on   hardware anti-clip"
		echo  "        waveline-hw --monitor 50     hear yourself in the mic's jack"
	else
		warn "waveline-hw did not run. Check python3 is installed, and see"
		warn "  docs/protocol.md."
	fi
fi

# What is different about this machine, said once, at the point someone is
# actually reading. All of it is in ATOMIC-SUPPORT.md at more length.
if [[ $ATOMIC -eq 1 ]]; then
	echo
	ok "atomic system notes"
	echo  "        Nothing was layered onto your image and no reboot is needed."
	if [[ "$BUILDENV" != "host" ]]; then
		echo  "        The build image ($BUILDENV_IMAGE) is kept for next time."
		echo  "        Reclaim its space with:  $BUILDENV_ENGINE rmi $BUILDENV_IMAGE"
	fi
	if [[ -x "$KMOD_TOOL" ]]; then
		echo  "        The patched snd-usb-audio lives in /var, not /usr, and is"
		echo  "        tied to kernel $KREL. After a kernel update, run:"
		echo  "            sudo waveline-kmod rebuild"
		echo  "        Check it any time with:  sudo waveline-kmod status"
	fi
	if [[ -d "$RUNTIMED" ]]; then
		echo  "        Libraries your image does not ship were copied to"
		echo  "        ~/.local/lib/waveline/runtime and are found by RPATH."
	fi
	[[ "$ATOMIC_KIND" == "steamos" ]] && \
	echo  "        A SteamOS update replaces /usr but leaves your home directory"
	[[ "$ATOMIC_KIND" == "steamos" ]] && \
	echo  "        and /var alone, so Waveline survives one."
fi

# Scoped to the microphone's own card. `pactl list cards` reports every card on
# the machine and the first "Active Profile:" line belongs to whichever one it
# happened to print first -- which is how you end up warning someone about their
# HDMI output. The card token is the ALSA node name minus its `alsa_input.`
# prefix, which is exactly what pactl names the card after.
if [[ -n "$PRIMARY_ALSA" ]]; then
	CARD_TOKEN="${PRIMARY_ALSA#alsa_input.}"
	PROF="$(runu pactl list cards 2>/dev/null \
	        | awk -v t="$CARD_TOKEN" '
	              index($0, t) { inCard = 1 }
	              inCard && /Active Profile:/ { print $3; exit }')"
	if [[ "$PROF" == "pro-audio" ]]; then
		echo
		warn "your microphone is in the pro-audio profile. That profile does not"
		warn "  expose hardware volume/mute to PipeWire, so the mute ring will not"
		warn "  follow desktop volume."
		warn "  playback may also stutter in this profile. If it does, set Latency"
		warn "  to \"Pro Audio (85 ms)\" in the mixer -- it applies instantly and"
		warn "  can be set back afterwards."
		warn "  For everyday use, switch to output:analog-stereo+input:mono-fallback"
		warn "  instead -- see docs/profiles.md."
	fi
fi

echo "    Revert with: sudo ./uninstall.sh"

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
# Waveline -- atomic / image-based distribution support.
#
# Sourced, never executed:
#
#   source scripts/lib/atomic.sh
#   atomic_detect          # fills ATOMIC, ATOMIC_KIND, ATOMIC_LABEL, ...
#   buildenv_init          # decides how to get a compiler
#   buildenv_run <dir> cmd # run a build command in that environment
#
# Why this file exists
# --------------------
# On Silverblue, Kinoite, Bazzite, Bluefin, SteamOS, MicroOS and friends /usr is
# a read-only image. Three things in a normal Waveline install assume otherwise:
#
#   1. installing build dependencies with the system package manager,
#   2. DKMS, which writes to /usr/src and /usr/lib/modules,
#   3. nothing else. Everything else Waveline installs already lands in $HOME
#      or /etc, both of which are writable on every one of these systems.
#
# So atomic support is not a port. It is (1) a compiler that does not come from
# the host image, and (2) a way to carry one patched kernel module without DKMS.
# The mixer, the routing, the noise suppression, the drop-ins and the services
# are byte-identical to what a mutable distro gets.
#
# (1) is a rootless podman container built from the *same base distribution and
#     release as the host*, so what it produces links against the same libraries
#     the host already ships. Nothing is layered, nothing needs a reboot, and
#     `podman rmi` undoes all of it.
#
# (2) is scripts/waveline-kmod: the module lives in /var/lib/waveline/modules,
#     keyed by kernel release, and a small system unit swaps it in for the stock
#     snd-usb-audio at boot. See ATOMIC-SUPPORT.md.
#
# Callers are expected to have already set: RUSER, HOME_N, UD, ROOT (repo root),
# KREL. install.sh and uninstall.sh both do.

# --------------------------------------------------------------- detection

ATOMIC=0             # 1 when /usr cannot be written to durably
ATOMIC_KIND=""       # ostree | steamos | microos | readonly
ATOMIC_LABEL=""      # human-readable, for the install log
ATOMIC_BASE=""       # fedora | arch | suse | debian | unknown -- picks a base image
ATOMIC_USR_RW=1      # 1 when /usr happens to be writable right now

# True on systems where writing to /usr either fails or does not survive an
# update. Deliberately three independent signals: the ostree marker, the
# distro's own transactional tooling, and an actual write test. A system that
# passes none of them but still has a read-only /usr (a bootc image, a
# hand-rolled setup) is caught by the write test and treated as generic-atomic
# rather than being told it is unsupported.
atomic_detect() {
	local id="" like="" variant=""
	id="$(. /etc/os-release 2>/dev/null; echo "${ID:-}")"
	like="$(. /etc/os-release 2>/dev/null; echo "${ID_LIKE:-}")"
	variant="$(. /etc/os-release 2>/dev/null; echo "${VARIANT_ID:-}")"

	case "$id $like" in
		*arch*)            ATOMIC_BASE=arch   ;;
		*fedora*|*rhel*)   ATOMIC_BASE=fedora ;;
		*suse*)            ATOMIC_BASE=suse   ;;
		*debian*|*ubuntu*) ATOMIC_BASE=debian ;;
		*)                 ATOMIC_BASE=unknown ;;
	esac

	# Does /usr take a write, right now? On SteamOS this flips depending on
	# whether steamos-readonly is disabled, so it is a separate question from
	# "is this an atomic system".
	#
	# Only meaningful as root: for anyone else /usr is read-only by design and
	# a failed write says nothing about the system. Both callers have already
	# established they are root by the time they get here; the guard is so that
	# sourcing this file from anywhere else cannot produce a false positive.
	ATOMIC_USR_RW=1
	if [[ $EUID -eq 0 ]]; then
		if ! touch /usr/.waveline-writetest 2>/dev/null; then
			ATOMIC_USR_RW=0
		else
			rm -f /usr/.waveline-writetest
		fi
	fi

	if [[ -f /run/ostree-booted || -d /ostree ]] \
	   || command -v rpm-ostree >/dev/null 2>&1 \
	   || command -v bootc >/dev/null 2>&1; then
		ATOMIC=1; ATOMIC_KIND=ostree
	elif command -v steamos-readonly >/dev/null 2>&1 || [[ "$id" == "steamos" ]]; then
		ATOMIC=1; ATOMIC_KIND=steamos
	elif command -v transactional-update >/dev/null 2>&1; then
		ATOMIC=1; ATOMIC_KIND=microos
	elif [[ $ATOMIC_USR_RW -eq 0 ]]; then
		ATOMIC=1; ATOMIC_KIND=readonly
	else
		ATOMIC=0; ATOMIC_KIND=""
	fi

	if [[ $ATOMIC -eq 1 ]]; then
		local pretty
		pretty="$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-$id}")"
		case "$ATOMIC_KIND" in
			ostree)   ATOMIC_LABEL="$pretty (ostree / image-based)" ;;
			steamos)  ATOMIC_LABEL="$pretty (read-only SteamOS image)" ;;
			microos)  ATOMIC_LABEL="$pretty (transactional / MicroOS)" ;;
			readonly) ATOMIC_LABEL="$pretty (read-only /usr)" ;;
		esac
		[[ -n "$variant" ]] && ATOMIC_LABEL="$ATOMIC_LABEL  variant: $variant"
	fi
	return 0
}

# The command this distribution family uses to add packages to the *image*.
# Printed as advice only -- Waveline never runs it. Layering is a reboot and a
# permanent change to someone's deployment, and that is their call, not an
# installer's.
atomic_layer_cmd() {
	case "$ATOMIC_KIND" in
		ostree)   printf 'rpm-ostree install %s   (then reboot)\n' "$*" ;;
		steamos)  printf 'sudo steamos-readonly disable && sudo pacman -S %s\n' "$*" ;;
		microos)  printf 'sudo transactional-update pkg install %s   (then reboot)\n' "$*" ;;
		*)        printf 'install with your package manager: %s\n' "$*" ;;
	esac
}

# --------------------------------------------------------- build environment
#
# BUILDENV is one of:
#   host    -- the toolchain is already usable, nothing containerised
#   podman  -- rootless podman, image built from the host's own base release
#   docker  -- same, for systems that only have docker
#   none    -- no way to build; callers degrade to "userspace-only install"

BUILDENV="none"
BUILDENV_ENGINE=""
BUILDENV_IMAGE=""
BUILDENV_BASE_IMAGE=""
BUILDENV_CACHE=""
BUILDENV_CARGO=""
BUILDENV_KERNEL=0        # 1 once the image also carries kernel build support
BUILDENV_READY=0
BUILDENV_ERR=""
BUILDENV_HOST_PATH=""    # PATH for host builds, when the caller needs a custom one
BUILDENV_SNAPSHOT=""     # host package snapshot the image was pinned to, if any
declare -a BUILDENV_MOUNTS=()
declare -a BUILDENV_PRUNED=()   # superseded build images removed by buildenv_ready

# Base image matching the host. The release matters more than anything else
# here: a binary built against Fedora 42's Qt runs on Fedora 42's Qt, and a
# binary built against something newer may not run at all. WAVELINE_BUILD_IMAGE
# overrides it for anyone on a base this does not know.
buildenv_base_image() {
	if [[ -n "${WAVELINE_BUILD_IMAGE:-}" ]]; then
		printf '%s\n' "$WAVELINE_BUILD_IMAGE"; return 0
	fi
	local ver
	ver="$(. /etc/os-release 2>/dev/null; echo "${VERSION_ID:-}")"
	case "$ATOMIC_BASE" in
		fedora)
			# uBlue-derived images (Bazzite, Bluefin, Aurora) keep Fedora's
			# VERSION_ID, so this lands on the right release for them too.
			[[ "$ver" =~ ^[0-9]+$ ]] || ver="latest"
			printf 'registry.fedoraproject.org/fedora:%s\n' "$ver" ;;
		arch)   printf 'docker.io/library/archlinux:base-devel\n' ;;
		suse)   printf 'registry.opensuse.org/opensuse/tumbleweed:latest\n' ;;
		debian) printf 'docker.io/library/debian:stable\n' ;;
		*)      printf '\n' ;;
	esac
}

# ------------------------------------------------- snapshot-pinned base repos
#
# `archlinux:base-devel` is rolling: it is whatever Arch shipped this week. A
# rolling *host* would be fine, but SteamOS is not one -- it is a frozen Arch
# snapshot, and by the time an image is a few months old the container is well
# ahead of it. The binaries then need a glibc the Deck does not have and the
# installer refuses to install them ("the build container is newer than this
# system"), which is the whole of why a container build fails there.
#
# Valve serves the exact snapshot the running image was built from, from the
# same mirror the Deck's own pacman uses, under versioned repository names
# (core-3.7, extra-3.7, ...). Point the container at those and `pacman -Syuu`
# walks the fresh image *back* to the host's package set: same glibc, same Qt,
# same everything the mixer links against.
STEAMOS_MIRROR='https://steamdeck-packages.steamos.cloud/archlinux-mirror/$repo/os/$arch'

# The snapshot suffix the host is pinned to, read from its own pacman.conf so a
# SteamOS release bump is picked up without editing anything here. Empty (and
# non-zero) on every system that is not a snapshot-pinned Arch.
buildenv_snapshot_suffix() {
	[[ "$ATOMIC_KIND" == "steamos" ]] || return 1
	local sfx
	sfx="$(sed -n 's/^[[:space:]]*\[core-\([0-9][0-9.]*\)\][[:space:]]*$/\1/p' \
	       /etc/pacman.conf 2>/dev/null | head -1)"
	[[ -n "$sfx" ]] || return 1
	printf '%s\n' "$sfx"
}

# Writes pacman.conf + mirrorlist for that snapshot into the build context.
# Only core/extra/multilib: jupiter and holo are Valve's own packages, signed
# by Valve's key, and nothing the mixer builds against lives in them.
#
# SigLevel = Never because the snapshot's packages predate the keyring the
# fresh image ships and re-importing keys inside a throwaway build container
# buys nothing: this is the same host, the same HTTPS mirror and the same
# packages the Deck already installed and is running right now.
buildenv_write_snapshot_repos() {
	local dir="$1" sfx="$2" r
	printf 'Server = %s\n' "$STEAMOS_MIRROR" > "$dir/mirrorlist"
	{
		printf '[options]\nArchitecture = auto\nSigLevel = Never\nParallelDownloads = 5\n'
		for r in core extra multilib; do
			printf '\n[%s-%s]\nInclude = /etc/pacman.d/mirrorlist\n' "$r" "$sfx"
		done
	} > "$dir/pacman.conf"
}

# Everything the mixer, waveline-hw and DeepFilterNet need to *build*. The
# runtime side is a separate question -- see buildenv_bundle_libs.
buildenv_packages() {
	case "$ATOMIC_BASE" in
	# fluidsynth is the odd one out: nothing compiles against it, it is
	# dlopened at run time. It is in the image so that the library can be
	# lifted out of it on a system whose read-only /usr does not carry one --
	# see buildenv_fluidsynth_lib.
	fedora) echo "gcc gcc-c++ make cmake pkgconf-pkg-config git curl xz tar findutils which python3 patchelf qt6-qtbase-devel qt6-qtsvg-devel qt6-qtwebsockets-devel pipewire-devel rnnoise-devel cargo rust elfutils-libelf-devel fluidsynth-libs" ;;
	arch)   echo "base-devel cmake pkgconf git curl xz tar python patchelf qt6-base qt6-svg qt6-websockets pipewire rnnoise rust fluidsynth" ;;
	suse)   echo "gcc gcc-c++ make cmake pkg-config git curl xz tar python3 patchelf qt6-base-devel qt6-svg-devel qt6-websockets-devel pipewire-devel rnnoise-devel cargo rust libfluidsynth3" ;;
	debian) echo "build-essential cmake pkg-config git curl xz-utils python3 patchelf qt6-base-dev libqt6svg6-dev libqt6websockets6-dev libpipewire-0.3-dev librnnoise-dev cargo libfluidsynth3" ;;
	*)      echo "" ;;
	esac
}

buildenv_install_line() {
	case "$ATOMIC_BASE" in
	fedora) echo "dnf -y install --setopt=install_weak_deps=False $(buildenv_packages) && dnf clean all" ;;
	# -Syuu and --overwrite: when the repos have been pinned back to a
	# snapshot (SteamOS, below) this transaction is a *downgrade*, and one
	# that crosses package splits -- newer Arch broke gcc-libs into libgomp,
	# libatomic and friends, so going back finds those files already on disk.
	# Both are harmless on a rolling base, where nothing is older than the
	# image. Everything is one transaction on purpose: pacman downgrades
	# itself here, and there must be no second pacman run afterwards.
	arch)   echo "pacman -Syuu --noconfirm --needed --overwrite '*' $(buildenv_packages) && pacman -Scc --noconfirm" ;;
	suse)   echo "zypper -n --gpg-auto-import-keys refresh && zypper -n install --no-recommends $(buildenv_packages) && zypper clean -a" ;;
	debian) echo "apt-get update && apt-get install -y --no-install-recommends $(buildenv_packages) && rm -rf /var/lib/apt/lists/*" ;;
	*)      echo "true" ;;
	esac
}

# Kernel headers for the *running* kernel. Preferred source is the host: an
# image-based distro that ships akmods (Bazzite does) already has an exact
# match under /usr/lib/modules, and mounting it beats hoping the distro's
# archive still has the matching -devel package. dnf is only the fallback.
buildenv_host_kbuild() {
	local d
	for d in "/usr/lib/modules/$KREL/build" "/lib/modules/$KREL/build"; do
		[[ -d "$d" ]] && { printf '%s\n' "$d"; return 0; }
	done
	return 1
}

buildenv_kernel_install_line() {
	case "$ATOMIC_BASE" in
	fedora) echo "dnf -y install kernel-devel-$KREL || dnf -y install kernel-devel || true" ;;
	arch)   echo "pacman -S --noconfirm --needed linux-headers || true" ;;
	suse)   echo "zypper -n install kernel-devel || true" ;;
	debian) echo "apt-get install -y --no-install-recommends linux-headers-$KREL || true" ;;
	*)      echo "true" ;;
	esac
}

# Picks a container engine, or decides the host toolchain is fine as it is.
# host_ok_fn is a caller-supplied predicate (install.sh passes mixer_deps_ok) so
# that a machine which already has a compiler and the Qt/PipeWire development
# files -- an atomic image with those layered, say -- never pays for a container.
buildenv_init() {
	local host_ok_fn="${1:-}"
	BUILDENV_CACHE="$HOME_N/.cache/waveline"
	BUILDENV_CARGO="$BUILDENV_CACHE/cargo"

	if [[ -n "$host_ok_fn" ]] && "$host_ok_fn" >/dev/null 2>&1; then
		BUILDENV="host"; BUILDENV_READY=1
		return 0
	fi

	local eng
	for eng in podman docker; do
		command -v "$eng" >/dev/null 2>&1 || continue
		BUILDENV_ENGINE="$eng"
		BUILDENV="$eng"
		break
	done
	if [[ -z "$BUILDENV_ENGINE" ]]; then
		BUILDENV="none"
		BUILDENV_ERR="no container engine (podman or docker) and no host toolchain"
		return 1
	fi

	BUILDENV_BASE_IMAGE="$(buildenv_base_image)"
	if [[ -z "$BUILDENV_BASE_IMAGE" ]]; then
		BUILDENV="none"
		BUILDENV_ERR="no base image known for this distribution; set WAVELINE_BUILD_IMAGE"
		return 1
	fi
	return 0
}

# Rootless, and as the invoking user rather than root, for three reasons: the
# image store then lives in that user's home where `podman rmi` can reach it,
# build products in the mounted repo come out owned by the user, and no part of
# this needs a daemon or root privileges the rest of the installer does not
# already hold.
buildenv_engine() {
	runuser -u "$RUSER" -- env \
		HOME="$HOME_N" XDG_RUNTIME_DIR="$UD" \
		PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
		"$BUILDENV_ENGINE" "$@"
}

# Builds (or reuses) the image. `want_kernel` adds kernel build support, and is
# a separate layer so that the common case -- a microphone that needs no kernel
# patch at all -- never downloads kernel-devel.
#
# The tag carries a hash of the package list, so changing what Waveline needs
# to build invalidates the image automatically instead of silently reusing a
# stale one.
buildenv_ready() {
	local want_kernel="${1:-0}"
	[[ "$BUILDENV" == "host" ]] && return 0
	[[ "$BUILDENV" == "none" ]] && return 1

	if [[ $BUILDENV_READY -eq 1 ]]; then
		# Already built, and either the kernel layer is not wanted or it is
		# already in there.
		[[ "$want_kernel" != "1" || $BUILDENV_KERNEL -eq 1 ]] && return 0
	fi

	local kb="" spec hash tag snap=""
	if [[ "$want_kernel" == "1" ]]; then
		kb="$(buildenv_host_kbuild || true)"
	fi
	# Part of the tag below: a SteamOS release bump moves the snapshot, and
	# the image built against the old one must not be reused.
	[[ -z "${WAVELINE_BUILD_IMAGE:-}" ]] && snap="$(buildenv_snapshot_suffix || true)"
	spec="$BUILDENV_BASE_IMAGE|$(buildenv_packages)|k=$want_kernel|hostkb=${kb:-no}|$KREL|snap=${snap:-no}"
	hash="$(printf '%s' "$spec" | (sha256sum 2>/dev/null || shasum -a 256) | cut -c1-12)"
	tag="localhost/waveline-build:$hash"

	local ctx cf
	ctx="$(runuser -u "$RUSER" -- mktemp -d -t waveline-buildenv-XXXXXX)" || return 1
	[[ -n "$ctx" && -d "$ctx" ]] || return 1
	cf="$ctx/Containerfile"

	{
		printf 'FROM %s\n' "$BUILDENV_BASE_IMAGE"
		if [[ -n "$snap" ]]; then
			buildenv_write_snapshot_repos "$ctx" "$snap"
			printf 'COPY pacman.conf /etc/pacman.conf\n'
			printf 'COPY mirrorlist /etc/pacman.d/mirrorlist\n'
		fi
		printf 'RUN %s\n' "$(buildenv_install_line)"
		# Only pull kernel headers into the image when the host cannot lend
		# its own; a mounted /usr/lib/modules/$KREL is both exact and free.
		if [[ "$want_kernel" == "1" && -z "$kb" ]]; then
			printf 'RUN %s\n' "$(buildenv_kernel_install_line)"
		fi
	} > "$cf"
	chown "$RUSER:$RUSER" "$cf" 2>/dev/null
	[[ -n "$snap" ]] && chown "$RUSER:$RUSER" "$ctx/pacman.conf" "$ctx/mirrorlist" 2>/dev/null
	BUILDENV_SNAPSHOT="$snap"

	# `image inspect` rather than podman's `image exists`: docker has no such
	# subcommand, and an unrecognised one would be read as "not built yet" and
	# rebuild the image on every single run.
	if ! buildenv_engine image inspect "$tag" >/dev/null 2>&1; then
		if ! buildenv_engine build -t "$tag" -f "$cf" "$ctx" \
		     >/tmp/waveline-buildenv.log 2>&1; then
			BUILDENV_ERR="could not build the container build environment -- see /tmp/waveline-buildenv.log"
			rm -rf "$ctx"
			return 1
		fi
	fi
	rm -rf "$ctx"

	buildenv_prune_stale "$tag"

	BUILDENV_IMAGE="$tag"
	BUILDENV_READY=1
	[[ "$want_kernel" == "1" ]] && BUILDENV_KERNEL=1

	# Mounts are fixed for the life of the run. The repository goes in at its
	# real path so that compiler diagnostics, CMake caches and the DKMS-free
	# module build all refer to paths that exist on the host too.
	BUILDENV_MOUNTS=("$ROOT:$ROOT" "$BUILDENV_CACHE:$BUILDENV_CACHE")
	if [[ "$want_kernel" == "1" && -n "$kb" ]]; then
		# The build symlink usually points into /usr/src/kernels; mount the
		# real directory as well or the link dangles inside the container.
		local real
		BUILDENV_MOUNTS+=("/usr/lib/modules/$KREL:/usr/lib/modules/$KREL:ro")
		real="$(readlink -f "$kb" 2>/dev/null || true)"
		[[ -n "$real" && "$real" != /usr/lib/modules/* && -d "$real" ]] \
		  && BUILDENV_MOUNTS+=("$real:$real:ro")
	fi
	install -d -o "$RUSER" -g "$RUSER" "$BUILDENV_CACHE" "$BUILDENV_CARGO"
	return 0
}

# Copies libfluidsynth.so.3 out of the build image into destdir, for a host
# whose /usr is read-only and has no FluidSynth in it. Distinct from
# buildenv_copy_libs only in that this library is wanted by name rather than
# because the loader asked for it: nothing links against FluidSynth, wavelined
# dlopens it, so it never shows up as a missing soname to go and fetch.
#
# What makes this safe on SteamOS is the snapshot pinning above -- the library
# comes out built against the same glibc as everything else on the Deck.
# Whatever *it* then needs and the host lacks is an ordinary missing-library
# problem, and buildenv_bundle_libs already solves that one.
buildenv_fluidsynth_lib() {
	local dest="$1"
	[[ "$BUILDENV" == "host" || "$BUILDENV" == "none" ]] && return 1
	[[ $BUILDENV_READY -eq 1 ]] || return 1
	buildenv_copy_libs "$dest" libfluidsynth.so.3 || return 1
	[[ -f "$dest/libfluidsynth.so.3" ]] || return 1
	return 0
}

# Removes build images this script made and no longer uses. The tag is a hash
# of the base image, the package list, the kernel and the host's package
# snapshot, so each of those moving strands a whole multi-gigabyte image --
# and a Steam Deck is the machine least able to spare the space. Only
# localhost/waveline-build is touched, never the base image, which is shared
# and is most of what a rebuild would have to download again.
buildenv_prune_stale() {
	local keep="$1" img
	BUILDENV_PRUNED=()
	while read -r img; do
		[[ -n "$img" && "$img" != "$keep" ]] || continue
		buildenv_engine rmi -f "$img" >/dev/null 2>&1 \
		  && BUILDENV_PRUNED+=("$img")
	done < <(buildenv_engine images --format '{{.Repository}}:{{.Tag}}' \
	         localhost/waveline-build 2>/dev/null)
	return 0
}

# Runs one command in the build environment, with the working directory given
# explicitly. Same signature in host and container mode, which is what keeps
# install.sh from growing a second copy of every build step.
buildenv_run() {
	local wd="$1"; shift
	if [[ "$BUILDENV" == "host" ]]; then
		# BUILDENV_HOST_PATH is set by the caller when the toolchain lives
		# somewhere root's PATH does not reach -- rustup's ~/.cargo/bin.
		runuser -u "$RUSER" -- env -C "$wd" HOME="$HOME_N" \
			PATH="${BUILDENV_HOST_PATH:-/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin}" \
			CARGO_HOME="${CARGO_HOME:-$HOME_N/.cargo}" "$@"
		return $?
	fi
	local -a args=(run --rm --security-opt label=disable --net=host
	               -w "$wd" -e "CARGO_HOME=$BUILDENV_CARGO" -e "HOME=$HOME_N")
	buildenv_idmap_args args
	local m
	for m in "${BUILDENV_MOUNTS[@]}"; do args+=(-v "$m"); done
	args+=("$BUILDENV_IMAGE" "$@")
	buildenv_engine "${args[@]}"
}

# Whatever it takes to have files written inside the container come out owned by
# the user on the host. Rootless podman maps the invoking uid with keep-id;
# docker has no such thing and needs the numeric uid:gid instead.
buildenv_idmap_args() {
	local -n _a="$1"
	if [[ "$BUILDENV_ENGINE" == "podman" ]]; then
		_a+=(--userns=keep-id)
	else
		_a+=(--user "$(id -u "$RUSER"):$(id -g "$RUSER")")
	fi
}

buildenv_describe() {
	case "$BUILDENV" in
		host)   printf 'host toolchain\n' ;;
		none)   printf 'none (%s)\n' "${BUILDENV_ERR:-unavailable}" ;;
		*)      if [[ -n "$BUILDENV_SNAPSHOT" ]]; then
			printf '%s container from %s, pinned to this system'"'"'s package snapshot (%s)\n' \
				"$BUILDENV_ENGINE" "$BUILDENV_BASE_IMAGE" "$BUILDENV_SNAPSHOT"
		else
			printf '%s container from %s\n' "$BUILDENV_ENGINE" "$BUILDENV_BASE_IMAGE"
		fi ;;
	esac
}

# ------------------------------------------------------- runtime library fixup
#
# A binary built in the container still has to run on the host. Same base
# distribution and same release means the ABI matches, but the host image may
# simply not ship every runtime library -- librnnoise in particular is a build
# dependency of the mixer and is on almost no desktop image.
#
# So: ask the *host's* loader what is missing, and copy exactly those files out
# of the container. Not a general-purpose bundler -- see the Qt exclusion below.

# Libraries that must never be bundled. Qt's core libraries load platform,
# style and image plugins out of a plugin directory found relative to the Qt
# installation; a copied libQt6Gui.so.6 with no plugin tree behind it aborts at
# startup with "could not load the Qt platform plugin", which is a far worse
# outcome than saying plainly that the host needs a Qt runtime. Leaf Qt
# libraries (Svg, WebSockets) carry no plugins of their own and are fine.
buildenv_lib_is_forbidden() {
	case "$1" in
		libQt6Core.so*|libQt6Gui.so*|libQt6Widgets.so*|libQt6DBus.so*|libQt6Network.so*)
			return 0 ;;
		libc.so*|libm.so*|libstdc++.so*|libgcc_s.so*|ld-linux*)
			return 0 ;;
	esac
	return 1
}

# Sonames the host loader cannot resolve for the given binaries, one per line.
# The $1 ~ /\.so/ guard matters: when the loader complains about a symbol
# version rather than a whole library it prints the *binary's* name first, and
# without the guard that would be mistaken for a library to go and fetch.
buildenv_missing_libs() {
	local extra="$1"; shift
	local out
	out="$(LD_LIBRARY_PATH="$extra" ldd "$@" 2>/dev/null \
	       | awk '/not found/ && $1 ~ /\.so/ { print $1 }' | sort -u)"
	printf '%s\n' "$out"
}

# True when the binaries need a newer glibc/libstdc++ than the host has. This
# is the one failure that bundling cannot paper over, and it means the base
# image was newer than the host -- so say so instead of installing something
# that will not start.
buildenv_abi_skew() {
	local extra="$1"; shift
	LD_LIBRARY_PATH="$extra" ldd "$@" 2>&1 \
	  | grep -E "version \`(GLIBC|GLIBCXX|CXXABI)_[0-9.]+' not found" | sort -u
}

# Copies the named sonames out of the build container into destdir, following
# symlinks so the result is a plain file per library.
buildenv_copy_libs() {
	local dest="$1"; shift
	[[ $# -gt 0 ]] || return 0
	[[ "$BUILDENV" == "host" ]] && return 1
	install -d -o "$RUSER" -g "$RUSER" "$dest"
	local -a args=(run --rm --security-opt label=disable --net=host)
	buildenv_idmap_args args
	args+=(-v "$dest:/out" -w /out "$BUILDENV_IMAGE"
	               /bin/sh -c '
	                 for s in "$@"; do
	                   p=$(ldconfig -p 2>/dev/null | awk -v s="$s" \
	                        "index(\$0, s) { print \$NF; exit }")
	                   [ -n "$p" ] && [ -e "$p" ] && cp -L "$p" "/out/$s"
	                 done' _ "$@")
	buildenv_engine "${args[@]}"
}

# Fills destdir with whatever the host is missing, up to a few rounds because a
# bundled library can itself pull in something else the host does not have.
# Returns 0 when the binaries can run, 1 when they cannot and why is in
# BUILDENV_ERR.
buildenv_bundle_libs() {
	local dest="$1"; shift
	local -a bins=("$@")
	local skew round missing lib
	local -a wanted=()

	for round in 1 2 3 4; do
		# Re-checked every round: a library copied in to satisfy one
		# dependency can itself want a newer libc than the host has, and that
		# is the one failure bundling cannot fix.
		skew="$(buildenv_abi_skew "$dest" "${bins[@]}")"
		if [[ -n "$skew" ]]; then
			BUILDENV_ERR="the build container is newer than this system: ${skew//$'\n'/; }"
			return 1
		fi
		missing="$(buildenv_missing_libs "$dest" "${bins[@]}" \
		           $( [[ -d "$dest" ]] && find "$dest" -maxdepth 1 -name '*.so*' 2>/dev/null ) )"
		[[ -n "${missing//[[:space:]]/}" ]] || return 0
		wanted=()
		while read -r lib; do
			[[ -n "$lib" ]] || continue
			if buildenv_lib_is_forbidden "$lib"; then
				BUILDENV_ERR="this system has no usable runtime for $lib"
				return 1
			fi
			wanted+=("$lib")
		done <<< "$missing"
		[[ ${#wanted[@]} -gt 0 ]] || return 0
		buildenv_copy_libs "$dest" "${wanted[@]}" >/dev/null 2>&1
		# Anything still missing after the copy is not in the container either.
		for lib in "${wanted[@]}"; do
			[[ -e "$dest/$lib" ]] || {
				BUILDENV_ERR="$lib is missing on this system and not in the build image"
				return 1
			}
		done
	done
	BUILDENV_ERR="could not resolve every runtime library"
	return 1
}

# Points the installed binaries at the bundle directory. Done with patchelf in
# the container (the host has no reason to have it) so that no wrapper script
# and no LD_LIBRARY_PATH leaks into whatever the mixer launches.
buildenv_set_rpath() {
	local rpath="$1"; shift
	[[ "$BUILDENV" == "host" ]] && return 0
	buildenv_run "$ROOT" patchelf --set-rpath "$rpath" "$@" >/dev/null 2>&1
}

# Points every bundled library at the directory it is sitting in.
#
# Necessary because patchelf writes DT_RUNPATH, and a RUNPATH applies only to
# the object that carries it -- not, as DT_RPATH did, to anything further down
# the chain. So wavelined's RUNPATH finds libfluidsynth.so.3 in the bundle, and
# then libfluidsynth's *own* search for libinstpatch ignores that RUNPATH
# entirely and comes up empty. One dependency deep is where the difference
# starts to show, which is exactly where a bundled FluidSynth lands.
#
# Giving each bundled library `$ORIGIN` closes it at every depth, and is a
# smaller hammer than --force-rpath on the executables: nothing outside the
# bundle directory changes its search order.
buildenv_set_bundle_rpaths() {
	local dir="$1" f
	[[ "$BUILDENV" == "host" ]] && return 0
	[[ -d "$dir" ]] || return 0
	local -a libs=()
	for f in "$dir"/*.so*; do [[ -f "$f" ]] && libs+=("$f"); done
	[[ ${#libs[@]} -gt 0 ]] || return 0
	buildenv_run "$ROOT" patchelf --set-rpath '$ORIGIN' "${libs[@]}" \
	  >/dev/null 2>&1
	return 0
}

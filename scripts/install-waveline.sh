#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
#
# Waveline bootstrap installer -- the one-line entry point.
#
#   sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Nakildias/Waveline/main/scripts/install-waveline.sh)"
#
# and to remove it again:
#
#   sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Nakildias/Waveline/main/scripts/install-waveline.sh)" -- --uninstall
#
# Any other arguments are handed straight to install.sh, so the one-liner can
# do anything the script can:
#
#   ... install-waveline.sh)" -- --app-only
#
# Why this file exists: install.sh is NOT self-contained. It reads device
# profiles from devices/, sources scripts/lib/, builds the mixer out of app/,
# and installs drop-ins from data/. Piping install.sh itself into bash gives it
# no repository to work from and it dies on the first `source`. So this script
# fetches the repository first, then runs the real installer out of it.
#
# The checkout is kept, not thrown away, at
#
#   ~/.cache/waveline-src
#
# deliberately NOT inside ~/.cache/waveline, which is already the container
# build environment's scratch directory on atomic systems and which
# uninstall.sh tells people they may delete.
#
# because it is worth keeping: .build/ inside it caches a ~150 MB kernel
# tarball that would otherwise be re-downloaded on every run, and uninstall.sh
# needs the same tree. Re-running this updates that clone instead of recloning.
set -uo pipefail

REPO_URL="${WAVELINE_REPO_URL:-https://github.com/Nakildias/Waveline.git}"
REPO_BRANCH="${WAVELINE_REPO_BRANCH:-main}"

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok()   { printf '    \033[1;32mok\033[0m   %s\n' "$*"; }
warn() { printf '    \033[1;33mwarn\033[0m %s\n' "$*"; }
die()  { printf '    \033[1;31m!!\033[0m   %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------- checks
[[ $EUID -eq 0 ]] || die "run this with sudo:
    sudo bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Nakildias/Waveline/main/scripts/install-waveline.sh)\""

# install.sh installs into a real user's home and builds as that user, so a
# root-owned session with no SUDO_USER has nowhere to put anything. Refuse
# early and clearly rather than letting install.sh fail further in.
RUSER="${SUDO_USER:-}"
[[ -n "$RUSER" && "$RUSER" != "root" ]] \
  || die "run this through sudo from your normal user account, not as root directly.
       Waveline installs into that user's home directory."

HOME_N="$(getent passwd "$RUSER" | cut -d: -f6)"
[[ -n "$HOME_N" && -d "$HOME_N" ]] || die "cannot find a home directory for $RUSER"

MODE=install
case "${1:-}" in
	--uninstall|--remove) MODE=uninstall; shift ;;
esac

say "Waveline bootstrap"
ok "user: $RUSER"
ok "mode: $MODE"

# --------------------------------------------------------------------- git
# git is needed before anything else can be fetched, so this is the one
# dependency this script installs itself. Everything else is install.sh's job.
if ! command -v git >/dev/null 2>&1; then
	say "Installing git"
	# shellcheck disable=SC1091
	FAM=unknown
	[[ -r /etc/os-release ]] && . /etc/os-release 2>/dev/null
	case "${ID:-}${ID_LIKE:-}" in
		*arch*)            FAM=arch   ;;
		*fedora*|*rhel*)   FAM=fedora ;;
		*debian*|*ubuntu*) FAM=debian ;;
	esac
	case "$FAM" in
		arch)   pacman -S --needed --noconfirm git ;;
		fedora) dnf install -y git ;;
		debian) DEBIAN_FRONTEND=noninteractive apt-get update -qq \
		          && apt-get install -y --no-install-recommends git ;;
		*)      die "git is not installed and this distribution was not recognised.
       Install git, then run this again." ;;
	esac
	command -v git >/dev/null 2>&1 || die "could not install git"
	ok "git installed"
fi

# ---------------------------------------------------------------- checkout
# Cloned as the user, not as root: install.sh builds the mixer with `runuser`,
# so the tree has to be writable by that user. A root-owned checkout would fail
# at the first cmake invocation.
CHECKOUT="$HOME_N/.cache/waveline-src"
install -d -o "$RUSER" -g "$RUSER" "$HOME_N/.cache"

if [[ -d "$CHECKOUT/.git" ]]; then
	say "Updating the existing checkout"
	ok "$CHECKOUT"
	if ! runuser -u "$RUSER" -- git -C "$CHECKOUT" fetch --depth 1 origin "$REPO_BRANCH"; then
		warn "could not reach the remote -- using the copy already on disk"
	elif ! runuser -u "$RUSER" -- git -C "$CHECKOUT" reset --hard "origin/$REPO_BRANCH"; then
		die "could not update $CHECKOUT. Delete it and run this again:
       rm -rf $CHECKOUT"
	else
		ok "updated to origin/$REPO_BRANCH"
	fi
else
	say "Downloading Waveline"
	# A leftover directory with no .git in it is either an interrupted clone or
	# something that is not ours; either way it cannot be cloned into.
	[[ -e "$CHECKOUT" ]] && rm -rf "$CHECKOUT"
	runuser -u "$RUSER" -- git clone --depth 1 --branch "$REPO_BRANCH" \
	  "$REPO_URL" "$CHECKOUT" \
	  || die "could not clone $REPO_URL"
	ok "$CHECKOUT"
fi

# ----------------------------------------------------------------- hand off
SCRIPT="install.sh"
[[ "$MODE" == "uninstall" ]] && SCRIPT="uninstall.sh"
[[ -f "$CHECKOUT/$SCRIPT" ]] || die "$SCRIPT is missing from the checkout"

say "Running $SCRIPT"
# Invoked through bash rather than executed: a clone can lose the execute bit
# (a restrictive umask, a filesystem mounted noexec) and the failure that
# produces is an opaque EACCES.
#
# SUDO_USER is passed through explicitly. This script is already root, so the
# child inherits it, but install.sh keys every "which user am I installing
# for" decision off that one variable and it is worth being obvious about.
exec env SUDO_USER="$RUSER" bash "$CHECKOUT/$SCRIPT" "$@"

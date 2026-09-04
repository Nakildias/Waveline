#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
#
# Configure, build, and install wavelined + waveline-mixer + wavelined-cli into
# ~/.local/bin.
#
#   ./app/build.sh              # build, install, restart wavelined if running
#   ./app/build.sh --no-restart # build and install only
#
# Needs: cmake, g++, Qt6 (Core, Widgets, DBus, Svg, Network, WebSockets),
#        libpipewire-0.3, rnnoise
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"
PREFIX="${WAVELINE_PREFIX:-$HOME/.local}"
RESTART=1
[[ "${1:-}" == "--no-restart" ]] && RESTART=0

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD"

WAS_RUNNING=0
if systemctl --user is-active --quiet wavelined 2>/dev/null; then
	WAS_RUNNING=1
	systemctl --user stop wavelined
fi

cmake --install "$BUILD" --prefix "$PREFIX"

# ALSA namehints for Audacity / Wine / etc. (no extra PipeWire nodes).
REPO="$(cd "$ROOT/.." && pwd)"
if [[ -x "$REPO/scripts/install-alsa-aliases.sh" ]]; then
	"$REPO/scripts/install-alsa-aliases.sh" install "$HOME"
fi
# Keep the user unit in sync (ReadWritePaths for ~/.config/alsa, etc.).
if [[ -f "$REPO/systemd/wavelined.service" ]]; then
	mkdir -p "$HOME/.config/systemd/user"
	install -m644 "$REPO/systemd/wavelined.service" \
		"$HOME/.config/systemd/user/wavelined.service"
	systemctl --user daemon-reload 2>/dev/null || true
fi

echo "Installed: $PREFIX/bin/wavelined  $PREFIX/bin/waveline-mixer  $PREFIX/bin/wavelined-cli"

if (( RESTART && WAS_RUNNING )); then
	systemctl --user start wavelined
	echo "Restarted wavelined"
elif (( WAS_RUNNING && !RESTART )); then
	echo "wavelined was stopped for install; start with: systemctl --user start wavelined"
elif (( RESTART )); then
	echo "wavelined is not running (start with: systemctl --user start wavelined)"
fi

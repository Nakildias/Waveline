#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
# Waveline -- remove everything install.sh added.
# https://github.com/Nakildias/Waveline
#
#   sudo ./uninstall.sh
#
# Removes the DKMS module, every device profile's config drop-ins and udev
# rules, the binaries and the services, then reloads the audio stack. Packages
# installed as build dependencies are deliberately NOT removed: other things on
# the system may need them, and removing dkms or kernel headers could break
# unrelated modules.
#
# Every profile is cleaned up, not only the ones whose device is plugged in
# right now. A microphone that is unplugged at uninstall time is exactly the
# case where leaving its files behind would be hardest to notice.
#
# Safety: refuses to unload the patched module unless a stock snd-usb-audio is
# actually available to replace it, so you cannot end up with no USB audio
# driver at all.
#
# `set -uo pipefail` without `-e`, for the same reason install.sh does it: this
# removes several independent things, and stopping at the first one that is
# already gone would leave the rest installed. Every step checks its own status.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WAVELINE_ROOT="$ROOT"
DKMS_NAME="snd-usb-audio-waveline"; DKMS_VER="1.0"
# The pre-Waveline package name. Removed too, so uninstalling from a tree that
# has been updated still cleans up what the older installer left.
DKMS_OLD="snd-usb-audio-wave3"
KREL="$(uname -r)"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
ok()   { printf '    \033[32mok\033[0m   %s\n' "$*"; }
warn() { printf '    \033[33mwarn\033[0m %s\n' "$*"; }
die()  { printf '\n\033[31mxx   %s\033[0m\n' "$*" >&2; exit 1; }

# shellcheck source=scripts/lib/profiles.sh
source "$ROOT/scripts/lib/profiles.sh"
# shellcheck source=scripts/lib/atomic.sh
source "$ROOT/scripts/lib/atomic.sh"

[[ $EUID -eq 0 ]] || die "run as root:  sudo ./uninstall.sh"
RUSER="${SUDO_USER:-}"
[[ -n "$RUSER" && "$RUSER" != "root" ]] \
  || die "run via sudo from your normal user account, not as root"
UID_N="$(id -u "$RUSER")"; UD="/run/user/$UID_N"
HOME_N="$(getent passwd "$RUSER" | cut -d: -f6)"
[[ -d "$HOME_N" ]] || die "cannot resolve home directory for $RUSER"
runu() { runuser -u "$RUSER" -- env XDG_RUNTIME_DIR="$UD" \
         DBUS_SESSION_BUS_ADDRESS="unix:path=$UD/bus" "$@"; }

atomic_detect
KMOD_TOOL=/var/lib/waveline/bin/waveline-kmod

# ------------------------------------------------------------ safety check
say "Checking it is safe to remove the module"
STOCK_FOUND=0
for p in "/var/lib/dkms/$DKMS_NAME/original_module/$KREL/$(uname -m)/snd-usb-audio.ko"* \
         "/var/lib/dkms/$DKMS_OLD/original_module/$KREL/$(uname -m)/snd-usb-audio.ko"* \
         "/usr/lib/modules/$KREL/kernel/sound/usb/snd-usb-audio.ko"* \
         "/lib/modules/$KREL/kernel/sound/usb/snd-usb-audio.ko"*; do
	[[ -f "$p" ]] && { STOCK_FOUND=1; ok "stock driver available: $p"; break; }
done
if [[ $STOCK_FOUND -eq 0 ]]; then
	warn "no stock snd-usb-audio found for $KREL."
	warn "  Config files will still be removed, but the module will be left"
	warn "  in place so you are not left without a USB audio driver."
	warn "  Reinstall your kernel package to restore it, then re-run this."
fi

# ------------------------------------------------------------- stop stack
say "Stopping audio stack"
runu systemctl --user stop wireplumber pipewire-pulse.service pipewire-pulse.socket \
     pipewire.service pipewire.socket >/dev/null 2>&1
sleep 2
REF="$(cat /sys/module/snd_usb_audio/refcnt 2>/dev/null || echo 0)"
[[ "$REF" == "0" ]] && ok "snd_usb_audio not in use" \
                    || warn "snd_usb_audio still in use (refcnt $REF); module swap may be skipped"

# ------------------------------------------------------------ remove dkms
say "Removing DKMS module"
REMOVED_DKMS=0
for pkg in "$DKMS_NAME" "$DKMS_OLD"; do
	if dkms status -m "$pkg" 2>/dev/null | grep -q "$pkg"; then
		if [[ $STOCK_FOUND -eq 1 ]]; then
			modprobe -r snd_usb_audio 2>/dev/null || warn "could not unload module now"
			dkms remove -m "$pkg" -v "$DKMS_VER" --all >/dev/null 2>&1 \
			  && ok "$pkg removed (stock driver restored)" \
			  || warn "dkms remove reported errors for $pkg"
			rm -rf "/usr/src/$pkg-$DKMS_VER"
			REMOVED_DKMS=1
		else
			warn "skipped $pkg: no stock driver to fall back to"
		fi
	else
		rm -rf "/usr/src/$pkg-$DKMS_VER"
	fi
done
# Pre-rename sources landed in /usr/src/Arch Linux-1.0/. The space breaks
# Arch's DKMS pacman hook on every kernel reinstall (Linux/1.0 errors).
dkms remove -m "Arch Linux" -v "$DKMS_VER" --all >/dev/null 2>&1
rm -rf "/usr/src/Arch Linux-$DKMS_VER"
[[ $REMOVED_DKMS -eq 1 ]] && { depmod -a "$KREL" && ok "depmod updated"; } \
                          || ok "no DKMS module of ours was installed"

# ------------------------------------------------- remove the DKMS-free module
# The atomic path: a module in /var and a boot unit that swaps it in. Checked
# unconditionally rather than only on an atomic system -- a tree copied between
# machines, or a SteamOS install later unlocked, should still be cleaned up.
say "Removing the out-of-tree kernel module"
if [[ -x "$KMOD_TOOL" ]]; then
	# waveline-kmod refuses to unload unless a stock driver is available, which
	# is the same interlock as above.
	bash "$KMOD_TOOL" remove || warn "waveline-kmod remove reported errors"
	rm -f /usr/local/bin/waveline-kmod 2>/dev/null
	ok "removed waveline-kmod"
elif [[ -e /etc/systemd/system/waveline-kmod.service || -d /var/lib/waveline/modules ]]; then
	# The tool is gone but its leftovers are not.
	systemctl disable --now waveline-kmod.service >/dev/null 2>&1
	rm -f /etc/systemd/system/waveline-kmod.service
	systemctl daemon-reload >/dev/null 2>&1
	rm -rf /var/lib/waveline/modules /var/lib/waveline/kmod.conf
	rm -f /usr/local/bin/waveline-kmod /run/waveline-kmod.state
	ok "removed /var/lib/waveline and waveline-kmod.service"
	warn "the patched module may still be loaded; it goes away at the next reboot"
else
	ok "no out-of-tree module of ours was installed"
fi
rmdir /var/lib/waveline/bin /var/lib/waveline 2>/dev/null

# ----------------------------------------------------------- remove config
# Driven off the profiles rather than a hardcoded list, so a device added later
# is cleaned up by this script without it being touched.
say "Removing device config files"
WP="$HOME_N/.config/wireplumber/wireplumber.conf.d"
PW="$HOME_N/.config/pipewire/pipewire.conf.d"
FOUND_CONF=0
while read -r dir; do
	[[ -n "$dir" ]] || continue
	profile_load "$dir"
	for pair in "$WIREPLUMBER_CONF:$WP" "$PIPEWIRE_CONF:$PW"; do
		rel="${pair%%:*}"; dest="${pair##*:}"
		[[ -n "$rel" ]] || continue
		f="$dest/$(basename "$rel")"
		if [[ -f "$f" ]]; then
			rm -f "$f" && ok "removed ${f/#$HOME_N/\~}"
			FOUND_CONF=1
		fi
	done
done < <(profile_dirs)
# Waveline's own graph policy, installed on every machine rather than carried
# by a device profile, so the profile-driven loop above never sees it.
for f in "$WP/50-waveline-driver-policy.conf" "$PW/50-waveline-clock.conf"; do
	[[ -f "$f" ]] && { rm -f "$f" && ok "removed ${f/#$HOME_N/\~}"; FOUND_CONF=1; }
done
# Names this project no longer uses. Removed by hand because the loop above
# asks the profiles what they installed, and a profile that has been renamed or
# has had a file taken out of it can no longer name what it left behind:
#
#   51-wave3.conf / 51-wave3-quantum.conf   pre-Waveline names
#   51-waveline-wave3-quantum.conf          the old global Pro Audio quantum
#   51-waveline-rocksmith.conf              renamed to -rocksmith-tone-cable
#   51-waveline-modmic.conf                 renamed to -modmicwireless
for f in "$WP/51-wave3.conf" "$PW/51-wave3-quantum.conf" \
         "$PW/51-waveline-wave3-quantum.conf" \
         "$WP/51-waveline-rocksmith.conf" \
         "$WP/51-waveline-modmic.conf"; do
	[[ -f "$f" ]] && { rm -f "$f" && ok "removed ${f/#$HOME_N/\~}"; FOUND_CONF=1; }
done
[[ $FOUND_CONF -eq 1 ]] || ok "no device config files were installed"
# Only remove the directories if they are now empty -- they may hold the user's
# own drop-ins, which are none of our business.
for d in "$WP" "$PW"; do
	rmdir "$d" 2>/dev/null && ok "removed empty ${d/#$HOME_N/\~}"
done

# ALSA PCM aliases (Audacity / Wine); leave any other asoundrc content alone.
if [[ -x "$ROOT/scripts/install-alsa-aliases.sh" ]]; then
	runu bash "$ROOT/scripts/install-alsa-aliases.sh" remove "$HOME_N" >/dev/null \
	  && ok "removed ~/.config/alsa/waveline.conf"
fi

# ------------------------------------------------------ remove control tools
say "Removing the binaries and services"
# Stop and disable the services before deleting the binaries they run. The
# pre-rename units are included: an enabled wave3d.service left behind would
# keep trying to start a binary that no longer exists.
for unit in waveline-sync wavelined wave3-sync wave3d; do
	if runu systemctl --user list-unit-files "$unit.service" >/dev/null 2>&1; then
		runu systemctl --user disable --now "$unit.service" >/dev/null 2>&1 \
		  && ok "$unit service stopped and disabled"
	fi
done
for f in "$HOME_N/.config/systemd/user/waveline-sync.service" \
         "$HOME_N/.config/systemd/user/wavelined.service" \
         "$HOME_N/.config/systemd/user/pipewire.service.d/waveline-limits.conf" \
         "$HOME_N/.config/systemd/user/wave3-sync.service" \
         "$HOME_N/.config/systemd/user/wave3d.service" \
         "$HOME_N/.local/bin/wavelined" \
         "$HOME_N/.local/bin/waveline-mixer" \
         "$HOME_N/.local/share/applications/waveline-mixer.desktop" \
         "$HOME_N/.local/share/icons/hicolor/scalable/apps/waveline-mixer.svg" \
         "$HOME_N/.local/bin/waveline-hw" \
         "$HOME_N/.local/bin/wave3d" \
         "$HOME_N/.local/bin/wave3-mixer" \
         "$HOME_N/.local/bin/wave3" \
         "$HOME_N/.local/lib/waveline/waveline-hw" \
         "$HOME_N/.local/lib/wave3/wave3" \
         "$HOME_N/.local/lib/wave3/wave3_usb.py" \
         "$HOME_N/.local/share/waveline/models/DeepFilterNet3_onnx.tar.gz" \
         "$HOME_N/.local/share/wave3/models/DeepFilterNet3_onnx.tar.gz"; do
	# -e is false for a dangling symlink, so test -L too.
	if [[ -e "$f" || -L "$f" ]]; then rm -f "$f" && ok "removed ${f/#$HOME_N/\~}"; fi
done
# One transport module per device, so glob rather than list them.
rm -f "$HOME_N/.local/lib/waveline"/waveline_*.py
rm -rf "$HOME_N/.local/lib/waveline/__pycache__" "$HOME_N/.local/lib/wave3/__pycache__" 2>/dev/null
# Libraries copied out of the build container because the host image did not
# ship them. Entirely ours -- nothing else has any reason to look in here.
if [[ -d "$HOME_N/.local/lib/waveline/runtime" ]]; then
	rm -rf "$HOME_N/.local/lib/waveline/runtime" \
	  && ok "removed ~/.local/lib/waveline/runtime (bundled libraries)"
fi
rmdir "$HOME_N/.local/lib/waveline" "$HOME_N/.local/lib/wave3" 2>/dev/null
rmdir "$HOME_N/.local/share/waveline/models" "$HOME_N/.local/share/waveline" \
      "$HOME_N/.local/share/wave3/models" "$HOME_N/.local/share/wave3" 2>/dev/null
# Only ours went in here; leave the directory alone if the user added drop-ins.
rmdir "$HOME_N/.config/systemd/user/pipewire.service.d" 2>/dev/null
runu systemctl --user daemon-reload >/dev/null 2>&1
runu update-desktop-database "$HOME_N/.local/share/applications" >/dev/null 2>&1 || true
runu gtk-update-icon-cache -f -t "$HOME_N/.local/share/icons/hicolor" >/dev/null 2>&1 || true

# ~/.local/lib/libdf.so is NOT removed automatically. It is a general-purpose
# DeepFilterNet build in a shared location: other things may use it, and the
# user may have installed it themselves long before this project. Point at it
# instead of deciding for them.
if [[ -e "$HOME_N/.local/lib/libdf.so" ]]; then
	ok "left in place: ~/.local/lib/libdf.so  (DeepFilterNet, 23 MB)"
	echo  "         Not removed automatically -- it is a shared library in a"
	echo  "         shared location and may not be ours. To delete it:"
	echo  "             rm ~/.local/lib/libdf.so"
fi

# ---------------------------------------------------------- remove udev rules
say "Removing udev rules"
FOUND_UDEV=0
while read -r dir; do
	[[ -n "$dir" ]] || continue
	profile_load "$dir"
	[[ -n "$UDEV_RULES" ]] || continue
	f="/etc/udev/rules.d/$(basename "$UDEV_RULES")"
	if [[ -f "$f" ]]; then
		rm -f "$f" && ok "removed $f"
		FOUND_UDEV=1
		for usbid in $USB_IDS; do
			udevadm trigger --subsystem-match=usb \
			  --attr-match=idVendor="${usbid%%:*}" \
			  --attr-match=idProduct="${usbid##*:}" >/dev/null 2>&1
		done
	fi
done < <(profile_dirs)
if [[ -f /etc/udev/rules.d/60-wave3-control.rules ]]; then
	rm -f /etc/udev/rules.d/60-wave3-control.rules \
	  && ok "removed /etc/udev/rules.d/60-wave3-control.rules"
	FOUND_UDEV=1
fi
[[ $FOUND_UDEV -eq 1 ]] && { udevadm control --reload-rules >/dev/null 2>&1
                             ok "udev rules reloaded"; } \
                        || ok "no udev rules of ours were installed"

# -------------------------------------------- remove real-time scheduling rights
# Both files are ours by name and describe nothing but this install, so they go.
#
# The running session keeps the raised limits until the next login, because
# lowering them under processes that are already real-time would be a worse
# thing to do than leaving them: nothing is harmed by a ceiling nobody reaches.
say "Removing real-time scheduling privileges"
FOUND_RT=0
UID_N="$(id -u "$RUSER" 2>/dev/null || echo "")"
for f in /etc/security/limits.d/99-waveline-realtime.conf \
         ${UID_N:+/etc/systemd/system/user@${UID_N}.service.d/10-waveline-realtime.conf}; do
	[[ -f "$f" ]] || continue
	rm -f "$f" && ok "removed $f" && FOUND_RT=1
done
# Only the drop-in directory, and only if we emptied it. limits.d stays even
# when nothing is left in it: it is normally the distribution's, and on the
# machines where install.sh had to create it, an empty pam_limits directory is
# inert and other packages drop files there too.
[[ -n "$UID_N" ]] \
  && rmdir "/etc/systemd/system/user@${UID_N}.service.d" 2>/dev/null
if [[ $FOUND_RT -eq 1 ]]; then
	systemctl daemon-reload >/dev/null 2>&1
	ok "the current session keeps them until you log out"
else
	ok "none were installed"
fi

warn "Hardware settings like Clipguard and the monitor mix are stored in the"
warn "  microphone, not on this machine, so they keep whatever value you last"
warn "  set. Set them back with waveline-hw before uninstalling if you care."

# The device profile IS ours and describes nothing but this install, so it goes.
# The mixer profiles are the user's own work and stay.
rm -f "$HOME_N/.config/waveline/profile.conf" \
  && ok "removed ~/.config/waveline/profile.conf"
if [[ -f "$HOME_N/.config/waveline/config.json" ]]; then
	ok "kept ~/.config/waveline/config.json (your mixer profiles) -- delete it by hand if you want it gone"
else
	rmdir "$HOME_N/.config/waveline" 2>/dev/null
fi
[[ -d "$HOME_N/.config/wave3" ]] \
  && ok "kept ~/.config/wave3 (mixer profiles from before the rename)"

# -------------------------------------------------------------- reload all
say "Restarting audio stack"
modprobe snd-usb-audio 2>/dev/null
sleep 1
runu systemctl --user start pipewire.socket pipewire-pulse.socket >/dev/null 2>&1
runu systemctl --user start wireplumber >/dev/null 2>&1
sleep 5
ok "restarted"

say "Verifying the system is healthy"
MOD="$(modinfo -F filename snd-usb-audio 2>/dev/null)"
[[ -n "$MOD" ]] && ok "snd-usb-audio present: $MOD" || warn "snd-usb-audio NOT found"
case "$MOD" in *updates/dkms*) warn "patched module still loaded -- reboot to finish" ;; esac
grep -q "" /proc/asound/cards 2>/dev/null && {
	printf '    sound cards:\n'; sed 's/^/      /' /proc/asound/cards; }
for s in pipewire wireplumber pipewire-pulse; do
	printf '    %-14s %s\n' "$s" "$(runu systemctl --user is-active $s 2>&1)"
done

say "Done"
echo "    Build dependencies (dkms, headers, gcc...) were left installed."
echo "    Any device behaviour the removed fixes worked around will return."

# Deliberately not reverted. The installer may have replaced jack2 with the
# PipeWire JACK shim; putting jack2 back would hand the sound card to a second
# audio server again, which is a bug on a PipeWire system whether or not
# Waveline is installed. Say so rather than doing it, because it is a change to
# the machine that outlives the uninstall.
if command -v pacman >/dev/null 2>&1 && pacman -Q pipewire-jack >/dev/null 2>&1; then
	echo "    libjack is provided by pipewire-jack (the installer may have"
	echo "    replaced jack2 with it). Left in place: it is the correct setup"
	echo "    for a PipeWire system. Only reinstall jack2 if you intend to run"
	echo "    a real jackd."
fi

# The checkout the one-line installer made, if that is what this is running from.
# It cannot delete itself out from under a running script, so it says so instead.
if [[ "$ROOT" == "$HOME_N/.cache/waveline-src" ]]; then
	echo
	ok "this ran from the checkout the one-line installer made"
	echo "        Nothing needs it now. To remove it as well:"
	echo "            rm -rf ~/.cache/waveline-src"
fi

# The container build environment is deliberately not deleted here. It is a
# cache -- possibly a slow one to rebuild -- and it lives in the user's own
# image store where podman's own tooling is the right thing to manage it with.
if [[ $ATOMIC -eq 1 ]]; then
	for eng in podman docker; do
		command -v "$eng" >/dev/null 2>&1 || continue
		if runu "$eng" images -q localhost/waveline-build 2>/dev/null | grep -q .; then
			echo
			ok "the Waveline build image was left in place"
			echo "        It is only a build cache. To reclaim its space (~1-2 GB):"
			echo "            $eng rmi \$($eng images -q localhost/waveline-build)"
			echo "            rm -rf ~/.cache/waveline"
		fi
		break
	done
fi

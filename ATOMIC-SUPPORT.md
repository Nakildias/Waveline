<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Atomic / image-based distribution support

Waveline installs and runs on **Bazzite, SteamOS, Fedora Silverblue, Kinoite,
Bluefin, Aurora, openSUSE MicroOS** and other image-based systems. It is
detected automatically — there is no flag to pass and no separate installer.

```bash
git clone https://github.com/Nakildias/Waveline.git
cd Waveline
sudo ./install.sh
```

Nothing is layered onto your image. Nothing needs a reboot. `sudo ./uninstall.sh`
puts the machine back.

---

## Why this needed doing at all

Waveline installs six things. Five of them were already fine on an atomic
system, because they were never in `/usr` to begin with:

| Step | Goes to | Read-only `/usr` a problem? |
|---|---|---|
| 2. WirePlumber drop-ins | `~/.config/wireplumber/wireplumber.conf.d/` | no |
| 3. PipeWire drop-ins | `~/.config/pipewire/pipewire.conf.d/` | no |
| 4. `waveline-hw` + udev rules | `~/.local/lib/waveline/`, `/etc/udev/rules.d/` | no |
| 5. `wavelined` + `waveline-mixer` | `~/.local/bin/`, `~/.config/systemd/user/` | no |
| 6. DeepFilterNet | `~/.local/lib/libdf.so` | no |
| **1. Kernel patch** | **`/usr/src`, `/usr/lib/modules` via DKMS** | **yes** |

So this is not a port. It is two problems:

1. **A compiler.** The host package manager either does not exist, or writes to
   a read-only image, or writes somewhere the next system update overwrites.
2. **One kernel module**, for the one in-tree device profile that needs a kernel
   change at all (the Elgato Wave:3 `usb_set_interface -110` lockup). DKMS has
   nowhere to put it.

Everything else is byte-identical to what a mutable distro gets. The mixer, the
routing, the noise suppression, the dual mixes, the Web Companion, the profiles
— none of it is affected by any of this.

---

## Detection

`atomic_detect` in [`scripts/lib/atomic.sh`](scripts/lib/atomic.sh) sets
`ATOMIC=1` on any of four independent signals:

| Signal | `ATOMIC_KIND` | Systems |
|---|---|---|
| `/run/ostree-booted`, `/ostree`, `rpm-ostree`, `bootc` | `ostree` | Silverblue, Kinoite, Bazzite, Bluefin, Aurora, bootc images |
| `steamos-readonly` present, or `ID=steamos` | `steamos` | SteamOS 3.x / Steam Deck |
| `transactional-update` present | `microos` | openSUSE MicroOS, Aeon, Kalpa |
| `/usr` refuses a write (tested as root) | `readonly` | anything else with an immutable `/usr` |

The fourth exists so that a system none of the first three recognise still gets
the working path rather than being told it is unsupported. Previous releases
did the opposite: they detected the same conditions and called `die`.

`ATOMIC_BASE` (`fedora` / `arch` / `suse` / `debian`) comes from `ID` and
`ID_LIKE` and decides which base image and package names to use. uBlue-derived
images keep Fedora's `VERSION_ID`, so Bazzite resolves to the right Fedora
release without a special case.

---

## Problem 1: where the compiler comes from

`buildenv_init` picks one of three answers, in order:

**`host`** — the toolchain is already usable. Nothing is containerised. This is
what every mutable distro gets, unchanged, and what an atomic system gets if
you have already layered the development packages yourself.

**`podman` / `docker`** — the normal atomic path; podman is preferred and is
what every distribution here ships, docker is accepted if it is what you have.
A build image is created from the **host's own base distribution and release**:

| Host | Base image |
|---|---|
| Silverblue / Kinoite / Bazzite / Bluefin / Aurora | `registry.fedoraproject.org/fedora:$VERSION_ID` |
| SteamOS | `docker.io/library/archlinux:base-devel` |
| MicroOS / Aeon / Kalpa | `registry.opensuse.org/opensuse/tumbleweed:latest` |
| anything else | `WAVELINE_BUILD_IMAGE=…` |

Matching the release is the whole point: a binary built against Fedora 42's Qt
runs on Fedora 42's Qt. Matching it loosely is what produces a binary that will
not start.

**`none`** — no engine and no host toolchain. The install continues and puts in
everything that needs no compiler; the mixer is skipped with an explanation.

The container is **rootless and runs as your user**, not root. That means the
image store lives in your home where `podman rmi` can reach it, build products
in the mounted repository come out owned by you, and no daemon is involved. The
repository is mounted at its real path so compiler diagnostics and CMake caches
refer to paths that exist on the host too. The cargo registry is cached in
`~/.cache/waveline/cargo` so a second install does not re-download the
DeepFilterNet crate graph.

The image tag carries a hash of the package list, so changing what Waveline
needs to build invalidates the image instead of silently reusing a stale one.

### Making a container-built binary run on the host

Same base and same release means the ABI matches. What can still be missing is a
library the host image simply does not ship — `librnnoise` above all, which is a
build dependency of the mixer and is on almost no desktop image.

So after the build, the **host's own loader** is asked what it cannot resolve
(`ldd`), exactly those files are copied out of the container into
`~/.local/lib/waveline/runtime/`, and the binaries are given an RPATH of
`$ORIGIN/../lib/waveline/runtime` with `patchelf` (which lives in the image, so
the host never needs it). No wrapper script, no `LD_LIBRARY_PATH` leaking into
whatever the mixer launches. Nothing is copied that the host already has, and
the directory is rebuilt from scratch on every install so a stale copy can never
shadow a newer system library.

Two things are deliberately **not** bundled:

- **Qt's core libraries** (`libQt6Core`, `Gui`, `Widgets`, `DBus`, `Network`).
  Qt loads platform, style and image plugins out of its own installation; a
  copied `libQt6Gui.so.6` with no plugin tree behind it aborts at startup with
  *"could not load the Qt platform plugin"*. If the host has no Qt 6 at all, the
  installer says so and prints the one-time `rpm-ostree install` line instead of
  producing something that does not work. Leaf Qt libraries (`Svg`,
  `WebSockets`) carry no plugins and are bundled normally.
- **glibc, libstdc++, libgcc.** If the binary needs a newer one than the host
  has, that is a base-image mismatch and bundling cannot fix it. The installer
  reports the exact symbol versions and points at `WAVELINE_BUILD_IMAGE`.

### SteamOS

SteamOS is the one atomic system with a supported, reversible way back to a
writable `/usr`. Using it is strictly better than a container, because the
binaries then link against the very libraries they will run against:

```bash
sudo WAVELINE_STEAMOS_UNLOCK=1 ./install.sh
```

This runs `steamos-readonly disable`, populates pacman's keyring, installs the
dependencies with pacman, builds natively, and **re-enables `steamos-readonly`
on the way out** — including if the script dies partway, via an `EXIT` trap.

It is opt-in because a SteamOS update replaces the image and takes those
packages with it. Waveline itself lives in `$HOME` and `/var` and survives; only
the build dependencies go, and re-running the installer restores them. Without
the flag, SteamOS takes the container path like everything else.

---

## Problem 2: the kernel module, without DKMS

Only one in-tree device profile needs a kernel change: the Elgato Wave:3. If you
do not have one, this entire section does nothing — the installer says
`not needed` and moves on.

DKMS writes the built module into `/usr/lib/modules/<release>/updates/dkms` and
runs `depmod`. On an atomic system it cannot write there, and on SteamOS-with-
`/usr`-unlocked it can, but the next system update replaces the image and takes
the module with it silently.

[`scripts/waveline-kmod`](scripts/waveline-kmod) replaces DKMS for this one job:

```
/var/lib/waveline/modules/<kernel release>/snd-usb-audio.ko   the module
/var/lib/waveline/bin/waveline-kmod                           the tool
/etc/systemd/system/waveline-kmod.service                     loads it at boot
```

`/var` and `/etc` are writable and persistent on every one of these systems.

**Keyed by kernel release on purpose.** A module built for one kernel must never
load into another. After a kernel update the path simply does not exist, the
stock in-tree driver is used, and nothing breaks — the patched behaviour is
just absent until you rebuild.

**The swap is deliberately not a `modprobe.d` override or a blacklist.** The
boot unit runs after udev has settled and before `sound.target`, and does:

1. `modprobe snd-usb-audio` — to pull in the dependency chain (`snd-hwdep`,
   `snd-usbmidi-lib`, `snd-pcm`, `mc`). `insmod` resolves nothing itself.
2. `rmmod snd_usb_audio` — the leaf only, so the dependencies stay loaded and
   the blast radius is one module.
3. `insmod` the patched build.
4. On any failure: reload the stock module and say why.

At every step, the outcome of a failure is *the stock driver stays loaded*. A
machine with a working USB audio driver is worth more than one with a patched
driver, and a blacklist would have made "our unit did not run" mean "no USB
audio at all".

### Secure Boot

An unsigned module is rejected under Secure Boot. `waveline-kmod install`
detects the state and, if the distribution already generated a machine-owner key
for exactly this purpose, signs with it using the kernel's own `sign-file`:

- `/etc/pki/akmods/private/private_key.priv` (Fedora / uBlue — Bazzite, Bluefin, Aurora)
- `/var/lib/shim-signed/mok/MOK.priv` (Debian / Ubuntu)
- `$WAVELINE_MOK_KEY` + `$WAVELINE_MOK_CERT` to point at your own

Waveline never creates a key or enrols one on your behalf — that is a firmware
password prompt on the next boot and it is your decision. Without a key it says
so and names the command for your distribution (`ujust enroll-secure-boot-key`
on Bazzite, `kmodgenca` + `mokutil --import` on Fedora atomic), then leaves the
stock driver in place.

### After a kernel update

```bash
sudo waveline-kmod status     # what is built, what is loaded, Secure Boot state
sudo waveline-kmod rebuild    # rebuild for the running kernel
```

`rebuild` re-enters the source tree it recorded at install time and runs
`install.sh --kernel-only`, which rebuilds and reinstalls the module and touches
nothing else — no mixer rebuild, no DeepFilterNet refetch, no rewriting your
drop-ins.

---

## Commands

```bash
sudo ./install.sh                      # full install, atomic detected automatically
sudo ./install.sh --app-only           # rebuild the GUI only
sudo ./install.sh --kernel-only        # rebuild the kernel patch only
sudo ./uninstall.sh                    # remove everything

sudo waveline-kmod status              # module state
sudo waveline-kmod load | unload       # swap by hand
sudo waveline-kmod rebuild             # after a kernel update
sudo waveline-kmod remove              # module + boot unit only
```

| Environment variable | Effect |
|---|---|
| `WAVELINE_BUILD_IMAGE=…` | Base image for the build container |
| `WAVELINE_NO_CONTAINER=1` | Never containerise; use the host toolchain or fail |
| `WAVELINE_STEAMOS_UNLOCK=1` | SteamOS: unlock `/usr` and build natively |
| `WAVELINE_MOK_KEY` / `_CERT` | Sign the module with your own enrolled key |

---

## What is left on the machine

| Path | What | Removed by |
|---|---|---|
| `~/.local/bin`, `~/.local/lib/waveline`, `~/.config/…` | the mixer and its config | `uninstall.sh` |
| `~/.local/lib/waveline/runtime/` | libraries your image does not ship | `uninstall.sh` |
| `/var/lib/waveline/` | the module, the tool, its config | `uninstall.sh` |
| `/etc/systemd/system/waveline-kmod.service` | the boot unit | `uninstall.sh` |
| `/etc/udev/rules.d/…` | per-profile udev rules | `uninstall.sh` |
| `localhost/waveline-build:*` image | build cache, 1–2 GB | **you** — `uninstall.sh` prints the command |
| `~/.cache/waveline/` | cargo registry, scratch | **you** |

The build image and cache are left alone on purpose. They are a cache, they may
be slow to rebuild, and they live in your own image store where podman's tooling
is the right thing to manage them with.

Your deployment is untouched throughout: `rpm-ostree status` shows no layered
packages, and there is nothing to roll back.

---

## Deliberate non-goals

- **No `rpm-ostree` layering.** Layering is a reboot and a permanent change to
  someone's deployment. The installer will *print* the command when the host is
  genuinely missing a runtime it cannot work around (a Qt 6 runtime), but it
  never runs it.
- **No systemd sysext for the module.** A sysext would need a regenerated
  `modules.dep` shadowing the base image's, which is more moving parts than an
  `insmod` in a boot unit for exactly one leaf module, and fails in stranger
  ways.
- **No Flatpak.** `wavelined` owns a PipeWire graph, opens a usbfs control
  endpoint on devices that have one, and runs as a systemd user unit. A sandbox
  is the wrong shape for it.
- **No automatic rebuild after a kernel update.** The same reason
  `AUTOINSTALL="no"` is set in [`dkms/dkms.conf`](dkms/dkms.conf): the module is
  a snapshot of one kernel's `sound/usb` tree with patches whose anchors are
  checked against that kernel. Rebuilding it unattended against a newer one is
  not safe, and falling back to the stock driver is the correct outcome.

---

## Files

| File | Role |
|---|---|
| [`scripts/lib/atomic.sh`](scripts/lib/atomic.sh) | detection, build-environment selection, container invocation, runtime-library fixup |
| [`scripts/waveline-kmod`](scripts/waveline-kmod) | the DKMS-free module: build staging, signing, boot unit, load/unload/status |
| [`install.sh`](install.sh) | routes every build step through the chosen environment; `--kernel-only` |
| [`uninstall.sh`](uninstall.sh) | removes the `/var` module, the boot unit and the bundled libraries |
| [`scripts/prepare-src.sh`](scripts/prepare-src.sh) | a missing kernel build directory is now a warning, since staging the sources does not need one |

---

## Troubleshooting

**"no way to compile anything on this system"** — install `podman`. Every atomic
distribution here ships it by default; if it is genuinely absent, that is the
one thing worth layering.

**"this system has no usable runtime for libQt6…"** — your image has no Qt 6.
Layer it once (`rpm-ostree install qt6-qtbase qt6-qtsvg qt6-qtwebsockets`),
reboot, re-run. Kinoite and Bazzite already have it; Silverblue may not.

**"the build container is newer than this system"** — the base image resolved to
a newer release than the host. Pin it:
`sudo WAVELINE_BUILD_IMAGE=registry.fedoraproject.org/fedora:41 ./install.sh`.

**Module will not load, Secure Boot enabled** — `sudo waveline-kmod status`
shows whether a signing key was found. Enrol one, or turn Secure Boot off in
firmware. The microphone works either way; the Wave:3 lockup fix is what is
missing.

**`snd_usb_audio is in use — cannot swap it now`** — something has the
microphone open. `systemctl --user stop wavelined`, close any recording
application, then `sudo waveline-kmod load`.

**Build logs**

```
/tmp/waveline-buildenv.log      building the container image
/tmp/waveline-prepare.log       staging and patching kernel sources
/tmp/waveline-build.log         compiling the module
/tmp/waveline-mixer-build.log   compiling the mixer
/tmp/waveline-dfn-build.log     compiling DeepFilterNet
```

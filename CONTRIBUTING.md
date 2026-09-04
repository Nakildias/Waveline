<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Contributing to Waveline

Issues and pull requests: <https://github.com/Nakildias/Waveline>.

Everything here is `SPDX-License-Identifier: GPL-2.0-or-later`. Keep the header
on new files; contributions are accepted under that licence.

**Adding support for a new microphone, mixer or capture card?** That is a
device profile, not a code change, and it has its own guide:
**[`devices/README.md`](devices/README.md)**. Read it first — profiles are
discovered automatically, so new hardware normally needs no installer edits and
no C++ at all. [`README.md`](README.md#adding-a-new-device) has the short
version. The rest of this file is about the code.

---

## Getting a development build

The full `sudo ./install.sh` is for users. For working on the mixer, build it
directly:

```bash
./app/build.sh              # configure, build, install to ~/.local, restart wavelined
./app/build.sh --no-restart # build and install only
```

Needs `cmake`, a C++20 compiler, Qt 6.4+ (Core, Widgets, DBus, Svg, Network,
WebSockets), `libpipewire-0.3` and `librnnoise`. `sudo ./install.sh --app-only` does the same
thing through the installer, which is worth using at least once because it is
the path most users will actually take.

`WAVELINE_PREFIX` overrides the install prefix (default `~/.local`).

Optional pieces that the plain build simply goes without:

- **FluidSynth** — MIDI instrument sounds. `install.sh` stages it into
  `app/lib/`; see [`app/lib/README.md`](app/lib/README.md). Without it the MIDI
  channel reports itself unavailable.
- **DeepFilterNet** — the better noise suppression engine, `dlopen`ed from
  `~/.local/lib`. Without it the mixer uses RNNoise.

Neither is required to build, run, or work on anything else.

## Where things live

| Path | What it is |
|---|---|
| `app/src/engine/` | The audio engine: PipeWire filters, DSP, routing. No Qt, no UI. |
| `app/src/daemon/` | `wavelined` — the D-Bus service (`org.waveline.Mixer`), config store, and the Web Companion server under `daemon/web/`. |
| `app/src/ui/` | `waveline-mixer` — the Qt window. |
| `app/src/device/` | Device profile parsing, shared by both. |
| `app/src/tools/` | Small standalone binaries: `wavelined-cli`, `waveline-probe`, `waveline-graph`, `waveline-nc`. |
| `devices/` | Device profiles. The **only** place hardware-specific behaviour belongs. |
| `scripts/` | Installer helpers, the kernel source staging, and the test harnesses below. |
| `data/`, `systemd/` | PipeWire/WirePlumber/ALSA drop-ins and user units installed on every machine. |

A change that is specific to one piece of hardware and ends up outside
`devices/` is almost always in the wrong place.

## House style

Match the file you are editing. Two things are not negotiable:

- **The SPDX header** on every new file.
- **Comments say *why*.** This codebase leans hard on comments that record the
  measurement, the failure, or the reasoning that produced a line — see
  `devices/*/wireplumber/*.conf` or the file-descriptor limit block in
  `install.sh` for the register. A setting with no explanation is a setting
  nobody can safely change later. If you tuned a number, say what you measured.

Shell scripts are bash, tab-indented, `set -euo pipefail` where they can abort
safely. `install.sh` and `uninstall.sh` deliberately omit `-e`: they check
statuses explicitly so that one failed optional step does not abandon a
half-finished installation.

## Testing

There is no unit test suite. What exists are harnesses that answer specific
questions on real hardware, which is the only place most of this can be tested:

| Script | Question it answers |
|---|---|
| `scripts/antipop-test.sh` | Is the click gone when an application stream is wired and unwired? |
| `scripts/control-test.sh` | Does capture work with the stock in-tree driver? (isolates a kernel quirk) |
| `scripts/check-monitor-clocks.sh` | Which driver clocks each sink, and is any monitor loopback linked to itself? |
| `scripts/latency-sweep.sh` | What does a buffer setting actually cost, and what does it buy? |
| `scripts/latency-offset-test.sh` | How much delay is inside the device itself? |
| `scripts/capture-startup-diag.sh` | Capture good vs. robotic startup states for comparison. |

For a device profile, [`devices/VERIFY_BEFORE_POSTING.md`](devices/VERIFY_BEFORE_POSTING.md)
is required: 20 daemon startups, 20 hotplugs and 20 rebuilds per device, with a
human sign-off. "Config looks right" is not support.

`scripts/collect-report.sh` gathers the state of a machine into one file. It
redacts the device serial number; check the output before pasting it anywhere
public.

## Pull requests

- Say what hardware you tested on, and how. For anything touching audio
  routing, latency or a device profile, this is the substance of the review.
- Keep hardware-specific changes inside `devices/`.
- One concern per PR. A profile addition and an engine change are two PRs.
- `scripts/clean.sh` removes the generated trees (`src/`, `app/lib/libfluidsynth.so*`,
  `__pycache__`) before you check what you are about to commit; `--all` also
  drops the cached kernel tarball in `.build/`.

If you are unsure whether something belongs here at all, open an issue first and
describe the hardware or the symptom. That is cheaper than a rejected PR.

<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Third-party components

This project is GPL-2.0-or-later. Everything below is somebody else's work,
under its own licence, and this file records what it is and how it reaches you.
Verbatim licence texts are in [`LICENSES/`](LICENSES/).

Nothing here is legal advice. It is a factual inventory so that you, or anyone
packaging this, can comply without having to re-derive it.

| Component | Licence | GPL-compatible | How it reaches you |
|---|---|---|---|
| [RNNoise](https://github.com/xiph/rnnoise) | BSD-3-Clause | yes | linked at build time against the system `librnnoise`; **not** redistributed here |
| [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet) | MIT **or** Apache-2.0 (dual) | yes (taken under MIT) | `dlopen`ed at runtime from the system `libdf.so`, if installed; **not** redistributed here, and not a build dependency |
| [FluidSynth](https://www.fluidsynth.org/) | LGPL-2.1-or-later | yes (see below) | installed from your distribution by `install.sh`, copied into `app/lib/` for the build, and `dlopen`ed at runtime; **not** redistributed here |
| [dr_wav / dr_mp3](https://github.com/mackron/dr_libs) | Unlicense **or** MIT-0 (dual) | yes | **copied into this repository** under `app/lib/thirdparty/`, and compiled into `wavelined` |
| [Tabler Icons](https://github.com/tabler/tabler-icons) | MIT | yes | **copied into this repository** under `app/src/icons/`, and compiled into `waveline-mixer` |
| [PipeWire](https://pipewire.org/) | MIT | yes | linked at build time against the system `libpipewire-0.3` |
| [Qt 6](https://www.qt.io/) | LGPL-3.0-only (open source) | see below | linked at build time against the system Qt |

## RNNoise

The noise suppression is RNNoise, by Jean-Marc Valin and the Xiph.Org
Foundation. `app/src/engine/noisefilter.cpp` calls it directly rather than
going through a LADSPA plugin host.

Licence: BSD-3-Clause, the *modified* BSD licence, without the old
advertising clause. That clause was the reason 4-clause BSD was incompatible
with the GPL; without it, BSD-3-Clause is a permissive licence that adds no
restrictions the GPL does not already allow, so BSD-3 code may be combined with
GPL code and the combination conveyed under the GPL. The RNNoise portions keep
their own notice, which is reproduced in
[`LICENSES/RNNoise-BSD-3-Clause.txt`](LICENSES/RNNoise-BSD-3-Clause.txt).

Copyright holders, per that file: Jean-Marc Valin; Amazon; Mozilla; Xiph.Org
Foundation; Mark Borgerding.

**This repository does not contain or redistribute any RNNoise code.** The
build links against whatever `librnnoise` the distribution provides
(`pkg-config --exists rnnoise`), so BSD-3-Clause clause 2 (reproduce the
notice with binary redistributions) is not triggered by cloning this repo.
It *is* triggered for anyone shipping a built binary (a distro package, an
AppImage, a Flatpak), which is why the notice is kept here ready to be
included.

The trained model weights are downloaded by upstream's own `autogen.sh` from
`media.xiph.org` and are built into `librnnoise`. Upstream ships no separate
licence for them; they are treated as part of the project and covered by the
same `COPYING`. They are trained on the public datasets listed in upstream's
`datasets.txt`.

## DeepFilterNet

The optional second noise suppression engine is DeepFilterNet, by Hendrik
Schröter. `app/src/engine/denoiser.cpp` drives it through libDF's C API
(`df_create`, `df_process_frame`, …).

Licence: **dual-licensed, MIT OR Apache-2.0**, at the recipient's option.
Upstream's wording is "All code in this repository is dual-licensed under
either: MIT License or Apache License, Version 2.0". Both texts are kept here:
[`LICENSES/DeepFilterNet-MIT.txt`](LICENSES/DeepFilterNet-MIT.txt) and
[`LICENSES/DeepFilterNet-Apache-2.0.txt`](LICENSES/DeepFilterNet-Apache-2.0.txt).

Copyright holder, per those files: Hendrik Schröter (2021).

### Which half of the dual licence this project takes, and why it matters

**This project takes DeepFilterNet under the MIT option.** That is not a
formality; it is the whole reason the combination is clean:

* **MIT** is a short permissive licence with no patent clause and no
  termination clause. It is compatible with GPLv2 *and* GPLv3, so it can be
  combined with this program however this program is conveyed.
* **Apache-2.0** is *not* compatible with GPL-2.0-**only**. Its patent
  retaliation and indemnification terms count as "further restrictions" under
  GPLv2 §6, which is precisely what that section forbids. The FSF says so
  explicitly. It *is* compatible with GPLv3, which added the terms needed to
  absorb it.

Because upstream offers both, the incompatible half can simply be declined.
Anyone redistributing this alongside DeepFilterNet should say they are taking
it under MIT and ship the MIT notice; there is no obligation to reproduce the
Apache text, and it is kept here only because upstream ships it.

Note that this project is GPL-2.0-**or-later** for the Qt 6 reason described
below, so even the Apache-2.0 option would work by way of GPLv3. Taking MIT
avoids having to rely on that.

### Why it is `dlopen`ed rather than linked

Three practical reasons, one of which is also a licensing convenience:

1. It ships no pkg-config file, and where it is packaged at all it is packaged
   under a different name on each distribution.
2. Making it a build dependency would mean nobody could build this project
   without first installing a Rust toolchain.
3. Nothing here links against it, contains it, or ships it. The engine is
   listed in the mixer as unavailable when the library is absent, and the
   mixer falls back to RNNoise.

`install.sh` *builds* it from upstream sources by default and drops the result
in the user's own `~/.local/lib`, but that is the installer acting on the
user's machine at their request. This repository still conveys none of it, and
`WAVE3_SKIP_DFN=1` turns that step off.

Cloning this repository therefore triggers no notice obligation for
DeepFilterNet at all. It is triggered for anyone shipping a bundle that
*includes* `libdf.so` (an AppImage or a Flatpak), which is why the notice is
kept here ready to be included.

### Model weights

The trained weights are the `DeepFilterNet*_onnx.tar.gz` archives in upstream's
`models/` directory. Upstream publishes **no separate licence for them**; they
are part of that repository and are covered by the same MIT-or-Apache-2.0
choice as the code.

This repository does not contain, download or redistribute them. The user
installs them, and `denoiser.cpp` looks for one in
`~/.local/share/waveline/models/`, the usual system paths, or wherever
`WAVE3_DFN_MODEL` points.

### What libdf brings with it

libDF runs the model with [tract](https://github.com/sonos/tract) (Sonos), not
ONNX Runtime. tract is itself dual MIT/Apache-2.0, as is the rest of the Rust
crate graph it pulls in. Since all of it is inside a shared library the user
installed through their package manager, none of it is this project's to
convey, but it is worth knowing that there is no GPL-incompatible component
hiding in there.

## FluidSynth

The MIDI channel's instrument sounds come from FluidSynth, a SoundFont
synthesiser. `app/src/engine/fluidsynthbackend.cpp` drives it through its C API
(`new_fluid_synth`, `fluid_synth_sfload`, `fluid_synth_write_float`, …).

Licence: **LGPL-2.1-or-later**, kept in
[`LICENSES/FluidSynth-LGPL-2.1-or-later.txt`](LICENSES/FluidSynth-LGPL-2.1-or-later.txt).

**This repository does not contain or redistribute FluidSynth**, in source or
binary form. It does not even carry FluidSynth's headers: the backend declares
the dozen function pointer types it needs itself
(`using fluid_synth_new_fn = fluid_synth_t *(*)(fluid_settings_t *);` and so on)
and resolves them with `dlsym`, so nothing of upstream's is copied in.

### How the library reaches a build

`install.sh` installs your distribution's runtime package — `fluidsynth` on
Arch, `fluidsynth-libs` on Fedora, `libfluidsynth3` on Debian and Ubuntu — and
copies the resulting `libfluidsynth.so.3` into `app/lib/`. That copy is
`.gitignore`d and is refreshed on every install run, so it is never anything but
a duplicate of a file the distribution already installed and licensed to the
user. `WAVELINE_SKIP_FLUIDSYNTH=1` skips the whole step.

The copy exists because `app/CMakeLists.txt` links `wavelined` against it
directly when it is present (`WAVELINE_FLUIDSYNTH_LINKED`). Without it the
backend still works: it `dlopen`s `libfluidsynth.so.3`, `.so.2`, `.so.1` or
`.so` from the ordinary linker paths, and if none is found the MIDI instrument
channel reports itself unavailable and the rest of the mixer is unaffected.

### Why LGPL-2.1-or-later is not a problem here

LGPLv2.1 section 3 lets a recipient relicense the library under the GPL of
version 2 **or later**, so an LGPL-2.1-or-later library can be combined with
GPL-2.0-or-later code with no conflict — unlike the Qt 6 situation described
below, which is what forces the "or-later" on this project in the first place.

Section 6, the obligation to let the user relink against a modified FluidSynth,
is satisfied trivially: the library is dynamically linked and, in the normal
case, is the distribution's own shared object, replaceable by the user at any
time. `$WAVELINE_FLUIDSYNTH_LIB` points the daemon at a different build without
recompiling anything.

Anyone redistributing a **built** Waveline together with a FluidSynth binary —
a distro package, a Flatpak, an AppImage — does take on section 6 properly, and
must ship the licence text and keep the linking replaceable. That is why the
text is kept here.

### SoundFonts

FluidSynth makes no sound without a SoundFont, and this project ships none. The
`.sf2` files are chosen by the user through a file picker in the Effects window
and are their own property under their own licences. Nothing is downloaded and
nothing is bundled.

## Tabler Icons

The interface icons under `app/src/icons/` are from Tabler Icons, © 2020-2026
Paweł Kuna, MIT.

Unlike everything else on this page these files **are** redistributed: they are
committed to this repository and compiled into the `waveline-mixer` binary through
the Qt resource system. MIT requires its copyright and permission notice to
accompany all copies and substantial portions, so
[`LICENSES/TablerIcons-MIT.txt`](LICENSES/TablerIcons-MIT.txt) must travel with
any copy of this repository or of a binary built from it.

MIT is GPL-compatible, so shipping them inside a GPL binary is fine as long as
the notice is preserved.

## dr_wav and dr_mp3

The Soundboard decodes `.wav` and `.mp3` files with David Reid's single-header
`dr_wav.h` (v0.14.6) and `dr_mp3.h` (v0.7.4), vendored under
`app/lib/thirdparty/`. They are used so that playing a sound file needs no
system audio-decoding library at all — `app/src/engine/soundboardengine.cpp` is
the only translation unit that defines `DR_WAV_IMPLEMENTATION` and
`DR_MP3_IMPLEMENTATION`, so the implementations are compiled exactly once and
every other includer sees declarations only.

Licence: **dual-licensed, the Unlicense (public domain dedication) OR MIT-0**,
at the recipient's option. © 2023 David Reid. Both texts are at the end of each
header, and the MIT-0 text is reproduced in
[`LICENSES/dr_libs-Unlicense-MIT0.txt`](LICENSES/dr_libs-Unlicense-MIT0.txt).

Like the Tabler icons, these files **are** redistributed: they are committed
here and compiled into `wavelined`. Unlike the icons, neither option carries an
attribution requirement — MIT-0 is MIT with the attribution clause removed, and
the Unlicense asks for nothing at all. So no notice has to travel with a binary.
The licence file is kept anyway, because "we took this from upstream and here is
what upstream said about it" is worth recording whether or not a licence demands
it.

Both options are GPL-compatible: a public domain dedication adds no
restrictions, and MIT-0 is strictly more permissive than MIT.

## Qt 6: why this project is GPL-2.0-**or-later**

Qt 6 is offered under LGPL-3.0-only or GPL-3.0-only (or commercially). Qt 6
dropped the LGPL-2.1 option that Qt 5 had.

GPL-2.0-**only** and LGPL-3.0 cannot be combined: LGPLv3 permits a recipient to
convey the work under GPLv3, and GPLv2-only forbids adding the terms GPLv3
brings. A GPLv2-only application linking Qt 6 is therefore not distributable.

The Qt Company GPL Exception 1.0 does not help here. It covers the *output* of
Qt's tools and the combination of Qt applications with plugins, not the licence
of a program that links Qt's libraries.

The remedy is the ordinary one: this project is **GPL-2.0-or-later**. GPLv2
remains one of the licences it is offered under, exactly as before, and because
GPLv3 is also available the combination with LGPLv3 Qt is permitted.

Only `wavelined` and `waveline-mixer` link Qt. The `waveline-hw` CLI (Python) and the DKMS
kernel module do not.

## The DKMS kernel module

`scripts/prepare-src.sh` patches the Linux kernel's own `snd-usb-audio`
sources. The resulting module is a derivative of the kernel and is therefore
conveyed as **GPL-2.0-only**, whatever this repository's scripts are licensed
as. The kernel is GPLv2-only and its licence governs the module.

This is not in tension with the paragraph above: the module is a separate work
from the Qt programs, built separately, loaded by the kernel, and never linked
against Qt.

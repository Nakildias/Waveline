<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Bundled libraries

This directory is where `libfluidsynth.so.3` goes, for MIDI instrument sounds.

**You do not have to put it there yourself.** `install.sh` installs your
distribution's FluidSynth package and copies its library into this directory
before building, on every run. The file is deliberately not in git: FluidSynth
is LGPL-2.1-or-later and is built against one distribution's ABI, so a binary
committed here would be both a licensing problem and useless to most people.

Skip the whole step with `WAVELINE_SKIP_FLUIDSYNTH=1`; the mixer then loses MIDI
instrument playback and nothing else.

To place it by hand anyway — for a build that does not go through `install.sh`:

```bash
cp /usr/lib/libfluidsynth.so.3 app/lib/
```

The next `install.sh` run overwrites that copy with the system one.

## How the daemon finds it

`wavelined` `dlopen`s FluidSynth at run time, searching in order:

1. `$WAVELINE_FLUIDSYNTH_LIB` if set
2. Next to the `wavelined` binary
3. This directory (when installed as `../lib/` relative to `bin/`)
4. Standard dynamic linker paths (`libfluidsynth.so.3`, `.so.2`, `.so.1`, `.so`)

When this directory holds `libfluidsynth.so.3` at CMake configure time,
`wavelined` is additionally linked against it directly
(`WAVELINE_FLUIDSYNTH_LINKED`), which is the tested arrangement.

MIDI input devices also require PipeWire to expose MIDI nodes (e.g. from
`pipewire-pulse` / ALSA sequencer bridges).

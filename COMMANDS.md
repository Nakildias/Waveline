<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# wavelined-cli

A command-line front end for `wavelined`, built for Stream Decks, keybinds
and scripts rather than a terminal session. Every action is **one flag plus
positional arguments** — there are no subcommands to chain and no state held
between invocations, so one button press is one command line.

It talks to the same `org.waveline.Mixer` D-Bus interface the GUI and the
Web Companion use. `wavelined` must be running (`systemctl --user status
wavelined`); if it isn't, every command fails with a clear message instead
of a D-Bus stack trace.

```
wavelined-cli --help
```

Built and installed alongside `wavelined` and `waveline-mixer` by
`app/build.sh` and `install.sh`, landing in `~/.local/bin/wavelined-cli`.

## Targets

Commands that act on an input device or a channel take one **target**
token:

| Form | Meaning |
|---|---|
| `input:1`, `input2`, `input_1` | the input device at that 1-based position, in the order `--list` shows |
| `input:mic`, `input:master-1` | an input device by its literal id |
| `channel:voice`, `channel_voice` | a channel by id |
| `channel:Voice` | a channel matched by display name too (case-insensitive) |

The separator (`:`, `_`, or nothing before a digit) is cosmetic — pick
whichever reads best in a Stream Deck button's command field. Run
`wavelined-cli --list` to see the exact ids, names and current values on
your machine; input device ids and channel ids are stable across restarts,
but the 1-based `input:N` position depends on the order devices were added.

## General

```
wavelined-cli --list
```
Lists every input device, channel and monitor output with its id/position
and current levels — the reference for building the rest of your commands.

## Mute and volume

```
wavelined-cli --toggle-mute <target> [monitor|stream]
wavelined-cli --set-volume  <target> <0-100> [monitor|stream]
wavelined-cli --offset-volume <target> <+N|-N> [monitor|stream]
```

- **`channel:<id>`** always needs a mix (`monitor` or `stream`) — a channel
  has separate levels in each.
- **`input:<id>`** *without* a mix acts on the device's own hardware
  gain/gate — the same control your system volume applet would show for that
  microphone (`SetMasterMicInputVolume` / `SetMasterMicInputMuted`).
- **`input:<id>` with a mix** acts on that input's level *within* the given
  mix (`SetMasterMicVolume` / `SetMasterMicMixMuted`) — the fader you'd move
  in the mixer's Monitor or Stream column for that device.

Examples:

```
wavelined-cli --toggle-mute input:1                    # mute the mic hardware (the gate)
wavelined-cli --toggle-mute input:1 monitor             # mute input 1 in the Monitor mix only
wavelined-cli --toggle-mute channel:voice stream        # mute the Voice channel in the Stream mix
wavelined-cli --set-volume input:1 50                   # input 1's hardware gain to 50%
wavelined-cli --set-volume channel:game 80 monitor      # Game channel, Monitor mix, to 80%
wavelined-cli --offset-volume channel:music -5 stream   # Music channel's Stream level, down 5
```

## A channel's own published microphone

Every channel can publish its own microphone recording device (the
"Publish as recording device" option in the Microphone tab). These commands
control *that* device's gain and monitor, not the channel's app-audio level:

```
wavelined-cli --toggle-channel-mic <channel>
wavelined-cli --set-channel-mic <channel> <0-100>
wavelined-cli --offset-channel-mic <channel> <+N|-N>
wavelined-cli --toggle-channel-mic-monitor <channel>
```

## Input device extras

```
wavelined-cli --toggle-monitor <input>     # "hear yourself" -- software self-monitor
wavelined-cli --toggle-rack <input>        # Virtual Rack on/off for this input's mic chain
```

## Effects

Every input device and every channel has an effects chain, and a channel's
chain has two independent **sides**: `microphone` (its input/mic stage) and
`app-audio` (its output stage, what apps play into it). Input devices only
have a microphone chain — there is no app-audio side to toggle on a hardware
input.

```
wavelined-cli --toggle-effects microphone|app-audio <target> [effect]
```

- **No `effect` given**: toggles the whole chain's bypass switch.
  - On a channel, this one switch covers **both** the microphone and
    app-audio sides together — the daemon has no separate whole-chain bypass
    per side, only per individual module (see below).
- **`effect` given**: toggles just that module, leaving the rest of the
  chain's settings untouched.

Valid effect names:

| Name | Where it applies | What it toggles |
|---|---|---|
| `noise-suppression` | microphone or app-audio | RNNoise/DeepFilterNet |
| `de-esser` | microphone or app-audio | de-esser |
| `gate` | microphone or app-audio | noise gate |
| `compressor` | microphone or app-audio | compressor |
| `limiter` | microphone or app-audio | limiter |
| `eq` | microphone or app-audio | the 3-band/Pro EQ |
| `lowcut` | microphone or app-audio | low-cut filter |
| `ducking` | app-audio only | sidechain ducking |
| `lufs-limiter` | app-audio only | LUFS loudness limiter |
| `bitcrusher`, `overdrive`, `chorus`, `flanger`, `phaser`, `tremolo`, `delay`, `reverb`, `creative-eq`, `ringmod`, `envelope-filter`, `pitch-shifter`, `reverse-delay`, `tape-saturator` | microphone or app-audio | one Creative FX pedalboard module |

`creative-eq` is the pedalboard's amp-style tone stack; it is a different
control from `eq` above (the ordinary 3-band/Pro EQ).

Examples:

```
wavelined-cli --toggle-effects microphone input:1                       # bypass input 1's whole mic chain
wavelined-cli --toggle-effects microphone input:1 noise-gate            # (invalid name -- see table: it's "gate")
wavelined-cli --toggle-effects microphone input:1 gate                  # toggle just the gate
wavelined-cli --toggle-effects app-audio channel:game ducking           # toggle ducking on Game's app audio
wavelined-cli --toggle-effects app-audio channel:voice reverb           # toggle Creative FX reverb on Voice's app audio
```

### Effects inside an active Rack

The Virtual Rack (`--toggle-rack`) has its own, separate Creative FX chain
from the microphone's ordinary Creative tab. Toggle a module inside it with:

```
wavelined-cli --toggle-rack-effect <input> <effect>
```

using the same effect names as the Creative FX row in the table above. This
only exists on input devices — channels don't have a Rack.

## Soundboard

```
wavelined-cli --soundboard-list
wavelined-cli --soundboard-play <id>
wavelined-cli --soundboard-stop <id>
wavelined-cli --soundboard-stop-all
```

Every soundboard sound has a stable 4-digit id (shown in the Soundboard
panel next to its name, and in `--soundboard-list`) -- that id is what a
Stream Deck button or keybind plays:

```
wavelined-cli --soundboard-play 4821
```

Sounds can overlap: playing one already playing starts a second instance
rather than restarting it. `--soundboard-stop` stops every instance of that
one sound; `--soundboard-stop-all` stops everything at once.

Which channel the soundboard plays on, whether it joins a microphone, and at
what volumes are board-wide settings (one set of controls at the top of the
Soundboard panel) rather than per sound -- set them there, not on the
command line. The id is the only thing a button needs.

## Global output mixes

The Monitor mix can feed more than one physical output (headphones, an
external mixer, etc.), so its per-device commands take an index; the Stream
mix is a single virtual device and does not.

```
wavelined-cli --toggle-output-monitor-mix <n>
wavelined-cli --set-output-monitor-mix <n> <0-100>
wavelined-cli --offset-output-monitor-mix <n> <+N|-N>
```
`<n>` is the 1-based position from `--list`'s "Monitor outputs" section.

```
wavelined-cli --toggle-output-monitor-master     # mute everything the Monitor mix sends, at once
wavelined-cli --set-output-monitor-master <0-100>
wavelined-cli --offset-output-monitor-master <+N|-N>
```
This is the master level/mute on top of every individual output above —
separate from muting one output device.

```
wavelined-cli --toggle-output-stream-mix
wavelined-cli --set-output-stream-mix <0-100>
wavelined-cli --offset-output-stream-mix <+N|-N>
```

## Exit codes

- `0` — success
- `1` — usage error (bad flag, unknown target/effect, out-of-range index)
- `2` — D-Bus error (wavelined not running, or the call itself failed)

## Using it with a Stream Deck or a keybind

Point a Stream Deck "System: Open" / "Run Command" action (or a `sxhkd` /
GNOME custom keybind) straight at the binary with one full command line per
button, e.g.:

```
/home/you/.local/bin/wavelined-cli --toggle-mute input:1
/home/you/.local/bin/wavelined-cli --toggle-effects app-audio channel:voice reverb
/home/you/.local/bin/wavelined-cli --offset-output-stream-mix +5
```

Because `--offset-*` reads the current value first, the same button can be
bound to a rotary "+5 / -5" pair without the deck needing to track state
itself. Run any command once from a terminal first to confirm the exact
target string, then paste that same line into the deck.

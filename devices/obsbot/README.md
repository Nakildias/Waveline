<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# OBSBOT profiles

```
obsbot/
  microphones/
    meet4k/                # Meet 4K
      device.conf
      udev/                # capture preamp default
      wireplumber/         # quantum lock
  capture-cards/
    <id>/
  mixers/
    <id>/
```

## Meet 4K

The microphone powers on with its preamp at 27 dB of an available 54 dB, and
nothing in a normal desktop stack can raise it: PipeWire's ACP never maps the
`OBSBOT Meet 4K Microphone Capture Volume` element into the capture route, so
the source reports "100% / 0.00 dB" while the raw value stays where the
firmware left it. That is the whole reason the microphone sounds quiet.

`udev/95-waveline-meet4k.rules` sets the element on every plug. See the rule
for why it is numbered 95 and how to change the value it writes.

Create a category folder only when you add a device into it. Each device is
`obsbot/<category>/<id>/device.conf`.

See [../README.md](../README.md) for the full profile format.

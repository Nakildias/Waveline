<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Hercules profiles

```
hercules/
  microphones/
    rocksmith-tone-cable/  # Rocksmith Tone Cable (USB guitar adapter)
      device.conf
      wireplumber/…
  capture-cards/
    <id>/
  mixers/
    <id>/
```

Create a category folder only when you add a device into it. Each device is
`hercules/<category>/<id>/device.conf`.

See [../README.md](../README.md) for the full profile format.

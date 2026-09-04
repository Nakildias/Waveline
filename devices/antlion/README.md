<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Antlion profiles

```
antlion/
  microphones/
    modmicwireless/        # ModMic Wireless -- shown in the UI as "ModMic"
      device.conf
      wireplumber/
  capture-cards/
    <id>/
  mixers/
    <id>/
```

Create a category folder only when you add a device into it. Each device is
`antlion/<category>/<id>/device.conf`.

See [../README.md](../README.md) for the full profile format.

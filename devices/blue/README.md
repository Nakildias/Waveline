<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Blue profiles

No devices here yet. When you add one:

```
blue/
  microphones/
    <id>/
      device.conf
  capture-cards/
    <id>/
  mixers/
    <id>/
```

Create only the category folders you need. Each device must live at
`blue/<category>/<id>/device.conf` (never directly under `blue/`).

See [../README.md](../README.md) for the full profile format.

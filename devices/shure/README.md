<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Shure profiles

No devices here yet. When you add one:

```
shure/
  microphones/
    <id>/
      device.conf
  capture-cards/
    <id>/
  mixers/
    <id>/
```

Create only the category folders you need. Each device must live at
`shure/<category>/<id>/device.conf` (never directly under `shure/`).

See [../README.md](../README.md) for the full profile format.

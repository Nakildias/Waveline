<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# HyperX profiles

No devices here yet. When you add one:

```
hyperx/
  microphones/
    <id>/
      device.conf
  capture-cards/
    <id>/
  mixers/
    <id>/
```

Create only the category folders you need. Each device must live at
`hyperx/<category>/<id>/device.conf` (never directly under `hyperx/`).

See [../README.md](../README.md) for the full profile format.

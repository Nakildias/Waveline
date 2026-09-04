<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Fifine profiles

No devices here yet. When you add one:

```
fifine/
  microphones/
    <id>/
      device.conf
  capture-cards/
    <id>/
  mixers/
    <id>/
```

Create only the category folders you need. Each device must live at
`fifine/<category>/<id>/device.conf` (never directly under `fifine/`).

See [../README.md](../README.md) for the full profile format.

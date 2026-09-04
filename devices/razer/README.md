<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Razer profiles

No devices here yet. When you add one:

```
razer/
  microphones/
    <id>/
      device.conf
  capture-cards/
    <id>/
  mixers/
    <id>/
```

Create only the category folders you need. Each device must live at
`razer/<category>/<id>/device.conf` (never directly under `razer/`).

See [../README.md](../README.md) for the full profile format.

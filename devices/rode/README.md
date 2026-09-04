<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Røde profiles

No devices here yet. When you add one:

```
rode/
  microphones/
    <id>/
      device.conf
  capture-cards/
    <id>/
  mixers/
    <id>/
```

Create only the category folders you need. Each device must live at
`rode/<category>/<id>/device.conf` (never directly under `rode/`).

See [../README.md](../README.md) for the full profile format.

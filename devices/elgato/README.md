<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Elgato profiles

Put each product under a category, then a device id directory that contains
`device.conf` (and any kernel / WirePlumber / udev bits for that product):

```
elgato/
  microphones/
    wave3/                 # Elgato Wave:3
      device.conf
  capture-cards/
    4k60mk2/               # 4K60 Pro MK.2
    4kpro/                 # 4K Pro
  mixers/
    <id>/                  # create when adding a mixer profile
      device.conf
```

Do not put `device.conf` directly under `elgato/`. Always use
`microphones/`, `capture-cards/`, or `mixers/`.

See [../README.md](../README.md) for the full profile format.

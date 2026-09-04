<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Logitech profiles

```
logitech/
  microphones/
    c922/                  # C922 Pro Stream Webcam mic
      device.conf
  capture-cards/
    <id>/
  mixers/
    <id>/
```

Create a category folder only when you add a device into it. Each device is
`logitech/<category>/<id>/device.conf`.

See [../README.md](../README.md) for the full profile format.

<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Misc profiles

One-off or uncommon hardware that does not warrant a dedicated vendor tree.

```
misc/
  microphones/
    <id>/
  capture-cards/
    <id>/
  mixers/
    <id>/
```

Create a category folder only when you add a device into it. Each device is
`misc/<category>/<id>/device.conf`.

See [../README.md](../README.md) for the full profile format.

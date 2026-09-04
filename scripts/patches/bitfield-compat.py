#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
"""Add field_get/field_prep shims for kernels before 6.17.

Upstream snd-usb-audio uses field_get() and field_prep() for the RME Digiface
mixer controls, but those helpers only exist in linux/bitfield.h from 6.17
onward.  Kernels such as 6.18 LTS still ship the older API (FIELD_GET and
FIELD_PREP, compile-time mask only).

Usage: bitfield-compat.py <path-to-sound/usb-copy>
"""

import sys
from pathlib import Path

ANCHOR = "#include <linux/bitfield.h>"
MARKER = "#ifndef field_get"
SHIM = """
#ifndef field_get
#define field_get(mask, reg)						\\
	({								\\
		typeof(mask) __mask = (mask);				\\
		typeof(reg) __reg = (reg);				\\
		unsigned int __shift = __bf_shf(__mask);		\\
		(__reg & __mask) >> __shift;				\\
	})
#endif

#ifndef field_prep
#define field_prep(mask, val)						\\
	({								\\
		typeof(mask) __mask = (mask);				\\
		typeof(val) __val = (val);				\\
		(__val << __bf_shf(__mask)) & __mask;			\\
	})
#endif
"""


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = Path(sys.argv[1]).resolve() / "mixer_quirks.c"
    text = path.read_text()
    if MARKER in text:
        print("  mixer_quirks.c: bitfield compat already applied")
        return
    if ANCHOR not in text:
        sys.exit(f"waveline/bitfield-compat: {path.name}: missing {ANCHOR}")
    path.write_text(text.replace(ANCHOR, ANCHOR + SHIM, 1))
    print("  mixer_quirks.c: added field_get/field_prep compat shims")


if __name__ == "__main__":
    main()

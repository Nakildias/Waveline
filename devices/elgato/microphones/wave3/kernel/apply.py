#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
"""Apply the Elgato Wave:3 UAC1 shared-clock quirk to a copy of sound/usb/.

Four files, no quirks-table.h entry: the descriptor parser already produces
correct audioformats for this device, so the only thing missing is the
knowledge that its two endpoints share one clock domain.

Usage: apply-wave3.py <path-to-sound/usb-copy>
"""

import re
import sys
from pathlib import Path

FLAG = "QUIRK_FLAG_UAC1_SHARED_CLOCK"


def edit(path, old, new, what):
    s = path.read_text()
    n = s.count(old)
    if n != 1:
        sys.exit(f"waveline/wave3: {path.name}: anchor for {what} matched {n} times")
    path.write_text(s.replace(old, new))
    print(f"  {path.name}: {what}")


def edit_re(path, pattern, repl, what):
    s = path.read_text()
    new, n = re.subn(pattern, repl, s)
    if n != 1:
        sys.exit(f"waveline/wave3: {path.name}: anchor for {what} matched {n} times")
    path.write_text(new)
    print(f"  {path.name}: {what}")


def next_quirk_bit(header):
    """The first unused QUIRK_TYPE_* bit in this kernel's usbaudio.h.

    Not a constant, because upstream keeps appending to that enum: 7.2 added
    QUIRK_TYPE_MIXER_GET_CUR_BROKEN, which took bit 30 and broke a patch that
    had hard-coded it. Reading the enum means a new neighbour costs nothing.
    """
    bits = [int(b) for b in
            re.findall(r"^\tQUIRK_TYPE_[A-Z0-9_]+\t+= (\d+),$",
                       header.read_text(), re.M)]
    if not bits:
        sys.exit("waveline/wave3: usbaudio.h: no QUIRK_TYPE_* enum found")
    bit = max(bits) + 1
    if bit > 31:
        sys.exit("waveline/wave3: usbaudio.h: quirk_flags is a u32 and every "
                 f"bit is taken (highest is {max(bits)}); this patch needs an "
                 "upstream widening before it can be applied")
    return bit


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = Path(sys.argv[1]).resolve()
    if FLAG in (src / "usbaudio.h").read_text():
        print("  already patched")
        return

    # ---- 1. usbaudio.h: flag bit, accessor macro, synthetic clock ID, docs.
    #
    # Appended after whatever the last entry happens to be, rather than pinned
    # behind a named neighbour. The enum is a list upstream adds to, and the
    # bit number and the name-table index below have to agree -- appending to
    # the end of both is the only arrangement that stays true across kernels.
    bit = next_quirk_bit(src / "usbaudio.h")
    edit(src / "usbaudio.h",
         "/* Please also edit snd_usb_audio_quirk_flag_names */",
         f"	QUIRK_TYPE_UAC1_SHARED_CLOCK		= {bit},\n"
         "/* Please also edit snd_usb_audio_quirk_flag_names */",
         "QUIRK_TYPE_UAC1_SHARED_CLOCK enum entry")

    edit(src / "usbaudio.h",
         "#define QUIRK_FLAG_IFB_SILENCE_ON_EMPTY		QUIRK_FLAG(IFB_SILENCE_ON_EMPTY)",
         "#define QUIRK_FLAG_IFB_SILENCE_ON_EMPTY		QUIRK_FLAG(IFB_SILENCE_ON_EMPTY)\n"
         "#define QUIRK_FLAG_UAC1_SHARED_CLOCK		QUIRK_FLAG(UAC1_SHARED_CLOCK)\n"
         "\n"
         "/*\n"
         " * Synthetic clock ID given to the UAC1 audioformats of a device carrying\n"
         " * QUIRK_FLAG_UAC1_SHARED_CLOCK.  UAC1 has no clock entities at all, so any\n"
         " * non-zero value works; it only has to be identical across the endpoints\n"
         " * that share the clock, and non-zero, because\n"
         " * snd_usb_endpoint_get_clock_rate() treats clock 0 as \"no clock\".\n"
         " */\n"
         "#define UAC1_SHARED_CLOCK_ID			0xff",
         "QUIRK_FLAG_UAC1_SHARED_CLOCK macro + UAC1_SHARED_CLOCK_ID")

    edit(src / "usbaudio.h",
         " * QUIRK_FLAG_IFB_SILENCE_ON_EMPTY",
         " * QUIRK_FLAG_UAC1_SHARED_CLOCK\n"
         " *  Treat all UAC1 audioformats of the device as driven by a single clock\n"
         " *  domain, so the rate of a second stream is arbitrated against the one\n"
         " *  already running instead of being programmed independently.\n"
         " * QUIRK_FLAG_IFB_SILENCE_ON_EMPTY",
         "flag documentation")

    # ---- 2. stream.c: tag UAC1 formats with the synthetic clock.
    edit(src / "stream.c",
         "		if (iterm) {\n"
         "			num_channels = iterm->bNrChannels;\n"
         "			chconfig = le16_to_cpu(iterm->wChannelConfig);\n"
         "		}\n"
         "	} else { /* UAC_VERSION_2 */",
         "		if (iterm) {\n"
         "			num_channels = iterm->bNrChannels;\n"
         "			chconfig = le16_to_cpu(iterm->wChannelConfig);\n"
         "		}\n"
         "\n"
         "		/*\n"
         "		 * UAC1 has no Clock Source unit, so snd_usb_clock_find_source()\n"
         "		 * cannot tell which endpoints share a clock domain and\n"
         "		 * snd_usb_endpoint_open() never instantiates a clock_ref for\n"
         "		 * them.  update_clock_ref_rate() and hw_rule_rate() are both\n"
         "		 * inert as a result, and nothing stops one substream being\n"
         "		 * driven to a rate the other, already streaming, endpoint\n"
         "		 * cannot follow.  Devices that really do run capture and\n"
         "		 * playback from one clock opt into the arbitration here.\n"
         "		 */\n"
         "		if (chip->quirk_flags & QUIRK_FLAG_UAC1_SHARED_CLOCK)\n"
         "			clock = UAC1_SHARED_CLOCK_ID;\n"
         "	} else { /* UAC_VERSION_2 */",
         "assign UAC1_SHARED_CLOCK_ID to UAC1 formats")

    # ---- 3. endpoint.c: let those formats get a clock_ref.
    edit(src / "endpoint.c",
         "		if (fp->protocol != UAC_VERSION_1) {\n"
         "			ep->clock_ref = clock_ref_find(chip, fp->clock);",
         "		/*\n"
         "		 * UAC1 formats normally carry no clock and must not share a\n"
         "		 * ref; those tagged by QUIRK_FLAG_UAC1_SHARED_CLOCK do, and\n"
         "		 * join the same arbitration as UAC2/3.  Parsed UAC1 formats\n"
         "		 * leave fp->clock at 0, so this is inert otherwise.\n"
         "		 */\n"
         "		if (fp->protocol != UAC_VERSION_1 || fp->clock) {\n"
         "			ep->clock_ref = clock_ref_find(chip, fp->clock);",
         "create clock_ref for tagged UAC1 formats")

    # ---- 4. quirks.c: name-table entry (index must match the enum) + device.
    #
    # Last entry, matching the enum bit taken above: this table is indexed by
    # QUIRK_TYPE_*, so the two lists have to be appended to in step.
    edit_re(src / "quirks.c",
            r"(\tQUIRK_STRING_ENTRY\([A-Z0-9_]+\),\n)(\tNULL\n)",
            r"\1\tQUIRK_STRING_ENTRY(UAC1_SHARED_CLOCK),\n\2",
            "quirk flag name-table entry")

    edit(src / "quirks.c",
         "	{} /* terminator */",
         "	/*\n"
         "	 * Elgato Wave:3.  A UAC1 device running a 2ch headphone output\n"
         "	 * (iface 1, EP 0x01) and a 1ch microphone (iface 2, EP 0x82) from\n"
         "	 * one clock, while exposing an independent sampling-frequency\n"
         "	 * control on each endpoint.  Without the arbitration the firmware\n"
         "	 * is asked to re-lock the clock underneath a live endpoint; it\n"
         "	 * then stops answering control transfers and the following\n"
         "	 * usb_set_interface() times out with -ETIMEDOUT while chip->mutex\n"
         "	 * is held, wedging the whole card.  The delays and the skipped\n"
         "	 * rate readback cover the same firmware's general slowness.\n"
         "	 */\n"
         "	DEVICE_FLG(0x0fd9, 0x0070, /* Elgato Wave:3 */\n"
         "		   QUIRK_FLAG_UAC1_SHARED_CLOCK | QUIRK_FLAG_GET_SAMPLE_RATE |\n"
         "		   QUIRK_FLAG_CTL_MSG_DELAY_1M | QUIRK_FLAG_IFACE_DELAY |\n"
         "		   QUIRK_FLAG_DISABLE_AUTOSUSPEND),\n"
         "	{} /* terminator */",
         "Elgato Wave:3 device entry")


if __name__ == "__main__":
    main()

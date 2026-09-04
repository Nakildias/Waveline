// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Headless check of the device layer, with no Qt linked. If the GUI will not
// start, this narrows the problem to USB versus Qt in one step.

#include "device/wave3device.h"

#include <cstdio>
#include <string>

int main(int argc, char **argv) {
    const bool doWrite = (argc > 1 && std::string(argv[1]) == "--write-test");

    waveline::Device dev;
    if (auto r = dev.open(); !r) {
        std::fprintf(stderr, "open failed: %s\n", r.message.c_str());
        return 1;
    }
    std::printf("node        %s\n", dev.nodePath().c_str());

    waveline::ClaimGuard guard(dev);
    if (!guard) {
        std::fprintf(stderr, "claim failed: %s\n", guard.result().message.c_str());
        return 1;
    }

    waveline::DeviceInfo info;
    if (auto r = dev.readInfo(info); r)
        std::printf("firmware    %s (api %s)\n", info.firmwareVersion.c_str(),
                    info.apiVersion.c_str());

    waveline::State st;
    if (auto r = dev.readState(st); !r) {
        std::fprintf(stderr, "readState failed: %s\n", r.message.c_str());
        return 1;
    }

    std::printf("raw        ");
    for (auto b : st.raw) std::printf(" %02x", b);
    std::printf("\n");
    std::printf("mic         %s, gain %+.1f dB\n", st.micMuted ? "MUTED" : "live",
                st.micGainDb);
    std::printf("headphone   %s, %+.1f dB\n", st.hpMuted ? "MUTED" : "on",
                st.hpVolumeDb);
    std::printf("clipguard   %s\n", st.clipguard ? "on" : "off");
    std::printf("monitor     %d%% (byte %d)\n", st.monitorPercent, st.monitorMix);
    std::printf("dial        mode %d, value %d\n", static_cast<int>(st.dialMode),
                st.dialValue);
    std::printf("telemetry   %u  (wValue=0x0001; not a level meter, see docs)\n",
                st.telemetry);

    // Pure-function checks, so the default run stays strictly read-only.
    std::printf("\nscale:\n");
    for (int pct : {0, 25, 50, 75, 100, 150}) {
        const int raw = waveline::Device::percentToMix(pct);
        std::printf("  %3d%% -> byte %2d -> %3d%%%s\n", pct, raw,
                    waveline::Device::mixToPercent(raw),
                    pct > 100 ? "   (clamped)" : "");
    }

    if (!doWrite) {
        std::printf("\nread-only run; pass --write-test to exercise a write\n");
        return 0;
    }

    // Writes, each restoring what it found.
    std::printf("\nwrite test:\n");
    const bool origGuard = st.clipguard;
    if (auto r = dev.setClipguard(!origGuard); !r)
        std::printf("  clipguard toggle failed: %s\n", r.message.c_str());
    waveline::State after;
    dev.readState(after);
    std::printf("  clipguard %s -> %s\n", origGuard ? "on" : "off",
                after.clipguard ? "on" : "off");
    dev.setClipguard(origGuard);
    dev.readState(after);
    std::printf("  restored  %s%s\n", after.clipguard ? "on" : "off",
                after.clipguard == origGuard ? "" : "   !! MISMATCH");

    return 0;
}

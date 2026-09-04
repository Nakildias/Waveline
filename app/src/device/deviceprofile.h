// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Which microphone Waveline was installed for.
//
// Waveline is a mixer for any microphone. A few things about it are not
// device-independent -- the name on the window, the name on the published
// mixes, and whether there is a vendor USB protocol to talk to at all -- and
// all of them are answered here rather than by scattering `if wave3` through
// the daemon and the GUI.
//
// The file is written by install.sh from the profile it detected, so the choice
// is made once, on the machine that has the hardware, and everything afterwards
// just reads it. It is deliberately the same key=value format as
// devices/<brand>/<category>/<id>/device.conf: one format, and a user can read or fix it by hand.
//
// Absent file means the generic profile, which is also what a build straight
// out of the tree gets. That is a working mixer, not a degraded one -- it is
// simply not branded for any particular microphone.

#pragma once

#include <string>

namespace waveline {

struct DeviceProfile {
    // Matches the device directory name (e.g. wave3 under devices/<brand>/<category>/).
    std::string id = "generic";
    std::string label = "Generic microphone";

    // Goes in front of every name this daemon publishes: input card,
    // "<brand> Stream Mix". "Wave:3" with a Wave:3 installed, otherwise
    // "Waveline". Never empty -- an unbranded "Stream Mix" is not findable in
    // anyone's device list.
    std::string brand = "Waveline";

    // False unless this microphone has controls ALSA cannot reach and we have
    // a transport for them. Gates the mixer's whole hardware panel and the
    // hardware monitor mode, and stops the daemon opening a USB device it has
    // no business opening.
    bool hardwareControls = false;

    // The ALSA node name prefix the microphone appears as, when the profile
    // pins one. Empty means "whatever PipeWire calls the default source",
    // which is the only sensible answer for an unknown device.
    std::string alsaNodeMatch;

    // ~/.config/waveline/profile.conf
    static std::string configPath();

    // Never fails: an unreadable or missing file yields the generic defaults.
    static DeviceProfile load();

    // "Stream Mix" -> "Wave:3 Stream Mix". The one place the brand is joined
    // to anything, so the separator cannot drift between call sites.
    std::string name(const std::string &suffix) const {
        return suffix.empty() ? brand : brand + " " + suffix;
    }
};

}  // namespace waveline

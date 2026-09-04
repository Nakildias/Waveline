// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Writes ~/.config/wireplumber/wireplumber.conf.d/99-waveline-output-headroom.conf,
// which gives non-USB output devices some ALSA headroom.
//
// ---------------------------------------------------------------- what it fixes
//
// One node in a driver group owns the clock; every other device in that group
// resamples onto it and has to land its writes at a position that drifts
// against the graph cycle. api.alsa.headroom is the slack that absorbs the
// drift. PipeWire ships 0 for PCI devices, which is fine until the machine is
// busy -- and then a late cycle is an immediate underrun on the follower.
//
// Measured 2026-08-09: with three Monitor outputs live, only the two follower
// sinks crackled under a Discord screen share, at every quantum from 2.7 ms to
// 21 ms. The driver device never did, because it does not resample and defines
// the deadline rather than chasing it. 1024 frames of headroom on the two
// followers ended it.
//
// This is a workaround for a scheduling property of the machine, not a fix for
// anything Waveline does -- hence its own file, and hence a control that says
// what it costs.
//
// ------------------------------------------------------ why non-USB, not "all"
//
// The rule matches node names starting alsa_output.pci- rather than every
// alsa_output, and that restriction is load-bearing: a WirePlumber rule
// matching the Wave:3's *output* node makes it reach the hardware before the
// capture node does, after which its ADC produces digital silence for the rest
// of the session. See the note at the top of
// data/wireplumber/50-waveline-driver-policy.conf. A PCI prefix excludes every
// USB device by construction, so no future device name can slip through.
//
// device.bus = "pci" would read better and is deliberately not used: WirePlumber
// takes it from the *device* properties (scripts/monitors/alsa.lua), and whether
// it has been inherited onto a node by the time node rules are matched is not
// something to bet a silent no-op on. The node name is the key the rule that was
// measured to work used.
//
// The cost of that choice: a USB DAC used as a secondary monitor output does
// not get headroom from here. That is the safe direction to be wrong in.
#pragma once

#include <QString>

namespace waveline {

// frames is the headroom in samples at the graph rate; 0 removes the file
// entirely rather than writing a rule that sets zero, so uninstalling the
// setting leaves WirePlumber exactly as it found it.
//
// Writes only when the content would change: WirePlumber watches this
// directory, and rewriting an identical file is a needless reconfigure.
bool writeOutputHeadroom(int frames, QString *error = nullptr);

// Nothing here takes effect until WirePlumber re-opens the devices, which it
// does on restart. Deliberately separate from the write so the caller can ask
// first -- a restart drops every stream on the machine for a moment.
bool restartWirePlumber(QString *error = nullptr);

}  // namespace waveline

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Detect whether an audio stream belongs to a Steam game session. Games rarely
// show up as "steam" in application.name -- Terraria is "Terraria.bin.x86_64",
// Proton titles are often "wine" or the .exe -- so name rules alone miss them.

#pragma once

#include <cstdint>
#include <string>

namespace waveline {

// True when pid (and optionally application.process.binary from PipeWire)
// belongs to a game launched through Steam. Covers native Linux, Proton/Wine,
// and Flatpak Steam (com.valvesoftware.Steam).
bool isSteamGameProcess(uint32_t pid, const std::string &binary = {});

// Zero when the process is not under a Steam reaper launch.
uint32_t steamAppIdForProcess(uint32_t pid);

}  // namespace waveline

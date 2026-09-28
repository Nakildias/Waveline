// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The installed application a process belongs to, from its .desktop entry.
//
// What a stream calls itself is often the toolkit rather than the program.
// Everything built on Chromium's embedded framework says "Chromium" -- a music
// player, a browser, a chat client alike -- because the audio comes from a
// Chromium helper process that has no idea what it is embedded in. The helper
// is still the program's own executable, though (/usr/bin/music
// --type=utility ...), and the program's launcher entry says Exec=music,
// Name=Music. Matching the one against the other is what names the stream the
// way the menu and the dock already do.
//
// Qt-free, like the rest of the engine: a small parser over the XDG
// applications directories, indexed once and re-read when one of them
// changes.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace waveline {

struct DesktopEntry {
    std::string id;                       // file name without .desktop
    std::string name;                     // Name, localised when LANG says how
    std::string icon;                     // Icon: a theme name or a path
    std::vector<std::string> categories;  // Categories
};

// The entry for the program running as `pid`, found by its executable -- the
// path first, then the file name, then StartupWMClass and the desktop id --
// or for `processBinary` (application.process.binary) when the process has
// already gone. Nothing for interpreters and runtimes (python, java, wine,
// electron...): an entry that launches one is no evidence about any other
// process running it.
std::optional<DesktopEntry> desktopEntryForProcess(uint32_t pid,
                                                   const std::string &processBinary);

// A channel id to route an application to from its entry's Categories, or
// empty when they say nothing useful. Consulted only after the name rules.
std::string channelForCategories(const std::vector<std::string> &categories);

}  // namespace waveline

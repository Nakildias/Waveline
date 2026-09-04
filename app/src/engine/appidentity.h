// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Stable application identity and human-readable names for audio streams.
// PipeWire often reports generic names ("java", "SDL Application") while the
// process binary or cmdline identifies the real program.

#pragma once

#include <string>
#include <vector>

namespace waveline {

struct PwNode;

// True for useless PipeWire labels like "java" or "SDL Application".
bool isGenericAppLabel(const std::string &name);

// True when a key is safe to persist (steam:/minecraft:/non-generic bin:…).
bool isStableIdentityKey(const std::string &key);

// Plausible identity keys, most stable first. Used for manual pin lookup.
std::vector<std::string> appIdentityKeyCandidates(const PwNode &node);

// Stable key for persisting manual channel assignments across restarts.
std::string appIdentityKey(const PwNode &node);

// Name to show in the Apps tab.
std::string appDisplayName(const PwNode &node);

// Key for merging multiple playback streams from one program (Discord spawns
// several WEBRTC helpers). Stable identity when available, else display name.
std::string appMergeKey(const PwNode &node);

}  // namespace waveline

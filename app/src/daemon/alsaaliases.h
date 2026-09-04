// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
#pragma once

#include <QString>

#include "engine/mixergraph.h"

namespace waveline {

// Rewrite ~/.config/alsa/waveline.conf from the live graph and ensure
// ~/.config/alsa/asoundrc includes it.
bool syncAlsaAliases(const MixerGraph &graph, const std::string &brand,
                     QString *error = nullptr);

}  // namespace waveline

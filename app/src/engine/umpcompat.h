// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#pragma once

// UMP (MIDI 2.0 Universal MIDI Packets) on a PipeWire that may not have it.
//
// PipeWire gained SPA_CONTROL_UMP and spa/control/ump-utils.h in 1.4. Before
// that a MIDI port carries SPA_CONTROL_Midi and nothing else, so the UMP branch
// is not merely unsupported, it does not compile: the header is absent and both
// the enumerator and spa_ump_to_midi() are undeclared.
//
// This matters for real targets and not just old ones -- SteamOS 3.7 ships
// PipeWire 1.2.7 -- and there is nothing to fall back to, because a server
// without UMP support never sends a UMP packet in the first place. So the
// branch is compiled out and the SPA_CONTROL_Midi path, which such a server
// does use, carries all the MIDI on its own.

#if defined(__has_include)
#  if __has_include(<spa/control/ump-utils.h>)
#    define WAVELINE_HAVE_SPA_UMP 1
#  else
#    define WAVELINE_HAVE_SPA_UMP 0
#  endif
#else
#  define WAVELINE_HAVE_SPA_UMP 0
#endif

#if WAVELINE_HAVE_SPA_UMP
#  include <spa/control/ump-utils.h>
#endif

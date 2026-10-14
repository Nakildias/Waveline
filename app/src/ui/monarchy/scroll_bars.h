// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Monarchy's Appearance -> Show scroll bars, on every scroll area in the mixer.
//
// Monarchy's own applications get it per view, through SmoothScroller. The
// mixer has scroll areas in a dozen windows built long before any of that, so
// rather than touch each one, this watches the application: the first time a
// scroll area is polished, both its bars are handed to ScrollBarAutoHide. One
// that already has one -- a view SmoothScroller drives -- keeps it, and an axis
// the view switched off stays off.
//
// Does nothing off Monarchy, where the bars are the mixer's own as before.

#pragma once

namespace Monarchy {

// Call once, after the QApplication exists and Monarchy::isActive() is known.
void followScrollBarSetting();

}  // namespace Monarchy

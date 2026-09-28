// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Monarchy's menus, for every menu and dropdown in the mixer when it runs
// there.
//
// Ported from Monarchy's src/common/shellstyle/shell_style_menu.cc and
// src/common/dialogkit/shell_menus.h (same author, same licence). There, an
// application's menus are ordinary QMenus drawn by a proxy style as one of the
// shell's own surfaces -- the translucent, blurred card with the light along
// its rim, rows with a pill-shaped hover, the bounce it opens with -- and its
// dropdowns open that same menu centred under the control instead of Qt's list
// popup, with the chosen value in the accent. The numbers are the shell menu's;
// keep them in step with the original.
//
// Only on Monarchy. The universal look keeps Qt's own menus and dropdowns.

#pragma once

class QComboBox;

namespace Monarchy {

// Makes every menu the application puts up -- its own right-click menus and
// Qt's (a text field's Cut/Copy/Paste) -- and every combo box's dropdown the
// shell's. Once, from main(), after Theme::apply(). Does nothing outside
// Monarchy.
void useShellMenus();

// Opens `combo`'s items as a shell menu dropped under it, the way Monarchy's
// dropdowns open; choosing one sets it and emits activated() as the list
// would. What a press on a combo box does once useShellMenus() has run --
// exposed for a combo that opens its popup from code.
void showComboMenu(QComboBox *combo);

}  // namespace Monarchy

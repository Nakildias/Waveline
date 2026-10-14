// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Ported unchanged from Monarchy's src/common/scroll (same author, same
// licence) rather than linked, for the reason chrome.h gives: that is a static
// library Monarchy's applications compile in from that tree, and the mixer is
// built on machines that have never seen it. Keep it in step with the
// original -- the point is that the rack scrolls like every other Monarchy
// view, and reads the same ~/.config tuning.

#ifndef COMMON_SCROLL_SETTINGS_H_
#define COMMON_SCROLL_SETTINGS_H_

#include <QString>

#include <functional>

namespace desktop {


// When a scroll bar is on screen. One choice for the whole desktop, so a view
// never has to decide for itself -- see ScrollBarAutoHide, which is what turns
// this into a visible bar.
//
//   Automatic     -- shown for a wheel mouse, hidden until you scroll for a
//                    trackpad. Which one is attached is not a question the
//                    system can answer, but the scroll frames themselves can
//                    (eventIsTrackpad), so the engine reports what it last saw
//                    and this follows it.
//   WhenScrolling -- hidden until you scroll, whatever is attached.
//   Always        -- always on screen. What every view did before this setting.
enum class ScrollBarVisibility { Automatic, WhenScrolling, Always };

// The config file spellings: "automatic", "when-scrolling", "always".
QString scrollBarVisibilityToString(ScrollBarVisibility visibility);
ScrollBarVisibility scrollBarVisibilityFromString(const QString& text,
                                                  ScrollBarVisibility fallback);


// How scrolling feels, everywhere in Monarchy. One set of values, read from the
// global config, shared by every component that scrolls -- there is deliberately
// no per-component override, so tuning the feel once tunes it everywhere.
//
// Defaults here are the values the notification centre was tuned to on hardware;
// they are what a machine with no config file gets.
struct ScrollSettings {
  bool smooth = true;
  bool trackpad = true;
  ScrollBarVisibility scrollbars = ScrollBarVisibility::WhenScrolling;
  double trackpad_speed = 1.0;
  bool trackpad_momentum = true;
  double overscroll_spring_k = 0.05;
  double overscroll_spring_damp = 0.60;
  double overscroll_edge_pull = 0.40;

  // Clamps every field to the range the engine is stable over.
  void clamp();
};

// Path of the global config the settings are read from and written to:
// $XDG_CONFIG_HOME/monarchy/interface.jsonc. Not config.jsonc beside it,
// which is the desktop's appearance and power settings.
QString scrollSettingsPath();

// The live global settings. Cheap to call -- the file is parsed once and then
// re-read only when it changes on disk, so a widget can call this from inside a
// wheel event without touching the filesystem.
const ScrollSettings& globalScrollSettings();

// Forces the next globalScrollSettings() call to re-read the file. The file
// watcher calls this itself; a component only needs it after writing settings
// from its own process.
void invalidateGlobalScrollSettings();

// Writes the settings into the global config, preserving every other key in the
// file. Returns false and leaves the file untouched if it cannot be written.
bool saveGlobalScrollSettings(const ScrollSettings& settings);

// Runs `callback` whenever the global settings change -- the file changed on
// disk, or this process saved it. Returns a token for removeScrollSettingsListener();
// a view that comes and goes must hand it back, since the list outlives it.
//
// This is how a change made in Settings reaches a window that is sitting still:
// nothing else would ask, because the settings are only re-read lazily.
int addScrollSettingsListener(std::function<void()> callback);
void removeScrollSettingsListener(int token);

// Reads the settings straight from disk, ignoring the cache. For the settings UI,
// which wants the file's contents rather than a possibly stale snapshot.
ScrollSettings loadScrollSettingsFromDisk();

}  // namespace desktop

#endif  // COMMON_SCROLL_SETTINGS_H_

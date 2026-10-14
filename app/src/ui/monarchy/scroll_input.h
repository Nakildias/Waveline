// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Ported unchanged from Monarchy's src/common/scroll (same author, same
// licence) rather than linked, for the reason chrome.h gives: that is a static
// library Monarchy's applications compile in from that tree, and the mixer is
// built on machines that have never seen it. Keep it in step with the
// original -- the point is that the rack scrolls like every other Monarchy
// view, and reads the same ~/.config tuning.

#ifndef COMMON_SCROLL_INPUT_H_
#define COMMON_SCROLL_INPUT_H_

#include <QPoint>

class QWheelEvent;

namespace desktop {


// Which way the content moves. Horizontal takes a sideways frame when there is
// one and falls back to the vertical figure, so a plain wheel over a horizontal
// strip still scrolls it -- which is what a horizontal row wants from a mouse.
enum class ScrollAxis { Vertical, Horizontal };

// Whether an axis may read the other one's figure when its own is empty.
//
// Allowed is the long-standing behaviour: a horizontal view takes a sideways
// frame when there is one and falls back to the vertical figure, so a plain
// wheel scrolls a horizontal strip -- which is what a row of things wants from
// a mouse. Strict reads only the axis asked for, which is how a view says it
// wants a real sideways swipe and nothing else.
enum class AxisFallback { Allowed, Strict };

// Reading a scroll frame. Split out from ScrollEngine so a component that still
// has its own scrolling can at least agree with everything else about what a
// frame means and which way it goes -- these are the rules, in one place.

// A finger scroll arrives twice over: as whole pixels, and as a wheel angle at
// twelve units to the pixel. Measured, not assumed -- over 1338 frames of real
// two-finger scrolling the angle summed to 12.19x the pixels, and the per-frame
// ratio's mode was exactly 12.
inline constexpr double kTrackpadAngleUnitsPerPx = 12.0;

// True when the frame came from a continuous source rather than a notched wheel.
//
// Do NOT ask the device what it is. Qt on Wayland names the seat pointer
// "touchpad" and types it TouchPad no matter what is actually attached -- a
// desktop with only a wheel mouse plugged in reports exactly the same device as
// a laptop trackpad. Trusting it turns every mouse into a trackpad.
//
// The scroll frames themselves do tell them apart, cleanly. A trackpad gesture
// is phased: begin, updates, end, on every single frame. A wheel notch carries
// no phase at all, only a flat +/-120 of angle. Across two machines that split
// was total -- 1338 trackpad frames all phased, 468 wheel frames none.
bool eventIsTrackpad(const QWheelEvent* event);

// What last drove a scroll in this process.
//
// Unknown until something scrolls, which is the state a freshly opened window
// starts in -- treat it as a mouse, since that is the arrangement that wants a
// bar on screen and being wrong for one flick is harmless.
enum class ScrollSource { Unknown, Wheel, Trackpad };

// The last source seen anywhere in this process. ScrollEngine records it on
// every frame it looks at, so a view that has never been scrolled still knows
// what the one next to it was scrolled with.
ScrollSource lastScrollSource();

// Records a frame's source. ScrollEngine calls this; a component with its own
// wheel handling should too, so the whole process agrees.
void noteScrollSource(ScrollSource source);

// Travel for one trackpad frame, in pixels, sign-flipped into scroll-offset
// space. Angle first: at trackpad speeds a frame is well under a pixel of
// travel, so a quarter of the frames report zero pixels while still reporting
// eight to twelve units of angle, and scrolling by the pixel figure throws that
// travel on the floor. Pixels are only the fallback for a stack that reports no
// angle at all.
//
// Direction is deliberately left alone: a trackpad's is a system-level
// preference that libinput has already applied to this delta, so obeying it is
// what makes Monarchy scroll the same way as every other app on the machine.
double trackpadDeltaPx(const QWheelEvent* event, ScrollAxis axis = ScrollAxis::Vertical,
                       AxisFallback fallback = AxisFallback::Allowed);

// There is no direction policy for the wheel either, and there deliberately is
// not one: a wheel delta arrives with the device's own natural-scroll setting
// already applied by libinput, exactly like a trackpad's, and the sign it
// arrives with is the one Settings -> Mouse & Trackpad asked for.
//
// This used to normalise that away -- inverted() says libinput flipped the
// frame, so flipping it back gave the raw physical direction -- and then
// re-decide from a natural-scroll key of the desktop's own. The two cancelled:
// the switch in Settings changed nothing whatsoever, and the direction was
// whatever that key said, which defaulted to natural. The device decides now,
// and the key is gone.

// The component of a wheel frame that drives `axis`, in Qt's own sign
// convention. Returns 0 when the frame carries nothing on that axis.
QPoint scrollDeltaForAxis(const QPoint& delta, ScrollAxis axis,
                          AxisFallback fallback = AxisFallback::Allowed);

}  // namespace desktop

#endif  // COMMON_SCROLL_INPUT_H_

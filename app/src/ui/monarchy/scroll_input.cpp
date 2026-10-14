// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Ported unchanged from Monarchy's src/common/scroll (same author, same
// licence) rather than linked, for the reason chrome.h gives: that is a static
// library Monarchy's applications compile in from that tree, and the mixer is
// built on machines that have never seen it. Keep it in step with the
// original -- the point is that the rack scrolls like every other Monarchy
// view, and reads the same ~/.config tuning.

#include "scroll_input.h"

#include <QWheelEvent>

namespace desktop {


bool eventIsTrackpad(const QWheelEvent* event) {
  if (event->phase() != Qt::NoScrollPhase) {
    return true;
  }
  // Backstop for a stack that reports no phases: pixel deltas only ever come
  // from a continuous source. A notched wheel reports (0,0) here, always, so
  // this cannot misfire on a mouse.
  return !event->pixelDelta().isNull();
}

namespace {
ScrollSource g_last_source = ScrollSource::Unknown;
}  // namespace

ScrollSource lastScrollSource() { return g_last_source; }

void noteScrollSource(ScrollSource source) {
  if (source != ScrollSource::Unknown) {
    g_last_source = source;
  }
}

int axisComponent(const QPoint& delta, ScrollAxis axis, AxisFallback fallback) {
  if (axis == ScrollAxis::Horizontal) {
    if (delta.x() != 0 || fallback == AxisFallback::Strict) {
      return delta.x();
    }
    return delta.y();
  }
  return delta.y();
}

QPoint scrollDeltaForAxis(const QPoint& delta, ScrollAxis axis, AxisFallback fallback) {
  return QPoint(0, axisComponent(delta, axis, fallback));
}

double trackpadDeltaPx(const QWheelEvent* event, ScrollAxis axis, AxisFallback fallback) {
  // Qt sign convention: a positive delta means scroll up/left, and the scroll
  // offset grows the other way, so the sign flips on the way in.
  const int angle = axisComponent(event->angleDelta(), axis, fallback);
  if (angle != 0) {
    return -angle / kTrackpadAngleUnitsPerPx;
  }
  const int pixels = axisComponent(event->pixelDelta(), axis, fallback);
  if (pixels == 0) {
    return 0.0;
  }
  return -static_cast<double>(pixels);
}

}  // namespace desktop

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Ported from Monarchy's src/apps/Settings/src/smooth_scroller (same author,
// same licence), for the reason scroll_engine.h gives. The one difference: it
// lives in namespace Monarchy with the rest of this folder.

#pragma once

#include <QObject>

#include "scroll_engine.h"

class QAbstractScrollArea;
class QScrollBar;

namespace Monarchy {

// Puts Monarchy's shared scrolling behaviour on a scroll area.
//
// desktop::ScrollEngine owns the physics — 1:1 trackpad tracking, wheel
// momentum, and the tuning from the global config — and never touches a
// widget. This adapter is the widget half: it takes the wheel events off a
// QAbstractScrollArea's viewport, feeds them to the engine, and writes the
// engine's position back onto the vertical scroll bar, so Settings scrolls
// with the same feel as the notification centre and the search panel.
//
// The engine's rubber band lets the position travel outside [0, max] at the
// ends, and a scroll bar's value cannot. So the bar takes the clamped part and
// the overscroll is drawn by sliding the viewport itself past its home in the
// area — the content stretches away from the edge and springs back, the same
// as the search panel, without the view underneath knowing anything happened.
class SmoothScroller : public QObject {
  Q_OBJECT

 public:
  // Attaches to `area`, taking ownership of itself via the area's parentage.
  // `step_px` is one wheel notch of travel, in pixels.
  //
  // `orientation` picks which of the area's scroll bars is driven. A
  // horizontal area is driven by an ordinary top-to-bottom wheel too — the
  // engine's own horizontal axis does that — so a mouse can scroll a sideways
  // strip without the user knowing it is sideways.
  // Returns the scroller so the caller can set a policy on it; it belongs to
  // `area` either way. Null only when `area` is.
  static SmoothScroller* attach(QAbstractScrollArea* area, int step_px,
                                Qt::Orientation orientation = Qt::Vertical);

  // Take a sideways trackpad swipe and nothing else — see
  // ScrollEngine::setSidewaysGestureOnly. A wheel notch is then left to
  // whatever is behind the area, and a mouse uses the scroll bar.
  void setSidewaysGestureOnly(bool enabled) { engine_.setSidewaysGestureOnly(enabled); }

  // The engine driving the area.
  desktop::ScrollEngine* engine() { return &engine_; }

  // Where the scrolling is heading, in the bar's units: mid-glide the place it
  // has not reached yet, otherwise simply where it is.
  //
  // What it is for: a view that pins itself to the end of a growing range has
  // to ask this rather than read the bar. A glide that has just started has
  // not moved the bar off the end yet, so the bar still says "at the end" — and
  // a view that believes it writes the bar back to the end, which pulls the
  // spring away from its target and the wheel does nothing at all.
  int destination() const;

  // The range grew at the end by `grew_by` while the view was sitting at the
  // end: stay there. A glide in flight is carried rather than stopped — the
  // coordinates move under it and it lands on the new end.
  void followEnd(int grew_by);

  // The coordinates moved under the scrolling: content was dropped off the
  // front, so the same content is now `delta_px` further up (a negative delta).
  // Position and destination move together, so a glide carries on untouched.
  void shiftOrigin(int delta_px);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  SmoothScroller(QAbstractScrollArea* area, int step_px, Qt::Orientation orientation);

  QScrollBar* bar() const;
  int maxOffset() const;
  // The engine's position onto the bar, without the bar bouncing it back into
  // the engine as a fresh jump.
  void writePosition();
  // Slides the viewport by however far the engine is past an end. Qt puts the
  // viewport back on any relayout; the next frame slides it out again.
  void applyOverscroll();

  QAbstractScrollArea* area_ = nullptr;
  desktop::ScrollEngine engine_;
  int step_px_;
  Qt::Orientation orientation_ = Qt::Vertical;
  // Set while the engine is writing to the scroll bar, so the scroll bar's own
  // valueChanged does not bounce straight back into the engine.
  bool syncing_ = false;
  // How far the viewport currently sits from where the area laid it out.
  int overscroll_applied_ = 0;
  // Set while this adapter moves the viewport, so its own Move is not taken
  // for Qt re-laying it out.
  bool moving_viewport_ = false;
};

}  // namespace Monarchy

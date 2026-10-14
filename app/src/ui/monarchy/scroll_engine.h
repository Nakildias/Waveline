// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Ported unchanged from Monarchy's src/common/scroll (same author, same
// licence) rather than linked, for the reason chrome.h gives: that is a static
// library Monarchy's applications compile in from that tree, and the mixer is
// built on machines that have never seen it. Keep it in step with the
// original -- the point is that the rack scrolls like every other Monarchy
// view, and reads the same ~/.config tuning.

#ifndef COMMON_SCROLL_ENGINE_H_
#define COMMON_SCROLL_ENGINE_H_

#include <functional>

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "scroll_input.h"
#include "scroll_settings.h"

class QWheelEvent;

namespace desktop {


// Monarchy scrolling, as one reusable object.
//
// This is the notification centre's scroll behaviour lifted out whole: 1:1
// trackpad tracking with a measured flick hand-off, wheel notches that step a
// fixed distance and gain on a spin, and a rubber band at both ends. It reads globalScrollSettings(), so
// every component that uses it feels the same and retunes together.
//
// Wiring one up takes three things -- how tall a "row" is, how far the content
// can travel, and what to do when the position moves:
//
//   scroll_.setHooks({
//       [this] { return rowHeight(); },
//       [this] { return maxScrollOffset(); },
//       [this] { updateScrollPosition(); },
//   });
//
//   void MyView::wheelEvent(QWheelEvent* event) override {
//     if (scroll_.handleWheel(event)) {
//       event->accept();
//       return;
//     }
//     QWidget::wheelEvent(event);
//   }
//
// The engine owns position() and nothing else. It never touches a widget, so it
// works equally well for a QScrollArea, a hand-laid-out list, or layer-shell
// windows positioned by margin.
class ScrollEngine : public QObject {
  Q_OBJECT

 public:
  struct Hooks {
    // One row of travel: the wheel moves this far per notch, and it is the unit
    // the velocity and overscroll limits are expressed in. Must be > 0.
    std::function<double()> step_px;
    // How far the content can scroll, in pixels. 0 means nothing to scroll.
    std::function<double()> max_offset_px;
    // The position moved. Reposition the content and repaint here.
    std::function<void()> on_changed;
    // Optional. Return false to make the engine decline the event -- for a view
    // that is hidden or empty and wants the wheel to fall through to its parent.
    std::function<bool()> scrollable;
  };

  explicit ScrollEngine(QObject* parent = nullptr);
  ~ScrollEngine() override;

  void setHooks(Hooks hooks);
  // Which way the content moves. Vertical by default; a horizontal strip also
  // accepts a plain wheel, so a mouse can drive it too.
  void setAxis(ScrollAxis axis) { axis_ = axis; }

  // Take a sideways trackpad swipe, and nothing else.
  //
  // Off by default, which is the behaviour every view has had: a horizontal
  // view is driven by a plain wheel too, because a mouse has no sideways
  // gesture to give it. A view that turns this on declines both — a wheel
  // notch, and a swipe travelling up and down — so the wheel keeps scrolling
  // whatever is behind the view instead of dragging it sideways, and a mouse
  // is left with the scroll bar. Only worth setting on a horizontal view.
  void setSidewaysGestureOnly(bool enabled) { sideways_gesture_only_ = enabled; }

  // Whether the content may be pulled past either end and spring back. On by
  // default, which is what every view has had. A view turns it off when going
  // past an end would cost it something: the notification centre maps a card's
  // surface only while the card fits the viewport whole, and a rubber band at
  // the top pushes the first card out and snaps it back, which there is a card
  // blinking away and returning rather than a stretch. Momentum and 1:1
  // tracking are untouched -- only the give at the ends goes.
  void setRubberBand(bool enabled) { rubber_band_ = enabled; }

  // The one call a widget needs. Returns true when the event was consumed.
  bool handleWheel(QWheelEvent* event);

  // Run the animation off someone else's frame clock instead of the engine's
  // own timer.
  //
  // The timer is a free-running QTimer on a fixed interval. That is fine for a
  // view Qt is painting anyway, because Qt's own repaint lands on the next
  // vertical blank either way and a frame or two of jitter never reaches the
  // screen. It is not fine for a view that hands each frame to something else
  // that draws it -- a browser page scrolled through Chromium's compositor --
  // where the engine's cadence *is* the cadence the motion arrives at. A 125Hz
  // timer against a 240Hz surface repeats roughly every other frame, and
  // because the timer is aligned to nothing the repeats come in an uneven beat
  // that reads as judder rather than as a lower frame rate.
  //
  // So such a view turns this on and pumps the engine itself: connect
  // frameRequested() to whatever asks for a frame, and call advance() once per
  // frame the display actually shows until it returns false.
  void setExternalClock(bool enabled);
  // One frame from the driver's clock. Returns true while the engine still has
  // motion to run. Does nothing without setExternalClock(true).
  bool advance();
  // The display's frame, in milliseconds, for an engine on an external clock.
  // Set, advance() moves by whole frames of it rather than by the time since
  // the last call: that time is when the driver got round to calling, which
  // wanders by a millisecond or two either side of a frame, while the motion
  // is shown one frame apart exactly -- so stepping by it drew the jitter of
  // the event loop into the scroll. Zero, the default, steps by elapsed time.
  void setFramePeriodMs(double ms) { frame_period_ms_ = ms; }

  // How far one wheel notch travels before the rate boost earns it more,
  // overriding the shared row-derived figure. Zero or less restores it.
  //
  // The shared one is a nudge, deliberately: it is tied to the height of a row
  // because the views this engine was written for are lists, and the smallest
  // useful thing a wheel can do to a list is move it a fraction of a row. A
  // document has no rows, and the same click on a web page has to be worth
  // what a click is worth in every other browser or the page feels stuck.
  void setWheelNotchPx(double px) { wheel_notch_px_ = px; }

  // The position moved, or a frame was taken -- either way this view is being
  // scrolled right now. A scroll bar that hides itself listens for this rather
  // than watching its own value, so a frame that lands against the end stop
  // still counts as scrolling. Fires on every momentum tick too, so the coast
  // keeps the bar alive.
 signals:
  void scrollActivity();

  // External clock only: there is motion to run, and nothing is running it.
  // The driver answers by scheduling a frame, and keeps scheduling them for as
  // long as advance() keeps returning true.
  void frameRequested();

 public:
  double position() const { return pos_; }
  // Where the position is heading, which mid-glide is somewhere it has not
  // reached yet and otherwise is simply where it is.
  //
  // What it is for: a notch that bought nothing. glideBy() clamps the target
  // into the range, so a notch pushing past an end leaves the destination
  // exactly where it was — and a caller that took a reading either side of
  // handleWheel() can see that, without having to know which way the wheel
  // turns or which end it is against. Compass uses it to notice that the page
  // has nowhere left to go and hand the wheel to whatever else is under the
  // pointer, the way a browser is supposed to.
  double destination() const { return wheel_gliding_ ? wheel_target_ : pos_; }
  // Sets the position directly, clamped to the scrollable range. Stops any
  // gesture or coast in flight, so a programmatic jump is not fought.
  void setPosition(double position);
  // Clamps the position back into range after the content resized. A live
  // trackpad gesture owns the position rubber band and all, so this leaves it
  // alone while the fingers are down. Does not run the changed hook: every
  // caller is already mid-relayout and about to repaint anyway.
  void clampToRange();
  // Sets the position without clamping, stopping anything, or running the
  // changed hook. For teardown and rebuild paths, where the content the hook
  // would touch is gone or about to be replaced.
  void resetPosition(double position) { pos_ = position; }
  // Move the whole model — where it is and where it is heading — by the same
  // distance, so a glide in flight carries on untouched while the coordinates
  // it runs in shift underneath it. resetPosition() moves only the position,
  // which mid-glide is the spring being pulled away from its target.
  //
  // For a view whose range is not the thing being scrolled: Compass drives a
  // pane inside a page through a free range, and when it discovers mid-flick
  // that the page is not what is moving it has to re-centre that range without
  // the flick stopping. Nothing with a real range has any use for this.
  void shiftOrigin(double delta) {
    pos_ += delta;
    wheel_target_ += delta;
  }

  // Drops any momentum, spring-back or gesture. Call when the view closes or
  // the content is torn down.
  void stop();

  bool animating() const { return external_clock_ ? running_ : anim_.isActive(); }
  bool gestureActive() const { return trackpad_active_; }
  // True while the position is outside [0, max] -- for a scroll bar that squishes
  // against the edge during rubber band.
  bool overscrolled() const;

 private:
  bool handleTrackpad(QWheelEvent* event, double max_offset);
  bool handleWheelNotches(QWheelEvent* event, double max_offset);
  // How much of a reference tick has passed since the last frame, which is
  // what every constant below is expressed in. Clamped: a frame is never worth
  // nothing, and a stall is never allowed to integrate in one jump.
  double framesSinceLastStep();
  // How far one wheel notch travels before acceleration, in pixels.
  double wheelNotchPx() const;
  // Folds this click into the measured notch rate and returns the multiplier it
  // has earned. Has to be called once per wheel event, in order.
  double noteWheelRate(double notches);
  // Moves the wheel's target by this much and eases the position toward it.
  void glideBy(double delta_px);
  bool tickWheelGlide(double frames);
  bool tick(double frames);
  void startAnimation();
  void beginGesture(qint64 now_ms);
  void endGesture();
  void armGestureEndTimer();

  // How this view reads a frame's axis: strict once it has asked for sideways
  // gestures only, so a swipe up and down is not read as travel across.
  AxisFallback axisFallback() const;

  double stepPx() const;
  double maxOffsetPx() const;
  // How far past an end the content may travel. Zero with the rubber band off,
  // which every clamp below then collapses to a plain [0, max].
  double overscrollMaxPx() const;
  void notifyChanged();

  Hooks hooks_;
  QTimer anim_;
  QTimer gesture_end_;

  // Every constant in the .cc is per reference tick, so the integration needs
  // to know how much of one a frame was worth -- the internal timer runs late
  // under load, and an external clock runs at whatever the display does.
  QElapsedTimer clock_;
  qint64 last_step_ns_ = 0;
  bool external_clock_ = false;
  double frame_period_ms_ = 0.0;
  bool running_ = false;

  double pos_ = 0.0;
  double velocity_ = 0.0;
  bool overscroll_rebound_ = false;
  bool rubber_band_ = true;

  // The wheel does not coast. Every notch adds to a target and the position
  // eases toward it, so what a click is worth is a distance rather than a shove
  // -- see the wheel constants in the .cc for why.
  bool wheel_gliding_ = false;
  double wheel_target_ = 0.0;
  double wheel_rate_hz_ = 0.0;
  qint64 wheel_last_notch_ms_ = 0;

  double wheel_notch_px_ = 0.0;

  ScrollAxis axis_ = ScrollAxis::Vertical;
  bool sideways_gesture_only_ = false;
  bool trackpad_active_ = false;
  bool trackpad_momentum_phase_ = false;
  qint64 trackpad_last_ms_ = 0;
  double trackpad_velocity_ = 0.0;
};

}  // namespace desktop

#endif  // COMMON_SCROLL_ENGINE_H_

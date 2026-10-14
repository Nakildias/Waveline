// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Ported unchanged from Monarchy's src/common/scroll (same author, same
// licence) rather than linked, for the reason chrome.h gives: that is a static
// library Monarchy's applications compile in from that tree, and the mixer is
// built on machines that have never seen it. Keep it in step with the
// original -- the point is that the rack scrolls like every other Monarchy
// view, and reads the same ~/.config tuning.

#include "scroll_engine.h"

#include "scroll_input.h"

#include <algorithm>
#include <cmath>

#include <QDateTime>
#include <QPointingDevice>
#include <QWheelEvent>
#include <QtGlobal>

namespace desktop {


namespace {

// ---------------------------------------------------------------------------
// The numbers below came off the notification centre, where they were measured
// on real hardware rather than guessed. They are the feel of Monarchy scrolling;
// changing one changes it everywhere. The per-machine knobs live in
// ScrollSettings instead.
// ---------------------------------------------------------------------------

// The reference tick. Every constant below is per one of these -- a velocity
// is px per reference tick, a friction is the fraction that survives one -- and
// a frame that took longer or shorter is scaled to match, so the same numbers
// produce the same motion whether they are run 125 times a second off the
// engine's own timer or 240 times a second off a display's.
constexpr int kSmoothTickMs = 8;
// A frame is never worth nothing, so a clock that reports two steps inside one
// millisecond still advances rather than sitting still until it rounds up. And
// a stall -- a slow paint, a tab coming back, a machine under load -- is never
// integrated in one jump, which a spring would answer by overshooting wildly.
constexpr double kMinStepFrames = 0.05;
constexpr double kMaxStepFrames = 4.0;
constexpr double kSmoothFriction = 0.925;
constexpr double kSmoothStopVelocityRows = 0.0015;

// A wheel notch is not a flick, so it is not modelled as one.
//
// It used to be: a notch pushed an impulse into the same friction integrator the
// trackpad coasts on, and friction of 0.925 multiplies whatever you put in by
// about thirteen before it stops. One notch travelled a row and a half, two
// notches stacked into a jump, and there was no way to nudge a list a little --
// the smallest thing the wheel could do was a swing.
//
// A notch is a click of a ratchet instead: it adds a fixed distance to a target
// the position then eases toward. Ten notches travel exactly ten notches' worth,
// the content is always moving toward somewhere it will actually stop, and a
// single click moves a readable fraction of a row. The wheel becomes something
// you work, rather than something you set off.
//
// How far one click travels with nothing behind it: a nudge, and deliberately
// less than anything you would call a scroll. Tied to the row so a list of tall
// cards still gets somewhere, bounded so it neither crawls nor teleports.
constexpr double kWheelNotchRows = 0.10;
constexpr double kWheelNotchMinPx = 10.0;
constexpr double kWheelNotchMaxPx = 26.0;

// Distance is bought by speed, not by click count.
//
// A per-click ladder (2nd click 2x, 3rd 3x...) has gears in it: the same spin
// lands on a different rung depending on where you started, and you feel the
// change of rung. So the multiplier comes off the measured rate instead -- how
// many clicks a second the wheel is actually turning, smoothed -- through one
// continuous curve. A slow spin is worth a little more than a click, a hard
// swing is worth eight, and everything between is on the curve rather than on a
// step. Stop turning and the rate is gone, so the next lone click is a nudge
// again.
//
// The rate that counts as "one notch, no boost". Above it the curve climbs.
constexpr double kWheelBaseRateHz = 9.0;
// Steeper than 1, so leaning on the wheel is worth more than turning it twice
// as long. This is the whole difference between a nudge and a swing.
constexpr double kWheelRateExponent = 1.4;
constexpr double kWheelRateBoostMax = 8.0;
// Weight of the newest interval in the rate estimate. Low enough that one
// stuttered click cannot fake a swing, high enough to answer within two clicks.
constexpr double kWheelRateBlend = 0.4;
// A gap this long means the swing is over: the rate goes to nothing rather than
// decaying, so a click after a pause is a click.
constexpr int kWheelRateWindowMs = 280;
// The floor on a measured interval. A high-resolution wheel reports a notch in
// fractions, so the numerator shrinks with the denominator and this is only
// here to keep two events sharing a millisecond from reading as infinite speed.
constexpr double kWheelRateMinIntervalMs = 8.0;

// The position chases the target on a spring rather than eating a fixed
// fraction of the gap per tick. An exponential ease is fastest on its first
// frame and only ever slows down, so every click puts a hard edge at the front
// of the motion; a spring has to accelerate, so a run of clicks reads as one
// continuous movement being pushed along. Damping is 2*sqrt(k) -- critical, the
// fastest approach that never overshoots, because a scroll that wobbles past
// where you aimed it is worse than one that is a frame slower.
constexpr double kWheelSpringK = 0.05;
constexpr double kWheelSpringDamp = 0.447;
// Close enough to arrived, and slow enough to be done. Both, or a spring still
// carrying speed would be cut off at the end of its travel.
constexpr double kWheelGlideSnapPx = 0.4;
constexpr double kWheelGlideStopVelocityPx = 0.05;
// How much of the view's rubber band the wheel may pull out. Less than a
// trackpad gets: fingers pull, a wheel only ever taps at the end.
constexpr double kWheelOverscrollShare = 0.55;
// How fast a target left past an end walks back into range. The position is
// chasing the target, so this is what the spring back is made of.
constexpr double kWheelEdgeReturnRate = 0.09;

// Trackpad scrolling is a direct manipulation: the content tracks the fingers
// 1:1 and none of the wheel smoothing above applies while they are down. These
// only govern what happens on either side of that -- how the flick is handed off
// once the fingers lift. Reading the frame itself is scroll_input's job.
// A gap this long in the event stream ends the gesture on stacks that send no
// scroll phases, and is also how long we wait after the last frame before
// deciding the fingers are up on stacks that do.
constexpr int kTrackpadGestureGapMs = 70;
// Release velocity is an EMA over the frames; this is the weight of the newest
// one. Low enough that a single stuttered frame cannot define the flick.
constexpr double kTrackpadVelocityBlend = 0.35;
// How much of the measured finger speed survives into the coast. Under 1 so the
// content never leaves the fingers going faster than they were.
constexpr double kTrackpadMomentumGain = 0.85;
// Below this the lift was a stop, not a flick, and the content stays put.
constexpr double kTrackpadMinFlickPxPerMs = 0.18;
// Rubber band: deep enough to read as a stretch on a hard flick, with a spring
// stiff enough that it snaps back without leaving a lingering gap.
constexpr double kOverscrollMaxRows = 0.30;
constexpr double kOverscrollSettleEpsRows = 0.02;
constexpr double kOverscrollReboundSnapRows = 0.15;

// Set LIQUID_SCROLL_DEBUG=1 to have every wheel frame logged to stderr. Input
// stacks disagree about what a trackpad even reports -- pixels, notches, phases,
// or some mix -- and this is how you find out which one you are on.
bool scrollDebugEnabled() {
  static const bool enabled = qEnvironmentVariableIntValue("LIQUID_SCROLL_DEBUG") != 0;
  return enabled;
}

}  // namespace

ScrollEngine::ScrollEngine(QObject* parent) : QObject(parent) {
  clock_.start();
  anim_.setInterval(kSmoothTickMs);
  // Precise, because the whole point of an 8ms tick is that it is 8ms. A coarse
  // timer is allowed to round an interval to keep the process asleep, which on
  // this one is a fifth of the animation's length.
  anim_.setTimerType(Qt::PreciseTimer);
  connect(&anim_, &QTimer::timeout, this, [this]() {
    if (!tick(framesSinceLastStep())) {
      anim_.stop();
    }
  });

  gesture_end_.setSingleShot(true);
  gesture_end_.setInterval(kTrackpadGestureGapMs);
  connect(&gesture_end_, &QTimer::timeout, this, [this]() { endGesture(); });
}

ScrollEngine::~ScrollEngine() = default;

void ScrollEngine::setHooks(Hooks hooks) { hooks_ = std::move(hooks); }

AxisFallback ScrollEngine::axisFallback() const {
  return sideways_gesture_only_ ? AxisFallback::Strict : AxisFallback::Allowed;
}

double ScrollEngine::stepPx() const {
  const double step = hooks_.step_px ? hooks_.step_px() : 0.0;
  return step > 0.0 ? step : 1.0;
}

double ScrollEngine::overscrollMaxPx() const {
  return rubber_band_ ? kOverscrollMaxRows * stepPx() : 0.0;
}

double ScrollEngine::maxOffsetPx() const {
  const double max_offset = hooks_.max_offset_px ? hooks_.max_offset_px() : 0.0;
  return std::max(0.0, max_offset);
}

void ScrollEngine::notifyChanged() {
  if (hooks_.on_changed) {
    hooks_.on_changed();
  }
  emit scrollActivity();
}

bool ScrollEngine::overscrolled() const { return pos_ < 0.0 || pos_ > maxOffsetPx(); }

void ScrollEngine::stop() {
  anim_.stop();
  running_ = false;
  velocity_ = 0.0;
  overscroll_rebound_ = false;
  wheel_gliding_ = false;
  wheel_rate_hz_ = 0.0;
  // Every caller is taking the content over or tearing it down, so a gesture
  // still in flight is abandoned rather than allowed to coast into the new
  // owner. beginGesture() calls this too, and re-arms itself immediately after.
  gesture_end_.stop();
  trackpad_active_ = false;
  trackpad_momentum_phase_ = false;
  trackpad_velocity_ = 0.0;
}

void ScrollEngine::setPosition(double position) {
  stop();
  pos_ = std::clamp(position, 0.0, maxOffsetPx());
  notifyChanged();
}

void ScrollEngine::clampToRange() {
  if (trackpad_active_) {
    return;
  }
  pos_ = std::clamp(pos_, 0.0, maxOffsetPx());
  if (wheel_gliding_) {
    wheel_target_ = std::clamp(wheel_target_, 0.0, maxOffsetPx());
  }
}

double ScrollEngine::framesSinceLastStep() {
  const qint64 now_ns = clock_.nsecsElapsed();
  const qint64 elapsed_ns = now_ns - last_step_ns_;
  last_step_ns_ = now_ns;
  const double frames = (elapsed_ns / 1'000'000.0) / kSmoothTickMs;
  return std::clamp(frames, kMinStepFrames, kMaxStepFrames);
}

void ScrollEngine::setExternalClock(bool enabled) {
  if (external_clock_ == enabled) {
    return;
  }
  external_clock_ = enabled;
  // Whatever was running was running on the other clock.
  anim_.stop();
  running_ = false;
}

bool ScrollEngine::advance() {
  if (!external_clock_ || !running_) {
    return false;
  }
  double frames = framesSinceLastStep();
  if (frame_period_ms_ > 0.0) {
    // Whole display frames: one per call, more only when frames were really
    // missed, so a late call does not become a long step and the early one
    // after it a short one. A call less than half a frame on is not a frame at
    // all -- a second caller in the same one -- and keeps its own small step
    // rather than being rounded up into a whole frame of extra speed.
    const double shown = std::round(frames * kSmoothTickMs / frame_period_ms_);
    if (shown >= 1.0) {
      frames = std::clamp(shown * frame_period_ms_ / kSmoothTickMs, kMinStepFrames, kMaxStepFrames);
    }
  }
  running_ = tick(frames);
  return running_;
}

void ScrollEngine::startAnimation() {
  if (animating()) {
    return;
  }
  // The first frame is measured from here, not from whenever the last piece of
  // motion stopped -- otherwise a glide started after a pause opens with a step
  // worth however long the view sat idle.
  last_step_ns_ = clock_.nsecsElapsed();
  if (external_clock_) {
    running_ = true;
    emit frameRequested();
    return;
  }
  anim_.start();
}

// Times this click, folds it into the rate estimate, and returns what the rate
// is worth. Called once per wheel event, in order, and nowhere else.
double ScrollEngine::noteWheelRate(double notches) {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const qint64 gap = now - wheel_last_notch_ms_;
  wheel_last_notch_ms_ = now;

  if (gap > kWheelRateWindowMs) {
    // A click on its own. No history to build on, so it is worth exactly itself.
    wheel_rate_hz_ = 0.0;
    return 1.0;
  }

  const double interval_ms = std::max(static_cast<double>(gap), kWheelRateMinIntervalMs);
  const double instant_hz = std::fabs(notches) * 1000.0 / interval_ms;
  wheel_rate_hz_ = wheel_rate_hz_ * (1.0 - kWheelRateBlend) + instant_hz * kWheelRateBlend;

  if (wheel_rate_hz_ <= kWheelBaseRateHz) {
    return 1.0;
  }
  return std::min(kWheelRateBoostMax,
                  std::pow(wheel_rate_hz_ / kWheelBaseRateHz, kWheelRateExponent));
}

double ScrollEngine::wheelNotchPx() const {
  if (wheel_notch_px_ > 0.0) {
    return wheel_notch_px_;
  }
  return std::clamp(kWheelNotchRows * stepPx(), kWheelNotchMinPx, kWheelNotchMaxPx);
}

void ScrollEngine::glideBy(double delta_px) {
  if (delta_px == 0.0) {
    return;
  }

  const double max_offset = maxOffsetPx();
  if (!wheel_gliding_) {
    // Start from where the content actually is, not from a stale target: a
    // glide picked up again after a trackpad gesture or a jump must not first
    // travel back to where the last one was heading.
    wheel_gliding_ = true;
    wheel_target_ = pos_;
    velocity_ = 0.0;
    // Note the spring keeps its speed between clicks while a glide is already
    // running -- that carried speed is what a run of clicks accelerates.
  }

  // Past an end each click buys less than the last, until the band is out and
  // clicking does nothing at all.
  const double overscroll_max = overscrollMaxPx() * kWheelOverscrollShare;
  if (overscroll_max > 0.0) {
    const double stretch = wheel_target_ < 0.0
                               ? -wheel_target_
                               : (wheel_target_ > max_offset ? wheel_target_ - max_offset : 0.0);
    const bool pushing_past_edge = (wheel_target_ <= 0.0 && delta_px < 0.0) ||
                                   (wheel_target_ >= max_offset && delta_px > 0.0);
    if (pushing_past_edge) {
      delta_px *= std::max(0.0, 1.0 - stretch / overscroll_max);
    }
  }

  wheel_target_ =
      std::clamp(wheel_target_ + delta_px, -overscroll_max, max_offset + overscroll_max);
  startAnimation();
}

// One frame of the ease. Nothing here is momentum: the position has a place to
// be and covers a fraction of the way there, so it slows into the target rather
// than being dragged to a stop by friction.
bool ScrollEngine::tickWheelGlide(double frames) {
  const double max_offset = maxOffsetPx();
  const double anchored = std::clamp(wheel_target_, 0.0, max_offset);

  if (wheel_target_ != anchored) {
    // The target walks home and the position follows it, which is the spring
    // back -- one mechanism instead of two fighting over the same pixels.
    // Per reference tick, so the walk home covers the same ground per
    // millisecond however often it is asked.
    wheel_target_ += (anchored - wheel_target_) *
                     (1.0 - std::pow(1.0 - kWheelEdgeReturnRate, frames));
    if (std::fabs(wheel_target_ - anchored) < kWheelGlideSnapPx) {
      wheel_target_ = anchored;
    }
  }

  const double remaining = wheel_target_ - pos_;
  if (std::fabs(remaining) < kWheelGlideSnapPx &&
      std::fabs(velocity_) < kWheelGlideStopVelocityPx && wheel_target_ == anchored) {
    pos_ = wheel_target_;
    velocity_ = 0.0;
    wheel_gliding_ = false;
    notifyChanged();
    return false;
  }

  velocity_ += (remaining * kWheelSpringK - velocity_ * kWheelSpringDamp) * frames;
  pos_ += velocity_ * frames;
  notifyChanged();
  return true;
}

bool ScrollEngine::tick(double frames) {
  if (hooks_.scrollable && !hooks_.scrollable()) {
    stop();
    return false;
  }

  if (wheel_gliding_) {
    return tickWheelGlide(frames);
  }

  const ScrollSettings& settings = globalScrollSettings();
  const double step = stepPx();
  const double max_offset = maxOffsetPx();

  const double travel = velocity_ * frames;
  pos_ += travel;

  const double prev_pos = pos_ - travel;
  const bool was_overscrolled = prev_pos < 0.0 || prev_pos > max_offset;

  const double overscroll_max = overscrollMaxPx();
  if (pos_ < -overscroll_max) {
    pos_ = -overscroll_max;
    if (velocity_ < 0.0) {
      velocity_ = 0.0;
    }
  } else if (pos_ > max_offset + overscroll_max) {
    pos_ = max_offset + overscroll_max;
    if (velocity_ > 0.0) {
      velocity_ = 0.0;
    }
  }

  if (was_overscrolled) {
    if (prev_pos < 0.0 && pos_ >= 0.0) {
      pos_ = 0.0;
      if (velocity_ > 0.0) {
        velocity_ = 0.0;
      }
    } else if (prev_pos > max_offset && pos_ <= max_offset) {
      pos_ = max_offset;
      if (velocity_ < 0.0) {
        velocity_ = 0.0;
      }
    }
  }

  const bool is_overscrolled = pos_ < 0.0 || pos_ > max_offset;

  if (is_overscrolled) {
    overscroll_rebound_ = true;
    const double anchor = pos_ < 0.0 ? 0.0 : (pos_ > max_offset ? max_offset : pos_);
    const double displacement = pos_ - anchor;

    velocity_ -= displacement * settings.overscroll_spring_k * frames;
    velocity_ *= std::pow(settings.overscroll_spring_damp, frames);

    if (pos_ < 0.0 && velocity_ < 0.0) {
      velocity_ *= std::pow(settings.overscroll_edge_pull, frames);
    } else if (pos_ > max_offset && velocity_ > 0.0) {
      velocity_ *= std::pow(settings.overscroll_edge_pull, frames);
    }
  } else {
    velocity_ *= std::pow(kSmoothFriction, frames);
  }

  const double stop_vel = kSmoothStopVelocityRows * step;
  const double settle_eps = kOverscrollSettleEpsRows * step;
  bool at_rest = false;

  if (is_overscrolled) {
    const double anchor = pos_ < 0.0 ? 0.0 : max_offset;
    if (std::fabs(pos_ - anchor) < settle_eps && std::fabs(velocity_) < stop_vel) {
      pos_ = anchor;
      velocity_ = 0.0;
      at_rest = true;
    }
  } else if (std::fabs(velocity_) < stop_vel) {
    velocity_ = 0.0;
    at_rest = true;
  }

  if (at_rest) {
    if (pos_ < 0.0) {
      pos_ = 0.0;
      if (velocity_ < 0.0) {
        velocity_ = 0.0;
      }
    } else if (pos_ > max_offset) {
      pos_ = max_offset;
      if (velocity_ > 0.0) {
        velocity_ = 0.0;
      }
    }
    if (overscroll_rebound_) {
      const double snap = kOverscrollReboundSnapRows * step;
      if (pos_ < snap) {
        pos_ = 0.0;
      } else if (max_offset > 0.0 && pos_ > max_offset - snap) {
        pos_ = max_offset;
      }
      overscroll_rebound_ = false;
    }
  }

  notifyChanged();
  return !at_rest;
}

void ScrollEngine::beginGesture(qint64 now_ms) {
  // The fingers take the content over from whatever was moving it, so any wheel
  // momentum or spring-back still running is dropped rather than fought.
  stop();
  trackpad_active_ = true;
  trackpad_momentum_phase_ = false;
  trackpad_velocity_ = 0.0;
  trackpad_last_ms_ = now_ms;
}

void ScrollEngine::armGestureEndTimer() { gesture_end_.start(); }

void ScrollEngine::endGesture() {
  gesture_end_.stop();
  if (!trackpad_active_) {
    return;
  }
  trackpad_active_ = false;
  trackpad_momentum_phase_ = false;

  const double velocity = trackpad_velocity_;
  trackpad_velocity_ = 0.0;

  if (hooks_.scrollable && !hooks_.scrollable()) {
    return;
  }

  const double max_offset = maxOffsetPx();
  const bool is_overscrolled = pos_ < 0.0 || pos_ > max_offset;

  if (!globalScrollSettings().trackpad_momentum) {
    // No coast and no rubber band: the content stops exactly where the fingers
    // left it, and the edges were already hard, so there is nothing to settle.
    return;
  }

  // The animator ticks on a fixed interval, so the coast is seeded in px/tick.
  double seed = 0.0;
  if (std::fabs(velocity) >= kTrackpadMinFlickPxPerMs && !is_overscrolled) {
    seed = velocity * kSmoothTickMs * kTrackpadMomentumGain;
  }
  if (seed == 0.0 && !is_overscrolled) {
    return;
  }
  velocity_ = seed;
  startAnimation();
}

bool ScrollEngine::handleTrackpad(QWheelEvent* event, double max_offset) {
  const ScrollSettings& settings = globalScrollSettings();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const Qt::ScrollPhase phase = event->phase();

  if (phase == Qt::ScrollEnd) {
    endGesture();
    return true;
  }

  if (phase == Qt::ScrollBegin || !trackpad_active_ ||
      (phase == Qt::NoScrollPhase && now - trackpad_last_ms_ > kTrackpadGestureGapMs)) {
    beginGesture(now);
  }

  if (phase == Qt::ScrollMomentum) {
    // The compositor is already coasting for us; replay its frames and do not
    // measure them, so the lift at the end adds no second coast.
    trackpad_momentum_phase_ = true;
    trackpad_velocity_ = 0.0;
  }

  double delta_px = trackpadDeltaPx(event, axis_, axisFallback());
  if (delta_px == 0.0) {
    trackpad_last_ms_ = now;
    armGestureEndTimer();
    return true;
  }

  delta_px *= settings.trackpad_speed;

  if (!trackpad_momentum_phase_) {
    // Frames arrive irregularly, so speed has to come from the clock rather than
    // from the frame count. The floor keeps a pair of frames sharing a
    // millisecond from reading as an infinite flick.
    const double dt = std::clamp(static_cast<double>(now - trackpad_last_ms_), 4.0, 100.0);
    const double instant = delta_px / dt;
    trackpad_velocity_ =
        trackpad_velocity_ * (1.0 - kTrackpadVelocityBlend) + instant * kTrackpadVelocityBlend;
  }
  trackpad_last_ms_ = now;

  if (settings.trackpad_momentum) {
    // Past an edge the content still follows the fingers, just less and less of
    // the way, which is what makes the end of a list feel like a wall you can
    // lean on rather than one you hit.
    const double overscroll_max = overscrollMaxPx();
    const double stretch =
        pos_ < 0.0 ? -pos_ : (pos_ > max_offset ? pos_ - max_offset : 0.0);
    const bool pulling_past_edge =
        (pos_ <= 0.0 && delta_px < 0.0) || (pos_ >= max_offset && delta_px > 0.0);
    if (pulling_past_edge && overscroll_max > 0.0) {
      delta_px *= std::max(0.0, 1.0 - stretch / overscroll_max);
    }
    pos_ = std::clamp(pos_ + delta_px, -overscroll_max, max_offset + overscroll_max);
  } else {
    pos_ = std::clamp(pos_ + delta_px, 0.0, max_offset);
  }

  notifyChanged();
  armGestureEndTimer();
  return true;
}

bool ScrollEngine::handleWheelNotches(QWheelEvent* event, double max_offset) {
  const ScrollSettings& settings = globalScrollSettings();
  const double step = stepPx();
  const QPoint pixel_delta = scrollDeltaForAxis(event->pixelDelta(), axis_);
  const QPoint angle_delta = scrollDeltaForAxis(event->angleDelta(), axis_);

  double delta_px = 0.0;
  if (pixel_delta.y() != 0) {
    // A device reporting pixels is already handing us the distance it wants
    // travelled, so it is taken as given -- no notch step and no spin, both of
    // which exist to turn a click into a distance.
    // Qt sign convention: positive delta means scroll up, same as angleDelta.
    delta_px = -static_cast<double>(pixel_delta.y());
  } else if (angle_delta.y() != 0) {
    // 120 units is one notch. Scaling by the count rather than testing its sign
    // is what keeps a high-resolution wheel, which reports a notch in fractions,
    // from moving a whole notch per fraction.
    const double notches = -angle_delta.y() / 120.0;
    // Smoothing off is the literal wheel: one notch, one row, no ease and no
    // acceleration. The tactile model is what smoothing buys.
    delta_px = settings.smooth ? notches * wheelNotchPx() * noteWheelRate(notches)
                               : notches * step;
  } else {
    return false;
  }

  // No direction policy here: the sign that arrived is the device's own
  // setting, already applied by libinput -- see scroll_input.h.

  if (settings.smooth) {
    glideBy(delta_px);
    return true;
  }

  pos_ = std::clamp(pos_ + delta_px, 0.0, max_offset);
  notifyChanged();
  return true;
}

bool ScrollEngine::handleWheel(QWheelEvent* event) {
  if (scrollDebugEnabled()) {
    const QPointingDevice* device = event->pointingDevice();
    qWarning(
        "desktop-scroll: pixel=(%d,%d) angle=(%d,%d) phase=%d inverted=%d device=%s type=%d "
        "trackpad=%d",
        event->pixelDelta().x(), event->pixelDelta().y(), event->angleDelta().x(),
        event->angleDelta().y(), static_cast<int>(event->phase()),
        static_cast<int>(event->inverted()),
        device ? qUtf8Printable(device->name()) : "?",
        device ? static_cast<int>(device->type()) : -1, static_cast<int>(eventIsTrackpad(event)));
  }

  if (hooks_.scrollable && !hooks_.scrollable()) {
    return false;
  }

  const double max_offset = maxOffsetPx();
  if (max_offset <= 0.0) {
    return false;
  }

  const ScrollSettings& settings = globalScrollSettings();
  const bool from_trackpad = eventIsTrackpad(event);
  // The hardware, not the handling: a trackpad is still a trackpad when
  // trackpad scrolling is switched off, and the scroll bar wants to know which
  // one is on the desk.
  noteScrollSource(from_trackpad ? ScrollSource::Trackpad : ScrollSource::Wheel);
  const bool trackpad = from_trackpad && settings.trackpad;

  if (sideways_gesture_only_) {
    // A wheel notch is not a sideways swipe, and neither is a swipe going up
    // and down. Declining leaves both to whatever is behind this view, which
    // is what should be scrolling when the fingers are not travelling across.
    if (!trackpad) {
      return false;
    }
    // A frame in a gesture already under way is kept even when it carries no
    // travel across — the end of the swipe arrives that way, and it is what
    // stops the coast.
    const bool mid_gesture = trackpad_active_ && event->phase() != Qt::ScrollBegin;
    if (!mid_gesture && trackpadDeltaPx(event, axis_, AxisFallback::Strict) == 0.0) {
      return false;
    }
  }

  // Past every reason to decline: this view is the one being scrolled.
  emit scrollActivity();

  if (trackpad) {
    return handleTrackpad(event, max_offset);
  }
  if (trackpad_active_) {
    // A wheel taking over mid-gesture wins outright: the abandoned gesture must
    // not coast on underneath the notches. Guarded, because this also clears the
    // wheel's own momentum, which every later notch is supposed to build on.
    stop();
  }
  return handleWheelNotches(event, max_offset);
}

}  // namespace desktop

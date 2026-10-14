// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Ported from Monarchy's src/apps/Settings/src/smooth_scroller (same author,
// same licence), for the reason scroll_engine.h gives. The one difference: it
// lives in namespace Monarchy with the rest of this folder.

#include "smooth_scroller.h"

#include <QAbstractScrollArea>
#include <QEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>

#include "scroll_bar_auto_hide.h"

namespace Monarchy {

SmoothScroller* SmoothScroller::attach(QAbstractScrollArea* area, int step_px,
                                       Qt::Orientation orientation) {
  if (area == nullptr) {
    return nullptr;
  }
  return new SmoothScroller(area, step_px, orientation);
}

SmoothScroller::SmoothScroller(QAbstractScrollArea* area, int step_px,
                               Qt::Orientation orientation)
    : QObject(area),
      area_(area),
      step_px_(step_px > 0 ? step_px : 1),
      orientation_(orientation) {
  engine_.setAxis(orientation_ == Qt::Horizontal ? desktop::ScrollAxis::Horizontal
                                                 : desktop::ScrollAxis::Vertical);
  engine_.setHooks({
      [this]() { return static_cast<double>(step_px_); },
      [this]() { return static_cast<double>(maxOffset()); },
      [this]() { writePosition(); },
      [this]() { return maxOffset() > 0; },
  });

  // Anything that is not the engine — dragging the handle, the keyboard, a
  // programmatic jump — moves the bar directly; the engine has to be told, or
  // the next wheel event would animate from a stale position.
  connect(bar(), &QScrollBar::valueChanged, this, [this](int value) {
    if (!syncing_) {
      engine_.resetPosition(value);
    }
  });

  area_->viewport()->installEventFilter(this);

  // Whether the bar is on screen is a desktop-wide setting, and this is where
  // every view that scrolls the shared way picks it up.
  ScrollBarAutoHide::attach(area_, orientation_, &engine_);
}

QScrollBar* SmoothScroller::bar() const {
  return orientation_ == Qt::Horizontal ? area_->horizontalScrollBar()
                                        : area_->verticalScrollBar();
}

int SmoothScroller::maxOffset() const {
  return area_ == nullptr ? 0 : bar()->maximum();
}

void SmoothScroller::writePosition() {
  syncing_ = true;
  bar()->setValue(qRound(engine_.position()));
  syncing_ = false;
  applyOverscroll();
}

void SmoothScroller::applyOverscroll() {
  QWidget* viewport = area_->viewport();
  const double pos = engine_.position();
  const double clamped = std::clamp(pos, 0.0, static_cast<double>(std::max(0, maxOffset())));
  // Past the top the content follows the pull down, past the bottom it rides up.
  const int overscroll = qRound(clamped - pos);
  if (overscroll == overscroll_applied_) {
    return;
  }
  const int delta = overscroll - overscroll_applied_;
  overscroll_applied_ = overscroll;
  // The overlay scroll bar reads this to stay put while the content bounces.
  const QPoint offset = orientation_ == Qt::Horizontal ? QPoint(overscroll, 0)
                                                       : QPoint(0, overscroll);
  viewport->setProperty("monarchyOverscrollOffset", offset);
  moving_viewport_ = true;
  viewport->move(viewport->pos() + (orientation_ == Qt::Horizontal ? QPoint(delta, 0)
                                                                   : QPoint(0, delta)));
  moving_viewport_ = false;
}

int SmoothScroller::destination() const {
  return qRound(engine_.destination());
}

void SmoothScroller::followEnd(int grew_by) {
  if (engine_.animating() && grew_by != 0) {
    // Mid-glide the target is the old end, which is no longer the end. Moving
    // both by the growth leaves the spring exactly as far from home as it was.
    engine_.shiftOrigin(grew_by);
  } else {
    engine_.resetPosition(maxOffset());
  }
  writePosition();
}

void SmoothScroller::shiftOrigin(int delta_px) {
  if (delta_px == 0) return;
  engine_.shiftOrigin(delta_px);
  // A shift can carry the position out past an end — the content it was
  // anchored to is the thing that went. The bar cannot hold a value out of
  // range anyway, so the engine is brought back to the same place rather than
  // left disagreeing with it.
  engine_.clampToRange();
  writePosition();
}

bool SmoothScroller::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() == QEvent::Move && watched == area_->viewport() && !moving_viewport_ &&
      overscroll_applied_ != 0) {
    // The area laid the viewport out again and put it back home.
    overscroll_applied_ = 0;
    area_->viewport()->setProperty("monarchyOverscrollOffset", QPoint());
  }
  if (event->type() == QEvent::Wheel && watched == area_->viewport()) {
    auto* wheel = static_cast<QWheelEvent*>(event);
    if (engine_.handleWheel(wheel)) {
      wheel->accept();
      return true;
    }
  }
  return QObject::eventFilter(watched, event);
}

}  // namespace Monarchy

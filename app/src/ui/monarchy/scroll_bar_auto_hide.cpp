// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Ported from Monarchy's src/common/scroll (same author, same licence), for the
// reason scroll_engine.h gives: Monarchy's Appearance -> Show scroll bars has
// to land on the mixer's scroll bars too, and it reads the same setting file.
// Two differences from the original: namespace Monarchy, with the rest of this
// folder, and includes by bare name.

#include "scroll_bar_auto_hide.h"

#include <algorithm>

#include <QAbstractScrollArea>
#include <QCoreApplication>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QTimer>

#include "scroll_engine.h"
#include "scroll_input.h"
#include "scroll_settings.h"

namespace Monarchy {

namespace {

// How long the bar stays up after the last frame, and how it comes and goes.
//
// It arrives with no fade at all. A bar is an answer to "where am I?", asked
// at the moment the content starts moving, and even a ninth of a second of
// fading in is a ninth of a second of the question going unanswered — long
// enough on a short flick that the bar was still arriving as the scrolling
// ended. Leaving is the opposite: nothing is being asked by then, and a bar
// that blinks off draws the eye to the corner it just left, so it withdraws
// slowly.
constexpr int kHideDelayMs = 900;
constexpr int kFadeInMs = 0;
constexpr int kFadeOutMs = 380;

// Fallback width for the overlay when neither the stylesheet nor the style
// gives one -- the width Settings' own bars are styled to.
constexpr int kFallbackBarThickness = 8;

// What the process was last scrolled with, as this file last saw it. A change
// means Automatic now means the other thing, and every area has to be told.
desktop::ScrollSource& lastSeenSource() {
  static desktop::ScrollSource source = desktop::lastScrollSource();
  return source;
}

}  // namespace

std::vector<ScrollBarAutoHide*>& ScrollBarAutoHide::instances() {
  static std::vector<ScrollBarAutoHide*> list;
  return list;
}

void ScrollBarAutoHide::refreshAll() {
  const auto list = instances();
  for (ScrollBarAutoHide* instance : list) {
    instance->applyState();
  }
}

ScrollBarAutoHide* ScrollBarAutoHide::forArea(const QAbstractScrollArea* area,
                                              Qt::Orientation orientation) {
  for (ScrollBarAutoHide* instance : instances()) {
    if (instance->area_ == area && instance->orientation_ == orientation) {
      return instance;
    }
  }
  return nullptr;
}

ScrollBarAutoHide* ScrollBarAutoHide::attach(QAbstractScrollArea* area,
                                             Qt::Orientation orientation,
                                             desktop::ScrollEngine* engine,
                                             std::optional<Qt::ScrollBarPolicy> gutter_policy) {
  if (area == nullptr) {
    return nullptr;
  }
  if (ScrollBarAutoHide* existing = forArea(area, orientation)) {
    // Attached twice -- typically the view set itself up and a SmoothScroller
    // arrived afterwards. Hand it the engine rather than building a second bar.
    existing->setEngine(engine);
    return existing;
  }
  const Qt::ScrollBarPolicy policy = orientation == Qt::Horizontal
                                         ? area->horizontalScrollBarPolicy()
                                         : area->verticalScrollBarPolicy();
  if (!gutter_policy.has_value()) {
    if (policy == Qt::ScrollBarAlwaysOff) {
      // The app wants no bar on this axis. Overlaying one would be overruling it.
      return nullptr;
    }
    gutter_policy = policy;
  }
  return new ScrollBarAutoHide(area, orientation, engine, *gutter_policy);
}

void ScrollBarAutoHide::setEngine(desktop::ScrollEngine* engine) {
  if (engine == nullptr) {
    return;
  }
  connect(engine, &desktop::ScrollEngine::scrollActivity, this, &ScrollBarAutoHide::poke,
          Qt::UniqueConnection);
}

ScrollBarAutoHide::ScrollBarAutoHide(QAbstractScrollArea* area, Qt::Orientation orientation,
                                     desktop::ScrollEngine* engine,
                                     Qt::ScrollBarPolicy gutter_policy)
    : QObject(area), area_(area), orientation_(orientation), gutter_policy_(gutter_policy) {
  instances().push_back(this);

  hide_timer_ = new QTimer(this);
  hide_timer_->setSingleShot(true);
  hide_timer_->setInterval(kHideDelayMs);
  connect(hide_timer_, &QTimer::timeout, this, [this]() {
    active_ = false;
    applyState();
  });

  setEngine(engine);
  // Anything that moves the bar without going through the engine -- the
  // keyboard, a jump to a selection -- is scrolling too.
  connect(realBar(), &QScrollBar::valueChanged, this, [this]() {
    syncOverlayFromBar();
    poke();
  });
  connect(realBar(), &QScrollBar::rangeChanged, this, [this]() {
    syncOverlayFromBar();
    applyState();
  });

  // The overlay stands over the viewport, so it follows the viewport's
  // geometry -- which moves when the area's own frame or a gutter appears, not
  // only when the window is resized.
  area->viewport()->installEventFilter(this);
  area->installEventFilter(this);

  settings_token_ = desktop::addScrollSettingsListener([]() { refreshAll(); });

  applyState();
}

ScrollBarAutoHide::~ScrollBarAutoHide() {
  desktop::removeScrollSettingsListener(settings_token_);
  auto& list = instances();
  list.erase(std::remove(list.begin(), list.end(), this), list.end());
}

QScrollBar* ScrollBarAutoHide::realBar() const {
  if (area_ == nullptr) {
    return nullptr;
  }
  return orientation_ == Qt::Horizontal ? area_->horizontalScrollBar()
                                        : area_->verticalScrollBar();
}

bool ScrollBarAutoHide::hasRange() const {
  const QScrollBar* bar = realBar();
  return bar != nullptr && bar->maximum() > bar->minimum();
}

bool ScrollBarAutoHide::overlays() const {
  switch (desktop::globalScrollSettings().scrollbars) {
    case desktop::ScrollBarVisibility::Always:
      return false;
    case desktop::ScrollBarVisibility::WhenScrolling:
      return true;
    case desktop::ScrollBarVisibility::Automatic:
      break;
  }
  // Automatic: a trackpad gets the content's full width and an overlay bar, a
  // wheel mouse keeps its gutter. Unknown counts as a mouse -- see
  // lastScrollSource().
  return desktop::lastScrollSource() == desktop::ScrollSource::Trackpad;
}

void ScrollBarAutoHide::poke() {
  // A source change flips what Automatic means, and the area next door needs
  // telling as much as this one does.
  if (desktop::lastScrollSource() != lastSeenSource()) {
    lastSeenSource() = desktop::lastScrollSource();
    refreshAll();
  }

  active_ = true;
  hide_timer_->start();
  applyState();
}

// Setting a policy is never free: QAbstractScrollArea relays out its children
// whether or not the value changed, and in a widgetResizable QScrollArea whose
// content is height-for-width that relayout can move the range, which lands
// back here through rangeChanged. applyState() runs on every value change, so
// re-setting a policy it already has is enough to keep that circling -- hence
// the guard.
void ScrollBarAutoHide::setPolicy(Qt::ScrollBarPolicy policy) {
  if (area_ == nullptr) {
    return;
  }
  if (orientation_ == Qt::Horizontal) {
    if (area_->horizontalScrollBarPolicy() != policy) {
      area_->setHorizontalScrollBarPolicy(policy);
    }
    return;
  }
  if (area_->verticalScrollBarPolicy() != policy) {
    area_->setVerticalScrollBarPolicy(policy);
  }
}

void ScrollBarAutoHide::applyState() {
  if (area_ == nullptr) {
    return;
  }

  const bool overlay_mode = overlays();

  if (!overlay_mode) {
    hide_timer_->stop();
    if (overlay_ != nullptr) {
      overlay_->hide();
    }
    // The gutter comes back, in whatever form this axis asked for.
    setPolicy(gutter_policy_);
    return;
  }

  // The real bar's width is what makes the gutter; switching it off is what
  // gives the content the full viewport. The bar itself keeps its range and
  // position, so it is still what the engine drives and what the overlay
  // mirrors.
  setPolicy(Qt::ScrollBarAlwaysOff);

  if (overlay_ == nullptr) {
    // A child of the area, not of the viewport: scrolling a scroll area is
    // viewport()->scroll(), which would carry a bar parented there up out of
    // sight along with the pixels.
    auto* overlay = new QScrollBar(orientation_, area_);
    // Named so a stylesheet can single it out; unnamed it is still matched by
    // whatever rule already styles this area's bars, which is the point.
    overlay->setObjectName(QStringLiteral("overlayScrollBar"));
    // Over the content, so the groove must not paint a slab over it. The handle
    // is left to the app's own styling.
    overlay->setStyleSheet(
        QStringLiteral("QScrollBar { background: transparent; }"
                       "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }"
                       "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"));

    effect_ = new QGraphicsOpacityEffect(overlay);
    effect_->setOpacity(0.0);
    overlay->setGraphicsEffect(effect_);

    fade_ = new QPropertyAnimation(effect_, "opacity", this);
    fade_->setEasingCurve(QEasingCurve::OutCubic);

    connect(overlay, &QScrollBar::valueChanged, this, [this](int value) {
      if (!syncing_ && realBar() != nullptr) {
        realBar()->setValue(value);
      }
    });
    overlay->installEventFilter(this);
    overlay_ = overlay;
    syncOverlayFromBar();
  }

  // Held up while the pointer is on the bar or dragging it, so it cannot fade
  // out from under the hand using it.
  const bool visible = hasRange() && (active_ || hovered_ || pressed_);
  overlay_->setVisible(hasRange());
  layoutOverlay();
  fadeTo(visible ? 1.0 : 0.0, visible ? kFadeInMs : kFadeOutMs);
}

void ScrollBarAutoHide::layoutOverlay() {
  if (overlay_ == nullptr || area_ == nullptr) {
    return;
  }
  // In the area's coordinates, over wherever the viewport actually sits --
  // less any rubber band SmoothScroller has slid it by, so the bar holds still
  // while the content bounces.
  const QPoint overscroll = area_->viewport()->property("monarchyOverscrollOffset").toPoint();
  const QRect viewport(area_->viewport()->mapTo(area_, QPoint(0, 0)) - overscroll,
                       area_->viewport()->size());
  const QSize hint = overlay_->sizeHint();
  if (orientation_ == Qt::Horizontal) {
    const int thickness = hint.height() > 0 ? hint.height() : kFallbackBarThickness;
    overlay_->setGeometry(viewport.x(), viewport.bottom() + 1 - thickness, viewport.width(),
                          thickness);
  } else {
    const int thickness = hint.width() > 0 ? hint.width() : kFallbackBarThickness;
    overlay_->setGeometry(viewport.right() + 1 - thickness, viewport.y(), thickness,
                          viewport.height());
  }
  overlay_->raise();
}

void ScrollBarAutoHide::syncOverlayFromBar() {
  const QScrollBar* bar = realBar();
  if (overlay_ == nullptr || bar == nullptr) {
    return;
  }
  syncing_ = true;
  overlay_->setRange(bar->minimum(), bar->maximum());
  overlay_->setPageStep(bar->pageStep());
  overlay_->setSingleStep(bar->singleStep());
  overlay_->setValue(bar->value());
  syncing_ = false;
}

void ScrollBarAutoHide::fadeTo(double opacity, int duration_ms) {
  if (effect_ == nullptr) {
    return;
  }
  const double current = fade_->state() == QAbstractAnimation::Running
                             ? fade_->currentValue().toDouble()
                             : effect_->opacity();
  if (fade_->state() != QAbstractAnimation::Running &&
      qFuzzyCompare(current + 1.0, opacity + 1.0)) {
    return;
  }

  fade_->stop();
  if (duration_ms <= 0) {
    effect_->setOpacity(opacity);
    return;
  }
  fade_->setDuration(duration_ms);
  fade_->setStartValue(current);
  fade_->setEndValue(opacity);
  fade_->start();
}

bool ScrollBarAutoHide::eventFilter(QObject* watched, QEvent* event) {
  if (area_ != nullptr && (watched == area_->viewport() || watched == area_)) {
    if (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
        event->type() == QEvent::Show) {
      layoutOverlay();
    }
    return QObject::eventFilter(watched, event);
  }

  if (watched == overlay_) {
    switch (event->type()) {
      case QEvent::Wheel:
        // A wheel over the overlay is a wheel over the content it is drawn on
        // top of: hand it to the viewport, so it goes through the scroll engine
        // like every other frame rather than the bar's own step.
        QCoreApplication::sendEvent(area_->viewport(), event);
        return true;
      case QEvent::Enter:
        hovered_ = true;
        applyState();
        break;
      case QEvent::Leave:
        hovered_ = false;
        applyState();
        break;
      case QEvent::MouseButtonPress:
        pressed_ = true;
        applyState();
        break;
      case QEvent::MouseButtonRelease:
        pressed_ = false;
        // The countdown starts from the release, not from the last frame of a
        // drag, so letting go does not hide it instantly.
        poke();
        break;
      default:
        break;
    }
  }
  return QObject::eventFilter(watched, event);
}

}  // namespace Monarchy

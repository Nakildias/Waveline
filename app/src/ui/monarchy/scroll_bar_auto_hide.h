// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Ported from Monarchy's src/common/scroll (same author, same licence), for the
// reason scroll_engine.h gives: Monarchy's Appearance -> Show scroll bars has
// to land on the mixer's scroll bars too, and it reads the same setting file.
// Two differences from the original: namespace Monarchy, with the rest of this
// folder, and includes by bare name.

#pragma once

#include <QObject>
#include <QPointer>
#include <QScrollBar>

#include <optional>
#include <vector>

class QAbstractScrollArea;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QTimer;

namespace desktop {
class ScrollEngine;
}

namespace Monarchy {

// Makes a scroll area obey Appearance -> Show scroll bars, on its own.
//
// The setting has three positions, and they are not just three amounts of
// paint -- two of them lay out differently:
//
//   Always        -- the area's own scroll bar, with the width of the gutter
//                    taken out of the content, exactly as before.
//   When scrolling -- the gutter goes away and the content is full width; a bar
//                    is drawn *over* it while something is scrolling and fades
//                    out after.
//   Automatic     -- whichever of the two suits what is on the desk: a wheel
//                    mouse gets the gutter, a trackpad gets the overlay. Which
//                    one is attached is not something the system can be asked
//                    -- a desktop with a mouse and a laptop report the same
//                    device -- so this follows what the scroll frames say, and
//                    changes over the moment the other one is used.
//
// The overlay is a second scroll bar kept in step with the real one, while the
// real one's policy is switched to AlwaysOff. That is what gives the content
// the gutter back: a bar that is merely transparent still reserves its width.
// Being a child of the area, the overlay is matched by the same stylesheet
// rules as the bar it stands in for, so it is styled without any app being
// told about it — and it is a child of the *area*, not of the viewport,
// because scrolling a scroll area is literally viewport()->scroll(), which
// carries the viewport's children along with the pixels.
//
// SmoothScroller::attach() sets one of these up for the area it drives, so an
// app that already scrolls Monarchy way needs no new code at all.
class ScrollBarAutoHide : public QObject {
  Q_OBJECT

 public:
  // Manages `orientation`'s scroll bar on `area`, watching `engine` for scroll
  // activity. Lives as long as `area`. `engine` may be null for an area that
  // has no engine behind it; the bar's own valueChanged is then what counts as
  // activity.
  //
  // Does nothing for an area whose bar is switched off already -- an app that
  // said it wants no bar on this axis means it -- unless `gutter_policy` says
  // otherwise, which is how such a view opts in.
  //
  // `gutter_policy` is what the axis is set to in the modes that keep a gutter;
  // it defaults to whatever the area is set to now. A view whose layout depends
  // on its own width -- a grid that spreads its tiles -- wants AlwaysOn here,
  // since AsNeeded lets the bar's own lane decide whether the bar is needed.
  static ScrollBarAutoHide* attach(QAbstractScrollArea* area, Qt::Orientation orientation,
                                   desktop::ScrollEngine* engine = nullptr,
                                   std::optional<Qt::ScrollBarPolicy> gutter_policy = {});

  // The one already managing this axis of `area`, if any.
  static ScrollBarAutoHide* forArea(const QAbstractScrollArea* area, Qt::Orientation orientation);

  // Watches `engine` for scroll activity. For a view that set itself up before
  // whatever scrolls it did -- see SmoothScroller::attach.
  void setEngine(desktop::ScrollEngine* engine);

  ~ScrollBarAutoHide() override;

  // Shows the bar and restarts the hide countdown. For a view that scrolled
  // without going through the engine -- a keyboard page, a jump to a selection.
  void poke();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  ScrollBarAutoHide(QAbstractScrollArea* area, Qt::Orientation orientation,
                    desktop::ScrollEngine* engine, Qt::ScrollBarPolicy gutter_policy);

  // True when the current setting, and what was last scrolled with, mean this
  // area gives its gutter back and draws the bar over the content instead.
  bool overlays() const;

  // Puts the area in the mode the setting asks for: which bar is in use, and
  // whether the gutter is there. Run when the setting changes, when the
  // pointer arrives or leaves, and when a scroll starts or the countdown ends.
  void applyState();
  // Sets this axis' bar policy only when it is actually changing -- see the
  // definition for why setting it unconditionally is not harmless.
  void setPolicy(Qt::ScrollBarPolicy policy);

  // Keeps the overlay over the viewport's edge, at the width the stylesheet
  // gives it, and above the content. In the area's own coordinates, since that
  // is what it is a child of.
  void layoutOverlay();

  // Range, page step and position, copied from the bar the area really uses.
  void syncOverlayFromBar();

  // Fades the overlay to `opacity`, or jumps there when it is not on screen.
  void fadeTo(double opacity, int duration_ms);

  QScrollBar* realBar() const;
  bool hasRange() const;

  // Every instance in this process, so a change to the setting -- or a switch
  // from mouse to trackpad -- reaches windows that are sitting still.
  static std::vector<ScrollBarAutoHide*>& instances();
  static void refreshAll();

  QPointer<QAbstractScrollArea> area_;
  Qt::Orientation orientation_ = Qt::Vertical;
  // The bar drawn over the content. Only built once the setting first calls for
  // it, so an install that never leaves "Always" never makes one.
  QPointer<QScrollBar> overlay_;
  QGraphicsOpacityEffect* effect_ = nullptr;
  QPropertyAnimation* fade_ = nullptr;
  QTimer* hide_timer_ = nullptr;
  // What this axis is set to whenever the gutter is there.
  Qt::ScrollBarPolicy gutter_policy_ = Qt::ScrollBarAsNeeded;
  bool hovered_ = false;
  bool pressed_ = false;
  bool active_ = false;
  bool syncing_ = false;
  int settings_token_ = 0;
};

}  // namespace Monarchy

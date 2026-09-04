// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "levelmeter.h"

#include <QElapsedTimer>
#include <QLinearGradient>
#include <QList>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <algorithm>
#include <cmath>

#include "theme.h"

namespace {

// Ballistics, in milliseconds of time constant. Fast up, slow down: that is
// how every hardware meter behaves, and it is what makes a transient readable
// instead of a flicker. These are time constants, not per-frame factors, so
// the motion is identical at 60 Hz and at 240 Hz -- a per-frame factor would
// make the meter four times faster on the faster screen.
constexpr double kAttackMs = 22.0;
constexpr double kReleaseMs = 170.0;

// The peak marker: hold, then fall at a steady rate.
constexpr double kPeakHoldMs = 900.0;
constexpr double kPeakFallPerSec = 0.55;

// Below this the bar is at rest and no longer needs repainting.
constexpr double kEpsilon = 0.0015;

// ------------------------------------------------------------------ ticker
struct Ticker {
    QTimer timer;
    QElapsedTimer clock;
    QList<LevelMeter *> meters;
    double intervalMs = 1000.0 / 60.0;
    bool started = false;
};

Ticker &ticker() {
    static Ticker t;
    return t;
}

void tick() {
    Ticker &t = ticker();
    // Real elapsed time, not the nominal interval: Qt timers are not exact,
    // and a missed frame must not slow the animation down.
    const double dt = t.clock.isValid() ? t.clock.nsecsElapsed() / 1.0e6
                                        : t.intervalMs;
    t.clock.restart();

    bool busy = false;
    for (LevelMeter *m : std::as_const(t.meters))
        if (m->advance(std::clamp(dt, 0.1, 100.0))) busy = true;

    // Nothing moving: stop rather than wake 240 times a second to do nothing.
    // setLevel() calls wake() when there is something to draw again.
    if (!busy) t.timer.stop();
}

}  // namespace

void MeterTicker::add(LevelMeter *m) {
    Ticker &t = ticker();
    if (!t.started) {
        t.started = true;
        t.timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&t.timer, &QTimer::timeout, &t.timer, [] { tick(); });
    }
    t.meters.append(m);
}

void MeterTicker::remove(LevelMeter *m) {
    Ticker &t = ticker();
    t.meters.removeAll(m);
    if (t.meters.isEmpty()) t.timer.stop();
}

void MeterTicker::wake() {
    Ticker &t = ticker();
    if (!t.started || t.timer.isActive()) return;
    t.clock.restart();
    t.timer.start(static_cast<int>(t.intervalMs));
}

void MeterTicker::setRefreshRate(double hz) {
    Ticker &t = ticker();
    // Clamped: a bogus 0 from a headless platform would busy-loop, and there
    // is no point drawing a meter faster than about 300 Hz.
    const double clamped = std::clamp(hz > 1.0 ? hz : 60.0, 30.0, 300.0);
    t.intervalMs = 1000.0 / clamped;
    if (t.timer.isActive()) t.timer.start(static_cast<int>(t.intervalMs));
}

// ================================================================ LevelMeter

LevelMeter::LevelMeter(Qt::Orientation orientation, QWidget *parent)
    : QWidget(parent), orient_(orientation) {
    if (orient_ == Qt::Horizontal)
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    else
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    MeterTicker::add(this);
}

LevelMeter::~LevelMeter() { MeterTicker::remove(this); }

void LevelMeter::setThickness(int px) {
    thickness_ = px;
    updateGeometry();
    update();
}

void LevelMeter::setTint(const QColor &tint) {
    tint_ = tint;
    update();
}

QSize LevelMeter::sizeHint() const {
    return orient_ == Qt::Horizontal ? QSize(180, thickness_)
                                     : QSize(thickness_, 120);
}

QSize LevelMeter::minimumSizeHint() const {
    return orient_ == Qt::Horizontal ? QSize(40, thickness_)
                                     : QSize(thickness_, 30);
}

double meterPosition(double amplitude) {
    // Logarithmic, like every real audio meter. A linear scale is useless here:
    // a quiet room at full gain sits near 0.05 RMS, so a linear meter is either
    // invisible or -- with the reference set low enough to see it -- pegged at
    // maximum during ordinary speech.
    //
    // Fixed reference rather than adaptive: an auto-scaling meter renormalises
    // silence to full scale, which is precisely wrong for showing that noise
    // suppression is working.
    constexpr double kFloorDb = -60.0;
    if (amplitude <= 0.0) return 0.0;
    const double db = 20.0 * std::log10(amplitude);
    return std::clamp((db - kFloorDb) / -kFloorDb, 0.0, 1.0);
}

void LevelMeter::setLevel(double fraction) {
    const double t = std::clamp(fraction, 0.0, 1.0);
    if (qFuzzyCompare(t + 1.0, target_ + 1.0)) return;
    target_ = t;
    MeterTicker::wake();
}

bool LevelMeter::advance(double dtMs) {
    // Hidden widgets still hold a value but must not cost a repaint; the
    // sidebar can be collapsed and the window can be minimised.
    const bool visible = isVisible();

    const double before = level_;
    const double beforePeak = peak_;

    // Exponential approach with a time constant, so the shape of the motion
    // does not depend on how often this is called.
    const double tau = (target_ > level_) ? kAttackMs : kReleaseMs;
    level_ += (target_ - level_) * (1.0 - std::exp(-dtMs / tau));
    if (std::abs(target_ - level_) < kEpsilon) level_ = target_;

    // Strictly greater, not >=. With >=, a silent meter sitting at 0 satisfies
    // `0 >= 0` on every frame and re-arms the hold forever, so the ticker never
    // reaches rest and the window burns a wakeup at the full refresh rate
    // around the clock.
    if (level_ > peak_) {
        peak_ = level_;
        peakHoldMs_ = kPeakHoldMs;
    } else if (peakHoldMs_ > 0.0) {
        peakHoldMs_ = std::max(0.0, peakHoldMs_ - dtMs);
    } else {
        peak_ = std::max(level_, peak_ - kPeakFallPerSec * dtMs / 1000.0);
    }

    const bool moved = std::abs(level_ - before) > 1e-6 ||
                       std::abs(peak_ - beforePeak) > 1e-6;
    if (moved && visible) update();

    // Still animating while the bar has not caught up or the peak is above it.
    return level_ != target_ || peak_ > level_ + kEpsilon || peakHoldMs_ > 0.0;
}

void LevelMeter::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = rect();
    const qreal radius = (orient_ == Qt::Horizontal ? r.height() : r.width()) / 2.0;

    QPainterPath trough;
    trough.addRoundedRect(r, radius, radius);
    p.fillPath(trough, Theme::Well);

    if (level_ > 0.002) {
        // The bar is clipped out of a full-length gradient rather than being a
        // solid block that changes colour. A single colour for the whole bar
        // makes a loud peak recolour the quiet part of the signal too, which
        // reads as the meter jumping rather than growing.
        QRectF fill = r;
        if (orient_ == Qt::Horizontal) {
            fill.setWidth(r.width() * level_);
        } else {
            fill.setTop(r.bottom() - r.height() * level_);
        }

        QPainterPath clip;
        clip.addRoundedRect(fill, radius, radius);
        p.save();
        p.setClipPath(trough);
        if (tint_.isValid()) {
            p.fillPath(clip, tint_);
        } else {
            QLinearGradient g(orient_ == Qt::Horizontal ? r.topLeft()
                                                        : r.bottomLeft(),
                              orient_ == Qt::Horizontal ? r.topRight()
                                                        : r.topLeft());
            g.setColorAt(0.00, Theme::Accent);
            g.setColorAt(0.70, Theme::Accent);
            g.setColorAt(0.88, Theme::Warn);
            g.setColorAt(1.00, Theme::Danger);
            p.fillPath(clip, g);
        }
        p.restore();
    }

    if (peak_ > 0.02) {
        p.save();
        p.setClipPath(trough);
        QColor c = peak_ > 0.95 ? Theme::Danger : Theme::Text;
        c.setAlpha(200);
        p.setPen(QPen(c, 2));
        if (orient_ == Qt::Horizontal) {
            const qreal x = r.left() + r.width() * peak_;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        } else {
            const qreal y = r.bottom() - r.height() * peak_;
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
        p.restore();
    }
}

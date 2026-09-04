// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "widgets.h"

#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QAbstractItemView>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QStyle>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <cmath>

#include "theme.h"

namespace {
// Keyboard focus only, for the controls below that paint an accent ring while
// focused. With StrongFocus a plain mouse click also takes focus, so the ring
// stayed behind after every click -- redundant next to the colour change these
// controls already make, and easy to misread as a state of its own. Tabbing to
// them still focuses them and still shows the ring, so keyboard navigation
// keeps its position indicator. Clicking works either way: focus policy only
// governs how focus is acquired, not whether mouse events are delivered.
constexpr Qt::FocusPolicy kRingFocus = Qt::TabFocus;
}  // namespace

// ============================================================= ToggleSwitch

ToggleSwitch::ToggleSwitch(QWidget *parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(kRingFocus);
}

void ToggleSwitch::animateKnob() {
    const qreal target = isChecked() ? 1.0 : 0.0;
    if (qFuzzyCompare(knob_ + 1.0, target + 1.0)) return;

    // Stopped rather than left to fight: clicking twice inside the 140 ms would
    // otherwise leave two animations driving the same property, and whichever
    // finished last would decide where the knob ended up.
    if (knobAnim_) knobAnim_->stop();
    knobAnim_ = new QPropertyAnimation(this, "knob", this);
    knobAnim_->setDuration(140);
    knobAnim_->setStartValue(knob_);
    knobAnim_->setEndValue(target);
    knobAnim_->setEasingCurve(QEasingCurve::OutCubic);
    knobAnim_->start(QAbstractAnimation::DeleteWhenStopped);
}

// Programmatic changes, including the ones made behind a QSignalBlocker while
// refreshing from the daemon.
void ToggleSwitch::checkStateSet() { animateKnob(); }

// The user's own click, and the keyboard equivalent. Qt routes both through
// here with blockRefresh set, which is exactly what stops checkStateSet() above
// from running -- so the animation has to be started again from this side.
void ToggleSwitch::nextCheckState() {
    QAbstractButton::nextCheckState();
    animateKnob();
}

QSize ToggleSwitch::sizeHint() const { return {42, 24}; }

void ToggleSwitch::setKnob(qreal v) {
    knob_ = v;
    update();
}

void ToggleSwitch::enterEvent(QEnterEvent *) { hover_ = true; update(); }
void ToggleSwitch::leaveEvent(QEvent *) { hover_ = false; update(); }

void ToggleSwitch::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    const qreal radius = r.height() / 2.0;

    QColor track = Theme::Well;
    if (isEnabled()) {
        // Interpolated rather than switched at the halfway point: the track
        // and the knob then finish moving at the same instant.
        track = QColor::fromRgbF(
            Theme::Well.redF()   + (Theme::Accent.redF()   - Theme::Well.redF())   * knob_,
            Theme::Well.greenF() + (Theme::Accent.greenF() - Theme::Well.greenF()) * knob_,
            Theme::Well.blueF()  + (Theme::Accent.blueF()  - Theme::Well.blueF())  * knob_);
    }
    p.setPen(QPen(isEnabled() && hover_ ? Theme::TextFaint : Theme::Line, 1));
    p.setBrush(track);
    p.drawRoundedRect(r, radius, radius);

    const qreal margin = 3.0;
    const qreal d = r.height() - margin * 2;
    const qreal x = r.left() + margin + (r.width() - margin * 2 - d) * knob_;
    p.setPen(Qt::NoPen);
    p.setBrush(isEnabled() ? Theme::Text : Theme::TextFaint);
    p.drawEllipse(QRectF(x, r.top() + margin, d, d));

    if (hasFocus()) {
        p.setPen(QPen(Theme::Accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                          radius + 1, radius + 1);
    }
}

// ===================================================================== Fader

namespace {
constexpr int kFaderWidth = 26;
constexpr int kHandleH = 8;
constexpr int kGrooveW = 3;
}  // namespace

Fader::Fader(QWidget *parent) : QSlider(Qt::Vertical, parent) {
    setRange(0, 100);
    setValue(100);
    setCursor(Qt::PointingHandCursor);
    accent_ = Theme::Accent;
}

void Fader::setAccent(const QColor &c) { accent_ = c; update(); }
void Fader::setMuted(bool muted) { muted_ = muted; update(); }

QSize Fader::sizeHint() const { return {kFaderWidth, 150}; }
QSize Fader::minimumSizeHint() const { return {kFaderWidth, 70}; }

qreal Fader::handleY() const {
    const int span = height() - kHandleH;
    // upsideDown=true so that the maximum is at the top, which is where a
    // fader's maximum belongs.
    const int pos = QStyle::sliderPositionFromValue(minimum(), maximum(), value(),
                                                    span, true);
    return pos + kHandleH / 2.0;
}

void Fader::mousePressEvent(QMouseEvent *e) {
    // Click anywhere on the track to jump there. QSlider's default is to page
    // towards the click, which on a 150 px fader means several clicks to get
    // where you already pointed.
    if (e->button() == Qt::LeftButton) {
        setValueFromPos(int(e->position().y()));
        setSliderDown(true);
        e->accept();
        return;
    }
    QSlider::mousePressEvent(e);
}

// Shared by press and drag: value from a pointer position on the track.
void Fader::setValueFromPos(int posY) {
    const int span = height() - kHandleH;
    const int y = std::clamp(posY - kHandleH / 2, 0, span);
    setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), y, span, true));
}

void Fader::mouseMoveEvent(QMouseEvent *e) {
    if (isSliderDown()) {
        setValueFromPos(int(e->position().y()));
        e->accept();
        return;
    }
    QSlider::mouseMoveEvent(e);
}

void Fader::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && isSliderDown()) {
        setSliderDown(false);
        e->accept();
        return;
    }
    QSlider::mouseReleaseEvent(e);
}

void Fader::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const qreal cx = width() / 2.0;
    const qreal top = kHandleH / 2.0;
    const qreal bottom = height() - kHandleH / 2.0;
    const qreal y = handleY();

    QRectF groove(cx - kGrooveW / 2.0, top, kGrooveW, bottom - top);
    QPainterPath gp;
    gp.addRoundedRect(groove, kGrooveW / 2.0, kGrooveW / 2.0);
    p.fillPath(gp, Theme::Well);

    // Filled from the handle down. Dimmed rather than hidden when muted: the
    // fader still has a value, it is just not being heard.
    QRectF filled(groove.left(), y, kGrooveW, bottom - y);
    if (filled.height() > 0) {
        QPainterPath fp;
        fp.addRoundedRect(filled, kGrooveW / 2.0, kGrooveW / 2.0);
        QColor c = isEnabled() ? accent_ : Theme::Line;
        // Held back from full saturation: six neon strips at once is a lot of
        // colour for something that is only meant to identify the channel.
        c.setAlpha(muted_ ? 60 : 190);
        p.fillPath(fp, c);
    }

    QColor handle = isEnabled() ? Theme::Fader : Theme::TextFaint;
    if (muted_) handle = Theme::TextFaint;
    if (isSliderDown() || hasFocus()) handle = handle.lighter(125);

    QRectF h(cx - 9, y - kHandleH / 2.0, 18, kHandleH);
    QPainterPath hp;
    hp.addRoundedRect(h, 3, 3);
    p.fillPath(hp, handle);
    // A darker seam across the middle, which is what makes it read as a
    // physical cap rather than a rectangle.
    p.setPen(QPen(QColor(0, 0, 0, 90), 1));
    p.drawLine(QPointF(h.left() + 3, y), QPointF(h.right() - 3, y));
}

// ============================================================= TrackSlider

namespace {
constexpr int kTrackGrooveH = 5;
constexpr int kTrackHandleW = 13;
constexpr int kTrackHeight = 24;
}  // namespace

TrackSlider::TrackSlider(QWidget *parent) : QSlider(Qt::Horizontal, parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    accent_ = Theme::Accent;
}

void TrackSlider::setAccent(const QColor &c) {
    accent_ = c;
    update();
}

QSize TrackSlider::sizeHint() const { return {120, kTrackHeight}; }
QSize TrackSlider::minimumSizeHint() const { return {60, kTrackHeight}; }

qreal TrackSlider::handleX() const {
    const int span = width() - kTrackHandleW;
    const int pos =
        QStyle::sliderPositionFromValue(minimum(), maximum(), value(), span, false);
    return pos + kTrackHandleW / 2.0;
}

void TrackSlider::mousePressEvent(QMouseEvent *e) {
    if (!isEnabled()) {
        e->ignore();
        return;
    }
    if (e->button() == Qt::LeftButton) {
        setValueFromPos(int(e->position().x()));
        setSliderDown(true);
        e->accept();
        return;
    }
    QSlider::mousePressEvent(e);
}

void TrackSlider::setValueFromPos(int posX) {
    const int span = width() - kTrackHandleW;
    const int x = std::clamp(posX - kTrackHandleW / 2, 0, span);
    setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), x, span, false));
}

void TrackSlider::mouseMoveEvent(QMouseEvent *e) {
    if (!isEnabled()) {
        e->ignore();
        return;
    }
    if (isSliderDown()) {
        setValueFromPos(int(e->position().x()));
        e->accept();
        return;
    }
    QSlider::mouseMoveEvent(e);
}

void TrackSlider::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && isSliderDown()) {
        setSliderDown(false);
        e->accept();
        return;
    }
    QSlider::mouseReleaseEvent(e);
}

void TrackSlider::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const qreal cy = height() / 2.0;
    const qreal left = kTrackHandleW / 2.0;
    const qreal right = width() - kTrackHandleW / 2.0;
    const qreal x = handleX();

    QRectF groove(left, cy - kTrackGrooveH / 2.0, right - left, kTrackGrooveH);
    QPainterPath gp;
    gp.addRoundedRect(groove, kTrackGrooveH / 2.0, kTrackGrooveH / 2.0);
    // Card reads clearly on the Well table background; Well-on-Well was invisible.
    p.fillPath(gp, Theme::Card);

    QRectF filled(groove.left(), groove.top(), x - groove.left(), kTrackGrooveH);
    if (filled.width() > 0) {
        QPainterPath fp;
        fp.addRoundedRect(filled, kTrackGrooveH / 2.0, kTrackGrooveH / 2.0);
        QColor c = isEnabled() ? accent_ : Theme::Line;
        p.fillPath(fp, c);
    }

    const qreal hw = kTrackHandleW / 2.0;
    QRectF handle(x - hw, cy - hw, kTrackHandleW, kTrackHandleW);
    QPainterPath hp;
    hp.addRoundedRect(handle, hw, hw);
    p.fillPath(hp, isEnabled() ? Theme::Fader : Theme::TextFaint);
}

// ======================================================================= Knob

namespace {
constexpr int kKnobPad = 3;
constexpr qreal kKnobArcWidth = 4.0;
// Qt's arc-angle convention: 0 = east (3 o'clock), positive = counter-
// clockwise. 225 is the 7:30 position; sweeping -270 from there (clockwise)
// lands at -45, the 4:30 position -- the usual hardware-knob gap at the
// bottom.
constexpr qreal kKnobStartAngle = 225.0;
constexpr qreal kKnobSweepDeg = 270.0;
constexpr qreal kKnobDragPxPerRange = 200.0;
constexpr qreal kKnobFineDivisor = 8.0;

QPointF pointOnCircle(const QPointF &center, qreal radius, qreal angleDeg) {
    const qreal rad = qDegreesToRadians(angleDeg);
    // Minus on the y term: Qt's arc angles assume y-up, screen coordinates
    // are y-down, so "north" has to flip sign to land in the right place.
    return {center.x() + radius * std::cos(rad), center.y() - radius * std::sin(rad)};
}
}  // namespace

Knob::Knob(QWidget *parent) : QAbstractSlider(parent) {
    // Orientation only matters to QAbstractSlider for keyboard arrow
    // direction; Vertical matches the vertical-drag gesture below.
    setOrientation(Qt::Vertical);
    setRange(0, 100);
    setSingleStep(1);
    setPageStep(10);
    setCursor(Qt::SizeVerCursor);
    setFocusPolicy(kRingFocus);
    accent_ = Theme::Accent;
    // A knob that shrinks under space pressure reads as broken, not
    // responsive -- fixed size means a container that runs out of room has
    // to scroll instead of squeezing every knob in it down small.
    setFixedSize(sizeHint());
}

void Knob::setAccent(const QColor &c) {
    accent_ = c;
    update();
}
void Knob::setBipolar(bool on) {
    bipolar_ = on;
    update();
}
void Knob::setDefaultValue(int v) {
    defaultValue_ = v;
    hasDefaultValue_ = true;
}

QSize Knob::sizeHint() const { return {44, 44}; }
QSize Knob::minimumSizeHint() const { return {44, 44}; }

void Knob::mousePressEvent(QMouseEvent *e) {
    if (!isEnabled() || e->button() != Qt::LeftButton) {
        QAbstractSlider::mousePressEvent(e);
        return;
    }
    dragStart_ = e->position().toPoint();
    dragStartValue_ = value();
    setSliderDown(true);
    e->accept();
}

void Knob::mouseMoveEvent(QMouseEvent *e) {
    if (!isSliderDown()) {
        QAbstractSlider::mouseMoveEvent(e);
        return;
    }
    const int dy = dragStart_.y() - e->position().toPoint().y();
    // Shift drags the same travel over a slower curve rather than a smaller
    // one -- 8x the pixels per unit of range, so the full sweep still
    // reaches both ends, it just takes a longer drag to get there.
    qreal pxPerRange = kKnobDragPxPerRange;
    if (e->modifiers() & Qt::ShiftModifier) pxPerRange *= kKnobFineDivisor;
    const qreal range = maximum() - minimum();
    const qreal delta = (dy / pxPerRange) * range;
    const int newValue =
        std::clamp(dragStartValue_ + int(std::lround(delta)), minimum(), maximum());
    setValue(newValue);
    e->accept();
}

void Knob::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && isSliderDown()) {
        setSliderDown(false);
        e->accept();
        return;
    }
    QAbstractSlider::mouseReleaseEvent(e);
}

void Knob::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && isEnabled()) {
        setValue(hasDefaultValue_ ? defaultValue_ : minimum());
        e->accept();
        return;
    }
    QAbstractSlider::mouseDoubleClickEvent(e);
}

void Knob::wheelEvent(QWheelEvent *e) {
    if (!isEnabled()) {
        e->ignore();
        return;
    }
    const int notches = e->angleDelta().y() / 120;
    if (notches == 0) {
        e->ignore();
        return;
    }
    const int step = (e->modifiers() & Qt::ControlModifier) ? pageStep() : singleStep();
    setValue(std::clamp(value() + notches * step, minimum(), maximum()));
    e->accept();
}

void Knob::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const qreal side = std::min(width(), height()) - kKnobPad * 2.0;
    const QPointF center(width() / 2.0, height() / 2.0);
    const qreal radius = side / 2.0;
    const QRectF ring(center.x() - radius, center.y() - radius, side, side);
    const QRectF arcRect = ring.adjusted(kKnobArcWidth / 2.0, kKnobArcWidth / 2.0,
                                         -kKnobArcWidth / 2.0, -kKnobArcWidth / 2.0);

    const qreal range = maximum() - minimum();
    const qreal t = range > 0 ? (value() - minimum()) / range : 0.0;

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::Well, kKnobArcWidth, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(arcRect, int(kKnobStartAngle * 16), int(-kKnobSweepDeg * 16));

    // Flat white progress rather than the per-instance accent colour -- the
    // rack reads as one piece of gear, not a control tinted per channel.
    QColor fill = isEnabled() ? QColor(Qt::white) : Theme::Line;
    p.setPen(QPen(fill, kKnobArcWidth, Qt::SolidLine, Qt::RoundCap));
    if (bipolar_) {
        constexpr qreal kCenterT = 0.5;
        const qreal a0 = kKnobStartAngle - kCenterT * kKnobSweepDeg;
        const qreal a1 = kKnobStartAngle - t * kKnobSweepDeg;
        p.drawArc(arcRect, int(a0 * 16), int((a1 - a0) * 16));
    } else if (t > 0.0) {
        p.drawArc(arcRect, int(kKnobStartAngle * 16), int(-t * kKnobSweepDeg * 16));
    }

    // A dark cap with a white pointer reads as a real knob body rather than
    // a UI chip -- matching the black housing already used on the rack's
    // power switch.
    const qreal capRadius = std::max(4.0, radius - kKnobArcWidth - 3.0);
    QColor capColor = isEnabled() ? QColor(24, 24, 26) : Theme::TextFaint;
    if (isSliderDown() || hasFocus()) capColor = capColor.lighter(160);
    p.setPen(Qt::NoPen);
    p.setBrush(capColor);
    p.drawEllipse(center, capRadius, capRadius);

    const qreal angle = kKnobStartAngle - t * kKnobSweepDeg;
    const QPointF inner = pointOnCircle(center, capRadius * 0.3, angle);
    const QPointF outer = pointOnCircle(center, capRadius * 0.85, angle);
    p.setPen(QPen(isEnabled() ? QColor(Qt::white) : Theme::TextFaint, 2, Qt::SolidLine,
                 Qt::RoundCap));
    p.drawLine(inner, outer);

    if (hasFocus()) {
        p.setPen(QPen(Theme::Accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(ring.adjusted(-2, -2, 2, 2));
    }
}

// ================================================================ PowerSwitch

PowerSwitch::PowerSwitch(QWidget *parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(kRingFocus);
    accent_ = Theme::Accent;
}

void PowerSwitch::setAccent(const QColor &c) {
    accent_ = c;
    update();
}

QSize PowerSwitch::sizeHint() const { return {22, 38}; }

void PowerSwitch::enterEvent(QEnterEvent *) { hover_ = true; update(); }
void PowerSwitch::leaveEvent(QEvent *) { hover_ = false; update(); }

// Modeled on the small black rocker switches on real rack gear: O on top,
// I on the bottom, a dark housing that does not follow the app theme (real
// hardware doesn't switch to light mode either), and the currently-pressed
// half lit so the position reads at a glance.
void PowerSwitch::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    constexpr qreal kRadius = 5.0;
    const bool on = isChecked();

    QPainterPath body;
    body.addRoundedRect(r, kRadius, kRadius);

    QColor housing(28, 28, 31);
    if (isEnabled() && hover_) housing = housing.lighter(130);
    if (!isEnabled()) housing = housing.lighter(110);
    p.setPen(Qt::NoPen);
    p.setBrush(housing);
    p.drawPath(body);

    // The pressed half only, clipped to the rounded housing so it never
    // overhangs the corners. Off is red rather than a neutral grey -- a
    // power indicator that doesn't visibly warn you it's off is just a
    // decoration.
    p.save();
    p.setClipPath(body);
    QRectF half = r;
    half.setHeight(r.height() / 2.0);
    if (on) half.moveTop(r.center().y());
    QColor paddle = isEnabled() ? (on ? accent_ : Theme::Danger) : QColor(60, 60, 64);
    p.fillRect(half.adjusted(1.5, 1.5, -1.5, -1.5), paddle);
    p.restore();

    p.setPen(QPen(QColor(0, 0, 0, 160), 1));
    p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
    p.setBrush(Qt::NoBrush);
    p.drawPath(body);

    QFont f = font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 0.75);
    p.setFont(f);
    const QRectF topHalf(r.left(), r.top(), r.width(), r.height() / 2.0);
    const QRectF botHalf(r.left(), r.center().y(), r.width(), r.height() / 2.0);
    const QColor dim(120, 120, 124);
    p.setPen(!on ? Qt::white : dim);
    p.drawText(topHalf, Qt::AlignCenter, QStringLiteral("O"));

    // The IEC "on" glyph is a vertical bar, not a letter -- the minus icon
    // turned on its side gives exactly that without a second icon asset.
    // Pure white rather than the near-white used for the "O" text: at this
    // icon's small size, anything short of full white reads as grey once
    // it's antialiased against the accent-coloured paddle behind it.
    const int iconPx = qRound(r.width() * 0.55);
    const QPixmap iPm =
        Theme::iconPixmap(QStringLiteral("minus"), on ? QColor(Qt::white) : dim, iconPx);
    p.save();
    p.translate(botHalf.center());
    p.rotate(90);
    p.drawPixmap(QRectF(-iconPx / 2.0, -iconPx / 2.0, iconPx, iconPx).toRect(), iPm);
    p.restore();
}

// ================================================================ IconToggle

IconToggle::IconToggle(const QString &iconOn, const QString &iconOff, QWidget *parent)
    : QAbstractButton(parent), on_(iconOn), off_(iconOff) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(kRingFocus);
    accent_ = Theme::Accent;
}

IconCombo::IconCombo(QWidget *parent) : QComboBox(parent) {
    setIconSize(QSize(24, 24));
    setCursor(Qt::PointingHandCursor);
    // The popup is not bound by the narrow cell this usually sits in, so the
    // names stay readable even though the closed control is icon-only.
    view()->setMinimumWidth(190);
}

QSize IconCombo::sizeHint() const {
    return {iconSize().width() + 6, iconSize().height() + 4};
}

void IconCombo::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QRect r(QPoint(0, 0), iconSize());
    r.moveCenter(rect().center());
    itemIcon(currentIndex()).paint(&p, r, Qt::AlignCenter, QIcon::Normal);
}

void IconToggle::setAccent(const QColor &c) { accent_ = c; update(); }
void IconToggle::setShape(Shape s) { shape_ = s; updateGeometry(); update(); }
void IconToggle::setIconSize(int px) { px_ = px; updateGeometry(); update(); }
void IconToggle::setIconVisible(bool on) {
    if (iconVisible_ == on) return;
    iconVisible_ = on;
    if (!on) hover_ = false;
    update();
}

QSize IconToggle::sizeHint() const {
    return shape_ == Pill ? QSize(px_ * 2 + 16, px_ + 10)
                          : QSize(px_ + 12, px_ + 12);
}

void IconToggle::enterEvent(QEnterEvent *) { hover_ = true; update(); }
void IconToggle::leaveEvent(QEvent *) { hover_ = false; update(); }

void IconToggle::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::RightButton) {
        emit rightClicked();
        e->accept();
        return;
    }
    if (e->button() == Qt::MiddleButton) {
        emit middleClicked();
        e->accept();
        return;
    }
    QAbstractButton::mousePressEvent(e);
}

// Shift+click used to open the effects panel from the heartbeat. It is gone:
// the gear button beside the heartbeat does that now, visibly.

void IconToggle::paintEvent(QPaintEvent *) {
    if (!iconVisible_) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = r.height() / 2.0;

    QColor bg = Qt::transparent;
    QColor fg = Theme::TextDim;
    if (!isEnabled()) {
        fg = Theme::TextFaint;
    } else if (isChecked()) {
        bg = accent_;
        bg.setAlpha(38);
        fg = accent_;
    } else if (hover_) {
        bg = Theme::CardHover;
        fg = Theme::Text;
    }

    if (bg.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(r, radius, radius);
    }
    if (isChecked() && isEnabled()) {
        QColor ring = accent_;
        ring.setAlpha(120);
        p.setPen(QPen(ring, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, radius, radius);
    }
    if (hasFocus()) {
        p.setPen(QPen(Theme::Accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, radius, radius);
    }

    const QPixmap pm = Theme::iconPixmap(isChecked() ? on_ : off_, fg, px_);
    if (!pm.isNull()) {
        const QPointF at((width() - px_) / 2.0, (height() - px_) / 2.0);
        p.drawPixmap(at, pm);
    }
}

// ================================================================ LinkToggle

EqualToggle::EqualToggle(QWidget *parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(kRingFocus);
    accent_ = Theme::Accent;
}

void EqualToggle::setAccent(const QColor &c) {
    accent_ = c.isValid() ? c : Theme::Accent;
    update();
}

QSize EqualToggle::sizeHint() const { return {38, 20}; }

void EqualToggle::enterEvent(QEnterEvent *) { hover_ = true; update(); }
void EqualToggle::leaveEvent(QEvent *) { hover_ = false; update(); }

void EqualToggle::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // A pill behind it, so it reads as a control rather than as a glyph
    // floating between the faders.
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = r.height() / 2.0;

    QColor fg = Theme::TextFaint;
    if (!isEnabled()) fg = Theme::Line;
    else if (isChecked()) fg = accent_;
    else if (hover_) fg = Theme::TextDim;

    QColor bg = Theme::Well;
    if (isChecked() && isEnabled()) { bg = accent_; bg.setAlpha(34); }
    else if (hover_ && isEnabled()) bg = Theme::CardHover;

    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(r, radius, radius);

    if (isChecked() && isEnabled()) {
        QColor ring = accent_;
        ring.setAlpha(120);
        p.setPen(QPen(ring, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, radius, radius);
    }
    if (hasFocus()) {
        p.setPen(QPen(Theme::Accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, radius, radius);
    }

    const int px = 15;
    p.drawPixmap(QPointF((width() - px) / 2.0, (height() - px) / 2.0),
                 Theme::iconPixmap(QStringLiteral("equal"), fg, px));
}

// ================================================================== CardBase

CardBase::CardBase(QWidget *parent) : QFrame(parent) { fill_ = Theme::Card; }

void CardBase::setRadius(int r) { radius_ = r; update(); }
void CardBase::setFillColor(const QColor &c) { fill_ = c; update(); }
void CardBase::setTopStripe(const QColor &c) { stripe_ = c; update(); }

void CardBase::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), radius_, radius_);
    p.fillPath(path, fill_);

    if (stripe_.isValid()) {
        p.save();
        p.setClipPath(path);
        p.fillRect(QRect(0, 0, width(), 3), stripe_);
        p.restore();
    }
}

// =================================================================== Section

Section::Section(const QString &title, QWidget *parent) : CardBase(parent) {
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(14, 12, 14, 14);
    lay->setSpacing(10);
    caption_ = caption(title, this);
    lay->addWidget(caption_);

    content_ = new QVBoxLayout;
    content_->setContentsMargins(0, 0, 0, 0);
    content_->setSpacing(9);
    lay->addLayout(content_);
}

void Section::setTitle(const QString &title) {
    // caption() upper-cases and letter-spaces; setting the text again keeps
    // that styling, which lives in the font and the palette rather than in the
    // string.
    if (caption_) caption_->setText(title.toUpper());
}

// ======================================================== MonitorModeButton

namespace {
// Blue for the software path, purple for the hardware one -- the same two
// colours the microphone and Browser cards use, so the association is already
// established by the time anyone looks down here.
const QColor kSoftware{0x4d, 0x9c, 0xff};
const QColor kHardware{0xa9, 0x6c, 0xf0};

QColor colorFor(MonitorModeButton::Mode m) {
    switch (m) {
        case MonitorModeButton::Software: return kSoftware;
        case MonitorModeButton::Hardware: return kHardware;
        case MonitorModeButton::Off:      break;
    }
    return Theme::TextFaint;
}
}  // namespace

MonitorModeButton::MonitorModeButton(QWidget *parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(kRingFocus);
    setFixedSize(sizeHint());
    updateTip();
}

QSize MonitorModeButton::sizeHint() const {
    // Same footprint formula as IconToggle in Round shape.
    const int px = 19;
    return {px + 12, px + 12};
}

void MonitorModeButton::setMode(Mode m) {
    if (mode_ == m) return;
    mode_ = m;
    updateTip();
    update();
}

void MonitorModeButton::setHardwareAvailable(bool on) {
    if (hardwareAvailable_ == on) return;
    hardwareAvailable_ = on;
    // Sitting in a position that no longer exists would leave the button
    // purple and unclickable-out-of. Off is the honest fallback: with no
    // hardware path there is nothing the old state could still be doing.
    if (!on && mode_ == Hardware) mode_ = Off;
    updateTip();
    update();
}

void MonitorModeButton::updateTip() {
    QString state;
    switch (mode_) {
        case Off:
            state = tr("Off -- you cannot hear yourself.");
            break;
        case Software:
            state = tr("Software: through the output chosen on the left, so it "
                       "works with headphones plugged into the computer. Adds a "
                       "few milliseconds of delay.");
            break;
        case Hardware:
            state = tr("Hardware: blended inside the microphone and sent to the "
                       "headphone jack ON THE MICROPHONE. No delay at all, but "
                       "only for headphones plugged into the microphone. Set "
                       "the blend in the hardware panel.");
            break;
    }
    setToolTip(tr("How you hear yourself.\n\n%1\n\nClick to cycle: %2.")
                   .arg(state, hardwareAvailable_
                                   ? tr("off, software, hardware")
                                   : tr("off, software")));
}

void MonitorModeButton::enterEvent(QEnterEvent *) { hover_ = true; update(); }
void MonitorModeButton::leaveEvent(QEvent *) { hover_ = false; update(); }

void MonitorModeButton::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || !isEnabled()) return;
    if (!rect().contains(e->position().toPoint())) return;
    emit modeChosen((mode_ + 1) % modeCount());
}

void MonitorModeButton::keyPressEvent(QKeyEvent *e) {
    if (!isEnabled()) return;
    if (e->key() == Qt::Key_Space || e->key() == Qt::Key_Return ||
        e->key() == Qt::Key_Enter) {
        emit modeChosen((mode_ + 1) % modeCount());
        return;
    }
    QWidget::keyPressEvent(e);
}

void MonitorModeButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal radius = r.height() / 2.0;
    const bool on = isEnabled() && mode_ != Off;
    const QColor accent = on ? colorFor(mode_)
                             : (isEnabled() ? Theme::TextDim : Theme::TextFaint);

    QColor bg = Qt::transparent;
    QColor fg = accent;
    if (!isEnabled()) {
        fg = Theme::TextFaint;
    } else if (on) {
        bg = colorFor(mode_);
        bg.setAlpha(38);
    } else if (hover_) {
        bg = Theme::CardHover;
        fg = Theme::Text;
    }

    if (bg.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(r, radius, radius);
    }
    if (on) {
        QColor ring = colorFor(mode_);
        ring.setAlpha(120);
        p.setPen(QPen(ring, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, radius, radius);
    }
    if (hasFocus()) {
        p.setPen(QPen(Theme::Accent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, radius, radius);
    }

    const int px = 19;
    const QString icon = mode_ == Off ? QStringLiteral("ear-off")
                                      : QStringLiteral("ear");
    p.drawPixmap(QPointF((width() - px) / 2.0, (height() - px) / 2.0),
                 Theme::iconPixmap(icon, fg, px));
}

// ================================================================= StatusDot

StatusDot::StatusDot(QWidget *parent) : QWidget(parent) {
    color_ = Theme::TextFaint;
    setFixedSize(sizeHint());
}

QSize StatusDot::sizeHint() const { return {10, 10}; }

void StatusDot::setColor(const QColor &c) {
    if (color_ == c) return;
    color_ = c;
    update();
}

void StatusDot::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // A soft halo, so a 10 px dot still reads as "lit" rather than as a speck.
    QColor halo = color_;
    halo.setAlpha(60);
    p.setPen(Qt::NoPen);
    p.setBrush(halo);
    p.drawEllipse(QRectF(rect()));

    p.setBrush(color_);
    p.drawEllipse(QRectF(rect()).adjusted(2, 2, -2, -2));
}

// =============================================================== ElidedLabel

ElidedLabel::ElidedLabel(const QString &text, QWidget *parent)
    : QWidget(parent), text_(text) {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void ElidedLabel::setText(const QString &text) {
    if (text_ == text) return;
    text_ = text;
    setToolTip(text);
    update();
}

QSize ElidedLabel::sizeHint() const {
    const QFontMetrics fm(font());
    return {fm.horizontalAdvance(text_), fm.height()};
}

QSize ElidedLabel::minimumSizeHint() const {
    const QFontMetrics fm(font());
    return {fm.horizontalAdvance(QStringLiteral("...")), fm.height()};
}

void ElidedLabel::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const QFontMetrics fm(font());
    p.setPen(Theme::TextDim);
    p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(text_, Qt::ElideRight, width()));
}

// =================================================================== helpers

QLabel *caption(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text.toUpper(), parent);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.82);
    f.setBold(true);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.1);
    l->setFont(f);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, Theme::TextFaint);
    l->setPalette(pal);
    return l;
}

QLabel *fixedReadout(const QString &widest, QWidget *parent) {
    auto *l = new QLabel(widest, parent);
    l->setFixedWidth(l->fontMetrics().horizontalAdvance(widest) + 6);
    l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(pal);
    return l;
}

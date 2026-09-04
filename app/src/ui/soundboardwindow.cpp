// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "soundboardwindow.h"
#include "soundtrimdialog.h"

#include <QClipboard>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLinearGradient>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>

#include "mixerclient.h"
#include "theme.h"
#include "widgets.h"

namespace {

// The row's name label is fixed at this width (see SoundboardSoundRow's
// constructor). Elided text uses this constant directly rather than the
// label's own width(): applyInfo() runs from refresh(), which the window
// constructor calls before the first layout pass ever runs, and a fixed-
// width widget does not report its real width() until laid out at least
// once -- eliding against 0 there would leave the very first paint showing
// nothing but an ellipsis.
constexpr int kSoundNameWidth = 190;

QLabel *dimLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(p);
    return l;
}

// A small caption row: a faint glyph, then a dim label -- what every column
// of the Soundboard Settings panel starts with.
QHBoxLayout *captionRow(const QString &iconName, const QString &text, QWidget *parent) {
    auto *row = new QHBoxLayout;
    row->setSpacing(6);
    auto *ic = new QLabel(parent);
    ic->setPixmap(Theme::iconPixmap(iconName, Theme::TextFaint, 13));
    row->addWidget(ic);
    row->addWidget(dimLabel(text, parent));
    row->addStretch();
    return row;
}

// Every chrome button in a title bar (close, minimize, gear, add, ...) is
// this size.
constexpr int kChromeBtnPx = 26;

// The Soundboard Settings panel's Playback tab: Channel and Audio Sharing
// selectors share this exact width, so the two paired rows (selector + its
// volume) line up with each other instead of the volume sliders starting at
// two different x positions.
constexpr int kSelectorWidth = 140;

// The Soundboard window's fixed width.
constexpr int kSoundboardWidth = 620;
// How many sound rows show before the rack switches from "the window grows
// to fit" to "the rows area scrolls instead" -- see
// SoundboardWindow::refresh()'s scroll_->setFixedHeight() call.
constexpr int kMaxVisibleRows = 10;

// The simple view's grid, sized to match the web companion's own pad cell
// (SB_CELL_W/SB_CELL_H/SB_GAP in app/src/daemon/web/app.js) as closely as a
// fixed-width desktop window can: the web page adapts its column count to
// whatever viewport it's given, but kSoundboardWidth never changes, so the
// column count here is worked out once from the same 128x64 target rather
// than picked by eye.
constexpr int kSimplePadW = 128;
constexpr int kSimplePadH = 64;
constexpr int kSimpleGap = 10;
// kSoundboardWidth minus the content area's own left/right margins (20+20,
// set on `content` in the constructor) and simpleLay_'s right margin (4, to
// match rowsLay_'s -- see its own contentsMargins()).
constexpr int kSimpleAvailWidth = kSoundboardWidth - 40 - 4;
constexpr int kSimpleCols =
    std::max(1, (kSimpleAvailWidth + kSimpleGap) / (kSimplePadW + kSimpleGap));
// kMaxVisibleRows applies to a *grid* row here (kSimpleCols pads wide), the
// same way it applies to one detailed card in the rack view -- so switching
// views changes what a "row" holds, not how many of them the window is
// willing to show before it scrolls instead of growing.

// A frameless window's title bar, styled after the Virtual Rack's: a plain
// black bar, draggable via startSystemMove() since a frameless window has no
// window-manager grip of its own.
class ChromeTitleBar : public QWidget {
public:
    explicit ChromeTitleBar(QWidget *parent) : QWidget(parent) {
        setAutoFillBackground(true);
        QPalette p = palette();
        p.setColor(QPalette::Window, Qt::black);
        setPalette(p);
        setFixedHeight(40);
    }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) return;
        if (QWindow *handle = window()->windowHandle()) handle->startSystemMove();
    }
};

// Builds a ChromeTitleBar for `owner` (the frameless top-level window the
// close/minimize buttons act on) with close, minimize and `heading` already
// placed. Returns the bar; `outLay` (if given) is the row's QHBoxLayout, so
// a caller with its own chrome buttons -- the Soundboard's gear and add --
// can append them after the heading the same way the Virtual Rack does.
QWidget *buildChromeTitleBar(QWidget *owner, const QString &heading,
                             QHBoxLayout **outLay = nullptr) {
    auto *bar = new ChromeTitleBar(owner);
    auto *lay = new QHBoxLayout(bar);
    lay->setContentsMargins(10, 0, 8, 0);
    lay->setSpacing(6);

    auto *closeBtn = new QPushButton(bar);
    closeBtn->setIcon(QIcon(Theme::iconPixmap(QStringLiteral("x"), Theme::Text, 14)));
    closeBtn->setToolTip(QObject::tr("Close"));
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setFixedSize(kChromeBtnPx, kChromeBtnPx);
    QObject::connect(closeBtn, &QPushButton::clicked, owner, &QWidget::close);
    lay->addWidget(closeBtn);

    auto *minimizeBtn = new QPushButton(bar);
    minimizeBtn->setIcon(QIcon(Theme::iconPixmap(QStringLiteral("minus"), Theme::Text, 14)));
    minimizeBtn->setToolTip(QObject::tr("Minimize"));
    minimizeBtn->setCursor(Qt::PointingHandCursor);
    minimizeBtn->setFixedSize(kChromeBtnPx, kChromeBtnPx);
    QObject::connect(minimizeBtn, &QPushButton::clicked, owner, &QWidget::showMinimized);
    lay->addWidget(minimizeBtn);

    auto *headingLabel = new QLabel(heading, bar);
    QFont hf = headingLabel->font();
    hf.setBold(true);
    hf.setPointSizeF(hf.pointSizeF() * 1.05);
    headingLabel->setFont(hf);
    QPalette hp = headingLabel->palette();
    hp.setColor(QPalette::WindowText, Theme::Text);
    headingLabel->setPalette(hp);
    lay->addSpacing(4);
    lay->addWidget(headingLabel);

    if (outLay) *outLay = lay;
    return bar;
}

// Traces `fraction` (0..1) of a rounded rectangle's own border, starting at
// the top-left corner and going clockwise -- SimplePad's progress ring,
// built by hand rather than via a dash pattern because Qt's QPen dash
// pattern is defined in pen-width units, not path-length ones, so it cannot
// express "stop after exactly this fraction of the perimeter" the way SVG's
// stroke-dasharray/stroke-dashoffset (what the web companion's .pad-ring
// actually uses) can. Walking the four edges and four corner arcs by their
// real lengths is the equivalent: at fraction 0 this returns an empty path,
// at 1 the whole border.
QPainterPath roundedRectProgressPath(const QRectF &r, qreal radius, qreal fraction) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    QPainterPath path;
    if (fraction <= 0.0) return path;

    const qreal x = r.x(), y = r.y(), w = r.width(), h = r.height();
    const qreal rad = std::min({radius, w / 2.0, h / 2.0});
    const qreal topLen = w - 2 * rad;
    const qreal rightLen = h - 2 * rad;
    const qreal bottomLen = w - 2 * rad;
    const qreal leftLen = h - 2 * rad;
    const qreal cornerLen = rad * M_PI / 2.0;  // one quarter-circle arc
    const qreal total = topLen + rightLen + bottomLen + leftLen + 4 * cornerLen;
    if (total <= 0.0) return path;

    qreal remaining = total * fraction;
    QPointF cur(x + rad, y);
    path.moveTo(cur);

    // Returns true once `remaining` is used up -- callers stop walking
    // segments as soon as one of these does.
    auto lineSeg = [&](QPointF to, qreal len) {
        if (remaining <= 0.0) return true;
        if (remaining >= len) {
            path.lineTo(to);
            cur = to;
            remaining -= len;
            return false;
        }
        path.lineTo(cur + (to - cur) * (remaining / len));
        remaining = 0.0;
        return true;
    };
    // `sweepDeg` is the corner's *full* turn (always -90: Qt's arc angles
    // increase counter-clockwise, so a clockwise quarter-turn is negative);
    // only the fraction of it `remaining` can still afford is actually drawn.
    auto arcSeg = [&](QRectF bounds, qreal startAngle, qreal sweepDeg, qreal len) {
        if (remaining <= 0.0) return true;
        const qreal frac = std::min(1.0, remaining / len);
        path.arcTo(bounds, startAngle, sweepDeg * frac);
        cur = path.currentPosition();
        if (remaining >= len) {
            remaining -= len;
            return false;
        }
        remaining = 0.0;
        return true;
    };

    if (lineSeg(QPointF(x + w - rad, y), topLen)) return path;
    if (rad > 0 && arcSeg(QRectF(x + w - 2 * rad, y, 2 * rad, 2 * rad), 90, -90, cornerLen))
        return path;
    if (lineSeg(QPointF(x + w, y + h - rad), rightLen)) return path;
    if (rad > 0 &&
        arcSeg(QRectF(x + w - 2 * rad, y + h - 2 * rad, 2 * rad, 2 * rad), 0, -90, cornerLen))
        return path;
    if (lineSeg(QPointF(x + rad, y + h), bottomLen)) return path;
    if (rad > 0 && arcSeg(QRectF(x, y + h - 2 * rad, 2 * rad, 2 * rad), -90, -90, cornerLen))
        return path;
    if (lineSeg(QPointF(x, y + rad), leftLen)) return path;
    if (rad > 0) arcSeg(QRectF(x, y, 2 * rad, 2 * rad), 180, -90, cornerLen);

    return path;
}

}  // namespace

// ------------------------------------------------------------------ PlayPad

PlayPad::PlayPad(QWidget *parent) : QAbstractButton(parent) {
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Play"));
}

void PlayPad::setPlaying(bool on) {
    if (playing_ == on) return;
    playing_ = on;
    setToolTip(on ? tr("Stop") : tr("Play"));
    update();
}

QSize PlayPad::sizeHint() const { return QSize(46, 46); }

void PlayPad::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(3, 3, -3, -3);
    // Neutral white rather than the channel's colour: "this pad is making
    // noise right now" is a status every channel shows the same way.
    const QColor active = Qt::white;

    if (playing_) {
        // A soft glow behind the pad -- the one flourish a soundboard button
        // is allowed that a mute switch is not, because "this is making
        // noise right now" is worth a little theatre.
        QColor glow = active;
        glow.setAlpha(55);
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(rect());
    }

    const QColor fill = playing_ ? active : (hover_ ? Theme::CardHover : Theme::Card);
    const QColor border = playing_ ? active : (hover_ ? active : Theme::Line);
    p.setPen(QPen(border, playing_ ? 0.0 : 1.5));
    p.setBrush(fill);
    p.drawEllipse(r);

    const QColor iconColor =
        playing_ ? Theme::glyphOn(active) : (hover_ ? active : Theme::TextDim);
    const int px = static_cast<int>(r.width() * 0.4);
    const QPointF center = r.center();
    if (playing_) {
        // Square, so it sits dead centre with no offset needed.
        const QPixmap icon = Theme::iconPixmap(QStringLiteral("stop"), iconColor, px);
        p.drawPixmap(QPointF(center.x() - icon.width() / 2.0, center.y() - icon.height() / 2.0),
                    icon);
    } else {
        const QPixmap icon = Theme::iconPixmap(QStringLiteral("play"), iconColor, px);
        // The play glyph's visual weight sits left of its own bounding box
        // (a triangle pointing right); nudge it right a hair so it reads
        // centred.
        p.drawPixmap(QPointF(center.x() - icon.width() / 2.0 + px * 0.06,
                             center.y() - icon.height() / 2.0),
                    icon);
    }
}

void PlayPad::enterEvent(QEnterEvent *) {
    hover_ = true;
    update();
}
void PlayPad::leaveEvent(QEvent *) {
    hover_ = false;
    update();
}

// ------------------------------------------------------------- ProgressClock

namespace {
// How far the daemon's polled reading has to disagree with the locally
// computed position before it is treated as a real desync instead of
// ordinary IPC/poll jitter -- see ProgressClock::setProgress(). Deliberately
// wide: the local clock is smoother and more precise than any value that
// only ever arrives over ~400ms polling, so it stays authoritative right up
// until it is actually wrong (a bad durationMs_ estimate, or the playhead
// having drifted more than three real seconds off over a very long clip).
constexpr double kProgressResyncThreshold = 0.03;
}  // namespace

void ProgressClock::setDurationMs(double ms) { durationMs_ = std::max(1.0, ms); }

void ProgressClock::reset() { clock_.invalidate(); }

void ProgressClock::setProgress(double p) {
    p = std::clamp(p, 0.0, 1.0);
    if (!clock_.isValid()) {
        // First reading for this run of playback: this *is* the baseline,
        // not a correction to one.
        clock_.start();
        startProgress_ = p;
        return;
    }
    if (std::fabs(p - current()) > kProgressResyncThreshold) {
        clock_.restart();
        startProgress_ = p;
    }
}

double ProgressClock::current() const {
    if (!clock_.isValid()) return startProgress_;
    return std::clamp(startProgress_ + static_cast<double>(clock_.elapsed()) / durationMs_, 0.0,
                      1.0);
}

// -------------------------------------------------------------- MiniWaveform

MiniWaveform::MiniWaveform(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(28);
    anim_ = new QTimer(this);
    anim_->setInterval(16);
    connect(anim_, &QTimer::timeout, this, &MiniWaveform::animate);
}

void MiniWaveform::setPeaks(const QVector<float> &peaks) {
    peaks_ = peaks;
    update();
}

void MiniWaveform::setActive(bool on) {
    if (active_ == on) return;
    active_ = on;
    if (on) {
        anim_->start();
        // Left invalid here on purpose: the setProgress() call that always
        // immediately follows in applyInfo() is what actually establishes
        // the baseline, using the daemon's own answer for where this run of
        // playback is really starting from.
        progress_.reset();
    } else {
        anim_->stop();
    }
    update();
}

void MiniWaveform::setDurationMs(double ms) { progress_.setDurationMs(ms); }

void MiniWaveform::setProgress(double p) { progress_.setProgress(p); }

void MiniWaveform::animate() {
    if (active_) update();
}

QSize MiniWaveform::sizeHint() const { return QSize(200, 32); }
QSize MiniWaveform::minimumSizeHint() const { return QSize(48, 24); }

void MiniWaveform::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (peaks_.isEmpty()) return;

    const int n = peaks_.size();
    const double gap = 2.0;
    const double barW = std::max(1.5, (width() - gap * (n - 1)) / n);
    const double midY = height() / 2.0;

    QPainterPath bars;
    for (int i = 0; i < n; ++i) {
        const double h = std::max(2.5, static_cast<double>(peaks_[i]) * height() * 0.92);
        const double x = i * (barW + gap);
        bars.addRoundedRect(QRectF(x, midY - h / 2.0, barW, h), barW / 2.0, barW / 2.0);
    }

    p.setPen(Qt::NoPen);

    if (!active_) {
        p.fillPath(bars, Theme::Line);
        return;
    }

    // One gradient across the *whole* path rather than colouring bar by bar:
    // a per-bar decision only ever has as many steps as there are bars, so
    // the sweep visibly hitched forward once per bar no matter how smoothly
    // the playhead itself moved. A gradient is evaluated per pixel, so the
    // played/ahead boundary moves exactly as smoothly as progress_.current()
    // does, with a narrow soft edge (rather than a hard cut) at the playhead
    // itself so it doesn't look like a second, competing bar boundary.
    const double playheadX = progress_.current() * width();
    const double band = std::clamp(4.0 / std::max(1.0, static_cast<double>(width())), 0.0, 0.4);
    const double mid = std::clamp(playheadX / std::max(1.0, static_cast<double>(width())), 0.0, 1.0);
    QLinearGradient grad(0.0, 0.0, width(), 0.0);
    grad.setColorAt(0.0, Theme::Text);
    grad.setColorAt(std::max(0.0, mid - band), Theme::Text);
    grad.setColorAt(std::min(1.0, mid + band), Theme::TextDim);
    grad.setColorAt(1.0, Theme::TextDim);
    p.fillPath(bars, grad);

    QPen pen(Qt::white, 1.5);
    p.setPen(pen);
    p.drawLine(QPointF(playheadX, 0.0), QPointF(playheadX, height()));
}

// ---------------------------------------------------------------- IdBadge

IdBadge::IdBadge(QWidget *parent) : QLabel(parent) {
    setCursor(Qt::PointingHandCursor);
}

void IdBadge::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) emit clicked();
    QLabel::mousePressEvent(e);
}

// ------------------------------------------------------------------ SimplePad

namespace {
// The pad's own corner radius -- matches .sb-pad's border-radius: 14px in
// app.css exactly, one of the few pixel values worth keeping identical
// rather than merely equivalent between the two surfaces.
constexpr qreal kSimplePadRadius = 14.0;
}  // namespace

SimplePad::SimplePad(QWidget *parent) : QAbstractButton(parent) {
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    anim_ = new QTimer(this);
    anim_->setInterval(16);
    connect(anim_, &QTimer::timeout, this, &SimplePad::animate);
}

void SimplePad::setSoundName(const QString &name) {
    if (name_ == name) return;
    name_ = name;
    setToolTip(name);
    update();
}

void SimplePad::setPlaying(bool on) {
    if (playing_ == on) return;
    playing_ = on;
    if (on) {
        anim_->start();
        // Left invalid on purpose -- the setProgress() call that always
        // immediately follows in SoundboardWindow::refresh() establishes the
        // real baseline, same as MiniWaveform::setActive().
        progress_.reset();
    } else {
        anim_->stop();
    }
    update();
}

void SimplePad::setDurationMs(double ms) { progress_.setDurationMs(ms); }
void SimplePad::setProgress(double p) { progress_.setProgress(p); }

void SimplePad::animate() {
    if (playing_) update();
}

QSize SimplePad::sizeHint() const { return QSize(kSimplePadW, kSimplePadH); }

void SimplePad::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);

    if (playing_) {
        // The soft glow PlayPad also uses for "this is making noise right
        // now" -- see its own comment on why that status earns a flourish a
        // mute switch doesn't.
        QColor glow = Qt::white;
        glow.setAlpha(45);
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawRoundedRect(r.adjusted(-4, -4, 4, 4), kSimplePadRadius + 4, kSimplePadRadius + 4);
    }

    p.setPen(Qt::NoPen);
    p.setBrush(hover_ ? Theme::CardHover : Theme::Card);
    p.drawRoundedRect(r, kSimplePadRadius, kSimplePadRadius);

    QFont f = font();
    f.setBold(true);
    p.setFont(f);
    // Neutral white/grey rather than a channel tint, same reasoning as every
    // other "is this making noise" indicator on the board.
    p.setPen(playing_ ? Qt::white : Theme::Text);
    p.drawText(r.adjusted(10, 6, -10, -6), Qt::AlignCenter | Qt::TextWordWrap, name_);

    if (playing_) {
        const QPainterPath ring =
            roundedRectProgressPath(r, kSimplePadRadius, progress_.current());
        QPen pen(Qt::white, 2.2);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(ring);
    }
}

void SimplePad::enterEvent(QEnterEvent *) {
    hover_ = true;
    update();
}
void SimplePad::leaveEvent(QEvent *) {
    hover_ = false;
    update();
}

// ----------------------------------------------------------- OverlayScrollBar

OverlayScrollBar::OverlayScrollBar(QScrollArea *area, QWidget *parent)
    : QWidget(parent), area_(area) {
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::ArrowCursor);
    QScrollBar *vbar = area_->verticalScrollBar();
    connect(vbar, &QScrollBar::rangeChanged, this, &OverlayScrollBar::sync);
    connect(vbar, &QScrollBar::valueChanged, this, &OverlayScrollBar::sync);
    sync();
}

void OverlayScrollBar::sync() {
    const QScrollBar *vbar = area_->verticalScrollBar();
    setVisible(vbar->maximum() > vbar->minimum());
    update();
}

QRectF OverlayScrollBar::thumbRect() const {
    const QScrollBar *vbar = area_->verticalScrollBar();
    const int range = vbar->maximum() - vbar->minimum();
    if (range <= 0) return {};
    const double totalSpan = range + vbar->pageStep();
    const double thumbH =
        std::clamp(height() * (vbar->pageStep() / totalSpan), 24.0, static_cast<double>(height()));
    const double avail = std::max(0.0, height() - thumbH);
    const double frac = static_cast<double>(vbar->value() - vbar->minimum()) / range;
    return QRectF(1.0, frac * avail, width() - 2.0, thumbH);
}

void OverlayScrollBar::paintEvent(QPaintEvent *) {
    const QRectF r = thumbRect();
    if (r.isEmpty()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, (dragging_ || hover_) ? 140 : 80));
    p.drawRoundedRect(r, r.width() / 2.0, r.width() / 2.0);
}

void OverlayScrollBar::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = true;
    dragStartY_ = e->pos().y();
    dragStartValue_ = area_->verticalScrollBar()->value();
    update();
}

void OverlayScrollBar::mouseMoveEvent(QMouseEvent *e) {
    if (!dragging_) return;
    QScrollBar *vbar = area_->verticalScrollBar();
    const int range = vbar->maximum() - vbar->minimum();
    if (range <= 0) return;
    const double totalSpan = range + vbar->pageStep();
    const double thumbH =
        std::clamp(height() * (vbar->pageStep() / totalSpan), 24.0, static_cast<double>(height()));
    const double avail = std::max(1.0, height() - thumbH);
    const double deltaVal = (e->pos().y() - dragStartY_) / avail * range;
    vbar->setValue(std::clamp(dragStartValue_ + static_cast<int>(std::lround(deltaVal)),
                              vbar->minimum(), vbar->maximum()));
}

void OverlayScrollBar::mouseReleaseEvent(QMouseEvent *) {
    dragging_ = false;
    update();
}

void OverlayScrollBar::enterEvent(QEnterEvent *) {
    hover_ = true;
    update();
}
void OverlayScrollBar::leaveEvent(QEvent *) {
    hover_ = false;
    update();
}

// ============================================================ SoundboardSoundRow

SoundboardSoundRow::SoundboardSoundRow(const QString &id, QWidget *parent)
    : QWidget(parent), id_(id) {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    card_ = new CardBase(this);
    card_->setRadius(12);
    outer->addWidget(card_);

    auto *row = new QHBoxLayout(card_);
    row->setContentsMargins(12, 10, 14, 10);
    row->setSpacing(14);

    auto *gripLabel = new QLabel(QStringLiteral("⋮⋮"), card_);
    gripLabel->setToolTip(tr("Drag to reorder."));
    gripLabel->setCursor(Qt::SizeAllCursor);
    gripLabel->setFixedSize(16, 36);
    gripLabel->setAlignment(Qt::AlignCenter);
    QPalette gp = gripLabel->palette();
    gp.setColor(QPalette::WindowText, Theme::TextFaint);
    gripLabel->setPalette(gp);
    row->addWidget(gripLabel);
    grip_ = gripLabel;

    playPad_ = new PlayPad(card_);
    row->addWidget(playPad_);
    connect(playPad_, &QAbstractButton::clicked, this, [this] { emit playRequested(id_); });

    auto *infoCol = new QVBoxLayout;
    infoCol->setSpacing(4);

    nameLabel_ = new QLabel(card_);
    QFont nf = nameLabel_->font();
    nf.setBold(true);
    nf.setPointSizeF(nf.pointSizeF() + 1);
    nameLabel_->setFont(nf);
    nameLabel_->setFixedWidth(kSoundNameWidth);
    infoCol->addWidget(nameLabel_);

    auto *metaRow = new QHBoxLayout;
    metaRow->setSpacing(8);
    idBadge_ = new IdBadge(card_);
    QFont idFont = idBadge_->font();
    idFont.setPointSizeF(idFont.pointSizeF() * 0.82);
    idFont.setBold(true);
    idFont.setLetterSpacing(QFont::AbsoluteSpacing, 0.4);
    idBadge_->setFont(idFont);
    idBadge_->setStyleSheet(QStringLiteral(
                                "background: %1; color: %2; border-radius: 4px; padding: 1px 6px;")
                                .arg(Theme::Well.name(), Theme::TextDim.name()));
    idBadge_->setToolTip(tr("Click to copy the wavelined-cli command that plays this sound\n"
                            "(for a Stream Deck button or a keybind)."));
    connect(idBadge_, &IdBadge::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(
            QStringLiteral("wavelined-cli --soundboard-play %1").arg(id_));
        QToolTip::showText(idBadge_->mapToGlobal(QPoint(0, idBadge_->height())),
                           tr("Copied to clipboard"), idBadge_);
    });
    metaRow->addWidget(idBadge_);
    durationLabel_ = dimLabel(QString(), card_);
    QFont durFont = durationLabel_->font();
    durFont.setPointSizeF(durFont.pointSizeF() * 0.88);
    durationLabel_->setFont(durFont);
    QPalette dp = durationLabel_->palette();
    dp.setColor(QPalette::WindowText, Theme::TextFaint);
    durationLabel_->setPalette(dp);
    metaRow->addWidget(durationLabel_);
    metaRow->addStretch();
    infoCol->addLayout(metaRow);

    row->addLayout(infoCol);

    waveform_ = new MiniWaveform(card_);
    row->addWidget(waveform_, 1);

    auto *editBtn = new QPushButton(card_);
    editBtn->setIcon(QIcon(Theme::iconPixmap(QStringLiteral("edit"), Theme::TextDim, 14)));
    editBtn->setToolTip(tr("Rename, re-trim or change this sound's volume."));
    editBtn->setFixedSize(30, 30);
    editBtn->setCursor(Qt::PointingHandCursor);
    row->addWidget(editBtn);
    connect(editBtn, &QPushButton::clicked, this, [this] { emit editRequested(id_); });

    auto *removeBtn = new QPushButton(card_);
    removeBtn->setIcon(QIcon(Theme::iconPixmap(QStringLiteral("trash"), Theme::TextDim, 14)));
    removeBtn->setToolTip(tr("Remove this sound from the soundboard."));
    removeBtn->setFixedSize(30, 30);
    removeBtn->setCursor(Qt::PointingHandCursor);
    row->addWidget(removeBtn);
    connect(removeBtn, &QPushButton::clicked, this, [this] { emit removeRequested(id_); });
}

void SoundboardSoundRow::applyInfo(const SoundboardSoundInfo &info, bool playing,
                                   double progress) {
    const QFontMetrics fm(nameLabel_->font());
    nameLabel_->setText(fm.elidedText(info.name, Qt::ElideRight, kSoundNameWidth));
    nameLabel_->setToolTip(info.name);

    idBadge_->setText(info.id);

    if (info.durationMs >= 0.0) {
        const int totalSec = static_cast<int>(info.durationMs / 1000.0);
        durationLabel_->setText(QStringLiteral("%1:%2")
                                    .arg(totalSec / 60)
                                    .arg(totalSec % 60, 2, 10, QLatin1Char('0')));
    } else {
        durationLabel_->setText(QStringLiteral("…"));
    }

    QVector<float> peaks;
    if (!info.peaks.isEmpty()) {
        const QStringList parts = info.peaks.split(QLatin1Char(','), Qt::SkipEmptyParts);
        peaks.reserve(parts.size());
        for (const QString &p : parts) peaks.push_back(p.toFloat());
    }
    waveform_->setPeaks(peaks);

    // The *trimmed* length -- what progress 0..1 actually spans, matching
    // MixerService::PlaySoundboardSound's own trimStartFrames/trimEndFrames
    // -- not the full file's durationMs shown above, so the playhead moves
    // at the clip's real playing speed rather than the untrimmed file's.
    if (info.durationMs > 0.0) {
        const double end = info.trimEndMs > 0 ? std::min<double>(info.trimEndMs, info.durationMs)
                                              : info.durationMs;
        const double start = std::min<double>(info.trimStartMs, end);
        waveform_->setDurationMs(end - start);
    }

    playing_ = playing;
    playPad_->setPlaying(playing);
    waveform_->setActive(playing);
    waveform_->setProgress(progress);
    // Neutral white rather than the channel's colour: "this sound is
    // playing" is a status every channel shows the same way.
    card_->setTopStripe(playing_ ? QColor(Qt::white) : QColor());
}

// ------------------------------------------------------- SoundboardSettingsWindow

SoundboardSettingsWindow::SoundboardSettingsWindow(MixerClient *client, QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint), client_(client) {
    setWindowTitle(tr("Soundboard Settings"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    QWidget *titleBar = buildChromeTitleBar(this, tr("Soundboard Settings"));
    titleBar->setMinimumWidth(320);
    outer->addWidget(titleBar);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildPlaybackTab(), tr("Playback"));
    tabs->addTab(buildBackupsTab(), tr("Backups"));
    outer->addWidget(tabs);

    connect(client_, &MixerClient::changed, this, &SoundboardSettingsWindow::refresh);
    refresh();
}

QWidget *SoundboardSettingsWindow::buildPlaybackTab() {
    auto *content = new QWidget(this);
    auto *lay = new QVBoxLayout(content);
    lay->setContentsMargins(20, 16, 20, 18);
    lay->setSpacing(14);

    lay->addWidget(dimLabel(
        tr("Where every soundboard sound plays, and how loud -- one setting for the whole board."),
        content));

    // Audio Sharing beside Share Volume, Channel beside Hear Volume: each
    // pair describes one thing -- where the sound is heard, and how loud --
    // rather than four separate controls stacked in an order that doesn't
    // say which volume belongs to which destination.
    auto *shareRow = new QHBoxLayout;
    shareRow->setSpacing(16);

    auto *shCol = new QVBoxLayout;
    shCol->setSpacing(6);
    shCol->addLayout(captionRow(QStringLiteral("microphone"), tr("Audio Sharing"), content));
    // Plain QComboBox, same as the Channel selector below: shows the target's
    // name next to its badge instead of just the icon.
    shareBox_ = new QComboBox(content);
    shareBox_->setToolTip(tr("Add every soundboard sound to a microphone, so whoever is\n"
                             "listening hears it alongside your voice."));
    shareBox_->setFixedWidth(kSelectorWidth);
    shCol->addWidget(shareBox_);
    shareRow->addLayout(shCol);
    connect(shareBox_, &QComboBox::activated, this, [this](int idx) {
        client_->setSoundboardShareTarget(shareBox_->itemData(idx).toString());
    });

    auto *shareVolCol = new QVBoxLayout;
    shareVolCol->setSpacing(6);
    shareVolCol->addLayout(captionRow(QStringLiteral("microphone"), tr("Share Volume"), content));
    auto *shareVolRow = new QHBoxLayout;
    shareVolRow->setSpacing(8);
    shareSlider_ = new TrackSlider(content);
    shareSlider_->setRange(0, 100);
    shareVolRow->addWidget(shareSlider_, 1);
    sharePct_ = fixedReadout(QStringLiteral("100%"), content);
    shareVolRow->addWidget(sharePct_);
    shareVolCol->addLayout(shareVolRow);
    shareRow->addLayout(shareVolCol, 1);
    connect(shareSlider_, &QSlider::valueChanged, this, [this](int v) {
        sharePct_->setText(QStringLiteral("%1%").arg(v));
        client_->setSoundboardShareVolume(v / 100.0);
    });

    lay->addLayout(shareRow);

    auto *localRow = new QHBoxLayout;
    localRow->setSpacing(16);

    auto *chCol = new QVBoxLayout;
    chCol->setSpacing(6);
    chCol->addLayout(captionRow(QStringLiteral("speaker"), tr("Channel"), content));
    // A plain QComboBox rather than IconCombo: this is the one selector the
    // panel shows the name on, not just the badge, so it reads at a glance
    // rather than needing a hover to confirm which channel is picked.
    channelBox_ = new QComboBox(content);
    channelBox_->setToolTip(tr("Channel every soundboard sound plays on."));
    channelBox_->setFixedWidth(kSelectorWidth);
    chCol->addWidget(channelBox_);
    localRow->addLayout(chCol);
    connect(channelBox_, &QComboBox::activated, this, [this](int idx) {
        client_->setSoundboardChannel(channelBox_->itemData(idx).toString());
    });

    auto *localVolCol = new QVBoxLayout;
    localVolCol->setSpacing(6);
    localVolCol->addLayout(captionRow(QStringLiteral("headphones"), tr("Hear Volume"), content));
    auto *localVolRow = new QHBoxLayout;
    localVolRow->setSpacing(8);
    localSlider_ = new TrackSlider(content);
    localSlider_->setRange(0, 150);
    localVolRow->addWidget(localSlider_, 1);
    localPct_ = fixedReadout(QStringLiteral("100%"), content);
    localVolRow->addWidget(localPct_);
    localVolCol->addLayout(localVolRow);
    localRow->addLayout(localVolCol, 1);
    connect(localSlider_, &QSlider::valueChanged, this, [this](int v) {
        localPct_->setText(QStringLiteral("%1%").arg(v));
        client_->setSoundboardLocalVolume(v / 100.0);
    });

    lay->addLayout(localRow);
    lay->addStretch(1);

    return content;
}

QWidget *SoundboardSettingsWindow::buildBackupsTab() {
    auto *content = new QWidget(this);
    auto *lay = new QVBoxLayout(content);
    lay->setContentsMargins(20, 16, 20, 18);
    lay->setSpacing(14);

    lay->addWidget(dimLabel(
        tr("Export the whole board -- every sound, its name, trim and volume -- as "
           "one file, to back it up or hand it to someone else. Import adds a "
           "board from a file someone exported; your own sounds are left alone."),
        content));

    auto *exportRow = new QVBoxLayout;
    exportRow->setSpacing(6);
    exportRow->addLayout(captionRow(QStringLiteral("export"), tr("Export"), content));
    exportBtn_ = new QPushButton(tr("Export Backup…"), content);
    exportBtn_->setCursor(Qt::PointingHandCursor);
    exportBtn_->setToolTip(
        tr("Saves every sound on the board -- audio included, not just names --\n"
           "into one file."));
    connect(exportBtn_, &QPushButton::clicked, this, &SoundboardSettingsWindow::exportBackup);
    exportRow->addWidget(exportBtn_);
    lay->addLayout(exportRow);

    auto *importRow = new QVBoxLayout;
    importRow->setSpacing(6);
    importRow->addLayout(captionRow(QStringLiteral("import"), tr("Import"), content));
    importBtn_ = new QPushButton(tr("Import Backup…"), content);
    importBtn_->setCursor(Qt::PointingHandCursor);
    importBtn_->setToolTip(
        tr("Adds every sound from a backup file to this board. Sounds already\n"
           "here are left as they are -- nothing is replaced or removed."));
    connect(importBtn_, &QPushButton::clicked, this, &SoundboardSettingsWindow::importBackup);
    importRow->addWidget(importBtn_);
    lay->addLayout(importRow);

    lay->addStretch(1);

    return content;
}

void SoundboardSettingsWindow::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refresh();
}

void SoundboardSettingsWindow::populateOptions(const QList<ChannelInfo> &channels,
                                               const QStringList &shareTargets) {
    // Selection is left wherever clear() puts it: the applySettings() call
    // that always follows a rebuild sets the real current item, so nothing
    // here needs to survive the repopulation.
    {
        const QSignalBlocker b(channelBox_);
        channelBox_->clear();
        for (const auto &c : channels) channelBox_->addItem(Theme::channelBadge(c.id), c.name, c.id);
    }
    {
        const QSignalBlocker b(shareBox_);
        shareBox_->clear();
        shareBox_->addItem(Theme::noneBadge(), tr("Not shared"), QString());
        for (const QString &t : shareTargets) {
            const int tab = t.indexOf(QLatin1Char('\t'));
            if (tab < 0) continue;
            const QString targetId = t.left(tab);
            shareBox_->addItem(Theme::channelBadge(targetId), t.mid(tab + 1), targetId);
        }
    }
}

void SoundboardSettingsWindow::applySettings(const SoundboardSettingsInfo &settings) {
    const QColor channelAccent = Theme::channelColor(settings.channelId);

    {
        const QSignalBlocker b(channelBox_);
        int at = channelBox_->findData(settings.channelId);
        if (at < 0) at = channelBox_->findData(QStringLiteral("sfx"));
        channelBox_->setCurrentIndex(std::max(0, at));
    }
    {
        const QSignalBlocker b(shareBox_);
        const int at = shareBox_->findData(settings.shareTarget);
        shareBox_->setCurrentIndex(at < 0 ? 0 : at);
    }

    const bool shared = !settings.shareTarget.isEmpty();
    shareSlider_->setEnabled(shared);
    shareSlider_->setAccent(shared ? Theme::channelColor(settings.shareTarget) : Theme::Line);
    shareSlider_->setCursor(shared ? Qt::PointingHandCursor : Qt::ArrowCursor);
    shareSlider_->setToolTip(shared ? tr("How loud the soundboard is in the microphone.\n"
                                        "Does not change how loudly it plays for you.")
                                    : tr("Pick an Audio Sharing target to adjust this level."));
    {
        const QSignalBlocker b(shareSlider_);
        shareSlider_->setValue(static_cast<int>(settings.shareVolume * 100.0 + 0.5));
    }
    sharePct_->setText(QStringLiteral("%1%").arg(shareSlider_->value()));

    localSlider_->setAccent(channelAccent);
    localSlider_->setToolTip(tr("How loud the soundboard plays for you and on its channel."));
    {
        const QSignalBlocker b(localSlider_);
        localSlider_->setValue(static_cast<int>(settings.localVolume * 100.0 + 0.5));
    }
    localPct_->setText(QStringLiteral("%1%").arg(localSlider_->value()));
}

void SoundboardSettingsWindow::refresh() {
    // Set regardless of the early return below: a backup export/import is
    // just as unusable without a daemon as anything on the Playback tab is.
    exportBtn_->setEnabled(client_->available());
    importBtn_->setEnabled(client_->available());
    if (!client_->available()) return;

    const auto channels = client_->channels();
    const QStringList shareTargets = client_->soundSharingTargets();

    QString sig;
    for (const auto &c : channels) sig += c.id + QLatin1Char(',');
    sig += QLatin1Char('#') + shareTargets.join(QLatin1Char(','));
    sig += QStringLiteral("looks%1").arg(Theme::cardLooksRevision());
    if (sig != signature_) {
        populateOptions(channels, shareTargets);
        signature_ = sig;
    }

    applySettings(client_->soundboardSettings());
}

namespace {
// Identifies the file as this and not just any JSON blob -- importBackup()
// refuses anything else rather than guessing.
constexpr char kBackupFormat[] = "waveline-soundboard-backup";
constexpr int kBackupVersion = 1;
// Mirrors ProfilesWindow's own .wlprofile: a distinct extension so a file
// manager or "open with" dialog offers this app for it, even though the
// contents are ordinary JSON underneath.
constexpr char kBackupSuffix[] = ".wlsoundboard";
}  // namespace

void SoundboardSettingsWindow::exportBackup() {
    const QList<SoundboardSoundInfo> sounds = client_->soundboardSounds();
    if (sounds.isEmpty()) {
        QMessageBox::information(this, tr("Export Backup"),
                                 tr("The soundboard is empty -- there is nothing to back up."));
        return;
    }

    // Each sound's audio is read and embedded (base64, inside the same JSON
    // document as the name/trim/volume rows) rather than the backup being a
    // manifest that points at files on this machine: the whole point is that
    // it survives being copied to someone else's, or outliving this
    // install's soundboard directory entirely.
    QJsonArray arr;
    for (const auto &s : sounds) {
        QFile f(s.file);
        if (!f.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(
                this, tr("Export Backup"),
                tr("Could not read \"%1\":\n%2").arg(s.name, f.errorString()));
            return;
        }
        QJsonObject obj;
        obj[QStringLiteral("name")] = s.name;
        obj[QStringLiteral("volume")] = s.volume;
        obj[QStringLiteral("trimStartMs")] = s.trimStartMs;
        obj[QStringLiteral("trimEndMs")] = s.trimEndMs;
        // Only for its extension, on the other end -- decodeSoundFile() there
        // sniffs real content, not this name, but a sensible extension is
        // still worth keeping for anyone who goes digging in a temp dir.
        obj[QStringLiteral("fileName")] = QFileInfo(s.file).fileName();
        obj[QStringLiteral("data")] = QString::fromLatin1(f.readAll().toBase64());
        arr.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("format")] = QLatin1String(kBackupFormat);
    root[QStringLiteral("version")] = kBackupVersion;
    root[QStringLiteral("sounds")] = arr;

    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Soundboard Backup"),
        QDir(lastBackupDir_).filePath(QStringLiteral("soundboard") + QLatin1String(kBackupSuffix)),
        tr("Waveline soundboard backup (*.wlsoundboard);;All files (*)"));
    if (path.isEmpty()) return;
    // Only when the user typed no extension at all: someone who asked for
    // "backup.json" meant it.
    if (QFileInfo(path).suffix().isEmpty()) path += QLatin1String(kBackupSuffix);
    lastBackupDir_ = QFileInfo(path).absolutePath();

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Export Backup"),
                             tr("Could not write %1:\n%2").arg(path, out.errorString()));
        return;
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    out.close();

    QMessageBox::information(
        this, tr("Export Backup"),
        tr("Exported %1 sound%2 to %3.")
            .arg(sounds.size())
            .arg(sounds.size() == 1 ? QString() : QStringLiteral("s"), QFileInfo(path).fileName()));
}

void SoundboardSettingsWindow::importBackup() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import Soundboard Backup"), lastBackupDir_,
        tr("Waveline soundboard backups (*.wlsoundboard);;All files (*)"));
    if (path.isEmpty()) return;
    lastBackupDir_ = QFileInfo(path).absolutePath();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import Backup"),
                             tr("Could not read %1:\n%2").arg(path, f.errorString()));
        return;
    }
    const QByteArray blob = f.readAll();
    f.close();

    const QJsonDocument doc = QJsonDocument::fromJson(blob);
    if (!doc.isObject() ||
        doc.object().value(QStringLiteral("format")).toString() != QLatin1String(kBackupFormat)) {
        QMessageBox::warning(
            this, tr("Import Backup"),
            tr("%1 is not a Waveline soundboard backup.").arg(QFileInfo(path).fileName()));
        return;
    }
    const QJsonArray arr = doc.object().value(QStringLiteral("sounds")).toArray();
    if (arr.isEmpty()) {
        QMessageBox::information(this, tr("Import Backup"), tr("That backup has no sounds in it."));
        return;
    }

    if (QMessageBox::question(
            this, tr("Import Backup"),
            tr("Add %1 sound%2 from this backup to your soundboard?\n\n"
               "Your existing sounds are left as they are.")
                .arg(arr.size())
                .arg(arr.size() == 1 ? QString() : QStringLiteral("s"))) != QMessageBox::Yes)
        return;

    // Not QTemporaryDir: that defaults to /tmp, and wavelined.service sets
    // PrivateTmp=true -- the daemon's /tmp is a different, empty mount
    // namespace from this process's, so AddSoundboardSound's own
    // QFile::copy(sourcePath, ...) would find nothing there no matter how
    // real the file looks from here. Its ProtectHome=read-only still leaves
    // it able to *read* anywhere under $HOME, though (that's how choosing an
    // ordinary file in Add Sound's own file picker already works), so the
    // decoded audio is staged under the cache directory instead, and swept
    // away again once every sound has had its turn.
    const QString stageDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                             QStringLiteral("/soundboard-import");
    QDir(stageDir).removeRecursively();
    QDir().mkpath(stageDir);

    int added = 0;
    QStringList failed;
    for (const QJsonValue &v : arr) {
        const QJsonObject obj = v.toObject();
        const QString name = obj.value(QStringLiteral("name")).toString();
        const QByteArray data =
            QByteArray::fromBase64(obj.value(QStringLiteral("data")).toString().toLatin1());
        QString fileName = obj.value(QStringLiteral("fileName")).toString();
        if (fileName.isEmpty()) fileName = QStringLiteral("sound.wav");

        if (data.isEmpty()) {
            failed << (name.isEmpty() ? fileName : name);
            continue;
        }
        // Re-derived per sound rather than reused: two rows sharing the same
        // fileName (two different exports both called "clip.wav", say) would
        // otherwise overwrite each other in the one shared stage dir.
        const QString stagePath = stageDir + QLatin1Char('/') +
                                  QStringLiteral("%1-%2").arg(added + failed.size()).arg(fileName);
        QFile stageFile(stagePath);
        if (!stageFile.open(QIODevice::WriteOnly) || stageFile.write(data) != data.size()) {
            failed << (name.isEmpty() ? fileName : name);
            continue;
        }
        stageFile.close();

        const QString id = client_->addSoundboardSound(
            name, stagePath, obj.value(QStringLiteral("trimStartMs")).toInt(),
            obj.value(QStringLiteral("trimEndMs")).toInt(),
            obj.value(QStringLiteral("volume")).toDouble(1.0));
        if (id.isEmpty()) failed << (name.isEmpty() ? fileName : name);
        else ++added;
    }
    QDir(stageDir).removeRecursively();

    if (failed.isEmpty()) {
        QMessageBox::information(
            this, tr("Import Backup"),
            tr("Added %1 sound%2.").arg(added).arg(added == 1 ? QString() : QStringLiteral("s")));
    } else {
        QMessageBox::warning(
            this, tr("Import Backup"),
            tr("Added %1 sound%2. Could not add: %3")
                .arg(added)
                .arg(added == 1 ? QString() : QStringLiteral("s"), failed.join(QStringLiteral(", "))));
    }
}

// =============================================================== SoundboardWindow

SoundboardWindow::SoundboardWindow(MixerClient *client, QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint), client_(client) {
    setWindowTitle(tr("Soundboard"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    outer->addWidget(buildTitleBar());

    auto *content = new QWidget(this);
    // Pinned to the same width as the title bar, not just given it as a
    // floor: a row's own natural width (name label + waveform + buttons)
    // adds up to more than kSoundboardWidth, and QVBoxLayout's sizeHint is
    // the *max* across its children -- left unconstrained, that wider
    // number is what SetFixedSize below would have sized the window to,
    // leaving the title bar (actually fixed) short of the window's real
    // right edge and its gear/add buttons looking off the edge. Fixing this
    // width forces the rows area to the same width instead, and the mini
    // waveform (the one stretch=1 item in each row) simply renders
    // narrower than its own preferred size to absorb the difference --
    // completely ordinary layout behaviour, not a compromise.
    content->setFixedWidth(kSoundboardWidth);
    auto *contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(20, 16, 20, 18);
    contentLay->setSpacing(14);

    scroll_ = new QScrollArea(content);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    contentLay->addWidget(scroll_);

    // Both views' widgets are built up front and kept alive the whole time
    // (rows_/simplePads_ stay populated regardless of which is showing), but
    // only ever *one* of them is actually scroll_'s widget at a time --
    // toggling calls scroll_->takeWidget()/setWidget() to swap which, rather
    // than putting both in one QStackedWidget as scroll_'s widget. That
    // isn't just a style choice: QStackedWidget::minimumSizeHint() is the
    // *maximum* across every page it holds, current or not, so with both
    // pages living in one stack, a resizable QScrollArea sizing against the
    // stack was refusing to shrink below the rack view's own uncapped
    // minimum (every sound's row, not just the kMaxVisibleRows-worth
    // actually shown) even while the far smaller grid view was the one
    // visible -- which is what the leftover scrollable space past the last
    // pad actually was. A page that was never handed to scroll_ in the
    // first place doesn't get a vote in its sizing.
    rowsContainer_ = new QWidget(scroll_);
    rowsLay_ = new QVBoxLayout(rowsContainer_);
    rowsLay_->setContentsMargins(0, 2, 4, 2);
    rowsLay_->setSpacing(10);
    rowsLay_->addStretch(1);

    // A resizable QScrollArea resizes its widget with a direct resize()
    // call, which -- unlike a layout handing out space by stretch factor --
    // does not consult that widget's own QSizePolicy at all: whatever
    // sizeHint()-vs-viewport math decided the viewport is bigger than the
    // content, simpleContainer_ (or rowsContainer_) is getting resized to
    // that full viewport height regardless. What actually determines
    // whether that surplus becomes visible dead space is the layout
    // *inside* the resized widget, once it has to fill a rect taller than
    // its own sizeHint. rowsLay_ handles that with a trailing addStretch(1)
    // -- a QVBoxLayout with a zero-stretch real item followed by a
    // stretch-1 spacer puts 100% of any surplus into that spacer and none
    // of it into the real item, a standard and reliable Qt idiom.
    // QGridLayout has no addStretch() and no equally reliable row-stretch
    // equivalent (an earlier version of this used setRowStretch() on a bare
    // row past the last real one, which does not cleanly avoid touching the
    // real rows the way a QVBoxLayout spacer does), so simpleContainer_
    // reuses the proven mechanism instead of reinventing one for grids: the
    // grid lives in its own plain sub-widget (simpleGrid, stretch 0, so it
    // never grows past its sizeHint), and simpleContainer_'s *own* layout is
    // an ordinary QVBoxLayout with that sub-widget followed by
    // addStretch(1), exactly rowsLay_'s own shape.
    // Parented to `this`, not scroll_, while it isn't the one attached --
    // see setSimpleView() for the swap, and the comment above for why the
    // two views don't share a QStackedWidget the way an earlier version of
    // this did.
    simpleContainer_ = new QWidget(this);
    simpleOuterLay_ = new QVBoxLayout(simpleContainer_);
    simpleOuterLay_->setContentsMargins(0, 2, 4, 2);
    simpleOuterLay_->setSpacing(0);

    auto *simpleGrid = new QWidget(simpleContainer_);
    simpleLay_ = new QGridLayout(simpleGrid);
    simpleLay_->setContentsMargins(0, 0, 0, 0);
    simpleLay_->setSpacing(kSimpleGap);
    for (int c = 0; c < kSimpleCols; ++c) simpleLay_->setColumnStretch(c, 1);
    simpleOuterLay_->addWidget(simpleGrid);
    simpleOuterLay_->addStretch(1);
    simpleContainer_->hide();

    // rowsContainer_ is the initial view regardless of the saved preference
    // -- setSimpleView(), called once more at the end of the constructor
    // once the preference has actually been read, swaps to simpleContainer_
    // then if that's what was saved. scroll_ always needs *a* widget from
    // the moment it exists, so this can't just wait for that call.
    scroll_->setWidget(rowsContainer_);

    // Qt's own scrollbar is turned off -- not hidden, off -- because it
    // reserves its column purely from the policy, regardless of the
    // scrollbar widget's own visibility: even a thin one would make every
    // row a few pixels narrower the moment a 10th sound showed up than it
    // was a moment before. It keeps tracking range/value normally either
    // way, so OverlayScrollBar below can stay a thin visible/draggable
    // proxy for it, with rows staying exactly kSoundboardWidth wide
    // regardless of whether there happens to be anything to scroll.
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Parented to `content`, not scroll_->viewport(): the viewport sits
    // inset by contentLay's own margins, so a bar confined to it would land
    // on top of the cards' own right edge instead of in the empty margin
    // beside them. Parented to content instead, it can sit flush with
    // content's (and so the window's) true right edge -- see the
    // eventFilter() branch on scroll_ below for how it's kept there.
    overlayScrollBar_ = new OverlayScrollBar(scroll_, content);
    scroll_->installEventFilter(this);

    contentLay->addWidget(buildEmptyState());

    outer->addWidget(content);

    // Ties the window's size to its own content, the same way the Virtual
    // Rack's window does: no manual resize() anywhere, it grows and shrinks
    // on its own as sounds are added or removed. The rows area's own height
    // is capped well before that ever has to fight this -- see refresh()'s
    // scroll_->setFixedHeight() -- so past kMaxVisibleRows sounds this
    // stops growing and scrolls internally instead of taking over the screen.
    outer->setSizeConstraint(QLayout::SetFixedSize);

    // setChecked() is signal-blocked and setSimpleView() called explicitly
    // right after, rather than relying on toggled() to fire it: setChecked()
    // only emits when the value actually changes, and the saved value is
    // false (matching the switch's own default) far more often than not.
    {
        QSettings s;
        s.beginGroup(QStringLiteral("soundboard"));
        const bool wantSimple = s.value(QStringLiteral("simpleView"), false).toBool();
        s.endGroup();
        const QSignalBlocker b(simpleViewToggle_);
        simpleViewToggle_->setChecked(wantSimple);
    }
    setSimpleView(simpleViewToggle_->isChecked());

    connect(client_, &MixerClient::changed, this, &SoundboardWindow::refresh);
    refresh();
}

QWidget *SoundboardWindow::buildTitleBar() {
    QHBoxLayout *lay = nullptr;
    QWidget *titleBar = buildChromeTitleBar(this, tr("Soundboard"), &lay);
    // Fixed, not a floor: the window's overall width is derived from its
    // content (see the SetFixedSize constraint below), and the title bar is
    // the widest single thing in it, so pinning this pins the window.
    titleBar->setFixedWidth(kSoundboardWidth);

    countLabel_ = new QLabel(titleBar);
    QPalette cp = countLabel_->palette();
    cp.setColor(QPalette::WindowText, Theme::TextFaint);
    countLabel_->setPalette(cp);
    lay->addSpacing(8);
    lay->addWidget(countLabel_);

    lay->addStretch(1);

    simpleViewToggle_ = new ToggleSwitch(titleBar);
    simpleViewToggle_->setToolTip(
        tr("Simple view: just the sound names, 1:1 with the web companion's "
           "soundboard page, instead of the full rack with waveforms and edit "
           "controls."));
    connect(simpleViewToggle_, &QAbstractButton::toggled, this, &SoundboardWindow::setSimpleView);
    lay->addWidget(simpleViewToggle_);
    lay->addSpacing(4);

    settingsBtn_ = new QPushButton(titleBar);
    settingsBtn_->setIcon(QIcon(Theme::iconPixmap(QStringLiteral("gear"), Theme::Text, 15)));
    settingsBtn_->setToolTip(tr("Soundboard Settings: which channel every sound plays on, "
                               "whether it joins a microphone, the two live volumes, and "
                               "backups."));
    settingsBtn_->setCursor(Qt::PointingHandCursor);
    settingsBtn_->setFixedSize(kChromeBtnPx, kChromeBtnPx);
    connect(settingsBtn_, &QPushButton::clicked, this, &SoundboardWindow::showSettings);
    lay->addWidget(settingsBtn_);

    addBtn_ = new QPushButton(titleBar);
    addBtn_->setIcon(QIcon(Theme::iconPixmap(QStringLiteral("plus"), Theme::Text, 14)));
    addBtn_->setToolTip(tr("Add a sound..."));
    addBtn_->setCursor(Qt::PointingHandCursor);
    addBtn_->setFixedSize(kChromeBtnPx, kChromeBtnPx);
    connect(addBtn_, &QPushButton::clicked, this, &SoundboardWindow::onAddSound);
    lay->addWidget(addBtn_);

    return titleBar;
}

void SoundboardWindow::showSettings() {
    if (!settingsWindow_) settingsWindow_ = new SoundboardSettingsWindow(client_, this);
    settingsWindow_->show();
    settingsWindow_->raise();
    settingsWindow_->activateWindow();
}

void SoundboardWindow::setSimpleView(bool on) {
    simpleView_ = on;

    QWidget *want = on ? simpleContainer_ : rowsContainer_;
    if (scroll_->widget() != want) {
        QWidget *other = on ? rowsContainer_ : simpleContainer_;
        // takeWidget(), not deleting and rebuilding: rows_/simplePads_ and
        // every widget they point at stay alive and populated the whole
        // time, so this swap is instant and the view that was just hidden
        // comes back exactly as it was, not rebuilt from cache_. See the
        // constructor's comment on why the two never share a QStackedWidget
        // instead.
        scroll_->takeWidget();
        other->hide();
        other->setParent(this);
        scroll_->setWidget(want);
        want->show();
    }

    // The rows area's height depends on which view is showing (a grid row
    // holds kSimpleCols pads, a rack row holds one card) -- see refresh()'s
    // scroll_->setFixedHeight() -- so a bare view switch has to redo that
    // measurement even though nothing about the sounds themselves changed.
    refresh();

    QSettings s;
    s.beginGroup(QStringLiteral("soundboard"));
    s.setValue(QStringLiteral("simpleView"), on);
    s.endGroup();
}

QWidget *SoundboardWindow::buildEmptyState() {
    emptyState_ = new QWidget(this);
    auto *lay = new QVBoxLayout(emptyState_);
    lay->setContentsMargins(30, 36, 30, 36);
    lay->setSpacing(10);

    auto *icon = new QLabel(emptyState_);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(Theme::iconPixmap(QStringLiteral("sfx"), Theme::TextFaint, 44));
    lay->addWidget(icon);

    auto *heading = new QLabel(tr("Your soundboard is empty"), emptyState_);
    QFont hf = heading->font();
    hf.setBold(true);
    hf.setPointSizeF(hf.pointSizeF() + 2);
    heading->setFont(hf);
    heading->setAlignment(Qt::AlignCenter);
    lay->addWidget(heading);

    auto *sub = dimLabel(tr("Import a .wav or .mp3 clip to get started."), emptyState_);
    sub->setAlignment(Qt::AlignCenter);
    lay->addWidget(sub);

    emptyAddBtn_ = new QPushButton(tr("+ Add Sound"), emptyState_);
    emptyAddBtn_->setObjectName(QStringLiteral("primary"));
    emptyAddBtn_->setCursor(Qt::PointingHandCursor);
    connect(emptyAddBtn_, &QPushButton::clicked, this, &SoundboardWindow::onAddSound);
    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    btnRow->addWidget(emptyAddBtn_);
    btnRow->addStretch();
    lay->addLayout(btnRow);

    return emptyState_;
}

void SoundboardWindow::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refresh();
}

void SoundboardWindow::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
}

SoundboardSoundRow *SoundboardWindow::addRowWidget(const QString &id) {
    auto *row = new SoundboardSoundRow(id, rowsContainer_);
    row->grip()->installEventFilter(this);
    connect(row, &SoundboardSoundRow::playRequested, this, &SoundboardWindow::onPlayRequested);
    connect(row, &SoundboardSoundRow::editRequested, this, &SoundboardWindow::onEditRequested);
    connect(row, &SoundboardSoundRow::removeRequested, this, &SoundboardWindow::onRemoveRequested);
    rowsLay_->insertWidget(rowsLay_->count() - 1, row);
    rows_.insert(id, row);
    return row;
}

SimplePad *SoundboardWindow::addSimplePadWidget(const QString &id) {
    auto *pad = new SimplePad(simpleContainer_);
    // Play/stop only -- no reorder, no edit, no remove, the same "1:1 with
    // the web companion" restriction the companion's own #view-soundboard
    // holds itself to: dragging a rectangle of buttons around is a touch
    // gesture, and there is nothing here this window's rack view doesn't
    // already do better for anyone at a mouse and keyboard.
    connect(pad, &QAbstractButton::clicked, this,
            [this, id] { onPlayRequested(id); });
    simplePads_.insert(id, pad);
    return pad;
}

void SoundboardWindow::refresh() {
    if (!client_->available()) return;

    cache_ = client_->soundboardSounds();
    const QStringList playing = client_->soundboardPlayingIds();
    const QHash<QString, double> progress = client_->soundboardProgress();

    countLabel_->setText(cache_.isEmpty()
                             ? tr("No sounds yet")
                             : tr("%1 sound%2").arg(cache_.size()).arg(cache_.size() == 1
                                                                          ? QString()
                                                                          : QStringLiteral("s")));

    // The playing set stays out of the signature -- exactly the trick
    // SoundSharingTab uses for the same reason: a poll that only started a
    // sound must not rebuild the rack out from under the user's cursor.
    // Sound order *is* included (via the ids, in cache_'s order), so a
    // reorder still repositions rows; card-look changes are included so a
    // recolour still refreshes the badges.
    QString sig;
    for (const auto &s : cache_) sig += s.id + QLatin1Char(',');
    sig += QStringLiteral("#looks%1").arg(Theme::cardLooksRevision());
    const bool structureChanged = sig != signature_;
    signature_ = sig;

    if (structureChanged) {
        // Drop rows for sounds that no longer exist. removeWidget() first
        // and unconditionally, not just deleteLater(): a deferred delete
        // does not take its widget out of rowsLay_ until Qt actually gets
        // around to destroying it, which is later than this function
        // returns, not "immediately" -- so the row stayed fully visible,
        // still taking up its full share of space, for however long that
        // took, which on a fast machine reads as "the panel didn't shrink"
        // rather than as the one-frame flash it was supposed to be.
        QSet<QString> keep;
        for (const auto &s : cache_) keep.insert(s.id);
        for (auto it = rows_.begin(); it != rows_.end();) {
            if (!keep.contains(it.key())) {
                rowsLay_->removeWidget(it.value());
                // removeWidget() only stops the layout from managing it --
                // the widget itself stays exactly where it was last painted
                // until the deferred delete below actually runs, so without
                // this it would sit on screen, stale, overlapping whatever
                // the rows below it just got laid out into.
                it.value()->hide();
                it.value()->deleteLater();
                it = rows_.erase(it);
            } else {
                ++it;
            }
        }
        // Add rows for new sounds, and put every row in the daemon's order.
        for (const auto &s : cache_) {
            SoundboardSoundRow *row = rows_.value(s.id);
            if (!row) row = addRowWidget(s.id);
            rowsLay_->removeWidget(row);
        }
        int insertAt = 0;
        for (const auto &s : cache_) {
            rowsLay_->insertWidget(insertAt++, rows_.value(s.id));
        }

        // The simple view's pads, same drop/add/reposition dance as the rack
        // rows just above -- `keep` already reflects the current sound set,
        // so it applies here unchanged.
        for (auto it = simplePads_.begin(); it != simplePads_.end();) {
            if (!keep.contains(it.key())) {
                simpleLay_->removeWidget(it.value());
                it.value()->hide();
                it.value()->deleteLater();
                it = simplePads_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto &s : cache_) {
            SimplePad *pad = simplePads_.value(s.id);
            if (!pad) pad = addSimplePadWidget(s.id);
            simpleLay_->removeWidget(pad);
        }
        int gridAt = 0;
        for (const auto &s : cache_) {
            simpleLay_->addWidget(simplePads_.value(s.id), gridAt / kSimpleCols,
                                  gridAt % kSimpleCols);
            ++gridAt;
        }
    }

    for (const auto &s : cache_) {
        const bool isPlaying = playing.contains(s.id);
        const double p = progress.value(s.id, 0.0);
        if (SoundboardSoundRow *row = rows_.value(s.id)) row->applyInfo(s, isPlaying, p);
        if (SimplePad *pad = simplePads_.value(s.id)) {
            pad->setSoundName(s.name);
            // The trimmed length, same reasoning as SoundboardSoundRow's own
            // waveform_->setDurationMs() call -- see its comment.
            if (s.durationMs > 0.0) {
                const double end = s.trimEndMs > 0 ? std::min<double>(s.trimEndMs, s.durationMs)
                                                    : s.durationMs;
                const double start = std::min<double>(s.trimStartMs, end);
                pad->setDurationMs(end - start);
            }
            pad->setPlaying(isPlaying);
            pad->setProgress(p);
        }
    }

    // Pinned to exactly min(row count, kMaxVisibleRows) rows worth of
    // height, every refresh rather than only on a structural change: this
    // used to run inside `if (structureChanged)` and read either a single
    // row's sizeHint() times a hand-tracked count, or rowsContainer_'s own
    // sizeHint() -- both of which could observe a row before applyInfo()
    // above had populated it with real content (name, duration, waveform
    // peaks), on exactly the first refresh a freshly-opened window with
    // sounds already on the board makes, which is what undersized it by
    // about one row's worth. Running this after applyInfo(), unconditionally,
    // means the measurement is always taken from fully-populated rows, and a
    // wrong measurement on any one refresh -- from this or any other cause --
    // corrects itself on the very next one instead of sticking until the
    // next add or remove. What counts as "a row" depends on which view is
    // showing -- one card in the rack, or a strip of kSimpleCols pads in the
    // grid -- so the two views are measured separately rather than sharing
    // one calculation.
    if (simpleView_) {
        if (!simplePads_.isEmpty()) {
            const int padH = (*simplePads_.begin())->sizeHint().height();
            const int spacing = simpleLay_->verticalSpacing();
            // simpleOuterLay_'s margins, not simpleLay_'s -- see the
            // constructor's comment on why the grid itself is margin-free
            // and its surrounding QVBoxLayout carries them instead.
            const QMargins m = simpleOuterLay_->contentsMargins();
            const int gridRows = (cache_.size() + kSimpleCols - 1) / kSimpleCols;
            const int visible = std::min(gridRows, kMaxVisibleRows);
            scroll_->setFixedHeight(visible * padH + std::max(0, visible - 1) * spacing +
                                    m.top() + m.bottom());
        }
    } else if (!rows_.isEmpty()) {
        const int rowH = (*rows_.begin())->sizeHint().height();
        const int spacing = rowsLay_->spacing();
        const QMargins m = rowsLay_->contentsMargins();
        const int visible = std::min(static_cast<int>(cache_.size()), kMaxVisibleRows);
        scroll_->setFixedHeight(visible * rowH + std::max(0, visible - 1) * spacing +
                                m.top() + m.bottom());
    }

    emptyState_->setVisible(cache_.isEmpty());
    scroll_->setVisible(!cache_.isEmpty());
}

void SoundboardWindow::onPlayRequested(const QString &id) {
    // Toggling off a currently-playing sound reads more naturally as "stop
    // it" than as "play one more overlapping instance" -- the pad already
    // glows while it is playing, so this is what it promises.
    if (client_->soundboardPlayingIds().contains(id)) {
        client_->stopSoundboardSound(id);
        return;
    }
    // playSoundboardSound() is async (see its own comment on why) -- guarded
    // by a QPointer rather than capturing `this` bare, since the reply can
    // land after this window has already been closed and destroyed.
    const QPointer<SoundboardWindow> guard(this);
    client_->playSoundboardSound(id, [guard](const QString &err) {
        if (!guard || err.isEmpty()) return;
        QMessageBox::warning(guard, tr("Soundboard"),
                             tr("Could not play that sound.\n\n%1").arg(err));
    });
}

void SoundboardWindow::onAddSound() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Add Sound"), lastDir_, tr("Audio files (*.wav *.mp3)"));
    if (path.isEmpty()) return;
    lastDir_ = QFileInfo(path).absolutePath();

    SoundTrimDialog dlg(client_, path, nullptr, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString id = client_->addSoundboardSound(dlg.soundName(), dlg.sourcePath(),
                                                    dlg.trimStartMs(), dlg.trimEndMs(),
                                                    dlg.volume());
    if (id.isEmpty()) {
        QMessageBox::warning(this, tr("Add Sound"),
                             tr("Could not add that sound.\n\n%1").arg(client_->lastError()));
    }
}

void SoundboardWindow::onEditRequested(const QString &id) {
    const auto it = std::find_if(cache_.begin(), cache_.end(),
                                 [&](const SoundboardSoundInfo &s) { return s.id == id; });
    if (it == cache_.end()) return;
    // A copy, not a reference into cache_: dlg.exec() below runs a nested
    // event loop, and this window's own poll-driven refresh() keeps firing
    // while it does, reassigning cache_ (and invalidating `it`) out from
    // under it. Reading through `it` after the dialog closes was reading
    // freed memory -- the file-path comparison below would come out wrong
    // essentially at random, which is exactly what was turning an ordinary
    // "just edit the trim" into "replace the file with itself", something
    // MixerService::UpdateSoundboardSound cannot actually do (it deletes the
    // old file before copying the new one, and here they were the same file).
    const SoundboardSoundInfo info = *it;

    SoundTrimDialog dlg(client_, info.file, &info, this);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString newSource = dlg.sourcePath() == info.file ? QString() : dlg.sourcePath();
    if (!client_->updateSoundboardSound(id, dlg.soundName(), newSource, dlg.trimStartMs(),
                                        dlg.trimEndMs(), dlg.volume())) {
        QMessageBox::warning(this, tr("Edit Sound"),
                             tr("Could not save changes.\n\n%1").arg(client_->lastError()));
    }
}

void SoundboardWindow::onRemoveRequested(const QString &id) {
    const auto it = std::find_if(cache_.begin(), cache_.end(),
                                 [&](const SoundboardSoundInfo &s) { return s.id == id; });
    const QString name = it != cache_.end() ? it->name : id;
    if (QMessageBox::question(this, tr("Remove Sound"),
                              tr("Remove \"%1\" from the soundboard? This cannot be undone.")
                                  .arg(name)) != QMessageBox::Yes)
        return;
    client_->removeSoundboardSound(id);
}

void SoundboardWindow::syncOrderFromRows() {
    QStringList order;
    for (int i = 0; i < rowsLay_->count(); ++i) {
        auto *row = qobject_cast<SoundboardSoundRow *>(rowsLay_->itemAt(i)->widget());
        if (row) order << row->id();
    }
    client_->reorderSoundboardSounds(order);
}

bool SoundboardWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == scroll_ &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        // Keeps the overlay scrollbar pinned to content's true right edge
        // (matching the window's own, since content is fixed at
        // kSoundboardWidth) and aligned with scroll_'s own vertical span.
        // It has no layout of its own to do this for it -- it is a plain
        // floating child, which is the whole point of it not costing any
        // width -- so this is what has to.
        constexpr int kBarWidth = 8;
        constexpr int kEdgeGap = 3;
        overlayScrollBar_->setGeometry(kSoundboardWidth - kBarWidth - kEdgeGap, scroll_->y(),
                                       kBarWidth, scroll_->height());
        return false;
    }

    auto *grip = qobject_cast<QWidget *>(watched);
    if (!grip) return QWidget::eventFilter(watched, event);

    SoundboardSoundRow *owner = nullptr;
    for (SoundboardSoundRow *row : std::as_const(rows_)) {
        if (row->grip() == grip) {
            owner = row;
            break;
        }
    }
    if (!owner) return QWidget::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            dragRow_ = owner;
            return true;
        }
    } else if (event->type() == QEvent::MouseMove && dragRow_ == owner) {
        auto *me = static_cast<QMouseEvent *>(event);
        const QPoint localPos = rowsContainer_->mapFromGlobal(me->globalPosition().toPoint());
        QWidget *under = rowsContainer_->childAt(localPos);
        while (under && !qobject_cast<SoundboardSoundRow *>(under)) under = under->parentWidget();
        auto *target = qobject_cast<SoundboardSoundRow *>(under);
        if (target && target != dragRow_) {
            const int from = rowsLay_->indexOf(dragRow_);
            const int to = rowsLay_->indexOf(target);
            if (from >= 0 && to >= 0 && from != to) {
                rowsLay_->removeWidget(dragRow_);
                rowsLay_->insertWidget(to, dragRow_);
            }
        }
        return true;
    } else if (event->type() == QEvent::MouseButtonRelease && dragRow_ == owner) {
        dragRow_ = nullptr;
        syncOrderFromRows();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

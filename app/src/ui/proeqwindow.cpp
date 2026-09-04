// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "proeqwindow.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QAction>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "engine/biquad.h"
#include "mixerclient.h"
#include "theme.h"
#include "widgets.h"

using waveline::Biquad;
using waveline::EqBand;
using waveline::EqBands;
using waveline::EqBandType;
using waveline::kProEqBands;

namespace {

// The curve is drawn for the rate the filters actually run at, so what is on
// screen is the response of the thing making the sound and not an idealised
// analogue sketch of it -- which matters most exactly where people look, up
// near Nyquist where a digital shelf stops behaving like its analogue cousin.
constexpr float kDisplayRate = 48000.0f;

constexpr double kMinHz = double(waveline::kEqMinHz);
constexpr double kMaxHz = double(waveline::kEqMaxHz);
constexpr double kMinDb = double(waveline::kEqMinDb);
constexpr double kMaxDb = double(waveline::kEqMaxDb);

// Ten hues far enough apart to be told apart at the size of a 7 px dot.
QColor bandColor(int index) {
    static constexpr int kHues[kProEqBands] = {6, 32, 52, 92, 152, 176, 202, 232, 276, 318};
    return QColor::fromHsv(kHues[index % kProEqBands], 165, 240);
}

QString typeName(EqBandType t) {
    switch (t) {
        case EqBandType::Peak:      return QObject::tr("Bell");
        case EqBandType::LowShelf:  return QObject::tr("Low shelf");
        case EqBandType::HighShelf: return QObject::tr("High shelf");
        case EqBandType::HighPass:  return QObject::tr("High-pass");
        case EqBandType::LowPass:   return QObject::tr("Low-pass");
        case EqBandType::Notch:     return QObject::tr("Notch");
        case EqBandType::BandPass:  return QObject::tr("Band-pass");
    }
    return QObject::tr("Bell");
}

QString shortFreq(double hz) {
    if (hz >= 10000.0) return QStringLiteral("%1k").arg(hz / 1000.0, 0, 'f', 0);
    if (hz >= 1000.0) {
        const double k = hz / 1000.0;
        return QStringLiteral("%1k").arg(k, 0, 'f', k < 10.0 && std::fmod(k, 1.0) > 0.05 ? 1 : 0);
    }
    return QStringLiteral("%1").arg(hz, 0, 'f', 0);
}

// ------------------------------------------------------------------ presets
// Every preset starts from the flat default layout and switches on the bands
// listed here, so a preset only ever has to say what it does -- and anything
// it does not mention is guaranteed to be off rather than left over from
// whatever was loaded before.
struct PresetBand {
    int slot;
    EqBandType type;
    float freq;
    float gainDb;
    float q;
};

struct EqPreset {
    const char *name;
    const char *tip;
    std::vector<PresetBand> bands;
};

const std::vector<EqPreset> &presets() {
    static const std::vector<EqPreset> kPresets = {
        {QT_TR_NOOP("Flat"), QT_TR_NOOP("Every band off. The EQ does nothing."), {}},
        {QT_TR_NOOP("Broadcast Voice"),
         QT_TR_NOOP("Rumble out, mud out, presence and air in -- the standard "
                    "radio voicing."),
         {{0, EqBandType::HighPass, 70.0f, 0.0f, 0.707f},
          {1, EqBandType::LowShelf, 130.0f, -2.0f, 0.707f},
          {2, EqBandType::Peak, 300.0f, -3.0f, 1.2f},
          {4, EqBandType::Peak, 1000.0f, 1.0f, 1.0f},
          {5, EqBandType::Peak, 2500.0f, 2.5f, 1.2f},
          {6, EqBandType::Peak, 5000.0f, 2.0f, 1.5f},
          {7, EqBandType::Peak, 7400.0f, -3.0f, 4.0f},
          {8, EqBandType::HighShelf, 11000.0f, 2.5f, 0.707f}}},
        {QT_TR_NOOP("Warm & Close"),
         QT_TR_NOOP("Fuller low end and a softer top. Good on a thin microphone."),
         {{0, EqBandType::HighPass, 45.0f, 0.0f, 0.707f},
          {1, EqBandType::LowShelf, 160.0f, 3.0f, 0.707f},
          {3, EqBandType::Peak, 420.0f, -1.5f, 1.2f},
          {6, EqBandType::Peak, 3200.0f, -1.5f, 1.0f},
          {8, EqBandType::HighShelf, 9000.0f, -1.5f, 0.707f}}},
        {QT_TR_NOOP("Bright & Airy"),
         QT_TR_NOOP("Opens up the top end and thins the body."),
         {{0, EqBandType::HighPass, 60.0f, 0.0f, 0.707f},
          {2, EqBandType::Peak, 240.0f, -2.0f, 1.0f},
          {6, EqBandType::Peak, 4200.0f, 2.5f, 1.0f},
          {8, EqBandType::HighShelf, 10000.0f, 4.0f, 0.707f}}},
        {QT_TR_NOOP("Podcast"),
         QT_TR_NOOP("Even and easy to listen to for an hour, with the sibilance "
                    "held down."),
         {{0, EqBandType::HighPass, 80.0f, 0.0f, 0.707f},
          {1, EqBandType::LowShelf, 110.0f, -1.5f, 0.707f},
          {2, EqBandType::Peak, 250.0f, -2.5f, 1.1f},
          {5, EqBandType::Peak, 1600.0f, 1.5f, 1.0f},
          {6, EqBandType::Peak, 3500.0f, 2.0f, 1.2f},
          {7, EqBandType::Peak, 6500.0f, -2.5f, 5.0f},
          {8, EqBandType::HighShelf, 12000.0f, 2.0f, 0.707f}}},
        {QT_TR_NOOP("Vocal Clarity"),
         QT_TR_NOOP("Cuts the boxiness and lifts the range speech is understood in."),
         {{0, EqBandType::HighPass, 90.0f, 0.0f, 0.707f},
          {2, EqBandType::Peak, 200.0f, -3.0f, 1.4f},
          {3, EqBandType::Peak, 800.0f, -1.5f, 1.2f},
          {5, EqBandType::Peak, 2800.0f, 3.5f, 1.1f},
          {7, EqBandType::Peak, 6000.0f, 1.5f, 1.2f}}},
        {QT_TR_NOOP("Bass Boost"),
         QT_TR_NOOP("Weight underneath, with the rumble below it still removed."),
         {{0, EqBandType::HighPass, 25.0f, 0.0f, 0.707f},
          {1, EqBandType::LowShelf, 90.0f, 6.0f, 0.707f},
          {2, EqBandType::Peak, 220.0f, -1.0f, 1.0f}}},
        {QT_TR_NOOP("Loudness (V-shape)"),
         QT_TR_NOOP("Both ends up, middle down. Flattering at low listening levels."),
         {{1, EqBandType::LowShelf, 100.0f, 4.5f, 0.707f},
          {4, EqBandType::Peak, 700.0f, -2.5f, 0.9f},
          {8, EqBandType::HighShelf, 8000.0f, 4.0f, 0.707f}}},
        {QT_TR_NOOP("Music Bus"),
         QT_TR_NOOP("A gentle smile for game and music channels rather than a voice."),
         {{1, EqBandType::LowShelf, 80.0f, 1.5f, 0.707f},
          {3, EqBandType::Peak, 400.0f, -1.5f, 1.0f},
          {6, EqBandType::Peak, 3500.0f, 1.0f, 1.0f},
          {8, EqBandType::HighShelf, 11000.0f, 2.0f, 0.707f}}},
        {QT_TR_NOOP("De-Rumble & De-Ess"),
         QT_TR_NOOP("Corrective only: desk thumps out of the bottom, harsh S out "
                    "of the top."),
         {{0, EqBandType::HighPass, 100.0f, 0.0f, 0.8f},
          {6, EqBandType::Peak, 6500.0f, -5.0f, 6.0f},
          {7, EqBandType::Peak, 8600.0f, -3.0f, 6.0f}}},
        {QT_TR_NOOP("Telephone / Radio"),
         QT_TR_NOOP("A deliberate effect: everything outside a phone line thrown away."),
         {{0, EqBandType::HighPass, 400.0f, 0.0f, 0.9f},
          {5, EqBandType::Peak, 1600.0f, 4.0f, 1.0f},
          {9, EqBandType::LowPass, 3000.0f, 0.0f, 0.9f}}},
    };
    return kPresets;
}

EqBands presetBands(int presetIndex) {
    EqBands b = waveline::defaultEqBands();
    if (presetIndex < 0 || presetIndex >= int(presets().size())) return b;
    for (const PresetBand &pb : presets()[size_t(presetIndex)].bands) {
        if (pb.slot < 0 || pb.slot >= kProEqBands) continue;
        EqBand &band = b[size_t(pb.slot)];
        band.on = true;
        band.type = pb.type;
        band.freq = pb.freq;
        band.gainDb = pb.gainDb;
        band.q = pb.q;
        waveline::clampEqBand(band);
    }
    return b;
}

QString encode(const EqBands &b) {
    return QString::fromStdString(waveline::encodeEqBands(b));
}

}  // namespace

// ============================================================= EqCurveView

EqCurveView::EqCurveView(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumHeight(240);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setToolTip(tr("Drag a dot to move a band. Scroll over one to change its width.\n"
                  "Double-click empty space to add a band, or a dot to switch it off.\n"
                  "Right-click a dot for its filter type."));
}

QSize EqCurveView::sizeHint() const { return QSize(720, 300); }

void EqCurveView::setBands(const EqBands &b) {
    bands_ = b;
    update();
}

void EqCurveView::setSelected(int index) {
    if (index < 0 || index >= kProEqBands || index == selected_) return;
    selected_ = index;
    update();
}

void EqCurveView::setEqEnabled(bool on) {
    if (eqEnabled_ == on) return;
    eqEnabled_ = on;
    update();
}

QRectF EqCurveView::plotRect() const {
    // Room on the left for the dB scale and underneath for the frequencies;
    // the curve gets everything else.
    return QRectF(rect()).adjusted(40, 10, -10, -20);
}

double EqCurveView::xForFreq(double hz) const {
    const QRectF r = plotRect();
    const double t = std::log(std::max(hz, kMinHz) / kMinHz) / std::log(kMaxHz / kMinHz);
    return r.left() + t * r.width();
}

double EqCurveView::freqForX(double x) const {
    const QRectF r = plotRect();
    if (r.width() <= 0.0) return 1000.0;
    const double t = (x - r.left()) / r.width();
    const double hz = kMinHz * std::pow(kMaxHz / kMinHz, t);
    return std::clamp(hz, kMinHz, kMaxHz);
}

double EqCurveView::yForDb(double db) const {
    const QRectF r = plotRect();
    const double t = (kMaxDb - db) / (kMaxDb - kMinDb);
    return r.top() + t * r.height();
}

double EqCurveView::dbForY(double y) const {
    const QRectF r = plotRect();
    if (r.height() <= 0.0) return 0.0;
    const double t = (y - r.top()) / r.height();
    return std::clamp(kMaxDb - t * (kMaxDb - kMinDb), kMinDb, kMaxDb);
}

QPointF EqCurveView::nodePos(int index) const {
    const EqBand &b = bands_[size_t(index)];
    // A filter with no gain has no natural height, so its handle sits on the
    // zero line where the curve it makes actually crosses.
    const double db = waveline::eqTypeUsesGain(b.type) ? double(b.gainDb) : 0.0;
    return QPointF(xForFreq(double(b.freq)), yForDb(db));
}

int EqCurveView::bandAt(const QPointF &p, double radius) const {
    int best = -1;
    double bestDist = radius * radius;
    // Backwards, so the band drawn on top -- the selected one -- is the one
    // picked up when two sit on each other.
    for (int i = kProEqBands - 1; i >= 0; --i) {
        if (!bands_[size_t(i)].on && i != selected_) continue;
        const QPointF d = nodePos(i) - p;
        const double dist = d.x() * d.x() + d.y() * d.y();
        if (dist <= bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

void EqCurveView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF full = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(Theme::Line, 1));
    p.setBrush(Theme::Well);
    p.drawRoundedRect(full, 8, 8);

    const QRectF r = plotRect();
    if (r.width() < 40 || r.height() < 40) return;

    // ---- grid
    static constexpr double kGridHz[] = {20,   30,   40,   50,   60,   80,   100,  200,
                                         300,  400,  500,  600,  800,  1000, 2000, 3000,
                                         4000, 5000, 6000, 8000, 10000, 20000};
    static constexpr double kLabelHz[] = {30, 100, 300, 1000, 3000, 10000};
    QFont small = font();
    small.setPointSizeF(std::max(7.0, font().pointSizeF() - 2.0));
    p.setFont(small);

    for (double hz : kGridHz) {
        const double x = xForFreq(hz);
        bool major = false;
        for (double lhz : kLabelHz) major = major || std::fabs(hz - lhz) < 0.5;
        p.setPen(QPen(major ? Theme::Line.lighter(130) : Theme::Line, 1));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        if (major) {
            p.setPen(Theme::TextFaint);
            p.drawText(QRectF(x - 24, r.bottom() + 2, 48, 16),
                       Qt::AlignHCenter | Qt::AlignTop, shortFreq(hz));
        }
    }

    for (int db = int(kMinDb); db <= int(kMaxDb); db += 6) {
        const double y = yForDb(db);
        const bool zero = db == 0;
        p.setPen(QPen(zero ? Theme::Line.lighter(160) : Theme::Line, zero ? 1.4 : 1.0));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(Theme::TextFaint);
        p.drawText(QRectF(0, y - 8, r.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                   db > 0 ? QStringLiteral("+%1").arg(db) : QString::number(db));
    }

    // ---- the filters, built once and then evaluated across the width
    Biquad filters[kProEqBands];
    bool live[kProEqBands] = {};
    for (int i = 0; i < kProEqBands; ++i) {
        const EqBand &b = bands_[size_t(i)];
        live[i] = b.on;
        if (!live[i]) continue;
        filters[i] = Biquad::forBand(kDisplayRate, b.type, b.freq, b.gainDb, b.q);
    }

    const int steps = std::max(2, int(r.width()));
    std::vector<double> total(size_t(steps + 1), 0.0);
    std::vector<double> hzAt(size_t(steps + 1), 0.0);
    for (int s = 0; s <= steps; ++s) {
        const double x = r.left() + r.width() * double(s) / double(steps);
        hzAt[size_t(s)] = freqForX(x);
    }

    const double dim = eqEnabled_ ? 1.0 : 0.35;

    // Per-band curves under the sum, so it is clear which dot is responsible
    // for which bump.
    for (int i = 0; i < kProEqBands; ++i) {
        if (!live[i]) continue;
        QPainterPath path;
        for (int s = 0; s <= steps; ++s) {
            const double db = double(filters[i].magnitudeDb(kDisplayRate, float(hzAt[size_t(s)])));
            total[size_t(s)] += db;
            const double x = r.left() + r.width() * double(s) / double(steps);
            const double y = yForDb(std::clamp(db, kMinDb, kMaxDb));
            if (s == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        QColor c = bandColor(i);
        c.setAlphaF(float((i == selected_ ? 0.55 : 0.3) * dim));
        p.setPen(QPen(c, i == selected_ ? 1.6 : 1.1));
        p.setBrush(Qt::NoBrush);
        p.setClipRect(r);
        p.drawPath(path);
        p.setClipping(false);
    }

    // ---- the sum
    QPainterPath curve;
    for (int s = 0; s <= steps; ++s) {
        const double x = r.left() + r.width() * double(s) / double(steps);
        const double y = yForDb(std::clamp(total[size_t(s)], kMinDb, kMaxDb));
        if (s == 0) curve.moveTo(x, y);
        else curve.lineTo(x, y);
    }
    QPainterPath fill = curve;
    fill.lineTo(r.right(), yForDb(0));
    fill.lineTo(r.left(), yForDb(0));
    fill.closeSubpath();

    p.setClipRect(r);
    QColor fillColor = Theme::Accent;
    fillColor.setAlphaF(float(0.16 * dim));
    p.setPen(Qt::NoPen);
    p.setBrush(fillColor);
    p.drawPath(fill);

    QColor lineColor = eqEnabled_ ? Theme::Accent : Theme::TextDim;
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(lineColor, 2.0));
    p.drawPath(curve);
    p.setClipping(false);

    // ---- handles
    QFont numFont = font();
    numFont.setPointSizeF(std::max(7.0, font().pointSizeF() - 2.5));
    numFont.setBold(true);
    for (int i = 0; i < kProEqBands; ++i) {
        const EqBand &b = bands_[size_t(i)];
        const bool sel = i == selected_;
        if (!b.on && !sel && i != hovered_) continue;

        const QPointF c = nodePos(i);
        const double rad = sel ? 9.0 : 7.0;
        QColor col = bandColor(i);
        if (!b.on) col = col.darker(220);
        col.setAlphaF(float((b.on ? 1.0 : 0.75) * dim));

        if (sel) {
            QColor halo = col;
            halo.setAlphaF(float(0.25 * dim));
            p.setPen(Qt::NoPen);
            p.setBrush(halo);
            p.drawEllipse(c, rad + 5, rad + 5);
        }
        p.setPen(QPen(sel ? Theme::Text : Theme::Bg, sel ? 2.0 : 1.5));
        p.setBrush(b.on ? col : QBrush(Qt::NoBrush));
        p.drawEllipse(c, rad, rad);

        p.setFont(numFont);
        p.setPen(b.on ? Theme::glyphOn(col) : col);
        p.drawText(QRectF(c.x() - rad, c.y() - rad, rad * 2, rad * 2), Qt::AlignCenter,
                   QString::number(i + 1));
    }

    // ---- readout for whichever band is under discussion
    const int show = hovered_ >= 0 ? hovered_ : selected_;
    if (show >= 0) {
        const EqBand &b = bands_[size_t(show)];
        QString text = QStringLiteral("%1  %2  %3 Hz")
                           .arg(show + 1)
                           .arg(typeName(b.type))
                           .arg(double(b.freq), 0, 'f', b.freq < 100.0f ? 1 : 0);
        if (waveline::eqTypeUsesGain(b.type))
            text += QStringLiteral("  %1%2 dB")
                        .arg(b.gainDb >= 0.0f ? QStringLiteral("+") : QString())
                        .arg(double(b.gainDb), 0, 'f', 1);
        text += QStringLiteral("  Q %1").arg(double(b.q), 0, 'f', 2);
        if (!b.on) text += QStringLiteral("  (off)");

        p.setFont(small);
        p.setPen(Theme::TextDim);
        p.drawText(QRectF(r.left() + 8, r.top() + 4, r.width() - 16, 16),
                   Qt::AlignLeft | Qt::AlignTop, text);
    }
}

void EqCurveView::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    const int hit = bandAt(e->position(), 14.0);
    if (hit >= 0) {
        dragging_ = hit;
        if (hit != selected_) {
            selected_ = hit;
            emit selectionChanged(selected_);
        }
        update();
        return;
    }
    // Clicking the empty graph picks the nearest band by frequency, so the
    // scroll wheel and the editor below always act on something visible.
    const int near = bandAt(e->position(), 1e9);
    if (near >= 0 && near != selected_) {
        selected_ = near;
        emit selectionChanged(selected_);
        update();
    }
}

void EqCurveView::mouseMoveEvent(QMouseEvent *e) {
    if (dragging_ >= 0) {
        EqBand &b = bands_[size_t(dragging_)];
        b.freq = float(freqForX(e->position().x()));
        if (waveline::eqTypeUsesGain(b.type)) {
            b.gainDb = float(dbForY(e->position().y()));
        } else {
            // Nothing to raise or lower, so the vertical axis drives width
            // instead of doing nothing: up is narrower, which is the way a
            // resonance peak grows.
            const QRectF r = plotRect();
            const double t = std::clamp((r.bottom() - e->position().y()) / r.height(), 0.0, 1.0);
            b.q = float(waveline::kEqMinQ *
                        std::pow(double(waveline::kEqMaxQ / waveline::kEqMinQ), t));
        }
        waveline::clampEqBand(b);
        update();
        emit bandEdited(dragging_);
        return;
    }

    const int hit = bandAt(e->position(), 14.0);
    if (hit != hovered_) {
        hovered_ = hit;
        setCursor(hit >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
        update();
    }
}

void EqCurveView::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && dragging_ >= 0) {
        dragging_ = -1;
        update();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void EqCurveView::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    const int hit = bandAt(e->position(), 14.0);
    if (hit >= 0) {
        bands_[size_t(hit)].on = !bands_[size_t(hit)].on;
        selected_ = hit;
        update();
        emit selectionChanged(selected_);
        emit bandEdited(hit);
        return;
    }

    // Empty space: switch on the first spare band and put it where the user
    // clicked. Adding a band should not also be a hunt for which of the ten
    // happens to be free.
    for (int i = 0; i < kProEqBands; ++i) {
        EqBand &b = bands_[size_t(i)];
        if (b.on) continue;
        b.on = true;
        b.type = EqBandType::Peak;
        b.freq = float(freqForX(e->position().x()));
        b.gainDb = float(dbForY(e->position().y()));
        b.q = 1.0f;
        waveline::clampEqBand(b);
        selected_ = i;
        update();
        emit selectionChanged(selected_);
        emit bandEdited(i);
        return;
    }
}

void EqCurveView::wheelEvent(QWheelEvent *e) {
    const int hit = bandAt(e->position(), 14.0);
    const int target = hit >= 0 ? hit : selected_;
    if (target < 0) {
        e->ignore();
        return;
    }
    const int notches = e->angleDelta().y();
    if (notches == 0) {
        e->ignore();
        return;
    }
    EqBand &b = bands_[size_t(target)];
    // Multiplicative, because Q is: a fixed step would crawl at 12 and leap
    // from 0.2 to 0.7.
    b.q *= std::pow(1.12f, float(notches) / 120.0f);
    waveline::clampEqBand(b);
    if (target != selected_) {
        selected_ = target;
        emit selectionChanged(selected_);
    }
    update();
    emit bandEdited(target);
    e->accept();
}

void EqCurveView::chooseType(int index, EqBandType type) {
    EqBand &b = bands_[size_t(index)];
    if (b.type == type) return;
    b.type = type;
    waveline::clampEqBand(b);
    update();
    emit bandEdited(index);
}

void EqCurveView::contextMenuEvent(QContextMenuEvent *e) {
    const int hit = bandAt(QPointF(e->pos()), 14.0);
    const int target = hit >= 0 ? hit : selected_;
    if (target < 0) return;
    if (target != selected_) {
        selected_ = target;
        emit selectionChanged(selected_);
        update();
    }

    QMenu menu(this);
    menu.addAction(tr("Band %1").arg(target + 1))->setEnabled(false);
    menu.addSeparator();

    EqBand &b = bands_[size_t(target)];
    QAction *toggle = menu.addAction(b.on ? tr("Switch off") : tr("Switch on"));
    menu.addSeparator();
    for (int t = 0; t < waveline::kEqBandTypeCount; ++t) {
        const auto type = static_cast<EqBandType>(t);
        QAction *a = menu.addAction(typeName(type));
        a->setCheckable(true);
        a->setChecked(b.type == type);
        a->setData(t);
    }
    menu.addSeparator();
    QAction *reset = menu.addAction(tr("Reset this band"));

    QAction *chosen = menu.exec(e->globalPos());
    if (!chosen) return;
    if (chosen == toggle) {
        b.on = !b.on;
        update();
        emit bandEdited(target);
    } else if (chosen == reset) {
        b = waveline::defaultEqBands()[size_t(target)];
        update();
        emit bandEdited(target);
    } else if (chosen->data().isValid()) {
        chooseType(target, static_cast<EqBandType>(chosen->data().toInt()));
    }
}

void EqCurveView::leaveEvent(QEvent *) {
    if (hovered_ < 0) return;
    hovered_ = -1;
    update();
}

// ============================================================= ProEqWindow

namespace {

QLabel *dimCaption(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(pal);
    return l;
}

ChannelFxInfo readTargetFx(MixerClient *client, const ProEqTarget &t) {
    return t.master ? client->masterChannelEffects(t.id, t.stage)
                    : client->channelEffects(t.id, t.stage);
}

}  // namespace

ProEqWindow::ProEqWindow(MixerClient *client, const ProEqTarget &target,
                         const QString &title, QWidget *parent)
    : QWidget(parent, Qt::Window), client_(client), target_(target) {
    setWindowTitle(tr("%1 — Advanced EQ").arg(title));
    setMinimumSize(820, 560);
    resize(940, 620);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 16, 16, 16);
    lay->setSpacing(12);

    // ---- top row: the master switch, the presets, and the way back to flat
    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    auto *eqLabel = new QLabel(tr("Equalizer"), this);
    QFont bold = eqLabel->font();
    bold.setBold(true);
    eqLabel->setFont(bold);
    top->addWidget(eqLabel);
    enabled_ = new ToggleSwitch(this);
    enabled_->setToolTip(tr("The same switch as on the Processing tab: this is one "
                            "EQ with two ways of setting it."));
    top->addWidget(enabled_);
    top->addSpacing(16);

    top->addWidget(dimCaption(tr("Preset"), this));
    preset_ = new QComboBox(this);
    preset_->addItem(tr("Custom"));
    for (const EqPreset &pre : presets()) {
        preset_->addItem(tr(pre.name));
        preset_->setItemData(preset_->count() - 1, tr(pre.tip), Qt::ToolTipRole);
    }
    preset_->setMinimumWidth(190);
    top->addWidget(preset_);
    top->addStretch();

    auto *resetBtn = new QPushButton(tr("Reset to flat"), this);
    resetBtn->setToolTip(tr("Switch every band off. The three-band EQ is left alone."));
    top->addWidget(resetBtn);
    lay->addLayout(top);

    // ---- the curve
    curve_ = new EqCurveView(this);
    lay->addWidget(curve_, 1);

    // ---- band picker
    auto *picker = new QHBoxLayout;
    picker->setSpacing(4);
    picker->addWidget(dimCaption(tr("Band"), this));
    for (int i = 0; i < kProEqBands; ++i) {
        auto *b = new QPushButton(QString::number(i + 1), this);
        b->setCheckable(true);
        b->setFixedHeight(26);
        b->setMinimumWidth(34);
        b->setCursor(Qt::PointingHandCursor);
        const QColor c = bandColor(i);
        b->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; color: %2; border: 1px solid %3;"
                           "  border-radius: 4px; font-weight: bold; }"
                           "QPushButton:hover { background: %4; }"
                           "QPushButton:checked { background: %5; color: %6;"
                           "  border: 1px solid %5; }")
                .arg(Theme::Well.name(), Theme::TextDim.name(), Theme::Line.name(),
                     Theme::CardHover.name(), c.name(), Theme::glyphOn(c).name()));
        bandButtons_[i] = b;
        picker->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, i] { selectBand(i); });
    }
    picker->addStretch();
    lay->addLayout(picker);

    // ---- the selected band's numbers, for when a dot is not precise enough
    auto *editor = new QHBoxLayout;
    editor->setSpacing(8);

    bandOn_ = new ToggleSwitch(this);
    bandOn_->setToolTip(tr("Switch this band in or out."));
    editor->addWidget(dimCaption(tr("On"), this));
    editor->addWidget(bandOn_);
    editor->addSpacing(10);

    editor->addWidget(dimCaption(tr("Type"), this));
    bandType_ = new QComboBox(this);
    for (int t = 0; t < waveline::kEqBandTypeCount; ++t)
        bandType_->addItem(typeName(static_cast<EqBandType>(t)));
    bandType_->setMinimumWidth(120);
    editor->addWidget(bandType_);
    editor->addSpacing(10);

    editor->addWidget(dimCaption(tr("Freq"), this));
    bandFreq_ = new QDoubleSpinBox(this);
    bandFreq_->setRange(double(waveline::kEqMinHz), double(waveline::kEqMaxHz));
    bandFreq_->setDecimals(1);
    bandFreq_->setSuffix(QStringLiteral(" Hz"));
    bandFreq_->setKeyboardTracking(false);
    bandFreq_->setMinimumWidth(110);
    editor->addWidget(bandFreq_);

    bandGainCaption_ = dimCaption(tr("Gain"), this);
    editor->addWidget(bandGainCaption_);
    bandGain_ = new QDoubleSpinBox(this);
    bandGain_->setRange(kMinDb, kMaxDb);
    bandGain_->setDecimals(1);
    bandGain_->setSingleStep(0.5);
    bandGain_->setSuffix(QStringLiteral(" dB"));
    bandGain_->setKeyboardTracking(false);
    bandGain_->setMinimumWidth(94);
    editor->addWidget(bandGain_);

    editor->addWidget(dimCaption(tr("Q"), this));
    bandQ_ = new QDoubleSpinBox(this);
    bandQ_->setRange(double(waveline::kEqMinQ), double(waveline::kEqMaxQ));
    bandQ_->setDecimals(2);
    bandQ_->setSingleStep(0.1);
    bandQ_->setKeyboardTracking(false);
    bandQ_->setMinimumWidth(84);
    editor->addWidget(bandQ_);
    editor->addStretch();
    lay->addLayout(editor);

    auto *hint = dimCaption(tr("Drag a dot to move a band, scroll over it to change its "
                            "width, double-click empty space to add one."),
                         this);
    lay->addWidget(hint);

    // ---- wiring
    connect(enabled_, &QAbstractButton::toggled, this, [this](bool on) {
        if (updating_) return;
        curve_->setEqEnabled(on);
        pushEnabled(on);
    });
    connect(resetBtn, &QPushButton::clicked, this, &ProEqWindow::resetToFlat);
    connect(preset_, &QComboBox::activated, this, &ProEqWindow::applyPreset);

    connect(curve_, &EqCurveView::bandEdited, this, [this](int) {
        bands_ = curve_->bands();
        updating_ = true;
        syncEditorToBand();
        syncBandButtons();
        syncPresetCombo();
        updating_ = false;
        pushBands();
    });
    connect(curve_, &EqCurveView::selectionChanged, this, [this](int i) {
        selected_ = i;
        updating_ = true;
        syncEditorToBand();
        syncBandButtons();
        updating_ = false;
    });

    const auto edited = [this] {
        if (updating_) return;
        EqBand &b = bands_[size_t(selected_)];
        b.on = bandOn_->isChecked();
        b.type = static_cast<EqBandType>(bandType_->currentIndex());
        b.freq = float(bandFreq_->value());
        b.gainDb = float(bandGain_->value());
        b.q = float(bandQ_->value());
        waveline::clampEqBand(b);
        curve_->setBands(bands_);
        updating_ = true;
        syncEditorToBand();
        syncBandButtons();
        syncPresetCombo();
        updating_ = false;
        pushBands();
    };
    connect(bandOn_, &QAbstractButton::toggled, this, edited);
    connect(bandType_, &QComboBox::currentIndexChanged, this, edited);
    connect(bandFreq_, &QDoubleSpinBox::valueChanged, this, edited);
    connect(bandGain_, &QDoubleSpinBox::valueChanged, this, edited);
    connect(bandQ_, &QDoubleSpinBox::valueChanged, this, edited);

    connect(client_, &MixerClient::changed, this, &ProEqWindow::refresh);

    updating_ = true;
    curve_->setBands(bands_);
    curve_->setSelected(selected_);
    syncEditorToBand();
    syncBandButtons();
    syncPresetCombo();
    updating_ = false;
}

void ProEqWindow::present() {
    show();
    raise();
    activateWindow();
}

void ProEqWindow::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refresh();
}

void ProEqWindow::refresh() {
    if (!client_ || !client_->available() || !isVisible()) return;
    const ChannelFxInfo fx = readTargetFx(client_, target_);

    // Someone switched this chain back to the three-band EQ from the effects
    // panel, or a profile did. Nothing here edits anything any more, and a
    // window that goes on looking live is the worse of the two options.
    if (haveState_ && !fx.eqAdvanced) {
        close();
        return;
    }

    if (!haveState_ || fx.proEqBands != lastSpec_) {
        lastSpec_ = fx.proEqBands;
        bands_ = waveline::decodeEqBands(fx.proEqBands.toStdString());
        updating_ = true;
        curve_->setBands(bands_);
        syncEditorToBand();
        syncBandButtons();
        syncPresetCombo();
        updating_ = false;
    }
    if (!haveState_ || fx.eq != lastEnabled_) {
        lastEnabled_ = fx.eq;
        updating_ = true;
        enabled_->setChecked(fx.eq);
        updating_ = false;
        curve_->setEqEnabled(fx.eq);
    }
    haveState_ = true;
}

void ProEqWindow::pushBands() {
    if (updating_ || !client_ || !client_->available()) return;
    const QString spec = encode(bands_);
    lastSpec_ = spec;
    // Always advanced: editing the curve is what selects it. Anything else
    // would let someone drag a band around and hear nothing.
    if (target_.master)
        client_->setMasterProEq(target_.id, target_.stage, true, spec);
    else
        client_->setChannelProEq(target_.id, target_.stage, true, spec);
}

void ProEqWindow::pushEnabled(bool on) {
    if (updating_ || !client_ || !client_->available()) return;
    // One EQ switch, shared with the Processing tab, and the daemon takes it
    // alongside the three-band values -- so those are read back and handed
    // over untouched rather than reset to whatever this panel would guess.
    const ChannelFxInfo fx = readTargetFx(client_, target_);
    lastEnabled_ = on;
    if (target_.master)
        client_->setMasterChannelEffects(target_.id, target_.stage, fx.lowCut, fx.lowCutHz,
                                         on, fx.lowDb, fx.midDb, fx.highDb);
    else
        client_->setChannelEffects(target_.id, target_.stage, fx.lowCut, fx.lowCutHz, on,
                                   fx.lowDb, fx.midDb, fx.highDb);
}

void ProEqWindow::selectBand(int index) {
    if (index < 0 || index >= kProEqBands) return;
    selected_ = index;
    curve_->setSelected(index);
    updating_ = true;
    syncEditorToBand();
    syncBandButtons();
    updating_ = false;
}

void ProEqWindow::syncEditorToBand() {
    const EqBand &b = bands_[size_t(selected_)];
    QSignalBlocker b1(bandOn_);
    QSignalBlocker b2(bandType_);
    QSignalBlocker b3(bandFreq_);
    QSignalBlocker b4(bandGain_);
    QSignalBlocker b5(bandQ_);
    bandOn_->setChecked(b.on);
    bandType_->setCurrentIndex(int(b.type));
    bandFreq_->setValue(double(b.freq));
    bandGain_->setValue(double(b.gainDb));
    bandQ_->setValue(double(b.q));

    // A high-pass has no gain, and a gain box that is there but does nothing
    // is worse than one that is visibly not on offer.
    const bool gain = waveline::eqTypeUsesGain(b.type);
    bandGain_->setEnabled(gain && b.on);
    bandGainCaption_->setEnabled(gain && b.on);
    bandFreq_->setEnabled(b.on);
    bandType_->setEnabled(b.on);
    bandQ_->setEnabled(b.on);
}

void ProEqWindow::syncBandButtons() {
    for (int i = 0; i < kProEqBands; ++i) {
        QPushButton *btn = bandButtons_[i];
        if (!btn) continue;
        QSignalBlocker blocker(btn);
        btn->setChecked(i == selected_);
        const EqBand &b = bands_[size_t(i)];
        QFont f = btn->font();
        f.setBold(b.on);
        btn->setFont(f);
        btn->setToolTip(b.on ? QStringLiteral("%1 · %2 Hz")
                                   .arg(typeName(b.type))
                                   .arg(double(b.freq), 0, 'f', 0)
                             : tr("Band %1 (off)").arg(i + 1));
    }
}

void ProEqWindow::syncPresetCombo() {
    const QString mine = encode(bands_);
    int match = 0;   // "Custom"
    for (int i = 0; i < int(presets().size()); ++i) {
        if (encode(presetBands(i)) == mine) {
            match = i + 1;
            break;
        }
    }
    QSignalBlocker blocker(preset_);
    preset_->setCurrentIndex(match);
}

void ProEqWindow::applyPreset(int comboIndex) {
    if (comboIndex <= 0) return;   // "Custom" is a readout, not a choice
    bands_ = presetBands(comboIndex - 1);
    updating_ = true;
    curve_->setBands(bands_);
    syncEditorToBand();
    syncBandButtons();
    updating_ = false;
    pushBands();
}

void ProEqWindow::resetToFlat() {
    bands_ = waveline::defaultEqBands();
    updating_ = true;
    curve_->setBands(bands_);
    syncEditorToBand();
    syncBandButtons();
    syncPresetCombo();
    updating_ = false;
    pushBands();
}

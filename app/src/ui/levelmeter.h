// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// A signal meter with a held peak. Horizontal for the output rows, vertical
// for anything that has to sit beside a fader.
//
// setLevel() sets a *target*; the drawn value chases it on every display frame
// rather than jumping. Meter data arrives at ~60 Hz over D-Bus, which on a
// 240 Hz screen would otherwise be four identical frames followed by a step.
// The interpolation is what makes the bar look like it is moving rather than
// ticking, and it costs nothing extra in D-Bus traffic.
//
// The value is a 0..1 bar position, not a raw amplitude: the dB mapping is the
// caller's business, because the sources feeding meters here are not calibrated
// alike -- docs/protocol.md records that the Wave:3's own meter block has a
// guessed full-scale reference.

#pragma once

#include <QColor>
#include <QWidget>

// Maps an amplitude -- RMS or peak, 0..1 -- onto a 0..1 bar position on a
// logarithmic scale with a -60 dB floor.
//
// Shared rather than written per window: the same signal has to reach the same
// height wherever it is drawn, and the effects panels used to run a floor of
// -120 dB, which put a microphone's idle noise floor (about -76 dBFS) a third
// of the way up the bar and made a muted microphone look live.
double meterPosition(double amplitude);

class LevelMeter : public QWidget {
    Q_OBJECT

public:
    explicit LevelMeter(Qt::Orientation orientation = Qt::Horizontal,
                        QWidget *parent = nullptr);
    ~LevelMeter() override;

    void setLevel(double fraction);   // 0..1, clamped. A target, not a jump.
    // Thickness across the bar: height when horizontal, width when vertical.
    void setThickness(int px);
    // Draws the bar in one colour instead of the green/amber/red ramp. The
    // channel cards use their identity colour, which is what makes a row of
    // otherwise identical meters readable at a glance.
    void setTint(const QColor &tint);

    // Advances the animation by dtMs and repaints if anything moved. Returns
    // true while there is still movement left to draw, which is how the ticker
    // knows it can stop. Called by MeterTicker; not part of the widget's
    // public contract.
    bool advance(double dtMs);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    Qt::Orientation orient_;
    double target_ = 0.0;   // where the audio says we should be
    double level_ = 0.0;    // where we are drawing right now
    double peak_ = 0.0;
    double peakHoldMs_ = 0.0;
    int thickness_ = 8;
    QColor tint_;   // invalid = use the ramp
};

// Drives every LevelMeter from one timer running at the screen's refresh rate,
// so all the bars in the window move in the same frame instead of each widget
// keeping its own clock.
class MeterTicker {
public:
    static void add(LevelMeter *m);
    static void remove(LevelMeter *m);
    // Restarts the timer after everything has come to rest.
    static void wake();
    // Follows the screen the window is actually on: a 240 Hz panel and a 60 Hz
    // one should not be driven at the same rate.
    static void setRefreshRate(double hz);
};

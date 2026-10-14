// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The hand-painted controls. Everything here is a real QAbstractButton or
// QSlider underneath, so keyboard focus, wheel scrolling, page steps and the
// usual signals all behave -- only the painting is ours.

#pragma once

#include <functional>

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QComboBox>
#include <QColor>
#include <QFrame>
#include <QMouseEvent>
#include <QPoint>
#include <QPointer>
#include <QPropertyAnimation>
#include <QSlider>

#include "theme.h"
#include <QString>
#include <QWheelEvent>

class QHBoxLayout;
class QLabel;
class QVBoxLayout;

// -------------------------------------------------------------- ToggleSwitch
// A sliding pill. Replaces QCheckBox for every on/off setting: a checkbox
// beside a fader reads as "select this row", a switch reads as "this is on".
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(qreal knob READ knob WRITE setKnob)

public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    qreal knob() const { return knob_; }
    void setKnob(qreal v);

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    // The two ways the checked state can change, and both are needed.
    //
    // checkStateSet() covers programmatic changes -- refreshing from the daemon
    // uses QSignalBlocker, which suppresses toggled(), so the animation cannot
    // hang off that signal.
    //
    // nextCheckState() covers the user: QAbstractButtonPrivate::click() sets
    // blockRefresh before calling it, and setChecked() skips checkStateSet()
    // while that flag is up. Without this override a clicked switch changes
    // isChecked() and never moves -- it keeps painting its old position until
    // something happens to set the state programmatically.
    void checkStateSet() override;
    void nextCheckState() override;

private:
    // Slides the knob to wherever isChecked() now says it belongs.
    void animateKnob();

    QPointer<QPropertyAnimation> knobAnim_;
    qreal knob_ = 0.0;   // 0 = off, 1 = on
    bool hover_ = false;
};

// --------------------------------------------------------------------- Fader
// A vertical fader with a bar handle, like the ones in Wave Link. The filled
// part below the handle carries the channel's identity colour.
class Fader : public QSlider {
    Q_OBJECT

public:
    explicit Fader(QWidget *parent = nullptr);

    void setAccent(const QColor &c);
    // Drawn dimmed and struck through when the mix this fader feeds is muted,
    // so a muted channel is obvious without reading the button below it.
    void setMuted(bool muted);
    // Draws the handle and nothing else: something behind the fader -- a mix
    // card's level meter -- is the track. See trackRect().
    void setTrackHidden(bool hidden);
    // Where the track runs, in the fader's own coordinates: the handle's
    // centre travels from its top to its bottom.
    QRectF trackRect(qreal width) const;
    // Where the handle's centre is, and how far it travels end to end, in
    // pixels: what something dragging the fader from outside it needs.
    qreal handleCenter() const { return handleY(); }
    int travel() const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    // Press jumps to the click, which means never handing QSlider its own
    // press. Without these two it also never learns the handle is being
    // dragged, so the fader could be clicked but not moved.
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    // Where the centre of the handle sits, in widget coordinates.
    qreal handleY() const;
    void setValueFromPos(int posY);

    Theme::Live accent_;
    bool muted_ = false;
    bool trackHidden_ = false;
};

// --------------------------------------------------------------- TrackSlider
// Horizontal level control with a visible track and click-to-jump dragging.
// Used where a plain QSlider would disappear on a Well background (same colour
// as the default groove) or lose focus when the parent rebuilds on valueChanged.
class TrackSlider : public QSlider {
    Q_OBJECT

public:
    explicit TrackSlider(QWidget *parent = nullptr);

    void setAccent(const QColor &c);
    // The faders' look, on its side: a thick track filled to the handle in the
    // accent, and the faders' cap for a handle -- so a row of these reads as
    // the same control as the cards' faders.
    void setFaderStyle(bool on);
    // As Fader's: the handle alone, over a meter that is the track.
    void setTrackHidden(bool hidden);
    QRectF trackRect(qreal thickness) const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    qreal handleX() const;
    void setValueFromPos(int posX);

    Theme::Live accent_;
    bool faderStyle_ = false;
    bool trackHidden_ = false;
};

// A level meter standing in for a slider's track: laid exactly under it, kept
// there as the slider moves, resizes, shows and hides, and left to the mouse
// to pass through. The slider stops drawing a track of its own, so the bar
// behind the handle is the level, and the handle is where it is set. The
// meter is a sibling of the slider, owned by its parent.
class LevelMeter;
LevelMeter *attachTrackMeter(Fader *fader, int thickness = 8);
LevelMeter *attachTrackMeter(TrackSlider *slider, int thickness = 8);

// --------------------------------------------------------------------- Knob
// A rotary pro-audio control for continuous Creative FX parameters. Built on
// QAbstractSlider rather than QSlider: a knob has no groove for the QStyle
// slider-position helpers Fader/TrackSlider lean on above, so this one maps
// value <-> angle by hand instead. Integer-ranged like QSlider, so it drops
// into the same int-value/QLabel-readout wiring every other control here
// already uses.
class Knob : public QAbstractSlider {
    Q_OBJECT

public:
    explicit Knob(QWidget *parent = nullptr);

    void setAccent(const QColor &c);
    // Bipolar knobs (a future pan-style control, not used by anything yet)
    // fill from the centre outward instead of from the minimum.
    void setBipolar(bool on);
    // Where a double-click resets to. Defaults to minimum().
    void setDefaultValue(int v);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

private:
    Theme::Live accent_;
    bool bipolar_ = false;
    int defaultValue_ = 0;
    bool hasDefaultValue_ = false;
    QPoint dragStart_;
    int dragStartValue_ = 0;
};

// ------------------------------------------------------------- PowerSwitch
// A rocker-style I/O power switch, for the Virtual Rack's per-module on/off
// -- deliberately not another ToggleSwitch, since the rack is meant to read
// as physical gear rather than as more app UI.
class PowerSwitch : public QAbstractButton {
    Q_OBJECT

public:
    explicit PowerSwitch(QWidget *parent = nullptr);

    void setAccent(const QColor &c);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    Theme::Live accent_;
    bool hover_ = false;
};

// ---------------------------------------------------------------- IconToggle
// A checkable icon button. Used for the per-mix mute buttons under each fader
// and for the pill-shaped state buttons.
// A combo that shows only the current item's icon, with no frame and no
// drop-down arrow. The popup still lists icon and name: it is drawn by the
// view, so clearing the collapsed label does not touch it.
class IconCombo : public QComboBox {
    Q_OBJECT

public:
    explicit IconCombo(QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override;
};

class IconToggle : public QAbstractButton {
    Q_OBJECT

public:
    enum Shape { Round, Pill };

    IconToggle(const QString &iconOn, const QString &iconOff, QWidget *parent = nullptr);

    void setAccent(const QColor &c);
    void setShape(Shape s);
    void setIconSize(int px);
    void setIconVisible(bool on);

    QSize sizeHint() const override;

signals:
    void rightClicked();
    void middleClicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;

private:
    QString on_, off_;
    Theme::Live accent_;
    Shape shape_ = Round;
    int px_ = 18;
    bool hover_ = false;
    bool iconVisible_ = true;
};

// --------------------------------------------------------------- EqualToggle
// Holds a channel's two faders at the same value. Green when on, grey when off.
class EqualToggle : public QAbstractButton {
    Q_OBJECT

public:
    explicit EqualToggle(QWidget *parent = nullptr);
    // Lit in the card's own colour rather than the global "on" green, so a
    // wall of channels stays scannable even where every one of them is linked.
    void setAccent(const QColor &c);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    bool hover_ = false;
    // Defaults to the global "on" colour until a card claims it.
    Theme::Live accent_;
};

// ------------------------------------------------------------------ CardBase
// A rounded panel. Qt's stylesheet border-radius does not clip child widgets,
// so the background is painted instead of styled.
class CardBase : public QFrame {
    Q_OBJECT

public:
    explicit CardBase(QWidget *parent = nullptr);
    void setRadius(int r);
    int radius() const { return radius_; }
    void setFillColor(const QColor &c);
    // A 3px strip of the channel colour along the top edge.
    void setTopStripe(const QColor &c);
    // A 1px outline all the way round, in place of the strip: the channel
    // cards' look. An invalid colour draws none.
    void setBorderColor(const QColor &c);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int radius_ = 12;
    Theme::Live fill_;
    Theme::Live stripe_;
    Theme::Live border_;
};

// ------------------------------------------------------------------ IconTile
// The rounded, colour-filled square holding a card's icon: an input device's,
// a channel's, a mix's, Application Settings'. Painted rather than styled so
// the corner radius actually clips the fill. The tile is the card's identity,
// so it is also where that is edited -- see setOnClicked().
class IconTile : public QWidget {
public:
    IconTile(const QString &iconName, const QColor &color, QWidget *parent = nullptr);
    void setAppearance(const QString &iconName, const QColor &color);
    // A callback rather than a signal, so the tile needs no moc pass.
    void setOnClicked(std::function<void()> fn);

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;

private:
    QString icon_;
    QColor color_;
    bool hover_ = false;
    std::function<void()> onClick_;
};

// ------------------------------------------------------------------ Section
// A titled card for the sidebar: small caps caption, then whatever is added
// to contentLayout().
class Section : public CardBase {
    Q_OBJECT

public:
    Section(const QString &title, QWidget *parent = nullptr);
    QVBoxLayout *contentLayout() const { return content_; }
    // The hardware panel names the microphone it belongs to, and which
    // microphone that is is only known once the device profile has been read.
    void setTitle(const QString &title);
    // Put before the caption, on its line: the Application Settings card's
    // tile. The section takes it over.
    void setLeadingWidget(QWidget *widget);
    // The caption centred across the card rather than flush left.
    void setCaptionCentered(bool centered);

private:
    QVBoxLayout *content_ = nullptr;
    QLabel *caption_ = nullptr;
    QHBoxLayout *header_ = nullptr;
};

// -------------------------------------------------------- MonitorModeButton
// How you hear yourself: not at all, through the computer, or through the
// microphone's own headphone jack.
//
// One control rather than three, because the two monitor paths are genuinely
// alternatives. With headphones in the microphone's own jack and the Monitor
// mix sent to the microphone's output, having both on means hearing yourself
// twice -- once
// direct and once delayed by the graph and by RNNoise's 10 ms frame, which
// comb-filters your own voice.
class MonitorModeButton : public QWidget {
    Q_OBJECT

public:
    enum Mode { Off, Software, Hardware };

    explicit MonitorModeButton(QWidget *parent = nullptr);

    Mode mode() const { return mode_; }
    void setMode(Mode m);   // does not emit

    // False on a microphone with no vendor protocol: there is no headphone
    // jack on it to send a hardware blend to, so the third position is dropped
    // from the cycle entirely rather than offered and then doing nothing. A
    // control that visibly changes state and produces no sound is worse than
    // one that is not there.
    void setHardwareAvailable(bool on);
    bool hardwareAvailable() const { return hardwareAvailable_; }

    QSize sizeHint() const override;

signals:
    void modeChosen(int mode);

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

private:
    void updateTip();

    // How many positions the click cycles through: three with a hardware
    // monitor, two without.
    int modeCount() const { return hardwareAvailable_ ? 3 : 2; }

    Mode mode_ = Off;
    bool hardwareAvailable_ = true;
    bool hover_ = false;
};

// ----------------------------------------------------------------- StatusDot
// The connection indicator. Painted rather than done as a coloured bullet in a
// QLabel: the application-wide stylesheet sets `color` on QWidget, and a
// stylesheet beats a per-widget palette, so a palette-coloured bullet is not
// reliably any colour at all.
class StatusDot : public QWidget {
    Q_OBJECT

public:
    explicit StatusDot(QWidget *parent = nullptr);
    void setColor(const QColor &c);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    Theme::Live color_;
};

// ------------------------------------------------------------- ElidedLabel
// A label that shortens its text with an ellipsis instead of being clipped.
// QLabel does neither on its own: it either forces the layout wide enough or
// gets cut off mid-word, and both spoil a row that has to survive a narrow
// window.
class ElidedLabel : public QWidget {
    Q_OBJECT

public:
    explicit ElidedLabel(const QString &text = {}, QWidget *parent = nullptr);
    void setText(const QString &text);
    QString text() const { return text_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString text_;
};

// ------------------------------------------------------------------- helpers
// A small-caps grey caption, the "INPUTS" / "OUTPUTS" style label.
QLabel *caption(const QString &text, QWidget *parent = nullptr);

// A label sized to the widest string it will ever hold. Sizing to the current
// text lets the label grow as the number does and takes the space straight out
// of the slider beside it, so the bar changes length while you drag it.
QLabel *fixedReadout(const QString &widest, QWidget *parent = nullptr);

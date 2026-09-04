// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The parametric EQ panel: ten bands on a draggable curve.
//
// A window of its own rather than another tab in the effects panel. The curve
// wants every pixel it can get and the effects panel is a fixed 760-wide
// column of rows; more to the point, this is the one control here people leave
// open beside what they are listening to while they work on it.

#pragma once

#include <QWidget>

#include "engine/eqspec.h"

class MixerClient;
class ToggleSwitch;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

// ------------------------------------------------------------- EqCurveView
// The graph. Owns nothing but a copy of the bands: it reports edits and lets
// the panel decide what to do with them, so the same widget could be shown
// read-only somewhere else without a daemon behind it.
class EqCurveView : public QWidget {
    Q_OBJECT

public:
    explicit EqCurveView(QWidget *parent = nullptr);

    void setBands(const waveline::EqBands &b);
    const waveline::EqBands &bands() const { return bands_; }
    void setSelected(int index);
    int selected() const { return selected_; }
    // Drawn greyed out when the EQ switch is off, because a curve that is
    // being edited and a curve that is doing something have to look different.
    void setEqEnabled(bool on);

    QSize sizeHint() const override;

signals:
    // A band moved. Fires continuously while dragging: the engine glides to
    // whatever it is told, so there is nothing to gain from batching.
    void bandEdited(int index);
    void selectionChanged(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void leaveEvent(QEvent *) override;

private:
    QRectF plotRect() const;
    double xForFreq(double hz) const;
    double freqForX(double x) const;
    double yForDb(double db) const;
    double dbForY(double y) const;
    QPointF nodePos(int index) const;
    int bandAt(const QPointF &p, double radius) const;
    void chooseType(int index, waveline::EqBandType type);

    waveline::EqBands bands_ = waveline::defaultEqBands();
    int selected_ = 4;
    int hovered_ = -1;
    int dragging_ = -1;
    bool eqEnabled_ = true;
};

// Which chain's EQ a panel is editing. The daemon splits these across two
// D-Bus methods -- input devices are master buses, everything else is a
// channel -- and this is the difference carried around the panel so nothing
// below the constructor has to care which it is.
struct ProEqTarget {
    bool master = false;
    QString id;
    QString stage = QStringLiteral("input");

    bool operator==(const ProEqTarget &o) const {
        return master == o.master && id == o.id && stage == o.stage;
    }
};

class ProEqWindow : public QWidget {
    Q_OBJECT

public:
    ProEqWindow(MixerClient *client, const ProEqTarget &target, const QString &title,
                QWidget *parent = nullptr);

    const ProEqTarget &target() const { return target_; }
    // Brings an already-open panel to the front instead of stacking a second
    // one behind it.
    void present();

protected:
    void showEvent(QShowEvent *e) override;

private slots:
    void refresh();

private:
    void pushBands();
    void pushEnabled(bool on);
    void selectBand(int index);
    void syncEditorToBand();
    void syncBandButtons();
    void syncPresetCombo();
    void applyPreset(int comboIndex);
    void resetToFlat();

    MixerClient *client_ = nullptr;
    ProEqTarget target_;

    waveline::EqBands bands_ = waveline::defaultEqBands();
    int selected_ = 4;
    // What the daemon last told us. A poll that changed nothing must not yank
    // a spin box out from under someone who is part-way through typing in it.
    QString lastSpec_;
    bool lastEnabled_ = false;
    bool haveState_ = false;
    bool updating_ = false;

    EqCurveView *curve_ = nullptr;
    ToggleSwitch *enabled_ = nullptr;
    QComboBox *preset_ = nullptr;
    QPushButton *bandButtons_[waveline::kProEqBands]{};
    ToggleSwitch *bandOn_ = nullptr;
    QComboBox *bandType_ = nullptr;
    QDoubleSpinBox *bandFreq_ = nullptr;
    QDoubleSpinBox *bandGain_ = nullptr;
    QDoubleSpinBox *bandQ_ = nullptr;
    QLabel *bandGainCaption_ = nullptr;
};

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "effectswindow.h"

#include <cmath>

#include <functional>

#include <algorithm>

#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QFileInfo>
#include <QFont>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "creativefxpanel.h"
#include "levelmeter.h"
#include "mixerclient.h"
#include "proeqwindow.h"
#include "theme.h"
#include "widgets.h"

namespace {

// Both effects windows, so they cannot drift apart. Fixed rather than
// resizable: every row inside is a caption, a control and a readout laid out
// against this width, and the sliders are the only thing that gains from the
// extra room -- which is exactly what the width is spent on.
constexpr int kEffectsWindowWidth = 760;

// The HW tab is inserted at index 0 and hidden for a microphone with no vendor
// controls; hiding the tab that happens to be current leaves the panel showing
// an empty page -- a panel that opened "on no tab". Processing is where anyone
// opening this starts, so it is both what a panel opens on and what it falls
// back to when the current tab is taken away.
void selectTab(QTabWidget *tabs, const QString &label) {
    if (!tabs) return;
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->isTabVisible(i) && tabs->tabText(i) == label) {
            tabs->setCurrentIndex(i);
            return;
        }
    }
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->isTabVisible(i)) {
            tabs->setCurrentIndex(i);
            return;
        }
    }
}

// Same, but only when the panel has been left with nothing selected or with a
// hidden tab selected -- so a refresh cannot yank someone off the tab they are
// working in.
void keepTabSelected(QTabWidget *tabs, const QString &label) {
    if (!tabs) return;
    const int cur = tabs->currentIndex();
    if (cur >= 0 && tabs->isTabVisible(cur)) return;
    selectTab(tabs, label);
}

QLabel *dimLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(p);
    return l;
}

QWidget *switchRow(const QString &label, const QString &tip, ToggleSwitch *&sw,
                   QWidget *parent) {
    auto *row = new QWidget(parent);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    auto *name = new QLabel(label, row);
    name->setToolTip(tip);
    lay->addWidget(name, 1);
    sw = new ToggleSwitch(row);
    sw->setToolTip(tip);
    lay->addWidget(sw);
    return row;
}

QWidget *sliderRow(const QString &label, QSlider *&slider, QLabel *&readout,
                   int maxWidth, QWidget *parent) {
    auto *row = new QWidget(parent);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    auto *name = dimLabel(label, row);
    name->setFixedWidth(92);
    lay->addWidget(name);
    slider = new QSlider(Qt::Horizontal, row);
    lay->addWidget(slider, 1);
    readout = dimLabel(QString(), row);
    readout->setFixedWidth(maxWidth);
    readout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lay->addWidget(readout);
    return row;
}

// The row Advanced mode puts where the Low/Mid/High sliders were. Laid out on
// the same three columns as sliderRow above -- caption, control, readout --
// so the section does not visibly shift when the mode is switched.
QWidget *eqOpenRow(QPushButton *&btn, QWidget *parent) {
    auto *row = new QWidget(parent);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    auto *name = dimLabel(QObject::tr("Curve"), row);
    name->setFixedWidth(92);
    lay->addWidget(name);
    btn = new QPushButton(QObject::tr("Open Advanced EQ…"), row);
    btn->setFixedHeight(26);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(QObject::tr("Ten fully parametric bands on a curve you can drag."));
    lay->addWidget(btn, 1);
    auto *spacer = new QWidget(row);
    spacer->setFixedWidth(48);
    lay->addWidget(spacer);
    return row;
}

// Easy shows three sliders, Advanced shows the button. Only ever one of them:
// two EQs on screen at once, of which one is doing nothing, is the reading a
// beginner would take away from a panel that showed both.
// Reads the mode back from the daemon without pushing it out again.
void showEqMode(EqAdvancedControls &adv, bool advanced);

void applyEqModeRows(EqAdvancedControls &adv) {
    const bool advanced = adv.mode && adv.mode->isChecked();
    if (adv.lowRow) adv.lowRow->setVisible(!advanced);
    if (adv.midRow) adv.midRow->setVisible(!advanced);
    if (adv.highRow) adv.highRow->setVisible(!advanced);
    if (adv.openRow) adv.openRow->setVisible(advanced);
}

void showEqMode(EqAdvancedControls &adv, bool advanced) {
    if (!adv.mode) return;
    QSignalBlocker blocker(adv.mode);
    adv.mode->setChecked(advanced);
    applyEqModeRows(adv);
}

ChannelFxInfo stageFx(MixerClient *client, bool master, const QString &id,
                      const QString &stage) {
    return master ? client->masterChannelEffects(id, stage)
                  : client->channelEffects(id, stage);
}

// The mode is stored beside the parametric bands, and the setter takes both.
// Re-reading the bands here rather than caching them costs one round trip per
// click of a switch and removes the only copy of the curve that could have
// gone stale -- worth it, given what a stale one would overwrite.
void pushEqMode(MixerClient *client, bool master, const QString &id, const QString &stage,
                bool advanced) {
    if (!client || !client->available()) return;
    const ChannelFxInfo fx = stageFx(client, master, id, stage);
    if (master)
        client->setMasterProEq(id, stage, advanced, fx.proEqBands);
    else
        client->setChannelProEq(id, stage, advanced, fx.proEqBands);
}

void openProEqPanel(QPointer<ProEqWindow> &slot, QWidget *owner, MixerClient *client,
                    bool master, const QString &id, const QString &stage,
                    const QString &title) {
    if (!client || !client->available()) return;
    if (!slot) {
        ProEqTarget target;
        target.master = master;
        target.id = id;
        target.stage = stage;
        slot = new ProEqWindow(client, target, title, owner);
    }
    slot->present();
}

// Ducking hold, in tenths of a second.
QString duckHoldReadout(int tenths) {
    if (tenths <= 0) return QStringLiteral("Off");
    return QStringLiteral("%1 s").arg(tenths / 10.0, 0, 'f', 1);
}

QString dbReadout(int tenths) {
    const double db = tenths / 10.0;
    if (db > 0.05) return QStringLiteral("+%1 dB").arg(db, 0, 'f', 1);
    if (db < -0.05) return QStringLiteral("%1 dB").arg(db, 0, 'f', 1);
    return QStringLiteral("0 dB");
}

QWidget *sectionHeaderRow(const QString &title, QWidget *parent,
                          const std::function<void()> &onDefault) {
    auto *row = new QWidget(parent);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    auto *label = new QLabel(title, row);
    QFont f = label->font();
    f.setBold(true);
    label->setFont(f);
    lay->addWidget(label);
    lay->addStretch();
    auto *btn = new QPushButton(QObject::tr("Default"), row);
    btn->setToolTip(QObject::tr("Reset this section to factory defaults."));
    btn->setFixedHeight(24);
    QObject::connect(btn, &QPushButton::clicked, parent,
                     [onDefault] { onDefault(); });
    lay->addWidget(btn);
    return row;
}

QWidget *defaultResetRow(QWidget *parent, const std::function<void()> &onDefault) {
    auto *row = new QWidget(parent);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addStretch();
    auto *btn = new QPushButton(QObject::tr("Default"), row);
    btn->setToolTip(QObject::tr("Reset this section to factory defaults."));
    btn->setFixedHeight(24);
    QObject::connect(btn, &QPushButton::clicked, parent,
                     [onDefault] { onDefault(); });
    lay->addWidget(btn);
    return row;
}

void styleEffectsTabs(QTabWidget *tabs) {
    tabs->setDocumentMode(false);
    tabs->tabBar()->setExpanding(true);
    tabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane {"
        "  border: 1px solid %1;"
        "  border-radius: 0 0 6px 6px;"
        "  background: %2;"
        "  top: -1px;"
        "}"
        "QTabBar::tab {"
        "  background: %3;"
        "  color: %4;"
        "  border: 1px solid %1;"
        "  border-bottom: none;"
        "  border-top-left-radius: 6px;"
        "  border-top-right-radius: 6px;"
        "  padding: 5px 14px;"
        "  margin-right: 4px;"
        "}"
        "QTabBar::tab:selected {"
        "  background: %2;"
        "  color: #ffffff;"
        "  border: 1px solid %1;"
        "  border-bottom: 1px solid %2;"
        "  font-weight: bold;"
        "}"
        "QTabBar::tab:!selected:hover {"
        "  background: %5;"
        "  color: %6;"
        "}"
        "QTabBar::tab:disabled {"
        "  color: %7;"
        "  background: %3;"
        "}")
                            .arg(Theme::Line.name(), Theme::Card.name(), Theme::Well.name(),
                                 Theme::TextDim.name(), Theme::CardHover.name(),
                                 Theme::Text.name(), Theme::TextFaint.name()));
}

QTabWidget *createEffectCategoryTabs(QWidget *parent) {
    auto *tabs = new QTabWidget(parent);
    styleEffectsTabs(tabs);
    return tabs;
}

QVBoxLayout *makeEffectTabPage(QTabWidget *tabs, const QString &label) {
    auto *page = new QWidget(tabs);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(10);
    tabs->addTab(page, label);
    return lay;
}

void applyProcessingDefaults(ChannelEffectsWindow::StageControls &ui) {
    const ChannelFxInfo fx;
    QSignalBlocker b1(ui.noise);
    QSignalBlocker b2(ui.intensity);
    QSignalBlocker b3(ui.lowCut);
    QSignalBlocker b4(ui.lowCutHz);
    QSignalBlocker b5(ui.eq);
    QSignalBlocker b6(ui.lowDb);
    QSignalBlocker b7(ui.midDb);
    QSignalBlocker b8(ui.highDb);
    Q_UNUSED(b1);
    Q_UNUSED(b2);
    Q_UNUSED(b3);
    Q_UNUSED(b4);
    Q_UNUSED(b5);
    Q_UNUSED(b6);
    Q_UNUSED(b7);
    Q_UNUSED(b8);
    ui.noise->setChecked(false);
    ui.intensity->setValue(100);
    ui.intensityLabel->setText(QStringLiteral("100%"));
    ui.lowCut->setChecked(fx.lowCut);
    ui.lowCutHz->setCurrentIndex(fx.lowCutHz == 120 ? 1 : 0);
    ui.eq->setChecked(fx.eq);
    ui.lowDb->setValue(int(std::lround(fx.lowDb * 10.0)));
    ui.midDb->setValue(int(std::lround(fx.midDb * 10.0)));
    ui.highDb->setValue(int(std::lround(fx.highDb * 10.0)));
    ui.lowReadout->setText(dbReadout(ui.lowDb->value()));
    ui.midReadout->setText(dbReadout(ui.midDb->value()));
    ui.highReadout->setText(dbReadout(ui.highDb->value()));
    // Back to the beginner EQ. The parametric curve itself is left alone --
    // this button resets the rows in this section, and the curve is not one of
    // them; the Advanced panel has a reset of its own.
    if (ui.adv.mode) {
        QSignalBlocker b9(ui.adv.mode);
        ui.adv.mode->setChecked(fx.eqAdvanced);
        applyEqModeRows(ui.adv);
    }
}

void applyDynamicsDefaults(ChannelEffectsWindow::StageControls &ui) {
    const MicDynamicsInfo dyn;
    if (ui.deEsser) {
        QSignalBlocker bd1(ui.deEsser);
        QSignalBlocker bd2(ui.deEsserAmount);
        ui.deEsser->setChecked(false);
        ui.deEsserAmount->setValue(50);
        if (ui.deEsserAmountReadout)
            ui.deEsserAmountReadout->setText(QStringLiteral("50%"));
    }
    QSignalBlocker b1(ui.gate);
    QSignalBlocker b2(ui.gateThreshold);
    QSignalBlocker b3(ui.compressor);
    QSignalBlocker b4(ui.compThreshold);
    QSignalBlocker b5(ui.compRatio);
    QSignalBlocker b6(ui.autoMakeup);
    QSignalBlocker b7(ui.limiter);
    QSignalBlocker b8(ui.limitThreshold);
    Q_UNUSED(b1);
    Q_UNUSED(b2);
    Q_UNUSED(b3);
    Q_UNUSED(b4);
    Q_UNUSED(b5);
    Q_UNUSED(b6);
    Q_UNUSED(b7);
    Q_UNUSED(b8);
    ui.gate->setChecked(dyn.gate);
    ui.gateThreshold->setValue(int(std::lround(dyn.gateThresholdDb * 10.0)));
    ui.gateThresholdReadout->setText(dbReadout(ui.gateThreshold->value()));
    ui.compressor->setChecked(dyn.compressor);
    ui.compThreshold->setValue(int(std::lround(dyn.compThresholdDb * 10.0)));
    ui.compThresholdReadout->setText(dbReadout(ui.compThreshold->value()));
    ui.compRatio->setValue(int(std::lround(dyn.compRatio * 10.0)));
    ui.compRatioReadout->setText(
        QStringLiteral("%1:1").arg(dyn.compRatio, 0, 'f', 1));
    ui.autoMakeup->setChecked(dyn.autoMakeup);
    ui.limiter->setChecked(dyn.limiter);
    ui.limitThreshold->setValue(int(std::lround(dyn.limitThresholdDb * 10.0)));
    ui.limitThresholdReadout->setText(dbReadout(ui.limitThreshold->value()));
}

void addEqSection(QWidget *parent, QVBoxLayout *lay, ToggleSwitch *&lowCut,
                  QComboBox *&lowCutHz, ToggleSwitch *&eq, QSlider *&lowDb,
                  QSlider *&midDb, QSlider *&highDb, QLabel *&lowReadout,
                  QLabel *&midReadout, QLabel *&highReadout, EqAdvancedControls &adv,
                  const QObject *receiver, auto pushSlot,
                  const std::function<void()> &onDepsChanged = {},
                  const std::function<void(bool)> &onModeChanged = {},
                  const std::function<void()> &onOpenAdvanced = {}) {
    lay->addWidget(switchRow(QObject::tr("Low-cut filter"),
                             QObject::tr("High-pass filter to reduce rumble."),
                             lowCut, parent));
    QObject::connect(lowCut, &QAbstractButton::toggled, receiver, pushSlot);
    if (onDepsChanged)
        QObject::connect(lowCut, &QAbstractButton::toggled, receiver,
                         [onDepsChanged] { onDepsChanged(); });

    auto *hzRow = new QHBoxLayout;
    hzRow->addWidget(dimLabel(QObject::tr("Cutoff"), parent));
    hzRow->addStretch();
    lowCutHz = new QComboBox(parent);
    lowCutHz->addItem(QStringLiteral("80 Hz"), 80);
    lowCutHz->addItem(QStringLiteral("120 Hz"), 120);
    lowCutHz->setFixedWidth(88);
    hzRow->addWidget(lowCutHz);
    lay->addLayout(hzRow);
    QObject::connect(lowCutHz, &QComboBox::currentIndexChanged, receiver, pushSlot);

    lay->addWidget(switchRow(QObject::tr("Equalizer"),
                             QObject::tr("Low, mid and high, or ten parametric bands."),
                             eq, parent));
    QObject::connect(eq, &QAbstractButton::toggled, receiver, pushSlot);
    if (onDepsChanged)
        QObject::connect(eq, &QAbstractButton::toggled, receiver,
                         [onDepsChanged] { onDepsChanged(); });

    lay->addWidget(switchRow(
        QObject::tr("Advanced EQ"),
        QObject::tr("Swap the three tone sliders for a ten-band parametric EQ with a "
                    "curve.\nBoth are kept: switching back brings your sliders with it."),
        adv.mode, parent));

    adv.lowRow = sliderRow(QObject::tr("Low"), lowDb, lowReadout, 48, parent);
    adv.midRow = sliderRow(QObject::tr("Mid"), midDb, midReadout, 48, parent);
    adv.highRow = sliderRow(QObject::tr("High"), highDb, highReadout, 48, parent);
    lay->addWidget(adv.lowRow);
    lay->addWidget(adv.midRow);
    lay->addWidget(adv.highRow);

    adv.openRow = eqOpenRow(adv.open, parent);
    lay->addWidget(adv.openRow);
    applyEqModeRows(adv);

    QObject::connect(adv.mode, &QAbstractButton::toggled, receiver,
                     [&adv, onModeChanged, onDepsChanged](bool on) {
                         applyEqModeRows(adv);
                         if (onModeChanged) onModeChanged(on);
                         if (onDepsChanged) onDepsChanged();
                     });
    if (onOpenAdvanced)
        QObject::connect(adv.open, &QPushButton::clicked, receiver,
                         [onOpenAdvanced] { onOpenAdvanced(); });

    for (auto *s : {lowDb, midDb, highDb}) {
        s->setRange(-120, 120);
        QObject::connect(s, &QSlider::valueChanged, receiver, pushSlot);
    }
    auto bindReadout = [](QSlider *s, QLabel *l) {
        QObject::connect(s, &QSlider::valueChanged, l, [s, l](int v) {
            l->setText(dbReadout(v));
        });
    };
    bindReadout(lowDb, lowReadout);
    bindReadout(midDb, midReadout);
    bindReadout(highDb, highReadout);
}

// The de-esser's two rows. One switch and one amount, wherever a dynamics
// section is built: an input device's microphone, a channel's microphone, and
// app audio all get the same pair.
// Reads the de-esser back into a stage's controls. Its own D-Bus pair, so it
// is not part of the dynamics block every refresh already fetches.
void showDeEsser(ChannelEffectsWindow::StageControls &ui, bool on, double amount) {
    if (!ui.deEsser || !ui.deEsserAmount) return;
    QSignalBlocker b1(ui.deEsser);
    QSignalBlocker b2(ui.deEsserAmount);
    ui.deEsser->setChecked(on);
    const int pct = int(std::lround(amount * 100.0));
    ui.deEsserAmount->setValue(pct);
    if (ui.deEsserAmountReadout)
        ui.deEsserAmountReadout->setText(QStringLiteral("%1%").arg(pct));
}

void addDeEsserRows(QWidget *parent, QVBoxLayout *lay, ToggleSwitch *&sw,
                    QSlider *&amount, QLabel *&readout,
                    const std::function<void(bool)> &onToggled,
                    const std::function<void(double)> &onAmount) {
    lay->addWidget(switchRow(
        QObject::tr("De-esser"),
        QObject::tr("Hold down sibilance -- the \"sss\" and \"tsh\" that a close\n"
                    "microphone exaggerates -- without touching the rest of the\n"
                    "voice. Listens above 5.5 kHz and shelves that band only."),
        sw, parent));
    QObject::connect(sw, &QAbstractButton::toggled, parent,
                     [onToggled](bool on) { onToggled(on); });

    lay->addWidget(sliderRow(QObject::tr("Amount"), amount, readout, 40, parent));
    amount->setRange(0, 100);
    amount->setToolTip(
        QObject::tr("How hard to lean on it. Higher starts lower down and\n"
                    "squeezes harder, up to about 18 dB off the band."));
    QObject::connect(amount, &QSlider::valueChanged, parent,
                     [onAmount](int v) { onAmount(v / 100.0); });
    QObject::connect(amount, &QSlider::valueChanged, readout, [readout](int v) {
        readout->setText(QStringLiteral("%1%").arg(v));
    });
}

void addDynamicsSection(QWidget *parent, QVBoxLayout *lay,
                        ChannelEffectsWindow::StageControls &ui,
                        const std::function<void()> &push,
                        const std::function<void()> &resetDynamics,
                        const std::function<void()> &onDepsChanged = {},
                        bool inTab = false,
                        const std::function<void(bool)> &onDeEsser = {},
                        const std::function<void(double)> &onDeEsserAmount = {}) {
    if (inTab)
        lay->addWidget(defaultResetRow(parent, resetDynamics));
    else
        lay->addWidget(sectionHeaderRow(QObject::tr("Dynamics"), parent, resetDynamics));

    lay->addWidget(switchRow(QObject::tr("Noise gate"),
                             QObject::tr("Mute when input falls below a threshold."),
                             ui.gate, parent));
    QObject::connect(ui.gate, &QAbstractButton::toggled, parent, [push, onDepsChanged] {
        push();
        if (onDepsChanged) onDepsChanged();
    });
    lay->addWidget(sliderRow(QObject::tr("Gate threshold"), ui.gateThreshold,
                             ui.gateThresholdReadout, 56, parent));
    ui.gateThreshold->setRange(-600, -200);
    QObject::connect(ui.gateThreshold, &QSlider::valueChanged, parent,
                     [push] { push(); });
    QObject::connect(ui.gateThreshold, &QSlider::valueChanged, ui.gateThresholdReadout,
                     [ui](int v) { ui.gateThresholdReadout->setText(dbReadout(v)); });

    lay->addWidget(switchRow(QObject::tr("Compressor"),
                             QObject::tr("Even out loud and quiet parts."),
                             ui.compressor, parent));
    QObject::connect(ui.compressor, &QAbstractButton::toggled, parent,
                     [push, onDepsChanged] {
                         push();
                         if (onDepsChanged) onDepsChanged();
                     });
    lay->addWidget(sliderRow(QObject::tr("Comp threshold"), ui.compThreshold,
                             ui.compThresholdReadout, 56, parent));
    lay->addWidget(
        sliderRow(QObject::tr("Ratio"), ui.compRatio, ui.compRatioReadout, 40, parent));
    ui.compThreshold->setRange(-400, -80);
    ui.compRatio->setRange(10, 120);
    QObject::connect(ui.compThreshold, &QSlider::valueChanged, parent,
                     [push] { push(); });
    QObject::connect(ui.compRatio, &QSlider::valueChanged, parent, [push] { push(); });
    QObject::connect(ui.compRatio, &QSlider::valueChanged, ui.compRatioReadout,
                     [ui](int v) {
                         ui.compRatioReadout->setText(
                             QStringLiteral("%1:1").arg(v / 10.0, 0, 'f', 1));
                     });
    QObject::connect(ui.compThreshold, &QSlider::valueChanged, ui.compThresholdReadout,
                     [ui](int v) { ui.compThresholdReadout->setText(dbReadout(v)); });

    lay->addWidget(switchRow(QObject::tr("Auto makeup gain"),
                             QObject::tr("Compensate level after compression."),
                             ui.autoMakeup, parent));
    QObject::connect(ui.autoMakeup, &QAbstractButton::toggled, parent,
                     [push, onDepsChanged] {
                         push();
                         if (onDepsChanged) onDepsChanged();
                     });

    lay->addWidget(switchRow(QObject::tr("Limiter"),
                             QObject::tr("Hard ceiling to prevent clipping."),
                             ui.limiter, parent));
    QObject::connect(ui.limiter, &QAbstractButton::toggled, parent,
                     [push, onDepsChanged] {
                         push();
                         if (onDepsChanged) onDepsChanged();
                     });
    lay->addWidget(sliderRow(QObject::tr("Ceiling"), ui.limitThreshold,
                             ui.limitThresholdReadout, 56, parent));
    ui.limitThreshold->setRange(-60, 0);
    QObject::connect(ui.limitThreshold, &QSlider::valueChanged, parent,
                     [push] { push(); });
    QObject::connect(ui.limitThreshold, &QSlider::valueChanged, ui.limitThresholdReadout,
                     [ui](int v) { ui.limitThresholdReadout->setText(dbReadout(v)); });

    // Last in the section, and on its own setters rather than push(): the
    // de-esser is stored beside the dynamics block, not inside it.
    if (onDeEsser && onDeEsserAmount)
        addDeEsserRows(
            parent, lay, ui.deEsser, ui.deEsserAmount, ui.deEsserAmountReadout,
            [onDeEsser, onDepsChanged](bool on) {
                onDeEsser(on);
                // Greys the amount slider with its switch, like every other
                // pair in the section.
                if (onDepsChanged) onDepsChanged();
            },
            onDeEsserAmount);
}

void addNcMeters(QWidget *parent, QVBoxLayout *lay, LevelMeter *&noiseIn,
                 LevelMeter *&noiseOut) {
    auto *meters = new QVBoxLayout;
    meters->setSpacing(5);
    noiseIn = new LevelMeter(Qt::Horizontal, parent);
    noiseOut = new LevelMeter(Qt::Horizontal, parent);
    noiseIn->setThickness(6);
    noiseOut->setThickness(6);
    auto *inRow = new QHBoxLayout;
    auto *inLabel = dimLabel(QObject::tr("In"), parent);
    inLabel->setFixedWidth(26);
    inRow->addWidget(inLabel);
    inRow->addWidget(noiseIn, 1);
    meters->addLayout(inRow);
    auto *outRow = new QHBoxLayout;
    auto *outLabel = dimLabel(QObject::tr("Out"), parent);
    outLabel->setFixedWidth(26);
    outRow->addWidget(outLabel);
    outRow->addWidget(noiseOut, 1);
    meters->addLayout(outRow);
    lay->addLayout(meters);
}

void addProcessingSection(const QString &ncTip, const QString &eqTip, QWidget *parent,
                          QVBoxLayout *lay, ChannelEffectsWindow::StageControls &ui,
                          const std::function<void()> &push,
                          const std::function<void()> &resetProcessing,
                          LevelMeter **ncIn = nullptr, LevelMeter **ncOut = nullptr,
                          const std::function<void()> &onDepsChanged = {},
                          bool inTab = false,
                          const std::function<void(bool)> &onEqModeChanged = {},
                          const std::function<void()> &onOpenAdvancedEq = {}) {
    if (inTab)
        lay->addWidget(defaultResetRow(parent, resetProcessing));
    else
        lay->addWidget(sectionHeaderRow(QObject::tr("Processing"), parent, resetProcessing));

    lay->addWidget(switchRow(QObject::tr("Noise suppression"), ncTip, ui.noise, parent));
    QObject::connect(ui.noise, &QAbstractButton::toggled, parent,
                     [push, onDepsChanged] {
                         push();
                         if (onDepsChanged) onDepsChanged();
                     });

    lay->addWidget(sliderRow(QObject::tr("Strength"), ui.intensity, ui.intensityLabel,
                             40, parent));
    ui.intensity->setRange(0, 100);

    if (ncIn && ncOut) addNcMeters(parent, lay, *ncIn, *ncOut);

    lay->addWidget(switchRow(QObject::tr("Low-cut filter"),
                             QObject::tr("High-pass filter to reduce rumble."),
                             ui.lowCut, parent));
    QObject::connect(ui.lowCut, &QAbstractButton::toggled, parent,
                     [push, onDepsChanged] {
                         push();
                         if (onDepsChanged) onDepsChanged();
                     });

    auto *hzRow = new QHBoxLayout;
    hzRow->addWidget(dimLabel(QObject::tr("Cutoff"), parent));
    hzRow->addStretch();
    ui.lowCutHz = new QComboBox(parent);
    ui.lowCutHz->addItem(QStringLiteral("80 Hz"), 80);
    ui.lowCutHz->addItem(QStringLiteral("120 Hz"), 120);
    ui.lowCutHz->setFixedWidth(88);
    hzRow->addWidget(ui.lowCutHz);
    lay->addLayout(hzRow);
    QObject::connect(ui.lowCutHz, &QComboBox::currentIndexChanged, parent,
                     [push] { push(); });

    lay->addWidget(switchRow(QObject::tr("Equalizer"), eqTip, ui.eq, parent));
    QObject::connect(ui.eq, &QAbstractButton::toggled, parent,
                     [push, onDepsChanged] {
                         push();
                         if (onDepsChanged) onDepsChanged();
                     });

    lay->addWidget(switchRow(
        QObject::tr("Advanced EQ"),
        QObject::tr("Swap the three tone sliders for a ten-band parametric EQ with a "
                    "curve.\nBoth are kept: switching back brings your sliders with it."),
        ui.adv.mode, parent));

    ui.adv.lowRow = sliderRow(QObject::tr("Low"), ui.lowDb, ui.lowReadout, 48, parent);
    ui.adv.midRow = sliderRow(QObject::tr("Mid"), ui.midDb, ui.midReadout, 48, parent);
    ui.adv.highRow = sliderRow(QObject::tr("High"), ui.highDb, ui.highReadout, 48, parent);
    lay->addWidget(ui.adv.lowRow);
    lay->addWidget(ui.adv.midRow);
    lay->addWidget(ui.adv.highRow);

    ui.adv.openRow = eqOpenRow(ui.adv.open, parent);
    lay->addWidget(ui.adv.openRow);
    applyEqModeRows(ui.adv);

    EqAdvancedControls *adv = &ui.adv;
    QObject::connect(ui.adv.mode, &QAbstractButton::toggled, parent,
                     [adv, onEqModeChanged, onDepsChanged](bool on) {
                         applyEqModeRows(*adv);
                         if (onEqModeChanged) onEqModeChanged(on);
                         if (onDepsChanged) onDepsChanged();
                     });
    if (onOpenAdvancedEq)
        QObject::connect(ui.adv.open, &QPushButton::clicked, parent,
                         [onOpenAdvancedEq] { onOpenAdvancedEq(); });

    for (auto *s : {ui.lowDb, ui.midDb, ui.highDb}) {
        s->setRange(-120, 120);
        QObject::connect(s, &QSlider::valueChanged, parent, [push] { push(); });
    }
    auto bindReadout = [](QSlider *s, QLabel *l) {
        QObject::connect(s, &QSlider::valueChanged, l, [s, l](int v) {
            l->setText(dbReadout(v));
        });
    };
    bindReadout(ui.lowDb, ui.lowReadout);
    bindReadout(ui.midDb, ui.midReadout);
    bindReadout(ui.highDb, ui.highReadout);

    if (onDepsChanged) onDepsChanged();
}

MicDynamicsInfo dynamicsFromStageControls(const FxStageControls &ui) {
    MicDynamicsInfo d;
    d.gate = ui.gate->isChecked();
    d.gateThresholdDb = ui.gateThreshold->value() / 10.0;
    d.gateAttackMs = 3.0;
    d.gateReleaseMs = 300.0;
    d.compressor = ui.compressor->isChecked();
    d.compThresholdDb = ui.compThreshold->value() / 10.0;
    d.compRatio = ui.compRatio->value() / 10.0;
    d.compAttackMs = 5.0;
    d.compReleaseMs = 150.0;
    d.compKneeDb = 6.0;
    d.makeupGainDb = 0.0;
    d.autoMakeup = ui.autoMakeup->isChecked();
    d.limiter = ui.limiter->isChecked();
    d.limitThresholdDb = ui.limitThreshold->value() / 10.0;
    d.limitAttackMs = 1.0;
    d.limitReleaseMs = 50.0;
    return d;
}

void setStageControlsEnabled(FxStageControls &ui, bool on) {
    if (ui.noise) ui.noise->setEnabled(on);
    if (ui.intensity) ui.intensity->setEnabled(on);
    if (ui.intensityLabel) ui.intensityLabel->setEnabled(on);
    if (ui.lowCut) ui.lowCut->setEnabled(on);
    if (ui.lowCutHz) ui.lowCutHz->setEnabled(on);
    if (ui.eq) ui.eq->setEnabled(on);
    if (ui.lowDb) ui.lowDb->setEnabled(on);
    if (ui.midDb) ui.midDb->setEnabled(on);
    if (ui.highDb) ui.highDb->setEnabled(on);
    if (ui.lowReadout) ui.lowReadout->setEnabled(on);
    if (ui.midReadout) ui.midReadout->setEnabled(on);
    if (ui.highReadout) ui.highReadout->setEnabled(on);
    if (ui.adv.mode) ui.adv.mode->setEnabled(on);
    if (ui.adv.open) ui.adv.open->setEnabled(on);
    if (ui.gate) ui.gate->setEnabled(on);
    if (ui.gateThreshold) ui.gateThreshold->setEnabled(on);
    if (ui.gateThresholdReadout) ui.gateThresholdReadout->setEnabled(on);
    if (ui.compressor) ui.compressor->setEnabled(on);
    if (ui.compThreshold) ui.compThreshold->setEnabled(on);
    if (ui.compThresholdReadout) ui.compThresholdReadout->setEnabled(on);
    if (ui.compRatio) ui.compRatio->setEnabled(on);
    if (ui.compRatioReadout) ui.compRatioReadout->setEnabled(on);
    if (ui.autoMakeup) ui.autoMakeup->setEnabled(on);
    if (ui.limiter) ui.limiter->setEnabled(on);
    if (ui.limitThreshold) ui.limitThreshold->setEnabled(on);
    if (ui.limitThresholdReadout) ui.limitThresholdReadout->setEnabled(on);
}

void applyStageControlDependencies(FxStageControls &ui, bool editable,
                                   LevelMeter *ncIn = nullptr,
                                   LevelMeter *ncOut = nullptr) {
    const bool nc = ui.noise && ui.noise->isChecked();
    const bool lowCut = ui.lowCut && ui.lowCut->isChecked();
    const bool eq = ui.eq && ui.eq->isChecked();
    const bool gate = ui.gate && ui.gate->isChecked();
    const bool comp = ui.compressor && ui.compressor->isChecked();
    const bool lim = ui.limiter && ui.limiter->isChecked();
    const bool deEss = ui.deEsser && ui.deEsser->isChecked();

    const auto dep = [](QWidget *w, bool on) {
        if (w) w->setEnabled(on);
    };

    dep(ui.deEsserAmount, editable && deEss);
    dep(ui.deEsserAmountReadout, editable && deEss);

    dep(ui.intensity, editable && nc);
    dep(ui.intensityLabel, editable && nc);
    if (ncIn) ncIn->setEnabled(editable && nc);
    if (ncOut) ncOut->setEnabled(editable && nc);

    dep(ui.lowCutHz, editable && lowCut);

    dep(ui.lowDb, editable && eq);
    dep(ui.midDb, editable && eq);
    dep(ui.highDb, editable && eq);
    dep(ui.lowReadout, editable && eq);
    dep(ui.midReadout, editable && eq);
    dep(ui.highReadout, editable && eq);
    // Not gated on the EQ switch, unlike the sliders it replaces. The panel
    // carries that switch too, so a user who is in Advanced with the EQ off
    // has to be able to reach the panel to turn it back on.
    dep(ui.adv.mode, editable);
    dep(ui.adv.open, editable);

    dep(ui.gateThreshold, editable && gate);
    dep(ui.gateThresholdReadout, editable && gate);

    dep(ui.compThreshold, editable && comp);
    dep(ui.compThresholdReadout, editable && comp);
    dep(ui.compRatio, editable && comp);
    dep(ui.compRatioReadout, editable && comp);
    dep(ui.autoMakeup, editable && comp);

    dep(ui.limitThreshold, editable && lim);
    dep(ui.limitThresholdReadout, editable && lim);
}

void applyDuckingControlDependencies(DuckingControls &d, bool editable) {
    const bool on = d.enabled && d.enabled->isChecked();
    const bool depOn = editable && on;
    if (d.intensity) d.intensity->setEnabled(depOn);
    if (d.intensityLabel) d.intensityLabel->setEnabled(depOn);
    if (d.hold) d.hold->setEnabled(depOn);
    if (d.holdLabel) d.holdLabel->setEnabled(depOn);
    if (d.addSource)
        d.addSource->setEnabled(depOn && d.sourceRows.size() < 6);
    for (const DuckingSourceRow &row : d.sourceRows) {
        if (row.kind) row.kind->setEnabled(depOn);
        if (row.channel) row.channel->setEnabled(depOn);
        if (row.remove) row.remove->setEnabled(depOn);
    }
}

QString lufsReadout(int tenths) {
    return QStringLiteral("%1 LUFS").arg(tenths / 10.0, 0, 'f', 1);
}

void styleEarProtectionSlider(QSlider *slider, int lufsTenths) {
    if (!slider) return;
    const double lufs = lufsTenths / 10.0;
    QColor groove = Theme::CardHover;
    QColor fill = Theme::AccentDim;
    QColor handle = Theme::Fader;
    if (lufs >= -12.0) {
        groove = Theme::Danger.darker(140);
        fill = Theme::Danger;
        handle = Theme::Danger;
    } else if (lufs >= -18.0) {
        groove = Theme::Warn.darker(140);
        fill = Theme::Warn;
        handle = Theme::Warn;
    }
    slider->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { height: 6px; background: %1; border-radius: 3px; }"
        "QSlider::sub-page:horizontal { background: %2; border-radius: 3px; }"
        "QSlider::add-page:horizontal { background: %1; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: %3; width: 14px; margin: -4px 0; "
        "border-radius: 7px; }")
                              .arg(groove.name(), fill.name(), handle.name()));
}

void applyLufsLimiterControlDependencies(LufsLimiterControls &ui, bool editable) {
    const bool on = ui.enabled && ui.enabled->isChecked();
    const bool depOn = editable && on;
    if (ui.maxLufs) ui.maxLufs->setEnabled(depOn);
    if (ui.maxLufsReadout) ui.maxLufsReadout->setEnabled(depOn);
}

void buildLufsLimiterSection(LufsLimiterControls &ui, QWidget *parent, QVBoxLayout *lay,
                             const std::function<void()> &onDefault,
                             const std::function<void()> &onChanged,
                             bool inTab = false) {
    if (inTab)
        lay->addWidget(defaultResetRow(parent, onDefault));
    else
        lay->addWidget(sectionHeaderRow(QObject::tr("LUFS Limiter / Ear-Protection"), parent,
                                        onDefault));
    lay->addWidget(switchRow(
        QObject::tr("Limiter"),
        QObject::tr("Caps app-audio loudness using a BS.1770-style LUFS meter.\n"
                    "Turn down the ceiling if headphones stay too loud."),
        ui.enabled, parent));
    QObject::connect(ui.enabled, &QAbstractButton::toggled, parent,
                     [onChanged](bool) { onChanged(); });
    lay->addWidget(sliderRow(QObject::tr("Max loudness"), ui.maxLufs, ui.maxLufsReadout, 72,
                             parent));
    ui.maxLufs->setRange(-300, -80);
    ui.maxLufs->setToolTip(
        QObject::tr("Maximum loudness allowed through this channel.\n"
                    "Audio louder than this is pulled down; quieter passages are "
                    "unchanged.\n"
                    "Affects what you hear in the Monitor mix as well as Stream.\n"
                    "Higher values (less negative) are louder and more likely to cause "
                    "hearing fatigue or damage with extended listening."));
    QObject::connect(ui.maxLufs, &QSlider::valueChanged, parent, [onChanged, &ui](int v) {
        if (ui.maxLufsReadout) ui.maxLufsReadout->setText(lufsReadout(v));
        styleEarProtectionSlider(ui.maxLufs, v);
        onChanged();
    });
}

QString stageSettingsSignature(MixerClient *client, const QString &channelId,
                               const QString &stage) {
    const ChannelFxInfo fx = client->channelEffects(channelId, stage);
    const MicDynamicsInfo dyn = client->channelDynamics(channelId, stage);
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13|%14|%15")
        .arg(fx.lowCut)
        .arg(fx.lowCutHz)
        .arg(fx.eq)
        .arg(fx.lowDb, 0, 'f', 2)
        .arg(fx.midDb, 0, 'f', 2)
        .arg(fx.highDb, 0, 'f', 2)
        .arg(client->channelNoiseSuppression(channelId, stage))
        .arg(client->channelNoiseIntensity(channelId, stage), 0, 'f', 3)
        .arg(dyn.gate)
        .arg(dyn.compressor)
        .arg(dyn.limiter)
        .arg(dyn.autoMakeup)
        .arg(dyn.gateThresholdDb, 0, 'f', 1)
        .arg(fx.eqAdvanced)
        .arg(fx.proEqBands);
}

QString masterBusDisplayLabel(const QList<MasterBusInfo> &buses,
                              const MasterBusInfo &bus) {
    int slot = 1;
    for (int i = 0; i < buses.size(); ++i) {
        if (buses[i].id == bus.id) {
            slot = i + 1;
            break;
        }
    }
    if (!bus.name.isEmpty()) return bus.name;
    return QObject::tr("Input #%1").arg(slot);
}

// Selected devices plus the devices there are to choose from: the picker has to
// be rebuilt when either changes, and nothing else about it can drift.
QString masterMicRowsSignature(const QStringList &ids,
                               const QList<MasterBusInfo> &buses) {
    QString s = ids.join(QLatin1Char('|')) + QLatin1Char('#');
    for (const MasterBusInfo &b : buses)
        s += b.id + QLatin1Char(':') + b.name + QLatin1Char(',');
    return s;
}

QPushButton *rowIconButton(const QString &icon, const QString &tip, QWidget *parent) {
    auto *b = new QPushButton(parent);
    b->setIcon(Theme::icon(icon, Theme::TextDim, 12));
    b->setFixedSize(22, 22);
    b->setFlat(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setToolTip(tip);
    return b;
}

QString masterStageSettingsSignature(MixerClient *client, const QString &masterId) {
    const ChannelFxInfo fx = client->masterChannelEffects(masterId, QStringLiteral("input"));
    const MicDynamicsInfo dyn = client->masterMicDynamics(masterId);
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13|%14|%15")
        .arg(fx.lowCut)
        .arg(fx.lowCutHz)
        .arg(fx.eq)
        .arg(fx.lowDb, 0, 'f', 2)
        .arg(fx.midDb, 0, 'f', 2)
        .arg(fx.highDb, 0, 'f', 2)
        .arg(client->masterNoiseSuppression(masterId))
        .arg(client->masterNoiseIntensity(masterId), 0, 'f', 3)
        .arg(dyn.gate)
        .arg(dyn.compressor)
        .arg(dyn.limiter)
        .arg(dyn.autoMakeup)
        .arg(dyn.gateThresholdDb, 0, 'f', 1)
        .arg(fx.eqAdvanced)
        .arg(fx.proEqBands);
}

void refreshMasterStage(MixerClient *client, const QString &masterId, FxStageControls &ui) {
    const ChannelFxInfo fx =
        client->masterChannelEffects(masterId, QStringLiteral("input"));
    const MicDynamicsInfo dyn = client->masterMicDynamics(masterId);

    QSignalBlocker b1(ui.noise);
    QSignalBlocker b2(ui.intensity);
    QSignalBlocker b3(ui.lowCut);
    QSignalBlocker b4(ui.lowCutHz);
    QSignalBlocker b5(ui.eq);
    QSignalBlocker b6(ui.lowDb);
    QSignalBlocker b7(ui.midDb);
    QSignalBlocker b8(ui.highDb);
    QSignalBlocker b9(ui.gate);
    QSignalBlocker b10(ui.gateThreshold);
    QSignalBlocker b11(ui.compressor);
    QSignalBlocker b12(ui.compThreshold);
    QSignalBlocker b13(ui.compRatio);
    QSignalBlocker b14(ui.autoMakeup);
    QSignalBlocker b15(ui.limiter);
    QSignalBlocker b16(ui.limitThreshold);

    ui.noise->setChecked(client->masterNoiseSuppression(masterId));
    const int pct = int(client->masterNoiseIntensity(masterId) * 100.0 + 0.5);
    ui.intensity->setValue(pct);
    ui.intensityLabel->setText(QStringLiteral("%1%").arg(pct));

    ui.lowCut->setChecked(fx.lowCut);
    ui.lowCutHz->setCurrentIndex(fx.lowCutHz == 120 ? 1 : 0);
    ui.eq->setChecked(fx.eq);
    ui.lowDb->setValue(int(std::lround(fx.lowDb * 10.0)));
    ui.midDb->setValue(int(std::lround(fx.midDb * 10.0)));
    ui.highDb->setValue(int(std::lround(fx.highDb * 10.0)));
    ui.lowReadout->setText(dbReadout(ui.lowDb->value()));
    ui.midReadout->setText(dbReadout(ui.midDb->value()));
    ui.highReadout->setText(dbReadout(ui.highDb->value()));
    showEqMode(ui.adv, fx.eqAdvanced);

    ui.gate->setChecked(dyn.gate);
    ui.gateThreshold->setValue(int(std::lround(dyn.gateThresholdDb * 10.0)));
    ui.gateThresholdReadout->setText(dbReadout(ui.gateThreshold->value()));
    ui.compressor->setChecked(dyn.compressor);
    ui.compThreshold->setValue(int(std::lround(dyn.compThresholdDb * 10.0)));
    ui.compThresholdReadout->setText(dbReadout(ui.compThreshold->value()));
    ui.compRatio->setValue(int(std::lround(dyn.compRatio * 10.0)));
    ui.compRatioReadout->setText(QStringLiteral("%1:1").arg(dyn.compRatio, 0, 'f', 1));
    ui.autoMakeup->setChecked(dyn.autoMakeup);
    ui.limiter->setChecked(dyn.limiter);
    ui.limitThreshold->setValue(int(std::lround(dyn.limitThresholdDb * 10.0)));
    ui.limitThresholdReadout->setText(dbReadout(ui.limitThreshold->value()));
}

// Not a bus id, and never stored as one: the daemon keeps this mode in its own
// flag, so the sentinel is translated at the two points the combo is read and
// written. A bus id here would be coerced to "mic" by resolveMasterBusId.
const char kDeviceFxSourceId[] = "@device-fx";

void populateEffectSourceCombo(QComboBox *combo, MixerClient *client,
                               bool withDeviceFx = false) {
    if (!combo || !client || !client->available()) return;
    const QString current = combo->currentData().toString();
    QSignalBlocker block(combo);
    combo->clear();
    combo->addItem(QObject::tr("Unique Channel Effects"), QString());
    if (withDeviceFx) {
        combo->addItem(QObject::tr("Use device(s) effects"),
                       QLatin1String(kDeviceFxSourceId));
    }
    const QList<MasterBusInfo> buses = client->masterBuses();
    for (const MasterBusInfo &b : buses)
        combo->addItem(masterBusDisplayLabel(buses, b), b.id);
    int idx = combo->findData(current);
    if (idx < 0 && !current.isEmpty())
        idx = combo->findData(QStringLiteral("mic"));
    if (idx >= 0) combo->setCurrentIndex(idx);
}

void populateDuckingTargetCombo(QComboBox *combo, MixerClient *client,
                                const QString &kind, const QString &previous) {
    if (!combo || !client || !client->available()) return;
    QSignalBlocker block(combo);
    combo->clear();
    if (kind == QLatin1String("master_mic")) {
        const QList<MasterBusInfo> buses = client->masterBuses();
        for (const MasterBusInfo &b : buses)
            combo->addItem(masterBusDisplayLabel(buses, b), b.id);
        QString prev = previous;
        if (prev.isEmpty()) prev = QStringLiteral("mic");
        int idx = combo->findData(prev);
        if (idx < 0) idx = combo->findData(QStringLiteral("mic"));
        if (idx < 0 && combo->count() > 0) idx = 0;
        if (idx >= 0) combo->setCurrentIndex(idx);
        return;
    }
    if (kind == QLatin1String("channel_mic")) {
        for (const ChannelInfo &ch : client->channels()) {
            if (ch.id == QLatin1String("mic")) continue;
            bool ok = false;
            if (!client->channelMicSource(ch.id, &ok) || !ok) continue;
            combo->addItem(ch.name, ch.id);
        }
    } else if (kind == QLatin1String("channel_audio")) {
        for (const ChannelInfo &ch : client->channels()) {
            if (ch.id == QLatin1String("mic")) continue;
            combo->addItem(ch.name, ch.id);
        }
    }
    int idx = combo->findData(previous);
    if (idx < 0 && combo->count() > 0) idx = 0;
    if (idx >= 0) combo->setCurrentIndex(idx);
}

QWidget *effectSourceRow(QComboBox *&combo, const QObject *receiver, auto changedSlot,
                         QWidget *parent) {
    auto *row = new QWidget(parent);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);
    auto *label = new QLabel(QObject::tr("Effect Source"), row);
    QFont f = label->font();
    f.setBold(true);
    label->setFont(f);
    lay->addWidget(label);
    lay->addStretch();
    combo = new QComboBox(row);
    combo->setMinimumWidth(220);
    combo->setToolTip(
        QObject::tr("Unique: adjust effects for this channel only.\n"
                    "Input Device: inherit effects from the selected input device."));
    lay->addWidget(combo);
    QObject::connect(combo, &QComboBox::currentIndexChanged, receiver, changedSlot);
    return row;
}

}  // namespace

void ChannelEffectsWindow::buildDuckingUi(QWidget *parent, QVBoxLayout *lay, bool inTab) {
    if (inTab) {
        lay->addWidget(defaultResetRow(parent, [this] {
            if (!client_->available()) return;
            updating_ = true;
            ducking_.enabled->setChecked(false);
            ducking_.intensity->setValue(75);
            ducking_.intensityLabel->setText(QStringLiteral("75%"));
            ducking_.hold->setValue(30);
            ducking_.holdLabel->setText(duckHoldReadout(30));
            setDuckingSourcesFromConfig(
                {DuckingSourceInfo{QStringLiteral("master_mic"), QStringLiteral("mic")}});
            updating_ = false;
            pushDuckingSettings();
            applyDuckingDependencies();
        }));
    } else {
        lay->addWidget(sectionHeaderRow(tr("Ducking"), parent, [this] {
            if (!client_->available()) return;
            updating_ = true;
            ducking_.enabled->setChecked(false);
            ducking_.intensity->setValue(75);
            ducking_.intensityLabel->setText(QStringLiteral("75%"));
            ducking_.hold->setValue(30);
            ducking_.holdLabel->setText(duckHoldReadout(30));
            setDuckingSourcesFromConfig(
                {DuckingSourceInfo{QStringLiteral("master_mic"), QStringLiteral("mic")}});
            updating_ = false;
            pushDuckingSettings();
            applyDuckingDependencies();
        }));
    }

    lay->addWidget(switchRow(
        tr("Enable ducking"),
        tr("Smoothly lower this channel when any sidechain source is active."),
        ducking_.enabled, parent));
    connect(ducking_.enabled, &QAbstractButton::toggled, this,
            [this] {
                pushDuckingSettings();
                applyDuckingDependencies();
            });

    auto *srcHint = dimLabel(
        tr("Sidechain sources (up to 6). Ducks when any source is active."), parent);
    srcHint->setWordWrap(true);
    lay->addWidget(srcHint);

    ducking_.sourcesHost = new QWidget(parent);
    ducking_.sourcesLayout = new QVBoxLayout(ducking_.sourcesHost);
    ducking_.sourcesLayout->setContentsMargins(0, 0, 0, 0);
    ducking_.sourcesLayout->setSpacing(6);
    lay->addWidget(ducking_.sourcesHost);

    ducking_.addSource = new QPushButton(tr("Add source"), parent);
    ducking_.addSource->setMaximumWidth(120);
    connect(ducking_.addSource, &QPushButton::clicked, this, [this] {
        addDuckingSourceRow();
        pushDuckingSettings();
    });
    lay->addWidget(ducking_.addSource);

    lay->addWidget(sliderRow(tr("Intensity"), ducking_.intensity, ducking_.intensityLabel,
                             40, parent));
    ducking_.intensity->setRange(0, 100);
    ducking_.intensity->setToolTip(
        tr("How much this channel ducks when a sidechain source is active."));
    connect(ducking_.intensity, &QSlider::valueChanged, this, [this](int v) {
        ducking_.intensityLabel->setText(QStringLiteral("%1%").arg(v));
        pushDuckingSettings();
    });

    lay->addWidget(
        sliderRow(tr("Hold"), ducking_.hold, ducking_.holdLabel, 44, parent));
    ducking_.hold->setRange(0, 100);
    ducking_.hold->setToolTip(
        tr("How long the sidechain has to stay quiet before this channel comes "
           "back up. Pauses between words will not lift it."));
    connect(ducking_.hold, &QSlider::valueChanged, this, [this](int v) {
        ducking_.holdLabel->setText(duckHoldReadout(v));
        pushDuckingSettings();
    });

    addDuckingSourceRow();
}

void ChannelEffectsWindow::clearDuckingSourceRows() {
    for (const DuckingSourceRow &row : ducking_.sourceRows) {
        ducking_.sourcesLayout->removeWidget(row.row);
        row.row->deleteLater();
    }
    ducking_.sourceRows.clear();
    updateDuckingAddButton();
}

void ChannelEffectsWindow::addDuckingSourceRow(const DuckingSourceInfo &src) {
    if (ducking_.sourceRows.size() >= 6) return;

    DuckingSourceRow row;
    row.row = new QWidget(ducking_.sourcesHost);
    auto *rowLay = new QHBoxLayout(row.row);
    rowLay->setContentsMargins(0, 0, 0, 0);
    rowLay->setSpacing(6);

    row.kind = new QComboBox(row.row);
    row.kind->addItem(tr("Input device"), QStringLiteral("master_mic"));
    row.kind->addItem(tr("Channel microphone"), QStringLiteral("channel_mic"));
    row.kind->addItem(tr("Channel audio"), QStringLiteral("channel_audio"));
    row.kind->setToolTip(
        tr("Input device: one of your Input Device strips.\n"
           "Channel mic: a published channel recording device.\n"
           "Channel audio: app audio on another channel (e.g. Game)."));
    rowLay->addWidget(row.kind, 1);

    row.channel = new QComboBox(row.row);
    row.channel->setMinimumWidth(100);
    rowLay->addWidget(row.channel, 1);

    row.remove = new QPushButton(tr("Remove"), row.row);
    row.remove->setMaximumWidth(72);
    rowLay->addWidget(row.remove);

    ducking_.sourceRows.append(row);
    ducking_.sourcesLayout->addWidget(row.row);

    const QString kind = src.kind.isEmpty() ? QStringLiteral("master_mic") : src.kind;
    const int kindIdx = row.kind->findData(kind);
    row.kind->setCurrentIndex(kindIdx >= 0 ? kindIdx : 0);
    populateDuckingChannelCombo(row);
    if (!src.channelId.isEmpty()) {
        const int chIdx = row.channel->findData(src.channelId);
        if (chIdx >= 0) row.channel->setCurrentIndex(chIdx);
    }

    connect(row.kind, &QComboBox::currentIndexChanged, this,
            [this, rowWidget = row.row] {
                for (DuckingSourceRow &r : ducking_.sourceRows) {
                    if (r.row != rowWidget) continue;
                    populateDuckingChannelCombo(r);
                    break;
                }
                pushDuckingSettings();
            });
    connect(row.channel, &QComboBox::currentIndexChanged, this,
            [this] { pushDuckingSettings(); });
    connect(row.remove, &QPushButton::clicked, this, [this, rowWidget = row.row] {
        for (int i = 0; i < ducking_.sourceRows.size(); ++i) {
            if (ducking_.sourceRows[i].row != rowWidget) continue;
            removeDuckingSourceRow(i);
            break;
        }
        pushDuckingSettings();
    });

    updateDuckingAddButton();
}

QStringList ChannelEffectsWindow::selectedMasterMics() const {
    QStringList ids;
    for (const QComboBox *box : masterMicBoxes_) {
        const QString id = box->currentData().toString();
        if (!id.isEmpty()) ids << id;
    }
    return ids;
}

void ChannelEffectsWindow::pushMasterMics() {
    if (!client_->available()) return;
    const QStringList ids = selectedMasterMics();
    if (ids.isEmpty()) return;
    // Claim the signature now. The daemon answers with Changed(), and without
    // this refresh() would tear the rows down and rebuild them under the mouse
    // on the very poll that follows the click.
    masterMicSignature_ = masterMicRowsSignature(ids, client_->masterBuses());
    client_->setChannelMasterMics(channelId_, ids);
}

void ChannelEffectsWindow::rebuildMasterMicRows(const QStringList &selected) {
    if (!masterMicRows_) return;
    QWidget *host = masterMicRows_->parentWidget();
    if (!host) return;

    // Rebuilt wholesale rather than patched: the rows differ only in which
    // button they carry, and their number is one or two in practice.
    updating_ = true;
    while (QLayoutItem *item = masterMicRows_->takeAt(0)) {
        if (QWidget *w = item->widget()) w->deleteLater();
        delete item;
    }
    masterMicBoxes_.clear();

    const QList<MasterBusInfo> buses = client_->masterBuses();
    const QStringList ids = selected.isEmpty() ? QStringList{QStringLiteral("mic")}
                                               : selected;
    const bool canAdd = ids.size() < buses.size();

    for (int i = 0; i < ids.size(); ++i) {
        auto *row = new QWidget(host);
        auto *lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(6);

        // Only the first row is captioned -- the ones under it are more of the
        // same setting, not new ones. The blank keeps the boxes aligned.
        auto *caption = dimLabel(i == 0 ? tr("Input device") : QString(), row);
        caption->setFixedWidth(
            QFontMetrics(caption->font()).horizontalAdvance(tr("Input device")) + 4);
        lay->addWidget(caption);

        auto *box = new QComboBox(row);
        box->setMinimumWidth(150);
        box->setToolTip(
            tr("Which input device supplies capture for this channel's "
               "microphone chain.\n"
               "Several can be listed: they are mixed together into the one "
               "recording device\nthis channel publishes."));
        for (const MasterBusInfo &b : buses)
            box->addItem(masterBusDisplayLabel(buses, b), b.id);
        const int at = box->findData(ids.at(i));
        box->setCurrentIndex(at >= 0 ? at : 0);
        lay->addWidget(box, 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this] {
            if (updating_) return;
            pushMasterMics();
        });
        masterMicBoxes_.append(box);

        if (i == 0) {
            auto *add = rowIconButton(
                QStringLiteral("plus"),
                tr("Add another input device to this channel's microphone."), row);
            add->setEnabled(canAdd);
            connect(add, &QPushButton::clicked, this, [this] {
                QStringList next = selectedMasterMics();
                // The first device not already listed, so the new row is never
                // a duplicate the daemon would drop again.
                for (const MasterBusInfo &b : client_->masterBuses()) {
                    if (next.contains(b.id)) continue;
                    next << b.id;
                    break;
                }
                if (next.size() == masterMicBoxes_.size()) return;
                rebuildMasterMicRows(next);
                pushMasterMics();
            });
            lay->addWidget(add);
        } else {
            auto *remove = rowIconButton(QStringLiteral("minus"),
                                         tr("Remove this input device."), row);
            connect(remove, &QPushButton::clicked, this, [this, i] {
                QStringList next = selectedMasterMics();
                if (i >= next.size()) return;
                next.removeAt(i);
                rebuildMasterMicRows(next);
                pushMasterMics();
            });
            lay->addWidget(remove);
        }

        masterMicRows_->addWidget(row);
    }
    updating_ = false;
    // Adding or removing a MIDI instrument changes whether the noise filter is
    // in the mic chain at all.
    applyMidiNoiseRule();
}

void ChannelEffectsWindow::removeDuckingSourceRow(int index) {
    if (index < 0 || index >= ducking_.sourceRows.size()) return;
    DuckingSourceRow row = ducking_.sourceRows.takeAt(index);
    ducking_.sourcesLayout->removeWidget(row.row);
    row.row->deleteLater();
    updateDuckingAddButton();
}

void ChannelEffectsWindow::populateDuckingChannelCombo(DuckingSourceRow &row) {
    if (!row.channel || !client_->available()) return;
    const QString kind = row.kind->currentData().toString();
    row.channel->setVisible(true);
    const QString prev = row.channel->currentData().toString();
    populateDuckingTargetCombo(row.channel, client_, kind, prev);
}

void ChannelEffectsWindow::updateDuckingAddButton() {
    if (ducking_.addSource)
        ducking_.addSource->setEnabled(ducking_.sourceRows.size() < 6);
}

QString ChannelEffectsWindow::encodeDuckingSourcesFromUi() const {
    QStringList parts;
    for (const DuckingSourceRow &row : ducking_.sourceRows) {
        const QString kind = row.kind->currentData().toString();
        if (kind == QLatin1String("master_mic")) {
            QString masterId = row.channel->currentData().toString();
            if (masterId.isEmpty()) masterId = QStringLiteral("mic");
            parts << QStringLiteral("master_mic:") + masterId;
        } else {
            const QString chId = row.channel->currentData().toString();
            if (!chId.isEmpty()) parts << kind + QLatin1Char(':') + chId;
        }
    }
    if (parts.isEmpty()) parts << QStringLiteral("master_mic");
    return parts.join(QLatin1Char('|'));
}

void ChannelEffectsWindow::setDuckingSourcesFromConfig(
    const QList<DuckingSourceInfo> &sources) {
    clearDuckingSourceRows();
    if (sources.isEmpty()) {
        addDuckingSourceRow();
        return;
    }
    for (const DuckingSourceInfo &src : sources) addDuckingSourceRow(src);
}

GlobalEffectsWindow::GlobalEffectsWindow(MixerClient *client, const QString &masterId,
                                         QWidget *parent)
    : QWidget(parent, Qt::Window), client_(client), masterId_(masterId) {
    setFixedWidth(kEffectsWindowWidth);
    updateWindowTitle();
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 16, 16, 16);
    lay->setSpacing(12);

    auto *centreRow = new QHBoxLayout;
    auto *centreLabel = new QLabel(tr("Centre the microphone"), this);
    centreLabel->setToolTip(
        tr("Copy the mono capture to both stereo channels in the mixes."));
    micStereo_ = new ToggleSwitch(this);
    micStereo_->setToolTip(centreLabel->toolTip());
    centreRow->addWidget(centreLabel);
    centreRow->addStretch();
    centreRow->addWidget(micStereo_);
    lay->addLayout(centreRow);
    connect(micStereo_, &QAbstractButton::toggled, this, [this](bool on) {
        client_->setMasterMicStereo(masterId_, on);
    });

    tabs_ = new QTabWidget(this);
    styleEffectsTabs(tabs_);
    lay->addWidget(tabs_, 1);

    auto *micTab = new QWidget(this);
    auto *micLay = new QVBoxLayout(micTab);
    micLay->setContentsMargins(8, 12, 8, 8);
    micLay->setSpacing(12);

    auto *captureRow = new QHBoxLayout;
    captureRowWidget_ = new QWidget(micTab);
    auto *captureRowLay = new QHBoxLayout(captureRowWidget_);
    captureRowLay->setContentsMargins(0, 0, 0, 0);
    captureRowLay->addWidget(dimLabel(tr("Capture device"), captureRowWidget_));
    captureDevice_ = new QComboBox(captureRowWidget_);
    captureDevice_->setMinimumWidth(180);
    captureDevice_->setToolTip(tr("Which microphone this input device captures from."));
    captureRowLay->addWidget(captureDevice_, 1);
    captureRow->addWidget(captureRowWidget_);
    connect(captureDevice_, &QComboBox::activated, this, [this](int idx) {
        if (!client_->available()) return;
        const QString match = captureDevice_->itemData(idx).toString();
        client_->setMasterCaptureDevice(masterId_, match);
    });

    midiPortRowWidget_ = new QWidget(micTab);
    auto *midiRowLay = new QHBoxLayout(midiPortRowWidget_);
    midiRowLay->setContentsMargins(0, 0, 0, 0);
    midiRowLay->addWidget(dimLabel(tr("MIDI input"), midiPortRowWidget_));
    midiPort_ = new QComboBox(midiPortRowWidget_);
    midiPort_->setMinimumWidth(180);
    midiPort_->setToolTip(tr("Which MIDI device feeds this instrument."));
    midiRowLay->addWidget(midiPort_, 1);
    captureRow->addWidget(midiPortRowWidget_);
    connect(midiPort_, &QComboBox::activated, this, [this](int idx) {
        if (!client_->available()) return;
        const QString match = midiPort_->itemData(idx).toString();
        client_->setMasterMidiPort(masterId_, match);
    });
    micLay->addLayout(captureRow);

    micEffectTabs_ = createEffectCategoryTabs(micTab);
    micLay->addWidget(micEffectTabs_, 1);

    buildHardwareUi(micTab, micEffectTabs_);

    auto *procLay = makeEffectTabPage(micEffectTabs_, tr("Processing"));
    procLay->addWidget(defaultResetRow(micTab, [this] { resetMicProcessing(); }));

    procLay->addWidget(switchRow(
        tr("Noise suppression"),
        tr("RNNoise on the microphone path, before EQ. Applies to both mixes."),
        noise_, micTab));
    connect(noise_, &QAbstractButton::toggled, this, [this](bool on) {
        client_->setMasterNoiseSuppression(masterId_, on);
        applyMicDependencies();
    });

    procLay->addWidget(sliderRow(tr("Strength"), intensity_, intensityLabel_, 40, micTab));
    intensity_->setRange(0, 100);
    intensity_->setToolTip(tr("How much of the denoised signal to use."));
    connect(intensity_, &QSlider::valueChanged, this, [this](int v) {
        intensityLabel_->setText(QStringLiteral("%1%").arg(v));
        client_->setMasterNoiseIntensity(masterId_, v / 100.0);
    });

    addNcMeters(micTab, procLay, noiseIn_, noiseOut_);

    addEqSection(
        micTab, procLay, lowCut_, lowCutHz_, eq_, lowDb_, midDb_, highDb_, lowReadout_,
        midReadout_, highReadout_, micAdv_, this, &GlobalEffectsWindow::pushMicFx,
        [this] { applyMicDependencies(); },
        [this](bool on) {
            if (updating_) return;
            pushEqMode(client_, true, masterId_, QStringLiteral("input"), on);
            // Leaving Advanced leaves nothing for an open panel to edit, and a
            // window still sitting there implying otherwise is worse than one
            // that goes away with the mode that opened it.
            if (!on && micProEq_) micProEq_->close();
        },
        [this] {
            openProEqPanel(micProEq_, this, client_, true, masterId_,
                           QStringLiteral("input"), windowTitle());
        });
    procLay->addStretch();

    auto *dynLay = makeEffectTabPage(micEffectTabs_, tr("Dynamics"));
    dynLay->addWidget(defaultResetRow(micTab, [this] { resetMicDynamics(); }));
    dynLay->addWidget(dimLabel(
        tr("Gate, compressor and limiter after EQ on the microphone path."),
        micTab));

    dynLay->addWidget(switchRow(tr("Noise gate"),
                             tr("Mute the mic when input falls below a threshold."),
                             gate_, micTab));
    connect(gate_, &QAbstractButton::toggled, this, [this] {
        pushMicDynamics();
        applyMicDependencies();
    });
    dynLay->addWidget(
        sliderRow(tr("Gate threshold"), gateThreshold_, gateThresholdReadout_, 56, micTab));
    gateThreshold_->setRange(-600, -200);
    connect(gateThreshold_, &QSlider::valueChanged, this,
            &GlobalEffectsWindow::pushMicDynamics);

    dynLay->addWidget(switchRow(tr("Compressor"),
                             tr("Even out loud and quiet parts of your voice."),
                             compressor_, micTab));
    connect(compressor_, &QAbstractButton::toggled, this, [this] {
        pushMicDynamics();
        applyMicDependencies();
    });
    dynLay->addWidget(
        sliderRow(tr("Comp threshold"), compThreshold_, compThresholdReadout_, 56, micTab));
    dynLay->addWidget(sliderRow(tr("Ratio"), compRatio_, compRatioReadout_, 40, micTab));
    compThreshold_->setRange(-400, -80);
    compRatio_->setRange(10, 120);
    connect(compThreshold_, &QSlider::valueChanged, this,
            &GlobalEffectsWindow::pushMicDynamics);
    connect(compRatio_, &QSlider::valueChanged, this,
            &GlobalEffectsWindow::pushMicDynamics);
    connect(compRatio_, &QSlider::valueChanged, compRatioReadout_,
            [this](int v) { compRatioReadout_->setText(QStringLiteral("%1:1").arg(v / 10.0, 0, 'f', 1)); });
    connect(gateThreshold_, &QSlider::valueChanged, gateThresholdReadout_,
            [this](int v) { gateThresholdReadout_->setText(dbReadout(v)); });
    connect(compThreshold_, &QSlider::valueChanged, compThresholdReadout_,
            [this](int v) { compThresholdReadout_->setText(dbReadout(v)); });

    dynLay->addWidget(switchRow(tr("Auto makeup gain"),
                             tr("Compensate level after compression automatically."),
                             autoMakeup_, micTab));
    connect(autoMakeup_, &QAbstractButton::toggled, this, [this] {
        pushMicDynamics();
        applyMicDependencies();
    });

    dynLay->addWidget(switchRow(tr("Limiter"),
                             tr("Hard ceiling to prevent clipping on peaks."),
                             limiter_, micTab));
    connect(limiter_, &QAbstractButton::toggled, this, [this] {
        pushMicDynamics();
        applyMicDependencies();
    });
    dynLay->addWidget(
        sliderRow(tr("Ceiling"), limitThreshold_, limitThresholdReadout_, 56, micTab));
    limitThreshold_->setRange(-60, 0);
    connect(limitThreshold_, &QSlider::valueChanged, this,
            &GlobalEffectsWindow::pushMicDynamics);
    connect(limitThreshold_, &QSlider::valueChanged, limitThresholdReadout_,
            [this](int v) { limitThresholdReadout_->setText(dbReadout(v)); });

    addDeEsserRows(
        micTab, dynLay, deEsser_, deEsserAmount_, deEsserAmountReadout_,
        [this](bool on) {
            applyMicDependencies();
            if (updating_) return;
            client_->setMasterDeEsser(masterId_, on);
        },
        [this](double v) {
            if (updating_) return;
            client_->setMasterDeEsserIntensity(masterId_, v);
        });
    dynLay->addStretch();

    auto *micCreativeLay = makeEffectTabPage(micEffectTabs_, tr("Creative"));
    micCreativeRackNotice_ = dimLabel(
        tr("The Virtual Rack for this device is open, so it's driving these "
           "settings instead of this tab. Close the Rack (middle-click this "
           "device's heartbeat button to reopen it) to edit these directly."),
        micTab);
    micCreativeRackNotice_->setVisible(false);
    micCreativeLay->addWidget(micCreativeRackNotice_);
    micCreative_ = new CreativeFxPanel(micTab);
    // The primary mic input chain's CreativeFxFilter instance runs mono
    // (MixerGraph starts it with channels=1), so Ping-Pong has nothing to
    // bounce between here.
    micCreative_->setStereoCapable(false);
    connect(micCreative_, &CreativeFxPanel::changed, this,
           [this] { pushMicCreativeSettings(); });
    micCreativeLay->addWidget(micCreative_);
    buildSoundfontUi(micTab, micEffectTabs_);
    tabs_->addTab(micTab, tr("Microphone"));

    if (isPrimary()) {
    auto *appTab = new QWidget(this);
    auto *appLay = new QVBoxLayout(appTab);
    appLay->setContentsMargins(8, 12, 8, 8);
    appLay->setSpacing(12);
    appLay->addWidget(dimLabel(
        tr("Default app-audio processing for channels set to Input Device Effects."),
        appTab));

    auto *appEffectTabs = createEffectCategoryTabs(appTab);
    appLay->addWidget(appEffectTabs, 1);

    const auto resetOutputProcessing = [this] {
        if (!client_->available()) return;
        updating_ = true;
        applyProcessingDefaults(output_);
        updating_ = false;
        client_->setChannelNoiseIntensity(QStringLiteral("mic"),
                                          QStringLiteral("output"), 1.0);
        pushOutputSettings();
        pushEqMode(client_, false, QStringLiteral("mic"), QStringLiteral("output"), false);
        if (outputProEq_) outputProEq_->close();
    };
    const auto resetOutputDynamics = [this] {
        if (!client_->available()) return;
        updating_ = true;
        applyDynamicsDefaults(output_);
        updating_ = false;
        pushOutputSettings();
        // Its own pair of setters, so resetting the widgets above does not
        // reach the daemon by itself.
        client_->setChannelDeEsser(QStringLiteral("mic"), QStringLiteral("output"),
                                   false);
        client_->setChannelDeEsserIntensity(QStringLiteral("mic"),
                                            QStringLiteral("output"), 0.5);
    };

    auto *procLay = makeEffectTabPage(appEffectTabs, tr("Processing"));
    addProcessingSection(
        tr("RNNoise and EQ on app audio routed through channels using input device effects."),
        tr("Channels that inherit input-device app-audio effects receive this chain."),
        appTab, procLay, output_, [this] { pushOutputSettings(); }, resetOutputProcessing,
        nullptr, nullptr, [this] { applyOutputDependencies(); }, true,
        [this](bool on) {
            if (updating_) return;
            pushEqMode(client_, false, QStringLiteral("mic"), QStringLiteral("output"), on);
            if (!on && outputProEq_) outputProEq_->close();
        },
        [this] {
            openProEqPanel(outputProEq_, this, client_, false, QStringLiteral("mic"),
                           QStringLiteral("output"), tr("App audio"));
        });
    procLay->addStretch();

    auto *dynLay = makeEffectTabPage(appEffectTabs, tr("Dynamics"));
    addDynamicsSection(
        appTab, dynLay, output_, [this] { pushOutputSettings(); }, resetOutputDynamics,
        [this] { applyOutputDependencies(); }, true,
        [this](bool on) {
            client_->setChannelDeEsser(QStringLiteral("mic"), QStringLiteral("output"),
                                       on);
        },
        [this](double v) {
            client_->setChannelDeEsserIntensity(QStringLiteral("mic"),
                                                QStringLiteral("output"), v);
        });
    dynLay->addStretch();

    auto *creativeLay = makeEffectTabPage(appEffectTabs, tr("Creative"));
    outputCreative_ = new CreativeFxPanel(appTab);
    connect(outputCreative_, &CreativeFxPanel::changed, this,
           [this] { pushOutputCreativeSettings(); });
    creativeLay->addWidget(outputCreative_);

    auto *duckLay = makeEffectTabPage(appEffectTabs, tr("Ducking"));
    buildMasterDuckingUi(appTab, duckLay, true);
    duckLay->addStretch();

    auto *protLay = makeEffectTabPage(appEffectTabs, tr("Protections"));
    buildMasterLufsLimiterUi(appTab, protLay, true);
    protLay->addStretch();

    connect(output_.intensity, &QSlider::valueChanged, this, [this](int v) {
        if (updating_ || !client_->available()) return;
        output_.intensityLabel->setText(QStringLiteral("%1%").arg(v));
        client_->setChannelNoiseIntensity(QStringLiteral("mic"), QStringLiteral("output"),
                                          v / 100.0);
    });
    tabs_->addTab(appTab, tr("App Audio"));
    }

    connect(client_, &MixerClient::changed, this, &GlobalEffectsWindow::refresh);
    connect(client_, &MixerClient::levelsChanged, this,
            &GlobalEffectsWindow::refreshLevels);
    applyMicDependencies();
    refresh();
}

void GlobalEffectsWindow::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refresh();
    refreshLevels();
    // After refresh(), which is what settles which tabs exist for this device.
    // Opening always lands on Processing: these panels are kept alive between
    // openings, so without this one would come back on whatever tab it was
    // left on -- or on none, if that tab has since been hidden.
    if (tabs_ && tabs_->count() > 0) tabs_->setCurrentIndex(0);
    for (QTabWidget *inner : findChildren<QTabWidget *>()) {
        if (inner != tabs_) selectTab(inner, tr("Processing"));
    }
}

void GlobalEffectsWindow::pushMicFx() {
    if (updating_ || !client_->available()) return;
    client_->setMasterChannelEffects(
        masterId_, QStringLiteral("input"), lowCut_->isChecked(),
        lowCutHz_->currentData().toInt(), eq_->isChecked(), lowDb_->value() / 10.0,
        midDb_->value() / 10.0, highDb_->value() / 10.0);
}

void GlobalEffectsWindow::pushMicDynamics() {
    if (updating_ || !client_->available()) return;
    MicDynamicsInfo d;
    d.gate = gate_->isChecked();
    d.gateThresholdDb = gateThreshold_->value() / 10.0;
    d.gateAttackMs = 3.0;
    d.gateReleaseMs = 300.0;
    d.compressor = compressor_->isChecked();
    d.compThresholdDb = compThreshold_->value() / 10.0;
    d.compRatio = compRatio_->value() / 10.0;
    d.compAttackMs = 5.0;
    d.compReleaseMs = 150.0;
    d.compKneeDb = 6.0;
    d.makeupGainDb = 0.0;
    d.autoMakeup = autoMakeup_->isChecked();
    d.limiter = limiter_->isChecked();
    d.limitThresholdDb = limitThreshold_->value() / 10.0;
    d.limitAttackMs = 1.0;
    d.limitReleaseMs = 50.0;
    client_->setMasterMicDynamics(masterId_, d);
}

void GlobalEffectsWindow::pushMicCreativeSettings() {
    if (updating_ || !client_->available()) return;
    client_->setMasterCreativeFx(masterId_, micCreative_->values());
}

void GlobalEffectsWindow::pushOutputCreativeSettings() {
    if (updating_ || !client_->available() || !isPrimary()) return;
    client_->setChannelCreativeFx(QStringLiteral("mic"), QStringLiteral("output"),
                                  outputCreative_->values());
}

void GlobalEffectsWindow::refreshMicCreative() {
    const CreativeFxInfo info = client_->masterCreativeFx(masterId_);
    updating_ = true;
    micCreative_->setValues(info);
    updating_ = false;

    const bool rackMode = client_->masterRackMode(masterId_);
    micCreative_->setEnabled(!rackMode);
    if (micCreativeRackNotice_) micCreativeRackNotice_->setVisible(rackMode);
}

void GlobalEffectsWindow::refreshOutputCreative() {
    if (!isPrimary()) return;
    const CreativeFxInfo info =
        client_->channelCreativeFx(QStringLiteral("mic"), QStringLiteral("output"));
    updating_ = true;
    outputCreative_->setValues(info);
    updating_ = false;
}

void GlobalEffectsWindow::resetMicProcessing() {
    if (!client_->available()) return;
    const ChannelFxInfo fx;
    updating_ = true;
    QSignalBlocker b1(lowCut_);
    QSignalBlocker b2(lowCutHz_);
    QSignalBlocker b3(eq_);
    QSignalBlocker b4(lowDb_);
    QSignalBlocker b5(midDb_);
    QSignalBlocker b6(highDb_);
    Q_UNUSED(b1);
    Q_UNUSED(b2);
    Q_UNUSED(b3);
    Q_UNUSED(b4);
    Q_UNUSED(b5);
    Q_UNUSED(b6);
    lowCut_->setChecked(fx.lowCut);
    lowCutHz_->setCurrentIndex(fx.lowCutHz == 120 ? 1 : 0);
    eq_->setChecked(fx.eq);
    lowDb_->setValue(int(std::lround(fx.lowDb * 10.0)));
    midDb_->setValue(int(std::lround(fx.midDb * 10.0)));
    highDb_->setValue(int(std::lround(fx.highDb * 10.0)));
    lowReadout_->setText(dbReadout(lowDb_->value()));
    midReadout_->setText(dbReadout(midDb_->value()));
    highReadout_->setText(dbReadout(highDb_->value()));
    showEqMode(micAdv_, fx.eqAdvanced);
    updating_ = false;
    pushMicFx();
    pushEqMode(client_, true, masterId_, QStringLiteral("input"), fx.eqAdvanced);
    if (!fx.eqAdvanced && micProEq_) micProEq_->close();
}

void GlobalEffectsWindow::resetMicDynamics() {
    if (!client_->available()) return;
    updating_ = true;
    QSignalBlocker b1(gate_);
    QSignalBlocker b2(gateThreshold_);
    QSignalBlocker b3(compressor_);
    QSignalBlocker b4(compThreshold_);
    QSignalBlocker b5(compRatio_);
    QSignalBlocker b6(autoMakeup_);
    QSignalBlocker b7(limiter_);
    QSignalBlocker b8(limitThreshold_);
    Q_UNUSED(b1);
    Q_UNUSED(b2);
    Q_UNUSED(b3);
    Q_UNUSED(b4);
    Q_UNUSED(b5);
    Q_UNUSED(b6);
    Q_UNUSED(b7);
    Q_UNUSED(b8);
    const MicDynamicsInfo dyn;
    if (deEsser_) {
        QSignalBlocker bd1(deEsser_);
        QSignalBlocker bd2(deEsserAmount_);
        deEsser_->setChecked(false);
        deEsserAmount_->setValue(50);
        deEsserAmountReadout_->setText(QStringLiteral("50%"));
        client_->setMasterDeEsser(masterId_, false);
        client_->setMasterDeEsserIntensity(masterId_, 0.5);
    }
    gate_->setChecked(dyn.gate);
    gateThreshold_->setValue(int(std::lround(dyn.gateThresholdDb * 10.0)));
    gateThresholdReadout_->setText(dbReadout(gateThreshold_->value()));
    compressor_->setChecked(dyn.compressor);
    compThreshold_->setValue(int(std::lround(dyn.compThresholdDb * 10.0)));
    compThresholdReadout_->setText(dbReadout(compThreshold_->value()));
    compRatio_->setValue(int(std::lround(dyn.compRatio * 10.0)));
    compRatioReadout_->setText(QStringLiteral("%1:1").arg(dyn.compRatio, 0, 'f', 1));
    autoMakeup_->setChecked(dyn.autoMakeup);
    limiter_->setChecked(dyn.limiter);
    limitThreshold_->setValue(int(std::lround(dyn.limitThresholdDb * 10.0)));
    limitThresholdReadout_->setText(dbReadout(limitThreshold_->value()));
    updating_ = false;
    pushMicDynamics();
}

void GlobalEffectsWindow::refresh() {
    if (!client_->available() || !isVisible()) return;

    updating_ = true;
    QSignalBlocker b1(noise_);
    QSignalBlocker b2(intensity_);
    QSignalBlocker b3(micStereo_);
    QSignalBlocker b4(lowCut_);
    QSignalBlocker b5(lowCutHz_);
    QSignalBlocker b6(eq_);
    QSignalBlocker b7(lowDb_);
    QSignalBlocker b8(midDb_);
    QSignalBlocker b9(highDb_);
    QSignalBlocker b10(gate_);
    QSignalBlocker b11(gateThreshold_);
    QSignalBlocker b12(compressor_);
    QSignalBlocker b13(compThreshold_);
    QSignalBlocker b14(compRatio_);
    QSignalBlocker b15(autoMakeup_);
    QSignalBlocker b16(limiter_);
    QSignalBlocker b17(limitThreshold_);

    noise_->setChecked(client_->masterNoiseSuppression(masterId_));
    const int pct = int(client_->masterNoiseIntensity(masterId_) * 100.0 + 0.5);
    intensity_->setValue(pct);
    intensityLabel_->setText(QStringLiteral("%1%").arg(pct));
    micStereo_->setChecked(client_->masterMicStereo(masterId_));

    const ChannelFxInfo fx =
        client_->masterChannelEffects(masterId_, QStringLiteral("input"));
    lowCut_->setChecked(fx.lowCut);
    lowCutHz_->setCurrentIndex(fx.lowCutHz == 120 ? 1 : 0);
    eq_->setChecked(fx.eq);
    lowDb_->setValue(int(std::lround(fx.lowDb * 10.0)));
    midDb_->setValue(int(std::lround(fx.midDb * 10.0)));
    highDb_->setValue(int(std::lround(fx.highDb * 10.0)));
    lowReadout_->setText(dbReadout(lowDb_->value()));
    midReadout_->setText(dbReadout(midDb_->value()));
    highReadout_->setText(dbReadout(highDb_->value()));
    showEqMode(micAdv_, fx.eqAdvanced);

    const MicDynamicsInfo dyn = client_->masterMicDynamics(masterId_);
    gate_->setChecked(dyn.gate);
    gateThreshold_->setValue(int(std::lround(dyn.gateThresholdDb * 10.0)));
    gateThresholdReadout_->setText(dbReadout(gateThreshold_->value()));
    compressor_->setChecked(dyn.compressor);
    compThreshold_->setValue(int(std::lround(dyn.compThresholdDb * 10.0)));
    compThresholdReadout_->setText(dbReadout(compThreshold_->value()));
    compRatio_->setValue(int(std::lround(dyn.compRatio * 10.0)));
    compRatioReadout_->setText(QStringLiteral("%1:1").arg(dyn.compRatio, 0, 'f', 1));
    autoMakeup_->setChecked(dyn.autoMakeup);
    limiter_->setChecked(dyn.limiter);
    limitThreshold_->setValue(int(std::lround(dyn.limitThresholdDb * 10.0)));
    limitThresholdReadout_->setText(dbReadout(limitThreshold_->value()));
    deEsser_->setChecked(client_->masterDeEsser(masterId_));
    {
        const int amount =
            int(std::lround(client_->masterDeEsserIntensity(masterId_) * 100.0));
        deEsserAmount_->setValue(amount);
        deEsserAmountReadout_->setText(QStringLiteral("%1%").arg(amount));
    }
    updating_ = false;
    applyMicDependencies();
    const bool midi = isMidiMaster();
    if (captureRowWidget_) captureRowWidget_->setVisible(!midi);
    if (midiPortRowWidget_) midiPortRowWidget_->setVisible(midi);
    if (noise_) noise_->setVisible(!midi);
    if (intensity_) intensity_->setVisible(!midi);
    if (intensityLabel_) intensityLabel_->setVisible(!midi);
    if (noiseIn_) noiseIn_->setVisible(!midi);
    if (noiseOut_) noiseOut_->setVisible(!midi);
    if (micEffectTabs_) {
        for (int i = 0; i < micEffectTabs_->count(); ++i) {
            if (micEffectTabs_->tabText(i) == tr("Soundfont"))
                micEffectTabs_->setTabVisible(i, midi);
        }
    }
    refreshCaptureDevices();
    if (midi) {
        refreshMidiDevices();
        refreshSoundfonts();
    }
    refreshHardware();
    refreshMicCreative();
    if (isPrimary()) {
        refreshOutput();
        refreshOutputCreative();
        refreshDucking();
        refreshLufsLimiter();
        applyDuckingControlDependencies(ducking_, true);
        applyLufsLimiterControlDependencies(lufsLimiter_, true);
    }
    // HW and Soundfont come and go with the device above; if one of them was
    // the tab in front of you, land somewhere real rather than on a blank page.
    keepTabSelected(micEffectTabs_, tr("Processing"));
    updateWindowTitle();
}

void GlobalEffectsWindow::refreshLevels() {
    if (!isVisible() || !client_->available()) return;
    const auto &l = client_->levels();
    noiseIn_->setLevel(meterPosition(l.value(levelInKey(), 0.0)));
    noiseOut_->setLevel(meterPosition(l.value(levelOutKey(), 0.0)));
}

QString GlobalEffectsWindow::levelInKey() const {
    return masterId_ == QLatin1String("mic") ? QStringLiteral("mic-in")
                                             : masterId_ + QStringLiteral("-in");
}

QString GlobalEffectsWindow::levelOutKey() const {
    return masterId_ == QLatin1String("mic") ? QStringLiteral("mic-out")
                                             : masterId_ + QStringLiteral("-out");
}

void GlobalEffectsWindow::updateWindowTitle() {
    if (!client_) return;
    QString masterName = client_->deviceBrand();
    for (const MasterBusInfo &b : client_->masterBuses()) {
        if (b.id == masterId_) {
            masterName = b.name.isEmpty() ? client_->deviceBrand() : b.name;
            break;
        }
    }
    setWindowTitle(tr("%1 — %2 Effects")
                       .arg(client_->deviceBrand(), masterName));
}

void GlobalEffectsWindow::buildSoundfontUi(QWidget *parent, QTabWidget *effectTabs) {
    auto *sfLay = makeEffectTabPage(effectTabs, tr("Soundfont"));
    sfLay->addWidget(dimLabel(
        tr("SoundFont (.sf2) files for this MIDI instrument. Double-click to activate."),
        parent));

    soundfontList_ = new QListWidget(parent);
    soundfontList_->setMinimumHeight(120);
    sfLay->addWidget(soundfontList_, 1);
    connect(soundfontList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (!item || !client_->available()) return;
        client_->setMasterSoundfont(masterId_, item->data(Qt::UserRole).toString());
        refreshSoundfonts();
    });

    auto *btnRow = new QHBoxLayout;
    addSoundfontBtn_ = new QPushButton(tr("Add…"), parent);
    removeSoundfontBtn_ = new QPushButton(tr("Remove"), parent);
    btnRow->addWidget(addSoundfontBtn_);
    btnRow->addWidget(removeSoundfontBtn_);
    btnRow->addStretch();
    sfLay->addLayout(btnRow);

    connect(addSoundfontBtn_, &QPushButton::clicked, this, [this] {
        if (!client_->available()) return;
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Add soundfont"), {},
            tr("SoundFont files (*.sf2 *.sf);;All files (*)"));
        if (path.isEmpty()) return;
        client_->addMasterSoundfont(masterId_, path);
        refreshSoundfonts();
    });
    connect(removeSoundfontBtn_, &QPushButton::clicked, this, [this] {
        if (!client_->available() || !soundfontList_->currentItem()) return;
        client_->removeMasterSoundfont(
            masterId_, soundfontList_->currentItem()->data(Qt::UserRole).toString());
        refreshSoundfonts();
    });
    sfLay->addStretch();
}

bool GlobalEffectsWindow::isMidiMaster() const {
    if (!client_) return false;
    for (const MasterBusInfo &b : client_->masterBuses()) {
        if (b.id == masterId_) return b.busType == QLatin1String("midi");
    }
    return false;
}

// The device a bus is configured for, kept in its picker even while it is
// unplugged: greyed and marked, the way a Monitor output that has gone away is.
// Dropping it left the combo on whatever sat at index 0, so an input device set
// up with a Wave:3 read as some other microphone the moment the Wave:3 was
// unplugged -- and looked, at a glance, as though it had been reassigned.
static void keepAbsentDevice(QComboBox *combo, const QString &match,
                             const QString &label) {
    if (!combo || match.isEmpty()) return;
    for (int i = 0; i < combo->count(); ++i)
        if (combo->itemData(i).toString() == match) return;

    // A device with no brand entry falls back to its ALSA node name, which is
    // long enough to stretch a fixed-width window; the tooltip carries it whole.
    QString name = label.isEmpty() ? match : label;
    if (name.size() > 40) name = name.left(39) + QChar(0x2026);

    const QString text = name + GlobalEffectsWindow::tr(" (disconnected)");
    combo->addItem(text, match);
    const int idx = combo->count() - 1;
    combo->setItemData(idx, text + QStringLiteral("\n") + match, Qt::ToolTipRole);
    combo->setItemData(idx, QBrush(Theme::TextFaint), Qt::ForegroundRole);
}

void GlobalEffectsWindow::refreshMidiDevices() {
    if (!midiPort_ || !client_->available()) return;
    QString current;
    QString label;
    for (const MasterBusInfo &b : client_->masterBuses()) {
        if (b.id == masterId_) {
            current = b.captureMatch;
            label = b.deviceLabel;
            break;
        }
    }
    QSignalBlocker block(midiPort_);
    midiPort_->clear();
    for (const MidiDeviceInfo &d : client_->midiDevices()) {
        midiPort_->addItem(d.description, d.nodeName);
        midiPort_->setItemData(midiPort_->count() - 1, d.description, Qt::ToolTipRole);
    }
    keepAbsentDevice(midiPort_, current, label);
    for (int i = 0; i < midiPort_->count(); ++i) {
        if (midiPort_->itemData(i).toString() == current) {
            midiPort_->setCurrentIndex(i);
            break;
        }
    }
}

void GlobalEffectsWindow::refreshSoundfonts() {
    if (!soundfontList_ || !client_->available()) return;
    soundfontList_->clear();
    for (const QString &row : client_->masterSoundfonts(masterId_)) {
        const int tab = row.indexOf(QLatin1Char('\t'));
        if (tab < 0) continue;
        const QString path = row.left(tab);
        const bool active = row.mid(tab + 1).toInt() != 0;
        auto *item = new QListWidgetItem(QFileInfo(path).fileName(), soundfontList_);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        if (active) {
            item->setSelected(true);
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
    }
}

void GlobalEffectsWindow::refreshCaptureDevices() {
    if (!captureDevice_ || !client_->available()) return;
    // A MIDI bus carries its port match in the same field; it belongs in the
    // MIDI picker, not here, and this row is hidden for it anyway.
    if (isMidiMaster()) return;
    QString current;
    QString label;
    for (const MasterBusInfo &b : client_->masterBuses()) {
        if (b.id == masterId_) {
            current = b.captureMatch;
            label = b.deviceLabel;
            break;
        }
    }
    QSignalBlocker block(captureDevice_);
    captureDevice_->clear();
    for (const CaptureDeviceInfo &d : client_->captureDevices()) {
        captureDevice_->addItem(d.description, d.nodeName);
        captureDevice_->setItemData(captureDevice_->count() - 1, d.description,
                                    Qt::ToolTipRole);
    }
    const int present = captureDevice_->count();
    keepAbsentDevice(captureDevice_, current, label);
    const bool connected = captureDevice_->count() == present;
    for (int i = 0; i < captureDevice_->count(); ++i) {
        if (captureDevice_->itemData(i).toString() == current) {
            captureDevice_->setCurrentIndex(i);
            break;
        }
    }
    // The picker itself stays usable -- picking a different microphone is
    // exactly what you come here to do while one is unplugged -- but everything
    // that acts on the absent device is greyed, as the Monitor output rows do.
    captureDevice_->setToolTip(
        connected ? tr("Which microphone this input device captures from.")
                  : tr("%1 is not connected.\n"
                       "This input device stays assigned to it and picks it up "
                       "again when it comes back.\n"
                       "Pick another microphone here to reassign it.")
                        .arg(label.isEmpty() ? current : label));
    if (hardwareSection_) hardwareSection_->setEnabled(connected);
}

void GlobalEffectsWindow::buildHardwareUi(QWidget *parent, QTabWidget *effectTabs) {
    hardwareSection_ = new QWidget(parent);
    auto *hwLay = new QVBoxLayout(hardwareSection_);
    hwLay->setContentsMargins(8, 8, 8, 8);
    hwLay->setSpacing(10);

    hwLay->addWidget(sliderRow(tr("Preamp gain"), hwMicGain_, hwMicGainLabel_, 56,
                               hardwareSection_));
    hwMicGain_->setRange(0, 80);
    hwMicGain_->setToolTip(
        tr("Hardware microphone gain (0–40 dB) inside the device.\n"
           "Distinct from the strip's input level, which is a software "
           "attenuator in the mixer."));
    connect(hwMicGain_, &QSlider::sliderPressed, this, [this] { hwMicGainHeld_ = true; });
    connect(hwMicGain_, &QSlider::sliderReleased, this,
            &GlobalEffectsWindow::onHardwareMicGainReleased);
    connect(hwMicGain_, &QSlider::valueChanged, this, [this](int v) {
        hwMicGainLabel_->setText(QStringLiteral("+%1").arg(v / 2.0, 0, 'f', 1));
        client_->setMasterMicGainDb(masterId_, v / 2.0);
    });
    auto *muteRow = new QHBoxLayout;
    muteRow->addWidget(dimLabel(tr("Hardware mute"), hardwareSection_));
    muteRow->addStretch();
    hwMicMute_ = new IconToggle(QStringLiteral("microphone-off"),
                                QStringLiteral("microphone"), hardwareSection_);
    hwMicMute_->setAccent(Theme::Danger);
    hwMicMute_->setIconSize(17);
    hwMicMute_->setToolTip(tr("Mute pad on the microphone itself."));
    connect(hwMicMute_, &QAbstractButton::toggled, this, [this](bool muted) {
        client_->setMasterHardwareMicMute(masterId_, muted);
    });
    muteRow->addWidget(hwMicMute_);
    hwLay->addLayout(muteRow);

    hwLay->addWidget(switchRow(tr("Clipguard"),
                               tr("Hardware anti-clipping inside the microphone."),
                               clipguard_, hardwareSection_));
    connect(clipguard_, &QAbstractButton::toggled, this, [this](bool on) {
        client_->setMasterClipguard(masterId_, on);
    });

    hwLay->addWidget(sliderRow(tr("Direct monitor"), hwMonitor_, hwMonitorLabel_, 56,
                               hardwareSection_));
    hwMonitor_->setRange(0, 100);
    hwMonitor_->setToolTip(tr("Hardware direct monitor blend on the microphone."));
    connect(hwMonitor_, &QSlider::sliderPressed, this, [this] { hwMonitorHeld_ = true; });
    connect(hwMonitor_, &QSlider::sliderReleased, this,
            &GlobalEffectsWindow::onHardwareMonitorReleased);
    connect(hwMonitor_, &QSlider::valueChanged, this,
            &GlobalEffectsWindow::onHardwareMonitorMoved);

    auto *hpRow = sliderRow(tr("Headphone"), hpVolume_, hpVolumeLabel_, 56,
                            hardwareSection_);
    hpMute_ = new IconToggle(QStringLiteral("speaker-muted"),
                             QStringLiteral("speaker-high"), hpRow);
    hpMute_->setIconSize(16);
    hpMute_->setAccent(Theme::Danger);
    hpMute_->setToolTip(tr("Mute the headphone jack on the microphone."));
    if (auto *hl = qobject_cast<QHBoxLayout *>(hpRow->layout())) hl->addWidget(hpMute_);
    hwLay->addWidget(hpRow);
    hpVolume_->setRange(0, 120);
    connect(hpMute_, &QAbstractButton::toggled, this, [this](bool muted) {
        client_->setMasterHeadphoneMuted(masterId_, muted);
    });
    connect(hpVolume_, &QSlider::sliderPressed, this, [this] { hpVolumeHeld_ = true; });
    connect(hpVolume_, &QSlider::sliderReleased, this, [this] {
        hpVolumeHeld_ = false;
        client_->setMasterHeadphoneVolumeDb(masterId_, hpVolume_->value() / 2.0 - 60.0);
    });
    connect(hpVolume_, &QSlider::valueChanged, this, [this](int v) {
        const double db = v / 2.0 - 60.0;
        hpVolumeLabel_->setText(tr("%1 dB").arg(db, 0, 'f', 1));
        client_->setMasterHeadphoneVolumeDb(masterId_, db);
    });

    // Two rows, not one. A single row labelled "Device" whose value was
    // "firmware 0.3.7" reads as a field called "Device firmware", and never
    // said which device you were looking at -- on a machine with several
    // microphones that is the one thing the row was there to answer.
    auto *devRow = new QHBoxLayout;
    devRow->addWidget(dimLabel(tr("Device"), hardwareSection_));
    devRow->addStretch();
    deviceLabel_ = dimLabel(QString(), hardwareSection_);
    devRow->addWidget(deviceLabel_);
    hwLay->addLayout(devRow);

    // Its own widget rather than a bare layout, so the whole row can be taken
    // away on a device that has hardware controls but reports no version --
    // a caption with nothing after it says less than no caption at all.
    firmwareRow_ = new QWidget(hardwareSection_);
    auto *fwRow = new QHBoxLayout(firmwareRow_);
    fwRow->setContentsMargins(0, 0, 0, 0);
    fwRow->addWidget(dimLabel(tr("Firmware"), firmwareRow_));
    fwRow->addStretch();
    firmwareLabel_ = dimLabel(QString(), firmwareRow_);
    fwRow->addWidget(firmwareLabel_);
    hwLay->addWidget(firmwareRow_);
    hwLay->addStretch();

    const int hwIdx = effectTabs->insertTab(0, hardwareSection_, tr("HW"));
    effectTabs->setTabVisible(hwIdx, client_->hasHardwareControlsFor(masterId_));
}

void GlobalEffectsWindow::refreshHardware() {
    if (!hardwareSection_ || !micEffectTabs_ || !client_->available()) return;
    const bool hw = client_->hasHardwareControlsFor(masterId_);
    const int hwIdx = micEffectTabs_->indexOf(hardwareSection_);
    if (hwIdx >= 0) micEffectTabs_->setTabVisible(hwIdx, hw);
    if (!hw) return;

    if (!hwMicGainHeld_) {
        QSignalBlocker b(hwMicGain_);
        const double db = client_->masterMicGainDb(masterId_);
        hwMicGain_->setValue(int(db * 2.0 + 0.5));
        hwMicGainLabel_->setText(QStringLiteral("+%1").arg(db, 0, 'f', 1));
    }
    {
        QSignalBlocker b(hwMicMute_);
        hwMicMute_->setChecked(client_->masterMicMuted(masterId_));
    }
    {
        QSignalBlocker b(clipguard_);
        clipguard_->setChecked(client_->masterClipguard(masterId_));
    }
    if (!hwMonitorHeld_) {
        QSignalBlocker b(hwMonitor_);
        hwMonitor_->setValue(client_->masterHardwareMonitor(masterId_));
    }
    if (hwMonitorLabel_) {
        const int v = hwMonitor_->value();
        if (v == 0) hwMonitorLabel_->setText(tr("all PC"));
        else if (v == 100) hwMonitorLabel_->setText(tr("all mic"));
        else hwMonitorLabel_->setText(tr("%1% you / %2% PC").arg(v).arg(100 - v));
    }
    {
        QSignalBlocker b(hpMute_);
        hpMute_->setChecked(client_->masterHeadphoneMuted(masterId_));
    }
    if (!hpVolumeHeld_) {
        QSignalBlocker b(hpVolume_);
        const double db = client_->masterHeadphoneVolumeDb(masterId_);
        hpVolume_->setValue(int((db + 60.0) * 2.0 + 0.5));
        hpVolumeLabel_->setText(tr("%1 dB").arg(db, 0, 'f', 1));
    }

    const bool dev = client_->masterDeviceConnected(masterId_);
    hwMicGain_->setEnabled(dev);
    hwMicMute_->setEnabled(dev);
    clipguard_->setEnabled(dev);
    hwMonitor_->setEnabled(dev);
    hpVolume_->setEnabled(dev);
    hpMute_->setEnabled(dev);
    // The name survives an unplug and the firmware does not, so they fall back
    // differently. MasterBusInfo::deviceLabel is stored by the daemon for
    // exactly this -- see MixerService::masterDeviceLabel() -- so the device
    // still names itself while it is away, and only the version it can no
    // longer be asked for says so.
    QString name;
    for (const MasterBusInfo &b : client_->masterBuses()) {
        if (b.id != masterId_) continue;
        name = b.deviceLabel;
        break;
    }
    deviceLabel_->setText(name.isEmpty() ? tr("unknown") : name);
    // Shown only when there is a version to show. A device can have hardware
    // controls and still report no firmware string, and an empty value next to
    // a "Firmware" caption reads as a fault rather than as an absence. While
    // the device is away the row stays, because "not connected" is the answer
    // to the question rather than the absence of one.
    const QString fw = client_->masterDeviceFirmware(masterId_);
    if (firmwareRow_) firmwareRow_->setVisible(!dev || !fw.isEmpty());
    firmwareLabel_->setText(dev ? fw : tr("not connected"));
}

void GlobalEffectsWindow::onHardwareMicGainReleased() {
    hwMicGainHeld_ = false;
    client_->setMasterMicGainDb(masterId_, hwMicGain_->value() / 2.0);
}

void GlobalEffectsWindow::onHardwareMonitorMoved(int value) {
    if (hwMonitorLabel_) {
        if (value == 0) hwMonitorLabel_->setText(tr("all PC"));
        else if (value == 100) hwMonitorLabel_->setText(tr("all mic"));
        else hwMonitorLabel_->setText(tr("%1% you / %2% PC").arg(value).arg(100 - value));
    }
    client_->setMasterHardwareMonitor(masterId_, value);
}

void GlobalEffectsWindow::onHardwareMonitorReleased() {
    hwMonitorHeld_ = false;
    client_->setMasterHardwareMonitor(masterId_, hwMonitor_->value());
}

MicDynamicsInfo GlobalEffectsWindow::dynamicsFromStage(const FxStageControls &ui) {
    return dynamicsFromStageControls(ui);
}

void GlobalEffectsWindow::applyMicDependencies() {
    FxStageControls ui;
    ui.noise = noise_;
    ui.intensity = intensity_;
    ui.intensityLabel = intensityLabel_;
    ui.lowCut = lowCut_;
    ui.lowCutHz = lowCutHz_;
    ui.eq = eq_;
    ui.lowDb = lowDb_;
    ui.midDb = midDb_;
    ui.highDb = highDb_;
    ui.lowReadout = lowReadout_;
    ui.midReadout = midReadout_;
    ui.highReadout = highReadout_;
    ui.gate = gate_;
    ui.gateThreshold = gateThreshold_;
    ui.gateThresholdReadout = gateThresholdReadout_;
    ui.compressor = compressor_;
    ui.compThreshold = compThreshold_;
    ui.compThresholdReadout = compThresholdReadout_;
    ui.compRatio = compRatio_;
    ui.compRatioReadout = compRatioReadout_;
    ui.autoMakeup = autoMakeup_;
    ui.limiter = limiter_;
    ui.limitThreshold = limitThreshold_;
    ui.limitThresholdReadout = limitThresholdReadout_;
    ui.deEsser = deEsser_;
    ui.deEsserAmount = deEsserAmount_;
    ui.deEsserAmountReadout = deEsserAmountReadout_;
    setStageControlsEnabled(ui, true);
    applyStageControlDependencies(ui, true, noiseIn_, noiseOut_);
}

void GlobalEffectsWindow::applyOutputDependencies() {
    setStageControlsEnabled(output_, true);
    applyStageControlDependencies(output_, true);
}

void GlobalEffectsWindow::pushOutputSettings() {
    if (updating_ || !client_->available()) return;
    client_->setChannelEffects(
        QStringLiteral("mic"), QStringLiteral("output"), output_.lowCut->isChecked(),
        output_.lowCutHz->currentData().toInt(), output_.eq->isChecked(),
        output_.lowDb->value() / 10.0, output_.midDb->value() / 10.0,
        output_.highDb->value() / 10.0);
    client_->setChannelNoiseSuppression(QStringLiteral("mic"), QStringLiteral("output"),
                                        output_.noise->isChecked());
    client_->setChannelDynamics(QStringLiteral("mic"), QStringLiteral("output"),
                                dynamicsFromStage(output_));
}

void GlobalEffectsWindow::pushDuckingSettings() {
    if (updating_ || !client_->available()) return;
    client_->setChannelDucking(QStringLiteral("mic"), ducking_.enabled->isChecked(),
                               ducking_.intensity->value() / 100.0,
                               encodeMasterDuckingSourcesFromUi(),
                               ducking_.hold->value() / 10.0);
}

void GlobalEffectsWindow::pushLufsLimiterSettings() {
    if (updating_ || !client_->available()) return;
    client_->setChannelLufsLimiter(QStringLiteral("mic"), lufsLimiter_.enabled->isChecked(),
                                   lufsLimiter_.maxLufs->value() / 10.0);
}

void GlobalEffectsWindow::buildMasterLufsLimiterUi(QWidget *parent, QVBoxLayout *lay,
                                                   bool inTab) {
    buildLufsLimiterSection(
        lufsLimiter_, parent, lay,
        [this] {
            if (!client_->available()) return;
            updating_ = true;
            QSignalBlocker b1(lufsLimiter_.enabled);
            QSignalBlocker b2(lufsLimiter_.maxLufs);
            lufsLimiter_.enabled->setChecked(false);
            lufsLimiter_.maxLufs->setValue(-180);
            lufsLimiter_.maxLufsReadout->setText(lufsReadout(-180));
            styleEarProtectionSlider(lufsLimiter_.maxLufs, -180);
            updating_ = false;
            pushLufsLimiterSettings();
            applyLufsLimiterControlDependencies(lufsLimiter_, true);
        },
        [this] {
            pushLufsLimiterSettings();
            applyLufsLimiterControlDependencies(lufsLimiter_, true);
        },
        inTab);
}

void GlobalEffectsWindow::refreshLufsLimiter() {
    const LufsLimiterInfo info = client_->channelLufsLimiter(QStringLiteral("mic"));
    updating_ = true;
    QSignalBlocker b1(lufsLimiter_.enabled);
    QSignalBlocker b2(lufsLimiter_.maxLufs);
    lufsLimiter_.enabled->setChecked(info.enabled);
    const int tenths = int(std::lround(info.maxLufs * 10.0));
    lufsLimiter_.maxLufs->setValue(tenths);
    lufsLimiter_.maxLufsReadout->setText(lufsReadout(tenths));
    styleEarProtectionSlider(lufsLimiter_.maxLufs, tenths);
    updating_ = false;
    applyLufsLimiterControlDependencies(lufsLimiter_, true);
}

void GlobalEffectsWindow::buildMasterDuckingUi(QWidget *parent, QVBoxLayout *lay,
                                               bool inTab) {
    if (inTab) {
        lay->addWidget(defaultResetRow(parent, [this] {
            if (!client_->available()) return;
            updating_ = true;
            ducking_.enabled->setChecked(false);
            ducking_.intensity->setValue(75);
            ducking_.intensityLabel->setText(QStringLiteral("75%"));
            ducking_.hold->setValue(30);
            ducking_.holdLabel->setText(duckHoldReadout(30));
            setMasterDuckingSourcesFromConfig(
                {DuckingSourceInfo{QStringLiteral("master_mic"), QStringLiteral("mic")}});
            updating_ = false;
            pushDuckingSettings();
            applyDuckingControlDependencies(ducking_, true);
        }));
    } else {
        lay->addWidget(sectionHeaderRow(tr("Ducking"), parent, [this] {
            if (!client_->available()) return;
            updating_ = true;
            ducking_.enabled->setChecked(false);
            ducking_.intensity->setValue(75);
            ducking_.intensityLabel->setText(QStringLiteral("75%"));
            ducking_.hold->setValue(30);
            ducking_.holdLabel->setText(duckHoldReadout(30));
            setMasterDuckingSourcesFromConfig(
                {DuckingSourceInfo{QStringLiteral("master_mic"), QStringLiteral("mic")}});
            updating_ = false;
            pushDuckingSettings();
            applyDuckingControlDependencies(ducking_, true);
        }));
    }

    lay->addWidget(switchRow(
        tr("Enable ducking"),
        tr("Smoothly lower app audio when any sidechain source is active."),
        ducking_.enabled, parent));
    connect(ducking_.enabled, &QAbstractButton::toggled, this,
            [this] {
                pushDuckingSettings();
                applyDuckingControlDependencies(ducking_, true);
            });

    auto *srcHint = dimLabel(
        tr("Sidechain sources (up to 6). Ducks when any source is active."), parent);
    srcHint->setWordWrap(true);
    lay->addWidget(srcHint);

    ducking_.sourcesHost = new QWidget(parent);
    ducking_.sourcesLayout = new QVBoxLayout(ducking_.sourcesHost);
    ducking_.sourcesLayout->setContentsMargins(0, 0, 0, 0);
    ducking_.sourcesLayout->setSpacing(6);
    lay->addWidget(ducking_.sourcesHost);

    ducking_.addSource = new QPushButton(tr("Add source"), parent);
    ducking_.addSource->setMaximumWidth(120);
    connect(ducking_.addSource, &QPushButton::clicked, this, [this] {
        addMasterDuckingSourceRow();
        pushDuckingSettings();
    });
    lay->addWidget(ducking_.addSource);

    lay->addWidget(sliderRow(tr("Intensity"), ducking_.intensity, ducking_.intensityLabel,
                             40, parent));
    ducking_.intensity->setRange(0, 100);
    connect(ducking_.intensity, &QSlider::valueChanged, this, [this](int v) {
        ducking_.intensityLabel->setText(QStringLiteral("%1%").arg(v));
        pushDuckingSettings();
    });

    lay->addWidget(
        sliderRow(tr("Hold"), ducking_.hold, ducking_.holdLabel, 44, parent));
    ducking_.hold->setRange(0, 100);
    ducking_.hold->setToolTip(
        tr("How long the sidechain has to stay quiet before app audio comes "
           "back up. Pauses between words will not lift it."));
    connect(ducking_.hold, &QSlider::valueChanged, this, [this](int v) {
        ducking_.holdLabel->setText(duckHoldReadout(v));
        pushDuckingSettings();
    });

    addMasterDuckingSourceRow();
}

void GlobalEffectsWindow::clearMasterDuckingSourceRows() {
    for (const DuckingSourceRow &row : ducking_.sourceRows) {
        ducking_.sourcesLayout->removeWidget(row.row);
        row.row->deleteLater();
    }
    ducking_.sourceRows.clear();
    updateMasterDuckingAddButton();
}

void GlobalEffectsWindow::addMasterDuckingSourceRow(const DuckingSourceInfo &src) {
    if (ducking_.sourceRows.size() >= 6) return;

    DuckingSourceRow row;
    row.row = new QWidget(ducking_.sourcesHost);
    auto *rowLay = new QHBoxLayout(row.row);
    rowLay->setContentsMargins(0, 0, 0, 0);
    rowLay->setSpacing(6);

    row.kind = new QComboBox(row.row);
    row.kind->addItem(tr("Input device"), QStringLiteral("master_mic"));
    row.kind->addItem(tr("Channel microphone"), QStringLiteral("channel_mic"));
    row.kind->addItem(tr("Channel audio"), QStringLiteral("channel_audio"));
    rowLay->addWidget(row.kind, 1);

    row.channel = new QComboBox(row.row);
    row.channel->setMinimumWidth(100);
    rowLay->addWidget(row.channel, 1);

    row.remove = new QPushButton(tr("Remove"), row.row);
    row.remove->setMaximumWidth(72);
    rowLay->addWidget(row.remove);

    ducking_.sourceRows.append(row);
    ducking_.sourcesLayout->addWidget(row.row);

    const QString kind = src.kind.isEmpty() ? QStringLiteral("master_mic") : src.kind;
    const int kindIdx = row.kind->findData(kind);
    row.kind->setCurrentIndex(kindIdx >= 0 ? kindIdx : 0);
    populateMasterDuckingChannelCombo(row);
    if (!src.channelId.isEmpty()) {
        const int chIdx = row.channel->findData(src.channelId);
        if (chIdx >= 0) row.channel->setCurrentIndex(chIdx);
    }

    connect(row.kind, &QComboBox::currentIndexChanged, this,
            [this, rowWidget = row.row] {
                for (DuckingSourceRow &r : ducking_.sourceRows) {
                    if (r.row != rowWidget) continue;
                    populateMasterDuckingChannelCombo(r);
                    break;
                }
                pushDuckingSettings();
            });
    connect(row.channel, &QComboBox::currentIndexChanged, this,
            [this] { pushDuckingSettings(); });
    connect(row.remove, &QPushButton::clicked, this, [this, rowWidget = row.row] {
        for (int i = 0; i < ducking_.sourceRows.size(); ++i) {
            if (ducking_.sourceRows[i].row != rowWidget) continue;
            removeMasterDuckingSourceRow(i);
            break;
        }
        pushDuckingSettings();
    });

    updateMasterDuckingAddButton();
}

void GlobalEffectsWindow::removeMasterDuckingSourceRow(int index) {
    if (index < 0 || index >= ducking_.sourceRows.size()) return;
    DuckingSourceRow row = ducking_.sourceRows.takeAt(index);
    ducking_.sourcesLayout->removeWidget(row.row);
    row.row->deleteLater();
    updateMasterDuckingAddButton();
}

void GlobalEffectsWindow::populateMasterDuckingChannelCombo(DuckingSourceRow &row) {
    if (!row.channel || !client_->available()) return;
    const QString kind = row.kind->currentData().toString();
    row.channel->setVisible(true);
    const QString prev = row.channel->currentData().toString();
    populateDuckingTargetCombo(row.channel, client_, kind, prev);
}

void GlobalEffectsWindow::updateMasterDuckingAddButton() {
    if (ducking_.addSource)
        ducking_.addSource->setEnabled(ducking_.sourceRows.size() < 6);
}

QString GlobalEffectsWindow::encodeMasterDuckingSourcesFromUi() const {
    QStringList parts;
    for (const DuckingSourceRow &row : ducking_.sourceRows) {
        const QString kind = row.kind->currentData().toString();
        if (kind == QLatin1String("master_mic")) {
            QString masterId = row.channel->currentData().toString();
            if (masterId.isEmpty()) masterId = QStringLiteral("mic");
            parts << QStringLiteral("master_mic:") + masterId;
        } else {
            const QString chId = row.channel->currentData().toString();
            if (!chId.isEmpty()) parts << kind + QLatin1Char(':') + chId;
        }
    }
    if (parts.isEmpty()) parts << QStringLiteral("master_mic");
    return parts.join(QLatin1Char('|'));
}

void GlobalEffectsWindow::setMasterDuckingSourcesFromConfig(
    const QList<DuckingSourceInfo> &sources) {
    clearMasterDuckingSourceRows();
    if (sources.isEmpty()) {
        addMasterDuckingSourceRow();
        return;
    }
    for (const DuckingSourceInfo &src : sources) addMasterDuckingSourceRow(src);
}

void GlobalEffectsWindow::refreshOutput() {
    const QString stage = QStringLiteral("output");
    const ChannelFxInfo fx = client_->channelEffects(QStringLiteral("mic"), stage);
    const MicDynamicsInfo dyn = client_->channelDynamics(QStringLiteral("mic"), stage);

    QSignalBlocker b1(output_.noise);
    QSignalBlocker b2(output_.intensity);
    QSignalBlocker b3(output_.lowCut);
    QSignalBlocker b4(output_.lowCutHz);
    QSignalBlocker b5(output_.eq);
    QSignalBlocker b6(output_.lowDb);
    QSignalBlocker b7(output_.midDb);
    QSignalBlocker b8(output_.highDb);
    QSignalBlocker b9(output_.gate);
    QSignalBlocker b10(output_.gateThreshold);
    QSignalBlocker b11(output_.compressor);
    QSignalBlocker b12(output_.compThreshold);
    QSignalBlocker b13(output_.compRatio);
    QSignalBlocker b14(output_.autoMakeup);
    QSignalBlocker b15(output_.limiter);
    QSignalBlocker b16(output_.limitThreshold);

    output_.noise->setChecked(client_->channelNoiseSuppression(QStringLiteral("mic"), stage));
    const int pct =
        int(client_->channelNoiseIntensity(QStringLiteral("mic"), stage) * 100.0 + 0.5);
    output_.intensity->setValue(pct);
    output_.intensityLabel->setText(QStringLiteral("%1%").arg(pct));
    output_.lowCut->setChecked(fx.lowCut);
    output_.lowCutHz->setCurrentIndex(fx.lowCutHz == 120 ? 1 : 0);
    output_.eq->setChecked(fx.eq);
    output_.lowDb->setValue(int(std::lround(fx.lowDb * 10.0)));
    output_.midDb->setValue(int(std::lround(fx.midDb * 10.0)));
    output_.highDb->setValue(int(std::lround(fx.highDb * 10.0)));
    output_.lowReadout->setText(dbReadout(output_.lowDb->value()));
    output_.midReadout->setText(dbReadout(output_.midDb->value()));
    output_.highReadout->setText(dbReadout(output_.highDb->value()));
    output_.gate->setChecked(dyn.gate);
    output_.gateThreshold->setValue(int(std::lround(dyn.gateThresholdDb * 10.0)));
    output_.gateThresholdReadout->setText(dbReadout(output_.gateThreshold->value()));
    output_.compressor->setChecked(dyn.compressor);
    output_.compThreshold->setValue(int(std::lround(dyn.compThresholdDb * 10.0)));
    output_.compThresholdReadout->setText(dbReadout(output_.compThreshold->value()));
    output_.compRatio->setValue(int(std::lround(dyn.compRatio * 10.0)));
    output_.compRatioReadout->setText(
        QStringLiteral("%1:1").arg(dyn.compRatio, 0, 'f', 1));
    output_.autoMakeup->setChecked(dyn.autoMakeup);
    output_.limiter->setChecked(dyn.limiter);
    output_.limitThreshold->setValue(int(std::lround(dyn.limitThresholdDb * 10.0)));
    output_.limitThresholdReadout->setText(dbReadout(output_.limitThreshold->value()));
    showDeEsser(output_,
                client_->channelDeEsser(QStringLiteral("mic"),
                                        QStringLiteral("output")),
                client_->channelDeEsserIntensity(QStringLiteral("mic"),
                                                 QStringLiteral("output")));
}

void GlobalEffectsWindow::refreshDucking() {
    const DuckingInfo d = client_->channelDucking(QStringLiteral("mic"));
    QSignalBlocker b1(ducking_.enabled);
    QSignalBlocker b3(ducking_.intensity);
    QSignalBlocker b4(ducking_.hold);
    ducking_.enabled->setChecked(d.enabled);
    const int pct = int(d.intensity * 100.0 + 0.5);
    ducking_.intensity->setValue(pct);
    ducking_.intensityLabel->setText(QStringLiteral("%1%").arg(pct));
    const int holdTenths = int(std::lround(d.holdSec * 10.0));
    ducking_.hold->setValue(holdTenths);
    ducking_.holdLabel->setText(duckHoldReadout(holdTenths));
    setMasterDuckingSourcesFromConfig(d.sources);
    applyDuckingControlDependencies(ducking_, true);
}

ChannelEffectsWindow::ChannelEffectsWindow(const QString &channelId,
                                           const QString &title,
                                           MixerClient *client, QWidget *parent)
    : QWidget(parent, Qt::Window), channelId_(channelId), client_(client) {
    setWindowTitle(tr("%1 Effects").arg(title));
    setFixedWidth(kEffectsWindowWidth);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 16, 16, 16);
    lay->setSpacing(12);

    auto *micRow = new QHBoxLayout;
    auto *micLabel = new QLabel(tr("Publish as a recording device"), this);
    micSource_ = new ToggleSwitch(this);
    micSource_->setToolTip(
        tr("Creates \"%1 %2 Microphone\" for applications to record from,\n"
           "processed by the Microphone tab below. Your input devices are untouched.")
            .arg(client_->deviceBrand(), title));
    micRow->addWidget(micLabel);
    micRow->addStretch();
    micRow->addWidget(micSource_);
    lay->addLayout(micRow);

    tabs_ = new QTabWidget(this);
    styleEffectsTabs(tabs_);
    lay->addWidget(tabs_, 1);

    auto *micTab = new QWidget(this);
    auto *micLay = new QVBoxLayout(micTab);
    micLay->setContentsMargins(8, 12, 8, 8);
    micLay->setSpacing(12);

    auto *masterMicHost = new QWidget(micTab);
    masterMicRows_ = new QVBoxLayout(masterMicHost);
    masterMicRows_->setContentsMargins(0, 0, 0, 0);
    masterMicRows_->setSpacing(6);
    micLay->addWidget(masterMicHost);
    rebuildMasterMicRows({QStringLiteral("mic")});

    micLay->addWidget(effectSourceRow(inputEffectSource_, this,
                                      [this] { updateEffectSourceUi(); }, micTab));
    if (inputEffectSource_) {
        inputEffectSource_->setToolTip(
            tr("Unique: this channel's own microphone effects, below.\n"
               "Use device(s) effects: publish each input device exactly as its "
               "own strip\nprocesses it -- how two microphones get different "
               "effects on one\nrecording device. The tabs below do not apply.\n"
               "An input device: copy that device's effect settings into this "
               "channel's chain."));
    }

    auto *micEffectTabs = createEffectCategoryTabs(micTab);
    micLay->addWidget(micEffectTabs, 1);

    const auto resetInputProcessing = [this] {
        if (!client_->available()) return;
        updating_ = true;
        applyProcessingDefaults(input_);
        updating_ = false;
        client_->setChannelNoiseIntensity(channelId_, QStringLiteral("input"), 1.0);
        pushInputSettings();
        pushEqMode(client_, false, channelId_, QStringLiteral("input"), false);
        if (inputProEq_) inputProEq_->close();
    };
    const auto resetInputDynamics = [this] {
        if (!client_->available()) return;
        updating_ = true;
        applyDynamicsDefaults(input_);
        updating_ = false;
        pushInputSettings();
        client_->setChannelDeEsser(channelId_, QStringLiteral("input"), false);
        client_->setChannelDeEsserIntensity(channelId_, QStringLiteral("input"), 0.5);
    };

    auto *micProcLay = makeEffectTabPage(micEffectTabs, tr("Processing"));
    addProcessingSection(
        tr("RNNoise and EQ on this channel's own microphone source."),
        tr("Applies only to the recording device, not to app audio."),
        micTab, micProcLay, input_, [this] { pushInputSettings(); }, resetInputProcessing,
        &inputNoiseIn_, &inputNoiseOut_, [this] { applyInputDependencies(); }, true,
        [this](bool on) {
            if (updating_) return;
            pushEqMode(client_, false, channelId_, QStringLiteral("input"), on);
            if (!on && inputProEq_) inputProEq_->close();
        },
        [this] {
            openProEqPanel(inputProEq_, this, client_, false, channelId_,
                           QStringLiteral("input"), tr("%1 microphone").arg(windowTitle()));
        });
    micProcLay->addStretch();

    auto *micDynLay = makeEffectTabPage(micEffectTabs, tr("Dynamics"));
    addDynamicsSection(
        micTab, micDynLay, input_, [this] { pushInputSettings(); }, resetInputDynamics,
        [this] { applyInputDependencies(); }, true,
        [this](bool on) {
            client_->setChannelDeEsser(channelId_, QStringLiteral("input"), on);
        },
        [this](double v) {
            client_->setChannelDeEsserIntensity(channelId_, QStringLiteral("input"), v);
        });
    micDynLay->addStretch();

    auto *micCreativeLay = makeEffectTabPage(micEffectTabs, tr("Creative"));
    inputCreative_ = new CreativeFxPanel(micTab);
    connect(inputCreative_, &CreativeFxPanel::changed, this,
           [this] { pushInputCreativeSettings(); });
    micCreativeLay->addWidget(inputCreative_);

    tabs_->addTab(micTab, tr("Microphone"));

    auto *appTab = new QWidget(this);
    auto *appLay = new QVBoxLayout(appTab);
    appLay->setContentsMargins(8, 12, 8, 8);
    appLay->setSpacing(12);
    appLay->addWidget(effectSourceRow(outputEffectSource_, this,
                                      [this] { updateEffectSourceUi(); }, appTab));

    auto *appEffectTabs = createEffectCategoryTabs(appTab);
    appLay->addWidget(appEffectTabs, 1);

    const auto resetOutputProcessing = [this] {
        if (!client_->available()) return;
        updating_ = true;
        applyProcessingDefaults(output_);
        updating_ = false;
        client_->setChannelNoiseIntensity(channelId_, QStringLiteral("output"), 1.0);
        pushOutputSettings();
        pushEqMode(client_, false, channelId_, QStringLiteral("output"), false);
        if (outputProEq_) outputProEq_->close();
    };
    const auto resetOutputDynamics = [this] {
        if (!client_->available()) return;
        updating_ = true;
        applyDynamicsDefaults(output_);
        updating_ = false;
        pushOutputSettings();
        client_->setChannelDeEsser(channelId_, QStringLiteral("output"), false);
        client_->setChannelDeEsserIntensity(channelId_, QStringLiteral("output"), 0.5);
    };

    auto *appProcLay = makeEffectTabPage(appEffectTabs, tr("Processing"));
    addProcessingSection(
        tr("RNNoise and EQ on what this channel sends to the Stream and Monitor mixes."),
        tr("Last processing step before this channel reaches the mixes."),
        appTab, appProcLay, output_, [this] { pushOutputSettings(); }, resetOutputProcessing,
        nullptr, nullptr, [this] { applyOutputDependencies(); }, true,
        [this](bool on) {
            if (updating_) return;
            pushEqMode(client_, false, channelId_, QStringLiteral("output"), on);
            if (!on && outputProEq_) outputProEq_->close();
        },
        [this] {
            openProEqPanel(outputProEq_, this, client_, false, channelId_,
                           QStringLiteral("output"), windowTitle());
        });
    appProcLay->addStretch();

    auto *appDynLay = makeEffectTabPage(appEffectTabs, tr("Dynamics"));
    addDynamicsSection(
        appTab, appDynLay, output_, [this] { pushOutputSettings(); }, resetOutputDynamics,
        [this] { applyOutputDependencies(); }, true,
        [this](bool on) {
            client_->setChannelDeEsser(channelId_, QStringLiteral("output"), on);
        },
        [this](double v) {
            client_->setChannelDeEsserIntensity(channelId_, QStringLiteral("output"), v);
        });
    appDynLay->addStretch();

    auto *appCreativeLay = makeEffectTabPage(appEffectTabs, tr("Creative"));
    outputCreative_ = new CreativeFxPanel(appTab);
    connect(outputCreative_, &CreativeFxPanel::changed, this,
           [this] { pushOutputCreativeSettings(); });
    appCreativeLay->addWidget(outputCreative_);

    auto *duckLay = makeEffectTabPage(appEffectTabs, tr("Ducking"));
    buildDuckingUi(appTab, duckLay, true);
    duckLay->addStretch();

    auto *protLay = makeEffectTabPage(appEffectTabs, tr("Protections"));
    buildLufsLimiterUi(appTab, protLay, true);
    protLay->addStretch();

    tabs_->addTab(appTab, tr("App Audio"));

    connect(micSource_, &QAbstractButton::toggled, this, [this](bool on) {
        if (updating_ || !client_->available()) return;
        client_->setChannelMicSource(channelId_, on);
        setMicTabEnabled(on);
        if (on) tabs_->setCurrentIndex(0);
    });

    connect(input_.intensity, &QSlider::valueChanged, this, [this](int v) {
        if (updating_ || !client_->available()) return;
        input_.intensityLabel->setText(QStringLiteral("%1%").arg(v));
        client_->setChannelNoiseIntensity(channelId_, QStringLiteral("input"),
                                          v / 100.0);
    });
    connect(output_.intensity, &QSlider::valueChanged, this, [this](int v) {
        if (updating_ || !client_->available()) return;
        output_.intensityLabel->setText(QStringLiteral("%1%").arg(v));
        client_->setChannelNoiseIntensity(channelId_, QStringLiteral("output"),
                                          v / 100.0);
    });

    connect(client_, &MixerClient::changed, this, &ChannelEffectsWindow::refresh);
    connect(client_, &MixerClient::levelsChanged, this,
            &ChannelEffectsWindow::refreshInputLevels);
    setMicTabEnabled(false);
    applyInputDependencies();
    applyOutputDependencies();
    applyDuckingDependencies();
    applyLufsLimiterDependencies();
    refresh();
}

void ChannelEffectsWindow::refreshInputLevels() {
    if (!isVisible() || !client_->available() || !inputNoiseIn_ || !inputNoiseOut_) return;
    const auto &l = client_->levels();
    // Which chain these two meters are actually straddling.
    //
    // The pair exists to show noise suppression working: the difference
    // between In and Out is the whole point of drawing them. So they have to
    // follow the filters that are in the path, and in two of the three modes
    // those are not this channel's own.
    //
    // An effect-source device was already handled. Device FX was not, and read
    // this channel's probes regardless -- but in that mode the channel's mic
    // filters are bypassed entirely, so both probes sit on the same untouched
    // signal and report the same number. That is the bug this fixes: two
    // meters at an identical third of full scale, under controls greyed out
    // for doing nothing, implying a before/after that was always a no-op.
    QString source = client_->channelEffectSourceMasterId(channelId_,
                                                          QStringLiteral("input"));
    if (source.isEmpty() && client_->channelMicUseDeviceFx(channelId_))
        source = client_->channelMasterMic(channelId_);

    const QString base = source.isEmpty() ? channelId_ : source;
    inputNoiseIn_->setLevel(
        meterPosition(l.value(base + QStringLiteral("-in"), 0.0)));
    inputNoiseOut_->setLevel(
        meterPosition(l.value(base + QStringLiteral("-out"), 0.0)));
}

void ChannelEffectsWindow::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refresh();
    refreshInputLevels();
    // Tab 0 is the channel's own microphone, disabled while the channel
    // publishes none; setMicTabEnabled() has already moved off it in that case.
    if (tabs_ && tabs_->count() > 0 && tabs_->isTabEnabled(0))
        tabs_->setCurrentIndex(0);
    for (QTabWidget *inner : findChildren<QTabWidget *>()) {
        if (inner != tabs_) selectTab(inner, tr("Processing"));
    }
}

void ChannelEffectsWindow::pushInputSettings() {
    if (updating_ || !client_->available()) return;
    if (client_->channelEffectSourceMaster(channelId_, QStringLiteral("input"))) return;
    client_->setChannelEffects(
        channelId_, QStringLiteral("input"), input_.lowCut->isChecked(),
        input_.lowCutHz->currentData().toInt(), input_.eq->isChecked(),
        input_.lowDb->value() / 10.0, input_.midDb->value() / 10.0,
        input_.highDb->value() / 10.0);
    client_->setChannelNoiseSuppression(channelId_, QStringLiteral("input"),
                                        input_.noise->isChecked());
    client_->setChannelDynamics(channelId_, QStringLiteral("input"),
                                dynamicsFromStage(input_));
}

void ChannelEffectsWindow::pushOutputSettings() {
    if (updating_ || !client_->available()) return;
    if (client_->channelEffectSourceMaster(channelId_, QStringLiteral("output"))) return;
    client_->setChannelEffects(
        channelId_, QStringLiteral("output"), output_.lowCut->isChecked(),
        output_.lowCutHz->currentData().toInt(), output_.eq->isChecked(),
        output_.lowDb->value() / 10.0, output_.midDb->value() / 10.0,
        output_.highDb->value() / 10.0);
    client_->setChannelNoiseSuppression(channelId_, QStringLiteral("output"),
                                        output_.noise->isChecked());
    client_->setChannelDynamics(channelId_, QStringLiteral("output"),
                                dynamicsFromStage(output_));
}

void ChannelEffectsWindow::pushDuckingSettings() {
    if (updating_ || !client_->available()) return;
    if (client_->channelEffectSourceMaster(channelId_, QStringLiteral("output"))) return;
    client_->setChannelDucking(channelId_, ducking_.enabled->isChecked(),
                               ducking_.intensity->value() / 100.0,
                               encodeDuckingSourcesFromUi(),
                               ducking_.hold->value() / 10.0);
}

void ChannelEffectsWindow::pushLufsLimiterSettings() {
    if (updating_ || !client_->available()) return;
    if (client_->channelEffectSourceMaster(channelId_, QStringLiteral("output"))) return;
    client_->setChannelLufsLimiter(channelId_, lufsLimiter_.enabled->isChecked(),
                                   lufsLimiter_.maxLufs->value() / 10.0);
}

void ChannelEffectsWindow::buildLufsLimiterUi(QWidget *parent, QVBoxLayout *lay,
                                              bool inTab) {
    buildLufsLimiterSection(
        lufsLimiter_, parent, lay,
        [this] {
            if (!client_->available()) return;
            updating_ = true;
            QSignalBlocker b1(lufsLimiter_.enabled);
            QSignalBlocker b2(lufsLimiter_.maxLufs);
            lufsLimiter_.enabled->setChecked(false);
            lufsLimiter_.maxLufs->setValue(-180);
            lufsLimiter_.maxLufsReadout->setText(lufsReadout(-180));
            styleEarProtectionSlider(lufsLimiter_.maxLufs, -180);
            updating_ = false;
            pushLufsLimiterSettings();
            applyLufsLimiterDependencies();
        },
        [this] {
            pushLufsLimiterSettings();
            applyLufsLimiterDependencies();
        },
        inTab);
}

void ChannelEffectsWindow::refreshLufsLimiter() {
    const LufsLimiterInfo info = client_->channelLufsLimiter(channelId_);
    updating_ = true;
    QSignalBlocker b1(lufsLimiter_.enabled);
    QSignalBlocker b2(lufsLimiter_.maxLufs);
    lufsLimiter_.enabled->setChecked(info.enabled);
    const int tenths = int(std::lround(info.maxLufs * 10.0));
    lufsLimiter_.maxLufs->setValue(tenths);
    lufsLimiter_.maxLufsReadout->setText(lufsReadout(tenths));
    styleEarProtectionSlider(lufsLimiter_.maxLufs, tenths);
    updating_ = false;
    applyLufsLimiterDependencies();
}

void ChannelEffectsWindow::refreshDucking() {
    const DuckingInfo d = client_->channelDucking(channelId_);
    QSignalBlocker b1(ducking_.enabled);
    QSignalBlocker b3(ducking_.intensity);
    QSignalBlocker b4(ducking_.hold);
    ducking_.enabled->setChecked(d.enabled);
    const int pct = int(d.intensity * 100.0 + 0.5);
    ducking_.intensity->setValue(pct);
    ducking_.intensityLabel->setText(QStringLiteral("%1%").arg(pct));
    const int holdTenths = int(std::lround(d.holdSec * 10.0));
    ducking_.hold->setValue(holdTenths);
    ducking_.holdLabel->setText(duckHoldReadout(holdTenths));
    setDuckingSourcesFromConfig(d.sources);
    applyDuckingDependencies();
}

MicDynamicsInfo ChannelEffectsWindow::dynamicsFromStage(const StageControls &ui) {
    return dynamicsFromStageControls(ui);
}

void ChannelEffectsWindow::applyInputDependencies() {
    if (client_->channelEffectSourceMaster(channelId_, QStringLiteral("input"))) return;
    // Device-fx mode keeps this channel's mic filters out of the path, and
    // updateEffectSourceUi has already greyed them -- re-enabling them here
    // would offer controls that adjust nothing.
    if (inputEffectSource_ && inputEffectSource_->currentData().toString() ==
                                  QLatin1String(kDeviceFxSourceId))
        return;
    setStageControlsEnabled(input_, true);
    applyStageControlDependencies(input_, true, inputNoiseIn_, inputNoiseOut_);
    applyMidiNoiseRule();
}

bool ChannelEffectsWindow::micSourcesIncludeMidi() const {
    const QStringList ids = selectedMasterMics();
    if (ids.isEmpty()) return false;
    for (const MasterBusInfo &b : client_->masterBuses()) {
        if (b.busType == QLatin1String("midi") && ids.contains(b.id)) return true;
    }
    return false;
}

// A MIDI instrument is not speech. The engine takes the mic chain around the
// noise filter when one feeds this channel (wireChannelMicSource), so the
// control here has nothing left to act on and says so rather than sitting on
// while nothing happens.
void ChannelEffectsWindow::applyMidiNoiseRule() {
    if (!input_.noise) return;
    if (inputNoiseTip_.isNull()) inputNoiseTip_ = input_.noise->toolTip();

    const bool midi = micSourcesIncludeMidi();
    const QString tip =
        midi ? tr("Unavailable while a MIDI instrument feeds this microphone.\n"
                  "Noise suppression is trained on speech and would silence an "
                  "instrument.")
             : inputNoiseTip_;

    input_.noise->setToolTip(tip);
    if (!midi) return;
    for (QWidget *w : {static_cast<QWidget *>(input_.noise),
                       static_cast<QWidget *>(input_.intensity),
                       static_cast<QWidget *>(input_.intensityLabel)}) {
        if (w) w->setEnabled(false);
    }
    if (inputNoiseIn_) inputNoiseIn_->setEnabled(false);
    if (inputNoiseOut_) inputNoiseOut_->setEnabled(false);
}

void ChannelEffectsWindow::applyOutputDependencies() {
    if (client_->channelEffectSourceMaster(channelId_, QStringLiteral("output"))) return;
    setStageControlsEnabled(output_, true);
    applyStageControlDependencies(output_, true);
}

void ChannelEffectsWindow::applyDuckingDependencies() {
    const bool editable =
        !client_->channelEffectSourceMaster(channelId_, QStringLiteral("output"));
    setDuckingControlsEnabled(editable);
    applyDuckingControlDependencies(ducking_, editable);
}

void ChannelEffectsWindow::applyLufsLimiterDependencies() {
    const bool editable =
        !client_->channelEffectSourceMaster(channelId_, QStringLiteral("output"));
    setLufsLimiterControlsEnabled(editable);
    applyLufsLimiterControlDependencies(lufsLimiter_, editable);
}

void ChannelEffectsWindow::setStageControlsEnabled(StageControls &ui, bool on) {
    ::setStageControlsEnabled(ui, on);
}

void ChannelEffectsWindow::pushInputCreativeSettings() {
    if (updating_ || !client_->available()) return;
    if (client_->channelEffectSourceMaster(channelId_, QStringLiteral("input"))) return;
    client_->setChannelCreativeFx(channelId_, QStringLiteral("input"),
                                  inputCreative_->values());
}

void ChannelEffectsWindow::pushOutputCreativeSettings() {
    if (updating_ || !client_->available()) return;
    if (client_->channelEffectSourceMaster(channelId_, QStringLiteral("output"))) return;
    client_->setChannelCreativeFx(channelId_, QStringLiteral("output"),
                                  outputCreative_->values());
}

void ChannelEffectsWindow::refreshInputCreative() {
    refreshCreativeFrom(channelId_, QStringLiteral("input"), inputCreative_);
}

void ChannelEffectsWindow::refreshOutputCreative() {
    refreshCreativeFrom(channelId_, QStringLiteral("output"), outputCreative_);
}

void ChannelEffectsWindow::refreshCreativeFrom(const QString &channelId,
                                               const QString &stage,
                                               CreativeFxPanel *ui) {
    const CreativeFxInfo info = client_->channelCreativeFx(channelId, stage);
    updating_ = true;
    ui->setValues(info);
    updating_ = false;
}

void ChannelEffectsWindow::setDuckingControlsEnabled(bool on) {
    if (ducking_.enabled) ducking_.enabled->setEnabled(on);
    if (ducking_.intensity) ducking_.intensity->setEnabled(on);
    if (ducking_.addSource) ducking_.addSource->setEnabled(on && ducking_.sourceRows.size() < 6);
    for (const DuckingSourceRow &row : ducking_.sourceRows) {
        if (row.kind) row.kind->setEnabled(on);
        if (row.channel) row.channel->setEnabled(on);
        if (row.remove) row.remove->setEnabled(on);
    }
}

void ChannelEffectsWindow::setLufsLimiterControlsEnabled(bool on) {
    if (lufsLimiter_.enabled) lufsLimiter_.enabled->setEnabled(on);
    if (lufsLimiter_.maxLufs) lufsLimiter_.maxLufs->setEnabled(on);
    if (lufsLimiter_.maxLufsReadout) lufsLimiter_.maxLufsReadout->setEnabled(on);
}

void ChannelEffectsWindow::updateEffectSourceUi() {
    if (!client_->available() || !inputEffectSource_ || !outputEffectSource_) return;

    const QString inputChoice = inputEffectSource_->currentData().toString();
    const bool deviceFx = inputChoice == QLatin1String(kDeviceFxSourceId);
    // The sentinel is a mode, not a device to inherit from, so it never reaches
    // the effect-source call.
    const QString inputMasterId = deviceFx ? QString() : inputChoice;
    const QString outputMasterId = outputEffectSource_->currentData().toString();
    const bool wantInputMaster = !inputMasterId.isEmpty();
    const bool wantOutputMaster = !outputMasterId.isEmpty();

    if (!updating_) {
        client_->setChannelMicUseDeviceFx(channelId_, deviceFx);
        client_->setChannelEffectSourceMaster(channelId_, QStringLiteral("input"),
                                              inputMasterId);
        client_->setChannelEffectSourceMaster(channelId_, QStringLiteral("output"),
                                              outputMasterId);
    }

    // In device-fx mode this channel's mic filters are not in the path at all,
    // so its controls would be adjusting nothing.
    setStageControlsEnabled(input_, !wantInputMaster && !deviceFx);
    inputCreative_->setEnabled(!wantInputMaster && !deviceFx);
    if (deviceFx) {
        if (inputNoiseIn_) inputNoiseIn_->setEnabled(false);
        if (inputNoiseOut_) inputNoiseOut_->setEnabled(false);
    }
    setStageControlsEnabled(output_, !wantOutputMaster);
    outputCreative_->setEnabled(!wantOutputMaster);
    setDuckingControlsEnabled(!wantOutputMaster);
    setLufsLimiterControlsEnabled(!wantOutputMaster);

    updating_ = true;
    if (wantInputMaster) {
        if (inputMasterId == QLatin1String("mic"))
            refreshStageFrom(QStringLiteral("mic"), QStringLiteral("input"), input_);
        else
            refreshMasterStage(client_, inputMasterId, input_);
        inputCreative_->setValues(client_->masterCreativeFx(inputMasterId));
    } else {
        refreshStageFrom(channelId_, QStringLiteral("input"), input_);
        refreshCreativeFrom(channelId_, QStringLiteral("input"), inputCreative_);
    }
    if (wantOutputMaster) {
        if (outputMasterId == QLatin1String("mic"))
            refreshStageFrom(QStringLiteral("mic"), QStringLiteral("output"), output_);
        else
            refreshMasterStage(client_, outputMasterId, output_);
        outputCreative_->setValues(
            client_->channelCreativeFx(QStringLiteral("mic"), QStringLiteral("output")));
        const DuckingInfo d = client_->channelDucking(QStringLiteral("mic"));
        QSignalBlocker b1(ducking_.enabled);
        QSignalBlocker b3(ducking_.intensity);
        ducking_.enabled->setChecked(d.enabled);
        const int pct = int(d.intensity * 100.0 + 0.5);
        ducking_.intensity->setValue(pct);
        ducking_.intensityLabel->setText(QStringLiteral("%1%").arg(pct));
        setDuckingSourcesFromConfig(d.sources);
        applyDuckingControlDependencies(ducking_, false);
        const LufsLimiterInfo lufs = client_->channelLufsLimiter(QStringLiteral("mic"));
        QSignalBlocker b4(lufsLimiter_.enabled);
        QSignalBlocker b5(lufsLimiter_.maxLufs);
        lufsLimiter_.enabled->setChecked(lufs.enabled);
        const int tenths = int(std::lround(lufs.maxLufs * 10.0));
        lufsLimiter_.maxLufs->setValue(tenths);
        lufsLimiter_.maxLufsReadout->setText(lufsReadout(tenths));
        styleEarProtectionSlider(lufsLimiter_.maxLufs, tenths);
        applyLufsLimiterControlDependencies(lufsLimiter_, false);
    } else {
        refreshStageFrom(channelId_, QStringLiteral("output"), output_);
        refreshCreativeFrom(channelId_, QStringLiteral("output"), outputCreative_);
        refreshDucking();
        refreshLufsLimiter();
    }
    updating_ = false;
    if (!wantInputMaster) {
        applyInputDependencies();
    } else {
        applyStageControlDependencies(input_, false, inputNoiseIn_, inputNoiseOut_);
    }
    if (!wantOutputMaster) {
        applyOutputDependencies();
        applyDuckingDependencies();
        applyLufsLimiterDependencies();
    } else {
        applyStageControlDependencies(output_, false);
        applyDuckingControlDependencies(ducking_, false);
        applyLufsLimiterControlDependencies(lufsLimiter_, false);
    }
    refreshInputLevels();
}

void ChannelEffectsWindow::refreshStage(const QString &stage, StageControls &ui) {
    refreshStageFrom(channelId_, stage, ui);
}

void ChannelEffectsWindow::refreshStageFrom(const QString &channelId, const QString &stage,
                                            StageControls &ui) {
    const ChannelFxInfo fx = client_->channelEffects(channelId, stage);
    const MicDynamicsInfo dyn = client_->channelDynamics(channelId, stage);

    QSignalBlocker b1(ui.noise);
    QSignalBlocker b2(ui.intensity);
    QSignalBlocker b3(ui.lowCut);
    QSignalBlocker b4(ui.lowCutHz);
    QSignalBlocker b5(ui.eq);
    QSignalBlocker b6(ui.lowDb);
    QSignalBlocker b7(ui.midDb);
    QSignalBlocker b8(ui.highDb);
    QSignalBlocker b9(ui.gate);
    QSignalBlocker b10(ui.gateThreshold);
    QSignalBlocker b11(ui.compressor);
    QSignalBlocker b12(ui.compThreshold);
    QSignalBlocker b13(ui.compRatio);
    QSignalBlocker b14(ui.autoMakeup);
    QSignalBlocker b15(ui.limiter);
    QSignalBlocker b16(ui.limitThreshold);

    ui.noise->setChecked(client_->channelNoiseSuppression(channelId, stage));
    const int pct = int(client_->channelNoiseIntensity(channelId, stage) * 100.0 + 0.5);
    ui.intensity->setValue(pct);
    ui.intensityLabel->setText(QStringLiteral("%1%").arg(pct));

    ui.lowCut->setChecked(fx.lowCut);
    ui.lowCutHz->setCurrentIndex(fx.lowCutHz == 120 ? 1 : 0);
    ui.eq->setChecked(fx.eq);
    ui.lowDb->setValue(int(std::lround(fx.lowDb * 10.0)));
    ui.midDb->setValue(int(std::lround(fx.midDb * 10.0)));
    ui.highDb->setValue(int(std::lround(fx.highDb * 10.0)));
    ui.lowReadout->setText(dbReadout(ui.lowDb->value()));
    ui.midReadout->setText(dbReadout(ui.midDb->value()));
    ui.highReadout->setText(dbReadout(ui.highDb->value()));
    showEqMode(ui.adv, fx.eqAdvanced);

    ui.gate->setChecked(dyn.gate);
    ui.gateThreshold->setValue(int(std::lround(dyn.gateThresholdDb * 10.0)));
    ui.gateThresholdReadout->setText(dbReadout(ui.gateThreshold->value()));
    ui.compressor->setChecked(dyn.compressor);
    ui.compThreshold->setValue(int(std::lround(dyn.compThresholdDb * 10.0)));
    ui.compThresholdReadout->setText(dbReadout(ui.compThreshold->value()));
    ui.compRatio->setValue(int(std::lround(dyn.compRatio * 10.0)));
    ui.compRatioReadout->setText(QStringLiteral("%1:1").arg(dyn.compRatio, 0, 'f', 1));
    ui.autoMakeup->setChecked(dyn.autoMakeup);
    ui.limiter->setChecked(dyn.limiter);
    ui.limitThreshold->setValue(int(std::lround(dyn.limitThresholdDb * 10.0)));
    ui.limitThresholdReadout->setText(dbReadout(ui.limitThreshold->value()));
    showDeEsser(ui, client_->channelDeEsser(channelId, stage),
                client_->channelDeEsserIntensity(channelId, stage));
}

void ChannelEffectsWindow::setMicTabEnabled(bool on) {
    if (!tabs_) return;
    tabs_->setTabEnabled(0, on);
    if (!on && tabs_->currentIndex() == 0) tabs_->setCurrentIndex(1);
}

void ChannelEffectsWindow::refresh() {
    if (!client_->available() || !isVisible()) return;

    // Ahead of the signature check below, which is computed from the effect
    // settings alone: turning the recording device on or off changes none of
    // them, so behind that check this never ran and the switch sat at whatever
    // it last showed -- reading "off" for a microphone that was on.
    // A failed read answers false, which is indistinguishable from a real "off"
    // -- and this runs on every poll, so one unanswered call would throw the
    // switch the user had just set. Only a reply the daemon actually gave is
    // allowed to move it.
    bool ok = false;
    if (micSource_) {
        const bool on = client_->channelMicSource(channelId_, &ok);
        if (ok) {
            if (on != micSource_->isChecked()) {
                updating_ = true;
                micSource_->setChecked(on);
                updating_ = false;
            }
            setMicTabEnabled(on);
        }
    }

    if (masterMicRows_) {
        // An unanswered call reads as an empty list, which would otherwise wipe
        // the rows the user just set.
        const QStringList ids = client_->channelMasterMics(channelId_);
        if (!ids.isEmpty()) {
            const QString sig = masterMicRowsSignature(ids, client_->masterBuses());
            if (sig != masterMicSignature_) {
                masterMicSignature_ = sig;
                rebuildMasterMicRows(ids);
            }
        }
    }

    const ChannelFxInfo inFx =
        client_->channelEffects(channelId_, QStringLiteral("input"));
    const MicDynamicsInfo inDyn =
        client_->channelDynamics(channelId_, QStringLiteral("input"));
    const QString inPart =
        QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
            .arg(inFx.lowCut)
            .arg(inFx.lowCutHz)
            .arg(inFx.eq)
            .arg(inFx.lowDb)
            .arg(inFx.midDb)
            .arg(inFx.highDb)
            .arg(client_->channelNoiseSuppression(channelId_, QStringLiteral("input")))
            .arg(client_->channelNoiseIntensity(channelId_, QStringLiteral("input")))
            .arg(inDyn.compressor ? 1 : 0);
    const ChannelFxInfo outFx =
        client_->channelEffects(channelId_, QStringLiteral("output"));
    const MicDynamicsInfo outDyn =
        client_->channelDynamics(channelId_, QStringLiteral("output"));
    const QString outPart =
        QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
            .arg(outFx.lowCut)
            .arg(outFx.lowCutHz)
            .arg(outFx.eq)
            .arg(outFx.lowDb)
            .arg(outFx.midDb)
            .arg(outFx.highDb)
            .arg(client_->channelNoiseSuppression(channelId_, QStringLiteral("output")))
            .arg(client_->channelNoiseIntensity(channelId_, QStringLiteral("output")))
            .arg(outDyn.compressor ? 1 : 0);
    const DuckingInfo duck = client_->channelDucking(channelId_);
    QString duckSourcesPart;
    for (const DuckingSourceInfo &s : duck.sources)
        duckSourcesPart += s.kind + QLatin1Char(':') + s.channelId + QLatin1Char('|');
    const QString duckPart = QStringLiteral("%1|%2|%3")
                                 .arg(duck.enabled)
                                 .arg(duck.intensity)
                                 .arg(duckSourcesPart);
    const LufsLimiterInfo lufs = client_->channelLufsLimiter(channelId_);
    const QString lufsPart = QStringLiteral("%1|%2").arg(lufs.enabled).arg(lufs.maxLufs);

    const QString inputMasterId =
        client_->channelEffectSourceMasterId(channelId_, QStringLiteral("input"));
    const QString outputMasterId =
        client_->channelEffectSourceMasterId(channelId_, QStringLiteral("output"));
    const QString masterInPart =
        inputMasterId.isEmpty()
            ? QString()
            : (inputMasterId == QLatin1String("mic")
                   ? stageSettingsSignature(client_, QStringLiteral("mic"),
                                            QStringLiteral("input"))
                   : masterStageSettingsSignature(client_, inputMasterId));
    const QString masterOutPart =
        outputMasterId.isEmpty()
            ? QString()
            : (outputMasterId == QLatin1String("mic")
                   ? stageSettingsSignature(client_, QStringLiteral("mic"),
                                            QStringLiteral("output"))
                   : masterStageSettingsSignature(client_, outputMasterId));
    const DuckingInfo masterDuck = client_->channelDucking(QStringLiteral("mic"));
    QString masterDuckPart;
    for (const DuckingSourceInfo &s : masterDuck.sources)
        masterDuckPart += s.kind + QLatin1Char(':') + s.channelId + QLatin1Char('|');
    const QString masterDuckSig = QStringLiteral("%1|%2|%3")
                                      .arg(masterDuck.enabled)
                                      .arg(masterDuck.intensity)
                                      .arg(masterDuckPart);
    const LufsLimiterInfo masterLufs = client_->channelLufsLimiter(QStringLiteral("mic"));
    const QString masterLufsSig =
        QStringLiteral("%1|%2").arg(masterLufs.enabled).arg(masterLufs.maxLufs);
    // Only a reply the daemon actually gave may move the selector; an
    // unanswered call would otherwise read as "not in device-fx mode" and throw
    // the user's choice back to Unique on the next poll.
    bool deviceFxOk = false;
    const bool deviceFx = client_->channelMicUseDeviceFx(channelId_, &deviceFxOk);
    const QString newSig =
        inPart + QLatin1Char(';') + outPart + QLatin1Char(';') + duckPart +
        QLatin1Char(';') + lufsPart + QLatin1Char(';') + inputMasterId +
        QLatin1Char('|') + outputMasterId + QLatin1Char(';') + masterInPart +
        QLatin1Char(';') + masterOutPart + QLatin1Char(';') + masterDuckSig +
        QLatin1Char(';') + masterLufsSig + QLatin1Char(';') +
        QString::number(deviceFxOk && deviceFx);
    if (newSig == signature_) return;
    signature_ = newSig;

    updating_ = true;
    populateEffectSourceCombo(inputEffectSource_, client_, true);
    populateEffectSourceCombo(outputEffectSource_, client_);
    if (inputEffectSource_) {
        const int idx = inputEffectSource_->findData(
            deviceFxOk && deviceFx ? QString::fromLatin1(kDeviceFxSourceId)
                                   : inputMasterId);
        if (idx >= 0) inputEffectSource_->setCurrentIndex(idx);
    }
    if (outputEffectSource_) {
        const int idx = outputEffectSource_->findData(outputMasterId);
        if (idx >= 0) outputEffectSource_->setCurrentIndex(idx);
    }
    updating_ = false;
    updateEffectSourceUi();
    refreshInputLevels();
}

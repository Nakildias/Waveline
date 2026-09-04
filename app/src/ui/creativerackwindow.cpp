// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "creativerackwindow.h"

#include <QAbstractButton>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>

#include "engine/creativefx.h"
#include "mixerclient.h"
#include "rackpresetstore.h"
#include "theme.h"
#include "widgets.h"

namespace {
int scaled(double v, double factor) { return int(std::lround(v * factor)); }

constexpr const char *kPresetSuffix = ".wlrackpreset";

// A preset name that is safe as a file name, for the export dialog's
// suggestion. Only the suggestion: the user can call the file anything --
// same convention and same regex as ProfilesWindow's fileStemFor().
QString fileStemForPreset(const QString &name) {
    static const QRegularExpression unsafe(QStringLiteral(R"([^\w.\- ]+)"));
    QString s = name;
    s.replace(unsafe, QStringLiteral("_"));
    s = s.trimmed();
    return s.isEmpty() ? QStringLiteral("rack-preset") : s;
}

QString stageTitle(int idx) {
    switch (idx) {
        case waveline::kFxBitcrusher: return QObject::tr("Bitcrusher");
        case waveline::kFxOverdrive: return QObject::tr("Overdrive");
        case waveline::kFxChorus: return QObject::tr("Chorus");
        case waveline::kFxFlanger: return QObject::tr("Flanger");
        case waveline::kFxPhaser: return QObject::tr("Phaser");
        case waveline::kFxTremolo: return QObject::tr("Tremolo");
        case waveline::kFxDelay: return QObject::tr("Delay");
        case waveline::kFxReverb: return QObject::tr("Reverb");
        case waveline::kFxEq: return QObject::tr("EQ");
        case waveline::kFxRingMod: return QObject::tr("Ring Mod");
        case waveline::kFxEnvFilter: return QObject::tr("Envelope Filter");
        case waveline::kFxPitch: return QObject::tr("Pitch");
        case waveline::kFxReverseDelay: return QObject::tr("Reverse Delay");
        case waveline::kFxTapeSat: return QObject::tr("Tape Sat");
        default: return QString();
    }
}

bool stageEnabled(const CreativeFxInfo &v, int idx) {
    switch (idx) {
        case waveline::kFxBitcrusher: return v.bitcrusher.enabled;
        case waveline::kFxOverdrive: return v.overdrive.enabled;
        case waveline::kFxChorus: return v.chorus.enabled;
        case waveline::kFxFlanger: return v.flanger.enabled;
        case waveline::kFxPhaser: return v.phaser.enabled;
        case waveline::kFxTremolo: return v.tremolo.enabled;
        case waveline::kFxDelay: return v.delay.enabled;
        case waveline::kFxReverb: return v.reverb.enabled;
        case waveline::kFxEq: return v.eq.enabled;
        case waveline::kFxRingMod: return v.ringMod.enabled;
        case waveline::kFxEnvFilter: return v.envFilter.enabled;
        case waveline::kFxPitch: return v.pitch.enabled;
        case waveline::kFxReverseDelay: return v.reverseDelay.enabled;
        case waveline::kFxTapeSat: return v.tapeSat.enabled;
        default: return false;
    }
}

void setStageEnabled(CreativeFxInfo &v, int idx, bool on) {
    switch (idx) {
        case waveline::kFxBitcrusher: v.bitcrusher.enabled = on; break;
        case waveline::kFxOverdrive: v.overdrive.enabled = on; break;
        case waveline::kFxChorus: v.chorus.enabled = on; break;
        case waveline::kFxFlanger: v.flanger.enabled = on; break;
        case waveline::kFxPhaser: v.phaser.enabled = on; break;
        case waveline::kFxTremolo: v.tremolo.enabled = on; break;
        case waveline::kFxDelay: v.delay.enabled = on; break;
        case waveline::kFxReverb: v.reverb.enabled = on; break;
        case waveline::kFxEq: v.eq.enabled = on; break;
        case waveline::kFxRingMod: v.ringMod.enabled = on; break;
        case waveline::kFxEnvFilter: v.envFilter.enabled = on; break;
        case waveline::kFxPitch: v.pitch.enabled = on; break;
        case waveline::kFxReverseDelay: v.reverseDelay.enabled = on; break;
        case waveline::kFxTapeSat: v.tapeSat.enabled = on; break;
        default: break;
    }
}

// A narrow vertical button with its label rotated -90 degrees, so "RESET"
// reads sideways up the same width the power switch already occupies on
// the module's other edge -- a horizontal button with that word on it
// would need far more width than this column has to spare.
class SidewaysButton : public QAbstractButton {
public:
    SidewaysButton(const QString &text, QWidget *parent) : QAbstractButton(parent) {
        setText(text);
        setCursor(Qt::PointingHandCursor);
    }

    QSize sizeHint() const override { return {24, 54}; }

protected:
    void enterEvent(QEnterEvent *) override {
        hover_ = true;
        update();
    }
    void leaveEvent(QEvent *) override {
        hover_ = false;
        update();
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        constexpr qreal kRadius = 5.0;
        QColor bg = Theme::Well;
        if (isDown()) bg = bg.darker(115);
        else if (hover_) bg = bg.lighter(120);
        p.setPen(QPen(Theme::Line, 1));
        p.setBrush(bg);
        p.drawRoundedRect(r, kRadius, kRadius);

        p.save();
        p.translate(r.center());
        p.rotate(-90);
        QFont f = font();
        f.setWeight(QFont::DemiBold);
        f.setPointSizeF(f.pointSizeF() * 0.68);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
        p.setFont(f);
        p.setPen(Theme::TextDim);
        // The text rect's local width, post-rotation, runs along the
        // button's actual height -- padding that in is what keeps "RESET"
        // off the top and bottom rounded corners. TextDontClip is a safety
        // net on top of sizing the font to actually fit: a stray pixel of
        // overflow into the padding reads far better than a clipped R.
        constexpr qreal kEndPad = 5.0;
        const qreal textSpan = std::max(0.0, r.height() - kEndPad * 2.0);
        p.drawText(QRectF(-textSpan / 2.0, -r.width() / 2.0, textSpan, r.width()),
                  Qt::AlignCenter | Qt::TextDontClip, text());
        p.restore();
    }

private:
    bool hover_ = false;
};

// The Rack's own title bar, painted flush black rather than left to
// whatever the window manager draws: Close and Minimize sit up front (the
// order the user asked for), the effect-chain title in the middle, and the
// "+" add-effect button anchored to the far right, all in one row instead
// of a native caption plus a second in-window heading.
class TitleBar : public QWidget {
public:
    explicit TitleBar(QWidget *parent) : QWidget(parent) {
        setAutoFillBackground(true);
        QPalette p = palette();
        p.setColor(QPalette::Window, Qt::black);
        setPalette(p);
        setFixedHeight(40);
    }

protected:
    // A frameless window has no WM-drawn grip to relocate it by -- dragging
    // this bar is the only way left to move the window. startSystemMove()
    // hands the drag to the windowing system itself rather than computing
    // positions by hand: on Wayland a client cannot reposition its own
    // top-level surface (QWindow::move() is a no-op there), so this is the
    // only approach that works on both X11 and Wayland.
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) return;
        if (QWindow *handle = window()->windowHandle()) handle->startSystemMove();
    }
};

}  // namespace

// ---------------------------------------------------------------- RackModule

RackModule::RackModule(int stageIndex, const QString &title, const QString &tip,
                       QWidget *parent)
    : QWidget(parent), stageIndex_(stageIndex) {
    setAutoFillBackground(false);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *card = new CardBase(this);
    card->setRadius(8);
    outer->addWidget(card);

    // A 2-row, 3-column grid rather than nested box layouts: a grid is what
    // actually keeps a cell's height in sync with its row-mates, which is
    // what lets the switch (row 1) come out vertically centered against the
    // knobs (also row 1) without hand-tuned stretches. Column 0 (grip) and
    // column 2 (remove button) are pinned to the same fixed width, so the
    // stretchy centre column -- and the title inside it -- sits on the
    // card's true centreline rather than one thrown off by a size mismatch
    // between the two side columns.
    auto *grid = new QGridLayout(card);
    grid->setContentsMargins(10, 8, 10, 8);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);

    constexpr int kSideColumnPx = 24;

    auto *gripLabel = new QLabel(QStringLiteral("⋮⋮"), card);
    gripLabel->setToolTip(QObject::tr("Drag to reorder this effect in the chain."));
    gripLabel->setCursor(Qt::SizeAllCursor);
    gripLabel->setFixedSize(kSideColumnPx, kSideColumnPx);
    gripLabel->setAlignment(Qt::AlignCenter);
    QPalette gp = gripLabel->palette();
    gp.setColor(QPalette::WindowText, Theme::TextFaint);
    gripLabel->setPalette(gp);
    grid->addWidget(gripLabel, 0, 0, Qt::AlignCenter);
    grip_ = gripLabel;

    auto *titleLabel = new QLabel(title, card);
    QFont f = titleLabel->font();
    f.setBold(true);
    titleLabel->setFont(f);
    titleLabel->setToolTip(tip);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    grid->addWidget(titleLabel, 0, 1, Qt::AlignVCenter);

    auto *removeBtn = new QPushButton(card);
    removeBtn->setIcon(QIcon(Theme::iconPixmap(QStringLiteral("minus"), Theme::TextDim, 14)));
    removeBtn->setToolTip(QObject::tr("Remove this effect from the rack."));
    removeBtn->setFixedSize(kSideColumnPx, kSideColumnPx);
    removeBtn->setCursor(Qt::PointingHandCursor);
    connect(removeBtn, &QPushButton::clicked, this,
           [this] { emit removeRequested(stageIndex_); });
    grid->addWidget(removeBtn, 0, 2, Qt::AlignCenter);

    // Reset sits directly under the grip, mirroring the power switch under
    // the remove button on the other side -- the two side columns each get
    // one control per row, so the knobs in between stay the only thing that
    // varies module to module. Sideways rather than a normal wide button:
    // this column is exactly grip-width, nowhere near enough to lay "RESET"
    // out horizontally.
    auto *resetBtn = new SidewaysButton(QObject::tr("RESET"), card);
    resetBtn->setToolTip(QObject::tr("Reset this effect to factory defaults."));
    resetBtn->setFixedSize(kSideColumnPx, 54);
    connect(resetBtn, &QAbstractButton::clicked, this, &RackModule::resetToDefaults);
    grid->addWidget(resetBtn, 1, 0, Qt::AlignCenter);

    controlsLay_ = new QHBoxLayout;
    controlsLay_->setSpacing(14);
    grid->addLayout(controlsLay_, 1, 1);

    power_ = new PowerSwitch(card);
    power_->setToolTip(QObject::tr("Power this effect on or off without removing it "
                                   "from the rack."));
    connect(power_, &QAbstractButton::toggled, this, [this](bool) { emitChanged(); });
    grid->addWidget(power_, 1, 2, Qt::AlignCenter);
}

void RackModule::resetToDefaults() {
    loadFrom(CreativeFxInfo{});
    emitChanged();
}

void RackModule::emitChanged() {
    if (!loading_) emit changed();
}

RackModule::KnobUi RackModule::addKnob(const QString &label, int min, int max,
                                       const std::function<QString(int)> &format,
                                       int quantizeSteps) {
    KnobUi ui;
    auto *col = new QVBoxLayout;
    col->setSpacing(2);

    auto *name = new QLabel(label, this);
    name->setAlignment(Qt::AlignCenter);
    QPalette np = name->palette();
    np.setColor(QPalette::WindowText, Theme::TextDim);
    name->setPalette(np);
    col->addWidget(name);

    auto *knobRow = new QHBoxLayout;
    knobRow->setContentsMargins(0, 0, 0, 0);
    ui.knob = new Knob(this);
    ui.knob->setRange(min, max);
    knobRow->addWidget(ui.knob);
    col->addLayout(knobRow);

    ui.readout = new QLabel(this);
    ui.readout->setAlignment(Qt::AlignCenter);
    col->addWidget(ui.readout);

    controlsLay_->addLayout(col);

    QLabel *readout = ui.readout;
    if (quantizeSteps > 1) {
        // Snap to the nearest stop before doing anything else -- if that
        // means correcting the value we were just handed, setValue() below
        // re-enters this same lambda with the corrected value, which is
        // already a stop, so it falls through to the normal path on that
        // second call. Never resting off a stop is what stops a drag from
        // reading as a flicker through every value in between.
        Knob *knobPtr = ui.knob;
        connect(ui.knob, &QAbstractSlider::valueChanged, this,
               [this, knobPtr, readout, format, min, max, quantizeSteps](int v) {
                   const double stepSize = double(max - min) / (quantizeSteps - 1);
                   const int snapped =
                       min + int(std::lround((v - min) / stepSize) * stepSize);
                   if (snapped != v) {
                       knobPtr->setValue(snapped);
                       return;
                   }
                   readout->setText(format(v));
                   emitChanged();
               });
    } else {
        connect(ui.knob, &QAbstractSlider::valueChanged, this, [this, readout, format](int v) {
            readout->setText(format(v));
            emitChanged();
        });
    }
    ui.readout->setText(format(ui.knob->value()));
    return ui;
}

namespace {

class BitcrusherModule : public RackModule {
public:
    explicit BitcrusherModule(QWidget *parent)
        : RackModule(waveline::kFxBitcrusher, stageTitle(waveline::kFxBitcrusher),
                    QObject::tr("Bit-depth and sample-rate reduction."), parent) {
        depth_ = addKnob(QObject::tr("Depth"), 1, 16,
                         [](int v) { return QStringLiteral("%1 bit").arg(v); });
        crush_ = addKnob(QObject::tr("Crush"), 0, 100,
                         [](int v) { return QStringLiteral("%1%").arg(v); });
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.bitcrusher = {power()->isChecked(), double(depth_.knob->value()),
                        crush_.knob->value() / 100.0, mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.bitcrusher.enabled);
        depth_.knob->setValue(int(std::lround(v.bitcrusher.bitDepth)));
        crush_.knob->setValue(scaled(v.bitcrusher.sampleRateReduction, 100.0));
        mix_.knob->setValue(scaled(v.bitcrusher.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi depth_, crush_, mix_;
};

class OverdriveModule : public RackModule {
public:
    explicit OverdriveModule(QWidget *parent)
        : RackModule(waveline::kFxOverdrive, stageTitle(waveline::kFxOverdrive),
                    QObject::tr("Warm saturation and harmonic crunch."), parent) {
        drive_ = addKnob(QObject::tr("Drive"), 0, 100,
                         [](int v) { return QStringLiteral("%1%").arg(v); });
        tone_ = addKnob(QObject::tr("Tone"), 0, 100, [](int v) {
            if (v == 50) return QStringLiteral("flat");
            return v < 50 ? QObject::tr("dark %1").arg(50 - v)
                          : QObject::tr("bright %1").arg(v - 50);
        });
        output_ = addKnob(QObject::tr("Output"), -240, 240, [](int v) {
            return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1);
        });
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.overdrive = {power()->isChecked(), drive_.knob->value() / 100.0,
                       tone_.knob->value() / 100.0, output_.knob->value() / 10.0,
                       mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.overdrive.enabled);
        drive_.knob->setValue(scaled(v.overdrive.drive, 100.0));
        tone_.knob->setValue(scaled(v.overdrive.tone, 100.0));
        output_.knob->setValue(scaled(v.overdrive.outputDb, 10.0));
        mix_.knob->setValue(scaled(v.overdrive.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi drive_, tone_, output_, mix_;
};

class ChorusModule : public RackModule {
public:
    explicit ChorusModule(QWidget *parent)
        : RackModule(waveline::kFxChorus, stageTitle(waveline::kFxChorus),
                    QObject::tr("Modulated thickening."), parent) {
        rate_ = addKnob(QObject::tr("Rate"), 2, 500, [](int v) {
            return QStringLiteral("%1 Hz").arg(v / 100.0, 0, 'f', 2);
        });
        depth_ = addKnob(QObject::tr("Depth"), 5, 80, [](int v) {
            return QStringLiteral("%1 ms").arg(v / 10.0, 0, 'f', 1);
        });
        feedback_ = addKnob(QObject::tr("Feedback"), 0, 90,
                            [](int v) { return QStringLiteral("%1%").arg(v); });
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.chorus = {power()->isChecked(), rate_.knob->value() / 100.0,
                   depth_.knob->value() / 10.0, feedback_.knob->value() / 100.0,
                   mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.chorus.enabled);
        rate_.knob->setValue(scaled(v.chorus.rateHz, 100.0));
        depth_.knob->setValue(scaled(v.chorus.depthMs, 10.0));
        feedback_.knob->setValue(scaled(v.chorus.feedback, 100.0));
        mix_.knob->setValue(scaled(v.chorus.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi rate_, depth_, feedback_, mix_;
};

class FlangerModule : public RackModule {
public:
    explicit FlangerModule(QWidget *parent)
        : RackModule(waveline::kFxFlanger, stageTitle(waveline::kFxFlanger),
                    QObject::tr("Short modulated delay with feedback."), parent) {
        rate_ = addKnob(QObject::tr("Rate"), 2, 200, [](int v) {
            return QStringLiteral("%1 Hz").arg(v / 100.0, 0, 'f', 2);
        });
        depth_ = addKnob(QObject::tr("Depth"), 1, 30, [](int v) {
            return QStringLiteral("%1 ms").arg(v / 10.0, 0, 'f', 1);
        });
        feedback_ = addKnob(QObject::tr("Feedback"), 0, 95,
                            [](int v) { return QStringLiteral("%1%").arg(v); });
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.flanger = {power()->isChecked(), rate_.knob->value() / 100.0,
                    depth_.knob->value() / 10.0, feedback_.knob->value() / 100.0,
                    mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.flanger.enabled);
        rate_.knob->setValue(scaled(v.flanger.rateHz, 100.0));
        depth_.knob->setValue(scaled(v.flanger.depthMs, 10.0));
        feedback_.knob->setValue(scaled(v.flanger.feedback, 100.0));
        mix_.knob->setValue(scaled(v.flanger.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi rate_, depth_, feedback_, mix_;
};

class PhaserModule : public RackModule {
public:
    explicit PhaserModule(QWidget *parent)
        : RackModule(waveline::kFxPhaser, stageTitle(waveline::kFxPhaser),
                    QObject::tr("Swept all-pass filtering."), parent) {
        rate_ = addKnob(QObject::tr("Rate"), 2, 500, [](int v) {
            return QStringLiteral("%1 Hz").arg(v / 100.0, 0, 'f', 2);
        });
        depth_ = addKnob(QObject::tr("Depth"), 0, 100,
                         [](int v) { return QStringLiteral("%1%").arg(v); });
        feedback_ = addKnob(QObject::tr("Resonance"), 0, 95,
                            [](int v) { return QStringLiteral("%1%").arg(v); });
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.phaser = {power()->isChecked(), rate_.knob->value() / 100.0,
                   depth_.knob->value() / 100.0, feedback_.knob->value() / 100.0,
                   mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.phaser.enabled);
        rate_.knob->setValue(scaled(v.phaser.rateHz, 100.0));
        depth_.knob->setValue(scaled(v.phaser.depth, 100.0));
        feedback_.knob->setValue(scaled(v.phaser.feedback, 100.0));
        mix_.knob->setValue(scaled(v.phaser.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi rate_, depth_, feedback_, mix_;
};

class TremoloModule : public RackModule {
public:
    explicit TremoloModule(QWidget *parent)
        : RackModule(waveline::kFxTremolo, stageTitle(waveline::kFxTremolo),
                    QObject::tr("Rhythmic volume pulsing."), parent) {
        rate_ = addKnob(QObject::tr("Rate"), 1, 200, [](int v) {
            return QStringLiteral("%1 Hz").arg(v / 10.0, 0, 'f', 1);
        });
        depth_ = addKnob(QObject::tr("Depth"), 0, 100,
                         [](int v) { return QStringLiteral("%1%").arg(v); });

        // Three stages on one knob rather than a combo, to match the rest
        // of the rack: 0=Sine, 50=Triangle, 100=Soft Sq. quantizeSteps=3
        // snaps to whichever of those it's on the moment it moves, so a
        // drag never rests on -- or even briefly shows -- anything between
        // them.
        shape_ = addKnob(
            QObject::tr("Shape"),
            0, 100,
            [](int v) {
                if (v <= 0) return QObject::tr("Sine");
                if (v < 100) return QObject::tr("Triangle");
                return QObject::tr("Soft Sq.");
            },
            3);
        shape_.knob->setSingleStep(50);
        shape_.knob->setPageStep(50);

        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        // Always exactly 0/50/100 once quantized, so integer division
        // recovers the stage index cleanly.
        const int shape = shape_.knob->value() / 50;
        v.tremolo = {power()->isChecked(), rate_.knob->value() / 10.0,
                    depth_.knob->value() / 100.0, shape, mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.tremolo.enabled);
        rate_.knob->setValue(scaled(v.tremolo.rateHz, 10.0));
        depth_.knob->setValue(scaled(v.tremolo.depth, 100.0));
        const int shape = std::clamp(v.tremolo.shape, 0, 2);
        shape_.knob->setValue(shape == 0 ? 0 : (shape == 1 ? 50 : 100));
        mix_.knob->setValue(scaled(v.tremolo.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi rate_, depth_, mix_, shape_;
};

class DelayModule : public RackModule {
public:
    explicit DelayModule(QWidget *parent)
        : RackModule(waveline::kFxDelay, stageTitle(waveline::kFxDelay),
                    QObject::tr("Echo with feedback."), parent) {
        time_ = addKnob(QObject::tr("Time"), 10, 2000,
                        [](int v) { return QStringLiteral("%1 ms").arg(v); });
        feedback_ = addKnob(QObject::tr("Feedback"), 0, 95,
                            [](int v) { return QStringLiteral("%1%").arg(v); });
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
        damping_ = addKnob(QObject::tr("Damping"), 0, 100,
                           [](int v) { return QStringLiteral("%1%").arg(v); });

        // A two-state knob rather than a switch, to match the rest of the
        // rack: 0 = off, 100 = on. quantizeSteps=2 means it only ever rests
        // at one of those two, never flickering through the range between.
        pingPong_ = addKnob(
            QObject::tr("Ping-Pong"), 0, 100,
            [](int v) { return v >= 100 ? QObject::tr("ON") : QObject::tr("OFF"); }, 2);
        pingPong_.knob->setSingleStep(100);
        pingPong_.knob->setPageStep(100);
        pingPong_.knob->setToolTip(
            QObject::tr("Bounces the echo between left and right. Needs a real "
                        "stereo pair."));
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.delay = {power()->isChecked(),
                  double(time_.knob->value()),
                  feedback_.knob->value() / 100.0,
                  mix_.knob->value() / 100.0,
                  damping_.knob->value() / 100.0,
                  pingPong_.knob->value() >= 100};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.delay.enabled);
        time_.knob->setValue(int(std::lround(v.delay.timeMs)));
        feedback_.knob->setValue(scaled(v.delay.feedback, 100.0));
        mix_.knob->setValue(scaled(v.delay.mix, 100.0));
        damping_.knob->setValue(scaled(v.delay.damping, 100.0));
        pingPong_.knob->setValue(v.delay.pingPong ? 100 : 0);
        loading_ = false;
    }

private:
    KnobUi time_, feedback_, mix_, damping_, pingPong_;
};

class ReverbModule : public RackModule {
public:
    explicit ReverbModule(QWidget *parent)
        : RackModule(waveline::kFxReverb, stageTitle(waveline::kFxReverb),
                    QObject::tr("Room ambience after EQ and dynamics."), parent) {
        size_ = addKnob(QObject::tr("Size"), 0, 100,
                        [](int v) { return QStringLiteral("%1%").arg(v); });
        damping_ = addKnob(QObject::tr("Damping"), 0, 100,
                           [](int v) { return QStringLiteral("%1%").arg(v); });
        predelay_ = addKnob(QObject::tr("Predelay"), 0, 100,
                            [](int v) { return QStringLiteral("%1 ms").arg(v); });
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.reverb = {power()->isChecked(), size_.knob->value() / 100.0,
                   damping_.knob->value() / 100.0, double(predelay_.knob->value()),
                   mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.reverb.enabled);
        size_.knob->setValue(scaled(v.reverb.size, 100.0));
        damping_.knob->setValue(scaled(v.reverb.damping, 100.0));
        predelay_.knob->setValue(int(std::lround(v.reverb.predelayMs)));
        mix_.knob->setValue(scaled(v.reverb.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi size_, damping_, predelay_, mix_;
};

class EqModule : public RackModule {
public:
    explicit EqModule(QWidget *parent)
        : RackModule(waveline::kFxEq, stageTitle(waveline::kFxEq),
                    QObject::tr("Amp-style tone stack. Bypasses the standard EQ in the "
                                "Processing tab while active."),
                    parent) {
        gain_ = addKnob(QObject::tr("Gain"), -240, 240, [](int v) {
            return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1);
        });
        bass_ = addKnob(QObject::tr("Bass"), -120, 120, [](int v) {
            return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1);
        });
        mid_ = addKnob(QObject::tr("Middle"), -120, 120, [](int v) {
            return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1);
        });
        treble_ = addKnob(QObject::tr("Treble"), -120, 120, [](int v) {
            return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1);
        });
        presence_ = addKnob(QObject::tr("Presence"), -120, 120, [](int v) {
            return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1);
        });
        master_ = addKnob(QObject::tr("Master"), -240, 240, [](int v) {
            return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1);
        });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.eq = {power()->isChecked(),      gain_.knob->value() / 10.0,
               bass_.knob->value() / 10.0,     mid_.knob->value() / 10.0,
               treble_.knob->value() / 10.0,   presence_.knob->value() / 10.0,
               master_.knob->value() / 10.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.eq.enabled);
        gain_.knob->setValue(scaled(v.eq.gain, 10.0));
        bass_.knob->setValue(scaled(v.eq.bass, 10.0));
        mid_.knob->setValue(scaled(v.eq.mid, 10.0));
        treble_.knob->setValue(scaled(v.eq.treble, 10.0));
        presence_.knob->setValue(scaled(v.eq.presence, 10.0));
        master_.knob->setValue(scaled(v.eq.master, 10.0));
        loading_ = false;
    }

private:
    KnobUi gain_, bass_, mid_, treble_, presence_, master_;
};

class RingModulatorModule : public RackModule {
public:
    explicit RingModulatorModule(QWidget *parent)
        : RackModule(waveline::kFxRingMod, stageTitle(waveline::kFxRingMod),
                    QObject::tr("Multiplies by an internal sine wave for metallic, "
                                "robotic, bell-like tones."),
                    parent) {
        freq_ = addKnob(QObject::tr("Frequency"), 10, 2000,
                        [](int v) { return QStringLiteral("%1 Hz").arg(v); });
        fineTune_ = addKnob(QObject::tr("Fine Tune"), -50, 50,
                            [](int v) { return QStringLiteral("%1 Hz").arg(v); });
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.ringMod = {power()->isChecked(), double(freq_.knob->value()),
                    double(fineTune_.knob->value()), mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.ringMod.enabled);
        freq_.knob->setValue(int(std::lround(v.ringMod.frequencyHz)));
        fineTune_.knob->setValue(int(std::lround(v.ringMod.fineTuneHz)));
        mix_.knob->setValue(scaled(v.ringMod.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi freq_, fineTune_, mix_;
};

class EnvelopeFilterModule : public RackModule {
public:
    explicit EnvelopeFilterModule(QWidget *parent)
        : RackModule(waveline::kFxEnvFilter, stageTitle(waveline::kFxEnvFilter),
                    QObject::tr("Auto-wah: a peak follower sweeps a resonant bandpass "
                                "filter with playing dynamics."),
                    parent) {
        sens_ = addKnob(QObject::tr("Sensitivity"), 0, 100,
                        [](int v) { return QStringLiteral("%1%").arg(v); });
        attack_ = addKnob(QObject::tr("Attack"), 1, 100,
                          [](int v) { return QStringLiteral("%1 ms").arg(v); });
        release_ = addKnob(QObject::tr("Release"), 10, 500,
                           [](int v) { return QStringLiteral("%1 ms").arg(v); });
        resonance_ = addKnob(QObject::tr("Resonance"), 0, 95,
                             [](int v) { return QStringLiteral("%1%").arg(v); });
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.envFilter = {power()->isChecked(),           sens_.knob->value() / 100.0,
                      double(attack_.knob->value()),  double(release_.knob->value()),
                      resonance_.knob->value() / 100.0, mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.envFilter.enabled);
        sens_.knob->setValue(scaled(v.envFilter.sensitivity, 100.0));
        attack_.knob->setValue(int(std::lround(v.envFilter.attackMs)));
        release_.knob->setValue(int(std::lround(v.envFilter.releaseMs)));
        resonance_.knob->setValue(scaled(v.envFilter.resonance, 100.0));
        mix_.knob->setValue(scaled(v.envFilter.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi sens_, attack_, release_, resonance_, mix_;
};

class PitchShifterModule : public RackModule {
public:
    explicit PitchShifterModule(QWidget *parent)
        : RackModule(waveline::kFxPitch, stageTitle(waveline::kFxPitch),
                    QObject::tr("Time-domain grain-delay pitch shift, for octave lines "
                                "or wide detune."),
                    parent) {
        semitones_ = addKnob(QObject::tr("Pitch"), -12, 12,
                             [](int v) { return QStringLiteral("%1 st").arg(v); });
        detune_ = addKnob(QObject::tr("Detune"), -50, 50,
                          [](int v) { return QStringLiteral("%1 ct").arg(v); });
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.pitch = {power()->isChecked(), double(semitones_.knob->value()),
                  double(detune_.knob->value()), mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.pitch.enabled);
        semitones_.knob->setValue(int(std::lround(v.pitch.semitones)));
        detune_.knob->setValue(int(std::lround(v.pitch.detuneCents)));
        mix_.knob->setValue(scaled(v.pitch.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi semitones_, detune_, mix_;
};

class ReverseDelayModule : public RackModule {
public:
    explicit ReverseDelayModule(QWidget *parent)
        : RackModule(waveline::kFxReverseDelay, stageTitle(waveline::kFxReverseDelay),
                    QObject::tr("Records into a rolling buffer and plays each block "
                                "back reversed."),
                    parent) {
        time_ = addKnob(QObject::tr("Time"), 50, 2000,
                        [](int v) { return QStringLiteral("%1 ms").arg(v); });
        feedback_ = addKnob(QObject::tr("Feedback"), 0, 95,
                            [](int v) { return QStringLiteral("%1%").arg(v); });
        fader_ = addKnob(QObject::tr("Fader"), 0, 100,
                         [](int v) { return QStringLiteral("%1%").arg(v); });
        fader_.knob->setToolTip(
            QObject::tr("Crossfade width at each block edge, smoothing the splice."));
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.reverseDelay = {power()->isChecked(),  double(time_.knob->value()),
                         feedback_.knob->value() / 100.0, fader_.knob->value() / 100.0,
                         mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.reverseDelay.enabled);
        time_.knob->setValue(int(std::lround(v.reverseDelay.timeMs)));
        feedback_.knob->setValue(scaled(v.reverseDelay.feedback, 100.0));
        fader_.knob->setValue(scaled(v.reverseDelay.smoothing, 100.0));
        mix_.knob->setValue(scaled(v.reverseDelay.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi time_, feedback_, fader_, mix_;
};

class TapeSaturatorModule : public RackModule {
public:
    explicit TapeSaturatorModule(QWidget *parent)
        : RackModule(waveline::kFxTapeSat, stageTitle(waveline::kFxTapeSat),
                    QObject::tr("Soft saturation, wow/flutter and bandwidth narrowing "
                                "for instant vintage tape or vinyl texture."),
                    parent) {
        drive_ = addKnob(QObject::tr("Drive"), 0, 100,
                         [](int v) { return QStringLiteral("%1%").arg(v); });
        flutter_ = addKnob(QObject::tr("Flutter"), 0, 100,
                           [](int v) { return QStringLiteral("%1%").arg(v); });
        age_ = addKnob(QObject::tr("Age"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
        mix_ = addKnob(QObject::tr("Mix"), 0, 100,
                       [](int v) { return QStringLiteral("%1%").arg(v); });
    }
    void applyTo(CreativeFxInfo &v) const override {
        v.tapeSat = {power()->isChecked(), drive_.knob->value() / 100.0,
                    flutter_.knob->value() / 100.0, age_.knob->value() / 100.0,
                    mix_.knob->value() / 100.0};
    }
    void loadFrom(const CreativeFxInfo &v) override {
        loading_ = true;
        power()->setChecked(v.tapeSat.enabled);
        drive_.knob->setValue(scaled(v.tapeSat.drive, 100.0));
        flutter_.knob->setValue(scaled(v.tapeSat.flutter, 100.0));
        age_.knob->setValue(scaled(v.tapeSat.age, 100.0));
        mix_.knob->setValue(scaled(v.tapeSat.mix, 100.0));
        loading_ = false;
    }

private:
    KnobUi drive_, flutter_, age_, mix_;
};

RackModule *createModuleFor(int stageIndex, QWidget *parent) {
    switch (stageIndex) {
        case waveline::kFxBitcrusher: return new BitcrusherModule(parent);
        case waveline::kFxOverdrive: return new OverdriveModule(parent);
        case waveline::kFxChorus: return new ChorusModule(parent);
        case waveline::kFxFlanger: return new FlangerModule(parent);
        case waveline::kFxPhaser: return new PhaserModule(parent);
        case waveline::kFxTremolo: return new TremoloModule(parent);
        case waveline::kFxDelay: return new DelayModule(parent);
        case waveline::kFxReverb: return new ReverbModule(parent);
        case waveline::kFxEq: return new EqModule(parent);
        case waveline::kFxRingMod: return new RingModulatorModule(parent);
        case waveline::kFxEnvFilter: return new EnvelopeFilterModule(parent);
        case waveline::kFxPitch: return new PitchShifterModule(parent);
        case waveline::kFxReverseDelay: return new ReverseDelayModule(parent);
        case waveline::kFxTapeSat: return new TapeSaturatorModule(parent);
        default: return nullptr;
    }
}

}  // namespace

// ----------------------------------------------------------- VirtualRackWindow

VirtualRackWindow::VirtualRackWindow(MixerClient *client, const QString &masterId,
                                     const QString &title, QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint), client_(client),
      masterId_(masterId) {
    setWindowTitle(tr("Creative FX Rack / %1").arg(title));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *titleBar = new TitleBar(this);
    // A floor under the layout's width, so the window doesn't pinch down to
    // however wide the title text happens to be once there are few (or no)
    // modules racked -- height is what's meant to track content here, not
    // width.
    titleBar->setMinimumWidth(500);
    auto *titleLay = new QHBoxLayout(titleBar);
    titleLay->setContentsMargins(10, 0, 8, 0);
    titleLay->setSpacing(6);

    constexpr int kChromeBtnPx = 26;

    auto *closeBtn = new QPushButton(titleBar);
    closeBtn->setIcon(QIcon(Theme::iconPixmap(QStringLiteral("x"), Theme::Text, 14)));
    closeBtn->setToolTip(tr("Close"));
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setFixedSize(kChromeBtnPx, kChromeBtnPx);
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
    titleLay->addWidget(closeBtn);

    auto *minimizeBtn = new QPushButton(titleBar);
    minimizeBtn->setIcon(QIcon(Theme::iconPixmap(QStringLiteral("minus"), Theme::Text, 14)));
    minimizeBtn->setToolTip(tr("Minimize"));
    minimizeBtn->setCursor(Qt::PointingHandCursor);
    minimizeBtn->setFixedSize(kChromeBtnPx, kChromeBtnPx);
    connect(minimizeBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
    titleLay->addWidget(minimizeBtn);

    auto *heading = new QLabel(tr("Creative FX Rack / %1").arg(title), titleBar);
    QFont hf = heading->font();
    hf.setBold(true);
    hf.setPointSizeF(hf.pointSizeF() * 1.05);
    heading->setFont(hf);
    QPalette hp = heading->palette();
    hp.setColor(QPalette::WindowText, Theme::Text);
    heading->setPalette(hp);
    titleLay->addSpacing(4);
    titleLay->addWidget(heading);
    titleLay->addStretch(1);

    presetsBtn_ = new QPushButton(titleBar);
    presetsBtn_->setIcon(QIcon(Theme::iconPixmap(QStringLiteral("save"), Theme::Text, 14)));
    presetsBtn_->setToolTip(tr("Save, load, rename, import or export rack presets."));
    presetsBtn_->setCursor(Qt::PointingHandCursor);
    presetsBtn_->setFixedSize(kChromeBtnPx, kChromeBtnPx);
    presetsMenu_ = new QMenu(presetsBtn_);
    connect(presetsBtn_, &QPushButton::clicked, this, [this] {
        presetsMenu_->exec(presetsBtn_->mapToGlobal(QPoint(0, presetsBtn_->height())));
    });
    titleLay->addWidget(presetsBtn_);

    addBtn_ = new QPushButton(titleBar);
    addBtn_->setIcon(QIcon(Theme::iconPixmap(QStringLiteral("plus"), Theme::Text, 14)));
    addBtn_->setToolTip(tr("Add an effect to the rack."));
    addBtn_->setCursor(Qt::PointingHandCursor);
    addBtn_->setFixedSize(kChromeBtnPx, kChromeBtnPx);
    // A plain popup rather than QPushButton::setMenu(): setMenu() paints its
    // own drop-down chevron over the icon, which is not wanted on a button
    // that is meant to read as a plain "+".
    addMenu_ = new QMenu(addBtn_);
    connect(addBtn_, &QPushButton::clicked, this, [this] {
        addMenu_->exec(addBtn_->mapToGlobal(QPoint(0, addBtn_->height())));
    });
    titleLay->addWidget(addBtn_);

    outer->addWidget(titleBar);

    auto *content = new QWidget(this);
    auto *contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(12, 12, 12, 12);
    contentLay->setSpacing(10);

    // Modules go straight into the content layout now, no QScrollArea: the
    // window itself grows and shrinks to fit them (see the SetFixedSize
    // constraint below), so there is never overflow left for a scrollbar to
    // handle.
    modulesContainer_ = content;
    modulesLay_ = contentLay;

    outer->addWidget(content);

    // Ties the whole window's size to modulesLay_'s actual content instead
    // of a size the user (or a resize event) could leave mismatched with
    // it: every time a module is added or removed, Qt re-activates this
    // layout and resizes the window to match, growing or shrinking on its
    // own with no manual resize()/adjustSize() calls needed anywhere else.
    outer->setSizeConstraint(QLayout::SetFixedSize);

    connect(client_, &MixerClient::changed, this, &VirtualRackWindow::refresh);

    lastPresetDir_ = QDir::homePath();
    rebuildPresetsMenu();
}

void VirtualRackWindow::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    // Opening the Rack is what "drive this device from the Rack" means now
    // -- no separate switch to leave on by accident after the window closes.
    if (client_->available()) client_->setMasterRackMode(masterId_, true);
    refresh();
}

void VirtualRackWindow::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    if (client_->available()) client_->setMasterRackMode(masterId_, false);
}

int VirtualRackWindow::visualIndexFor(int stageIndex) const {
    int pos = 0;
    for (int i = 0; i < waveline::kFxStageCount; ++i) {
        if (current_.order[i] == stageIndex) {
            pos = i;
            break;
        }
    }
    int visual = 0;
    for (int i = 0; i < pos; ++i) {
        if (modules_.contains(current_.order[i])) ++visual;
    }
    return visual;
}

RackModule *VirtualRackWindow::addModuleWidget(int stageIndex) {
    RackModule *mod = createModuleFor(stageIndex, modulesContainer_);
    if (!mod) return nullptr;
    mod->loadFrom(current_);
    connect(mod, &RackModule::changed, this, &VirtualRackWindow::onModuleChanged);
    connect(mod, &RackModule::removeRequested, this, &VirtualRackWindow::onRemoveRequested);
    mod->grip()->installEventFilter(this);
    modulesLay_->insertWidget(visualIndexFor(stageIndex), mod);
    modules_.insert(stageIndex, mod);
    return mod;
}

void VirtualRackWindow::refresh() {
    if (!client_->available() || !isVisible()) return;
    updating_ = true;
    current_ = client_->masterRackCreativeFx(masterId_);
    if (current_.presentCount >= 0) {
        // The front presentCount entries of order are exactly which stages
        // this rack had a module for, in sequence -- restores a module even
        // if it's sitting powered off, which stageEnabled() alone can't
        // tell apart from "never added."
        const int count =
            std::min(current_.presentCount, static_cast<int>(waveline::kFxStageCount));
        for (int i = 0; i < count; ++i) {
            const int idx = current_.order[i];
            if (!modules_.contains(idx)) addModuleWidget(idx);
        }
    } else {
        // A spec saved before presence was tracked separately from on/off
        // state -- the only fallback available is what this window always
        // did before that existed.
        for (int idx = 0; idx < waveline::kFxStageCount; ++idx) {
            if (stageEnabled(current_, idx) && !modules_.contains(idx)) addModuleWidget(idx);
        }
    }
    for (RackModule *m : std::as_const(modules_)) m->loadFrom(current_);
    rebuildAddMenu();
    updating_ = false;
}

void VirtualRackWindow::onAddRequested(int stageIndex) {
    if (modules_.contains(stageIndex)) return;
    addModuleWidget(stageIndex);
    rebuildAddMenu();
    syncFromModulesAndPush();
}

void VirtualRackWindow::onRemoveRequested(int stageIndex) {
    RackModule *m = modules_.value(stageIndex);
    if (!m) return;
    modules_.remove(stageIndex);
    m->grip()->removeEventFilter(this);
    modulesLay_->removeWidget(m);
    m->hide();
    m->deleteLater();
    setStageEnabled(current_, stageIndex, false);
    // removeWidget() and deleteLater() both only take effect on the next
    // pass through the event loop -- left alone, Qt paints one frame with
    // the remaining modules not yet shifted up and the window not yet
    // shrunk, then snaps to the correct layout a frame later. That
    // gap-then-snap is the flicker. Forcing both layouts to activate here,
    // synchronously, collapses it into the single final frame instead.
    modulesLay_->activate();
    layout()->activate();
    rebuildAddMenu();
    syncFromModulesAndPush();
}

void VirtualRackWindow::onModuleChanged() {
    if (updating_) return;
    syncFromModulesAndPush();
}

void VirtualRackWindow::clearAllModules() {
    for (RackModule *m : std::as_const(modules_)) {
        m->grip()->removeEventFilter(this);
        modulesLay_->removeWidget(m);
        m->hide();
        m->deleteLater();
    }
    modules_.clear();
    modulesLay_->activate();
    layout()->activate();
}

void VirtualRackWindow::syncFromModulesAndPush() {
    if (updating_ || !client_->available()) return;
    std::array<int, waveline::kFxStageCount> order{};
    bool present[waveline::kFxStageCount] = {};
    int k = 0;
    for (int i = 0; i < modulesLay_->count(); ++i) {
        QLayoutItem *item = modulesLay_->itemAt(i);
        auto *m = item ? qobject_cast<RackModule *>(item->widget()) : nullptr;
        if (!m) continue;
        m->applyTo(current_);
        order[k++] = m->stageIndex();
        present[m->stageIndex()] = true;
    }
    // Every module actually in the rack layout comes first in order, so k
    // here is exactly the boundary refresh() needs on the next app start to
    // tell "racked, however it's powered" apart from "never added."
    current_.presentCount = k;
    for (int idx = 0; idx < waveline::kFxStageCount && k < waveline::kFxStageCount; ++idx) {
        if (!present[idx]) order[k++] = idx;
    }
    current_.order = order;
    updating_ = true;
    client_->setMasterRackCreativeFx(masterId_, current_);
    updating_ = false;
}

void VirtualRackWindow::rebuildAddMenu() {
    int mask = 0;
    for (int idx : modules_.keys()) mask |= (1 << idx);
    if (mask == presentSignature_) return;
    presentSignature_ = mask;

    addMenu_->clear();
    bool any = false;
    for (int idx = 0; idx < waveline::kFxStageCount; ++idx) {
        if (modules_.contains(idx)) continue;
        any = true;
        QAction *act = addMenu_->addAction(stageTitle(idx));
        connect(act, &QAction::triggered, this, [this, idx] { onAddRequested(idx); });
    }
    addBtn_->setEnabled(any);
}

bool VirtualRackWindow::eventFilter(QObject *watched, QEvent *event) {
    auto *grip = qobject_cast<QWidget *>(watched);
    if (!grip) return QWidget::eventFilter(watched, event);

    RackModule *owner = nullptr;
    for (RackModule *m : std::as_const(modules_)) {
        if (m->grip() == grip) {
            owner = m;
            break;
        }
    }
    if (!owner) return QWidget::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            dragModule_ = owner;
            return true;
        }
    } else if (event->type() == QEvent::MouseMove && dragModule_ == owner) {
        auto *me = static_cast<QMouseEvent *>(event);
        const QPoint localPos =
            modulesContainer_->mapFromGlobal(me->globalPosition().toPoint());
        QWidget *under = modulesContainer_->childAt(localPos);
        while (under && !qobject_cast<RackModule *>(under)) under = under->parentWidget();
        auto *target = qobject_cast<RackModule *>(under);
        if (target && target != dragModule_) {
            const int from = modulesLay_->indexOf(dragModule_);
            const int to = modulesLay_->indexOf(target);
            if (from >= 0 && to >= 0 && from != to) {
                modulesLay_->removeWidget(dragModule_);
                modulesLay_->insertWidget(to, dragModule_);
            }
        }
        return true;
    } else if (event->type() == QEvent::MouseButtonRelease && dragModule_ == owner) {
        dragModule_ = nullptr;
        syncFromModulesAndPush();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

// -------------------------------------------------------------------- presets

void VirtualRackWindow::rebuildPresetsMenu() {
    presetsMenu_->clear();

    QAction *saveAct = presetsMenu_->addAction(tr("Save As New Preset..."));
    connect(saveAct, &QAction::triggered, this, &VirtualRackWindow::onSavePresetAs);

    const QStringList names = RackPresetStore::instance().names();
    if (!names.isEmpty()) {
        presetsMenu_->addSeparator();
        // One submenu per preset rather than a flat "Load" list plus a
        // separate manage dialog: Load/Rename/Export/Delete all need "which
        // preset", and a submenu answers that once instead of asking twice.
        for (const QString &name : names) {
            QMenu *sub = presetsMenu_->addMenu(name);
            QAction *loadAct = sub->addAction(tr("Load"));
            connect(loadAct, &QAction::triggered, this, [this, name] { onLoadPreset(name); });
            QAction *renameAct = sub->addAction(tr("Rename..."));
            connect(renameAct, &QAction::triggered, this, [this, name] { onRenamePreset(name); });
            QAction *exportAct = sub->addAction(tr("Export to File..."));
            connect(exportAct, &QAction::triggered, this, [this, name] { onExportPreset(name); });
            sub->addSeparator();
            QAction *deleteAct = sub->addAction(tr("Delete"));
            connect(deleteAct, &QAction::triggered, this, [this, name] { onDeletePreset(name); });
        }
    }

    presetsMenu_->addSeparator();
    QAction *importAct = presetsMenu_->addAction(tr("Import from File..."));
    connect(importAct, &QAction::triggered, this, &VirtualRackWindow::onImportPresetFromFile);
}

void VirtualRackWindow::onSavePresetAs() {
    const QString name =
        askForPresetName(tr("Save rack preset"), tr("Name for this preset:"), QString());
    if (name.isEmpty()) return;
    RackPresetStore::instance().save(name, encodeCreativeFxSpec(current_));
    rebuildPresetsMenu();
}

void VirtualRackWindow::onLoadPreset(const QString &name) {
    if (!client_->available()) return;
    const QString spec = RackPresetStore::instance().spec(name);
    if (spec.isEmpty()) return;
    // The preset's own effect set can differ arbitrarily from what's
    // currently racked, so every existing module is torn down first rather
    // than reused -- refresh() below only ever adds modules for stages it
    // finds present, it never removes ones the new state dropped.
    clearAllModules();
    presentSignature_ = -1;
    client_->setMasterRackCreativeFx(masterId_, decodeCreativeFxSpec(spec));
    // Reads are ordered behind our own writes on this connection (see
    // MixerClient's own comment on this), so refresh() here is guaranteed to
    // read back the state just pushed above, not a stale poll.
    refresh();
}

void VirtualRackWindow::onRenamePreset(const QString &name) {
    const QString wanted =
        askForPresetName(tr("Rename preset"), tr("New name:"), name, name);
    if (wanted.isEmpty() || wanted == name) return;
    if (!RackPresetStore::instance().rename(name, wanted)) {
        QMessageBox::warning(this, tr("Rename preset"),
                             tr("Could not rename “%1”.").arg(name));
        return;
    }
    rebuildPresetsMenu();
}

void VirtualRackWindow::onDeletePreset(const QString &name) {
    if (QMessageBox::question(this, tr("Delete preset"),
                              tr("Delete the preset “%1”?").arg(name)) !=
        QMessageBox::Yes)
        return;
    RackPresetStore::instance().remove(name);
    rebuildPresetsMenu();
}

void VirtualRackWindow::onExportPreset(const QString &name) {
    const QString spec = RackPresetStore::instance().spec(name);
    if (spec.isEmpty()) return;

    QJsonObject root;
    root[QStringLiteral("waveline_rack_preset")] = true;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("name")] = name;
    root[QStringLiteral("exported")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    root[QStringLiteral("spec")] = spec;
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);

    QString path = QFileDialog::getSaveFileName(
        this, tr("Export rack preset"),
        QDir(lastPresetDir_).filePath(fileStemForPreset(name) + QLatin1String(kPresetSuffix)),
        tr("Waveline rack preset (*.wlrackpreset);;JSON (*.json);;All files (*)"));
    if (path.isEmpty()) return;
    // Only when the user typed no extension at all: someone who asked for
    // "backup.json" meant it.
    if (QFileInfo(path).suffix().isEmpty()) path += QLatin1String(kPresetSuffix);
    lastPresetDir_ = QFileInfo(path).absolutePath();

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Export rack preset"),
                             tr("Could not write %1:\n%2").arg(path, f.errorString()));
        return;
    }
    f.write(json);
}

void VirtualRackWindow::onImportPresetFromFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import rack preset"), lastPresetDir_,
        tr("Waveline rack presets (*.wlrackpreset *.json);;All files (*)"));
    if (path.isEmpty()) return;
    lastPresetDir_ = QFileInfo(path).absolutePath();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import rack preset"),
                             tr("Could not read %1:\n%2").arg(path, f.errorString()));
        return;
    }
    const QByteArray blob = f.readAll();
    f.close();

    QString spec;
    QString suggested;
    const QJsonDocument doc = QJsonDocument::fromJson(blob);
    if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        spec = obj.value(QStringLiteral("spec")).toString();
        suggested = obj.value(QStringLiteral("name")).toString().trimmed();
    }
    // A bare spec string with no JSON envelope at all -- accepted as-is
    // rather than insisting on the wrapper, the same "be liberal in what you
    // accept" ProfilesWindow's importFromFile()/the daemon's ImportProfile
    // already apply to hand-edited or recovered files.
    if (spec.isEmpty()) spec = QString::fromUtf8(blob).trimmed();
    if (spec.isEmpty()) {
        QMessageBox::warning(
            this, tr("Import rack preset"),
            tr("%1 is not a Waveline rack preset.").arg(QFileInfo(path).fileName()));
        return;
    }
    if (suggested.isEmpty()) suggested = QFileInfo(path).completeBaseName();

    QString name = suggested;
    if (RackPresetStore::instance().contains(name)) {
        QMessageBox box(this);
        box.setWindowTitle(tr("Import rack preset"));
        box.setIcon(QMessageBox::Question);
        box.setText(tr("There is already a preset called “%1”.").arg(name));
        box.setInformativeText(tr("Replace it with the one in this file, or keep both?"));
        QPushButton *replace = box.addButton(tr("Replace"), QMessageBox::DestructiveRole);
        QPushButton *both = box.addButton(tr("Keep both"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(both);
        box.exec();
        if (box.clickedButton() == both) name = RackPresetStore::instance().freeName(name);
        else if (box.clickedButton() != replace) return;
    }

    RackPresetStore::instance().save(name, spec);
    rebuildPresetsMenu();
}

QString VirtualRackWindow::askForPresetName(const QString &title, const QString &label,
                                            const QString &initial, const QString &allowed) {
    QString seed = initial;
    for (;;) {
        bool ok = false;
        const QString typed =
            QInputDialog::getText(this, title, label, QLineEdit::Normal, seed, &ok).trimmed();
        if (!ok) return {};
        if (typed.isEmpty()) {
            QMessageBox::warning(this, title, tr("A preset needs a name."));
            seed = initial;
            continue;
        }
        // Asked again rather than refused outright: the answer to a clash is
        // almost always "then call it something else", and dropping the
        // user back to the menu to start over is a worse way to say so.
        if (typed != allowed && RackPresetStore::instance().contains(typed)) {
            if (QMessageBox::question(
                    this, title,
                    tr("There is already a preset called “%1”. Replace it?")
                        .arg(typed)) == QMessageBox::Yes)
                return typed;
            seed = typed;
            continue;
        }
        return typed;
    }
}

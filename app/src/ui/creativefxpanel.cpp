// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "creativefxpanel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "widgets.h"

namespace {
int scaled(double v, double factor) { return int(std::lround(v * factor)); }
}  // namespace

CreativeFxPanel::CreativeFxPanel(QWidget *parent) : QWidget(parent) {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(8);

    tabs_ = new QTabWidget(this);
    tabs_->addTab(buildBitcrusherTab(), tr("Bitcrusher"));
    tabs_->addTab(buildOverdriveTab(), tr("Overdrive"));
    tabs_->addTab(buildChorusTab(), tr("Chorus"));
    tabs_->addTab(buildFlangerTab(), tr("Flanger"));
    tabs_->addTab(buildPhaserTab(), tr("Phaser"));
    tabs_->addTab(buildTremoloTab(), tr("Tremolo"));
    tabs_->addTab(buildDelayTab(), tr("Delay"));
    tabs_->addTab(buildReverbTab(), tr("Reverb"));
    tabs_->addTab(buildEqTab(), tr("EQ"));
    tabs_->addTab(buildRingModTab(), tr("Ring Mod"));
    tabs_->addTab(buildEnvFilterTab(), tr("Envelope Filter"));
    tabs_->addTab(buildPitchTab(), tr("Pitch"));
    tabs_->addTab(buildReverseDelayTab(), tr("Reverse Delay"));
    tabs_->addTab(buildTapeSatTab(), tr("Tape Sat"));
    outer->addWidget(tabs_, 1);
}

void CreativeFxPanel::emitChanged() {
    if (!updating_) emit changed();
}

void CreativeFxPanel::addResetRow(QVBoxLayout *pageLay, const std::function<void()> &onDefault) {
    auto *row = new QWidget(this);
    auto *rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(0, 0, 0, 0);
    rowLay->addStretch();
    auto *btn = new QPushButton(tr("Default"), row);
    btn->setToolTip(tr("Reset this effect to factory defaults."));
    btn->setFixedHeight(24);
    connect(btn, &QPushButton::clicked, this, [onDefault] { onDefault(); });
    rowLay->addWidget(btn);
    pageLay->addWidget(row);
}

CreativeFxPanel::EffectRowUi CreativeFxPanel::addEffectHeader(QVBoxLayout *pageLay,
                                                              const QString &title,
                                                              const QString &tip) {
    EffectRowUi ui;
    auto *row = new QWidget(this);
    auto *rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(0, 0, 0, 0);
    rowLay->setSpacing(8);
    auto *name = new QLabel(title, row);
    QFont f = name->font();
    f.setBold(true);
    name->setFont(f);
    name->setToolTip(tip);
    rowLay->addWidget(name, 1);
    ui.enabled = new ToggleSwitch(row);
    ui.enabled->setToolTip(tip);
    rowLay->addWidget(ui.enabled);
    pageLay->addWidget(row);

    ui.body = new QWidget(this);
    auto *bodyLay = new QVBoxLayout(ui.body);
    bodyLay->setContentsMargins(0, 4, 0, 0);
    bodyLay->setSpacing(8);
    pageLay->addWidget(ui.body);

    QWidget *body = ui.body;
    connect(ui.enabled, &QAbstractButton::toggled, this, [this, body](bool on) {
        body->setEnabled(on);
        emitChanged();
    });
    return ui;
}

CreativeFxPanel::SliderUi CreativeFxPanel::addSliderRow(QVBoxLayout *lay, const QString &label,
                                                        int min, int max,
                                                        const std::function<QString(int)> &format) {
    SliderUi ui;
    auto *row = new QWidget(this);
    auto *rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(0, 0, 0, 0);
    rowLay->setSpacing(8);
    auto *name = new QLabel(label, row);
    name->setFixedWidth(64);
    rowLay->addWidget(name);
    ui.slider = new TrackSlider(row);
    ui.slider->setRange(min, max);
    rowLay->addWidget(ui.slider, 1);
    ui.readout = new QLabel(row);
    ui.readout->setFixedWidth(64);
    ui.readout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    rowLay->addWidget(ui.readout);
    lay->addWidget(row);

    QLabel *readout = ui.readout;
    connect(ui.slider, &QAbstractSlider::valueChanged, this, [this, readout, format](int v) {
        readout->setText(format(v));
        emitChanged();
    });
    ui.readout->setText(format(ui.slider->value()));
    return ui;
}

// -------------------------------------------------------------- Bitcrusher

QWidget *CreativeFxPanel::buildBitcrusherTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.bitcrusher = BitcrusherInfo{};
        setValues(v);
        emit changed();
    });

    bitOn_ = addEffectHeader(
        lay, tr("Bitcrusher"),
        tr("Bit-depth and sample-rate reduction for gritty, lo-fi textures."));
    auto *body = static_cast<QVBoxLayout *>(bitOn_.body->layout());

    bitDepth_ = addSliderRow(body, tr("Depth"), 1, 16,
                             [](int v) { return QStringLiteral("%1 bit").arg(v); });
    bitDepth_.slider->setToolTip(tr("Quantization depth. Lower is gritter."));

    bitSrReduction_ = addSliderRow(body, tr("Crush"), 0, 100,
                                   [](int v) { return QStringLiteral("%1%").arg(v); });
    bitSrReduction_.slider->setToolTip(
        tr("Sample-rate decimation. Higher sounds more broken up."));

    bitMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                           [](int v) { return QStringLiteral("%1%").arg(v); });
    bitMix_.slider->setToolTip(tr("Dry/wet blend."));

    lay->addStretch(1);
    return page;
}

// --------------------------------------------------------------- Overdrive

QWidget *CreativeFxPanel::buildOverdriveTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.overdrive = OverdriveInfo{};
        setValues(v);
        emit changed();
    });

    odOn_ = addEffectHeader(lay, tr("Overdrive"),
                            tr("Warm saturation and harmonic crunch."));
    auto *body = static_cast<QVBoxLayout *>(odOn_.body->layout());

    odDrive_ = addSliderRow(body, tr("Drive"), 0, 100,
                            [](int v) { return QStringLiteral("%1%").arg(v); });
    odDrive_.slider->setToolTip(tr("How hard the signal is pushed into saturation."));

    odTone_ = addSliderRow(body, tr("Tone"), 0, 100, [](int v) {
        if (v == 50) return QStringLiteral("flat");
        return v < 50 ? tr("dark %1").arg(50 - v) : tr("bright %1").arg(v - 50);
    });
    odTone_.slider->setToolTip(tr("Tilts the saturated tone dark or bright. 50 is neutral."));

    odOutput_ = addSliderRow(body, tr("Output"), -240, 240,
                             [](int v) { return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1); });
    odOutput_.slider->setToolTip(tr("Makeup gain after the tone stage."));

    odMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                          [](int v) { return QStringLiteral("%1%").arg(v); });
    odMix_.slider->setToolTip(tr("Dry/wet blend."));

    lay->addStretch(1);
    return page;
}

// ------------------------------------------------------------------ Chorus

QWidget *CreativeFxPanel::buildChorusTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.chorus = ChorusInfo{};
        setValues(v);
        emit changed();
    });

    chorusOn_ =
        addEffectHeader(lay, tr("Chorus"), tr("Modulated thickening, for both voice and guitar."));
    auto *body = static_cast<QVBoxLayout *>(chorusOn_.body->layout());

    chorusRate_ = addSliderRow(body, tr("Rate"), 2, 500, [](int v) {
        return QStringLiteral("%1 Hz").arg(v / 100.0, 0, 'f', 2);
    });
    chorusDepth_ = addSliderRow(body, tr("Depth"), 5, 80, [](int v) {
        return QStringLiteral("%1 ms").arg(v / 10.0, 0, 'f', 1);
    });
    chorusFeedback_ = addSliderRow(body, tr("Feedback"), 0, 90,
                                   [](int v) { return QStringLiteral("%1%").arg(v); });
    chorusMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                              [](int v) { return QStringLiteral("%1%").arg(v); });

    lay->addStretch(1);
    return page;
}

// ----------------------------------------------------------------- Flanger

QWidget *CreativeFxPanel::buildFlangerTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.flanger = FlangerInfo{};
        setValues(v);
        emit changed();
    });

    flangerOn_ =
        addEffectHeader(lay, tr("Flanger"), tr("Short modulated delay with feedback -- the jet sweep."));
    auto *body = static_cast<QVBoxLayout *>(flangerOn_.body->layout());

    flangerRate_ = addSliderRow(body, tr("Rate"), 2, 200, [](int v) {
        return QStringLiteral("%1 Hz").arg(v / 100.0, 0, 'f', 2);
    });
    flangerDepth_ = addSliderRow(body, tr("Depth"), 1, 30, [](int v) {
        return QStringLiteral("%1 ms").arg(v / 10.0, 0, 'f', 1);
    });
    flangerFeedback_ = addSliderRow(body, tr("Feedback"), 0, 95,
                                    [](int v) { return QStringLiteral("%1%").arg(v); });
    flangerMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                               [](int v) { return QStringLiteral("%1%").arg(v); });

    lay->addStretch(1);
    return page;
}

// ------------------------------------------------------------------ Phaser

QWidget *CreativeFxPanel::buildPhaserTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.phaser = PhaserInfo{};
        setValues(v);
        emit changed();
    });

    phaserOn_ =
        addEffectHeader(lay, tr("Phaser"), tr("Swept all-pass filtering -- a swirling, watery motion."));
    auto *body = static_cast<QVBoxLayout *>(phaserOn_.body->layout());

    phaserRate_ = addSliderRow(body, tr("Rate"), 2, 500, [](int v) {
        return QStringLiteral("%1 Hz").arg(v / 100.0, 0, 'f', 2);
    });
    phaserDepth_ = addSliderRow(body, tr("Depth"), 0, 100,
                                [](int v) { return QStringLiteral("%1%").arg(v); });
    phaserFeedback_ = addSliderRow(body, tr("Resonance"), 0, 95,
                                   [](int v) { return QStringLiteral("%1%").arg(v); });
    phaserMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                              [](int v) { return QStringLiteral("%1%").arg(v); });

    lay->addStretch(1);
    return page;
}

// ----------------------------------------------------------------- Tremolo

QWidget *CreativeFxPanel::buildTremoloTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.tremolo = TremoloInfo{};
        setValues(v);
        emit changed();
    });

    tremOn_ = addEffectHeader(lay, tr("Tremolo"), tr("Rhythmic volume pulsing."));
    auto *body = static_cast<QVBoxLayout *>(tremOn_.body->layout());

    tremRate_ = addSliderRow(body, tr("Rate"), 1, 200, [](int v) {
        return QStringLiteral("%1 Hz").arg(v / 10.0, 0, 'f', 1);
    });
    tremDepth_ = addSliderRow(body, tr("Depth"), 0, 100,
                              [](int v) { return QStringLiteral("%1%").arg(v); });

    auto *shapeRow = new QWidget(this);
    auto *shapeLay = new QHBoxLayout(shapeRow);
    shapeLay->setContentsMargins(0, 0, 0, 0);
    shapeLay->setSpacing(8);
    auto *shapeLabel = new QLabel(tr("Shape"), shapeRow);
    shapeLabel->setFixedWidth(64);
    shapeLay->addWidget(shapeLabel);
    tremShape_ = new QComboBox(shapeRow);
    tremShape_->addItem(tr("Sine"));
    tremShape_->addItem(tr("Triangle"));
    tremShape_->addItem(tr("Soft square"));
    shapeLay->addWidget(tremShape_, 1);
    body->addWidget(shapeRow);
    connect(tremShape_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
           [this](int) { emitChanged(); });

    tremMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                            [](int v) { return QStringLiteral("%1%").arg(v); });

    lay->addStretch(1);
    return page;
}

// ------------------------------------------------------------------- Delay

QWidget *CreativeFxPanel::buildDelayTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.delay = DelayFxInfo{};
        setValues(v);
        emit changed();
    });

    delayOn_ = addEffectHeader(lay, tr("Delay"), tr("Echo with feedback."));
    auto *body = static_cast<QVBoxLayout *>(delayOn_.body->layout());

    delayTime_ = addSliderRow(body, tr("Time"), 10, 2000,
                              [](int v) { return QStringLiteral("%1 ms").arg(v); });
    delayFeedback_ = addSliderRow(body, tr("Feedback"), 0, 95,
                                  [](int v) { return QStringLiteral("%1%").arg(v); });
    delayMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                             [](int v) { return QStringLiteral("%1%").arg(v); });
    delayDamping_ = addSliderRow(body, tr("Damping"), 0, 100,
                                 [](int v) { return QStringLiteral("%1%").arg(v); });
    delayDamping_.slider->setToolTip(
        tr("Darkens the repeats over time, like a worn tape head."));

    pingPongRow_ = new QWidget(this);
    auto *ppLay = new QHBoxLayout(pingPongRow_);
    ppLay->setContentsMargins(0, 0, 0, 0);
    ppLay->setSpacing(8);
    auto *ppLabel = new QLabel(tr("Ping-Pong"), pingPongRow_);
    ppLabel->setToolTip(
        tr("Bounces the echo between left and right instead of repeating in place. "
           "Needs a real stereo pair."));
    ppLay->addWidget(ppLabel, 1);
    delayPingPong_ = new ToggleSwitch(pingPongRow_);
    delayPingPong_->setToolTip(ppLabel->toolTip());
    ppLay->addWidget(delayPingPong_);
    body->addWidget(pingPongRow_);
    connect(delayPingPong_, &QAbstractButton::toggled, this, [this](bool) { emitChanged(); });

    lay->addStretch(1);
    return page;
}

// ------------------------------------------------------------------ Reverb

QWidget *CreativeFxPanel::buildReverbTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.reverb = ReverbInfo{};
        setValues(v);
        emit changed();
    });

    reverbOn_ =
        addEffectHeader(lay, tr("Reverb"), tr("Room ambience after EQ and dynamics."));
    auto *body = static_cast<QVBoxLayout *>(reverbOn_.body->layout());

    reverbSize_ = addSliderRow(body, tr("Size"), 0, 100,
                               [](int v) { return QStringLiteral("%1%").arg(v); });
    reverbSize_.slider->setToolTip(tr("How long the reverb tail lasts."));

    reverbDamping_ = addSliderRow(body, tr("Damping"), 0, 100,
                                  [](int v) { return QStringLiteral("%1%").arg(v); });
    reverbDamping_.slider->setToolTip(tr("Rolls off high frequencies as the tail decays."));

    reverbPredelay_ = addSliderRow(body, tr("Predelay"), 0, 100,
                                   [](int v) { return QStringLiteral("%1 ms").arg(v); });
    reverbPredelay_.slider->setToolTip(
        tr("Silence before the reverb tail starts, separating it from the dry sound."));

    reverbMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                              [](int v) { return QStringLiteral("%1%").arg(v); });

    lay->addStretch(1);
    return page;
}

// ---------------------------------------------------------------------- EQ

QWidget *CreativeFxPanel::buildEqTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.eq = CreativeEqInfo{};
        setValues(v);
        emit changed();
    });

    eqOn_ = addEffectHeader(
        lay, tr("EQ"),
        tr("Amp-style tone stack. Bypasses the standard EQ in the Processing tab "
           "while active, so the signal isn't equalized twice."));
    auto *body = static_cast<QVBoxLayout *>(eqOn_.body->layout());

    eqGain_ = addSliderRow(body, tr("Gain"), -240, 240,
                           [](int v) { return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1); });
    eqGain_.slider->setToolTip(tr("Input trim before the tone stack."));

    eqBass_ = addSliderRow(body, tr("Bass"), -120, 120,
                           [](int v) { return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1); });
    eqMid_ = addSliderRow(body, tr("Middle"), -120, 120,
                          [](int v) { return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1); });
    eqTreble_ = addSliderRow(
        body, tr("Treble"), -120, 120,
        [](int v) { return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1); });
    eqPresence_ = addSliderRow(
        body, tr("Presence"), -120, 120,
        [](int v) { return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1); });

    eqMaster_ = addSliderRow(
        body, tr("Master"), -240, 240,
        [](int v) { return QStringLiteral("%1 dB").arg(v / 10.0, 0, 'f', 1); });
    eqMaster_.slider->setToolTip(tr("Output trim after the tone stack."));

    lay->addStretch(1);
    return page;
}

// ------------------------------------------------------------- Ring Mod

QWidget *CreativeFxPanel::buildRingModTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.ringMod = RingModulatorInfo{};
        setValues(v);
        emit changed();
    });

    ringModOn_ = addEffectHeader(
        lay, tr("Ring Modulator"),
        tr("Multiplies the signal by an internal sine wave for metallic, robotic, "
           "bell-like tones."));
    auto *body = static_cast<QVBoxLayout *>(ringModOn_.body->layout());

    ringModFreq_ = addSliderRow(body, tr("Frequency"), 10, 2000,
                                [](int v) { return QStringLiteral("%1 Hz").arg(v); });
    ringModFineTune_ = addSliderRow(body, tr("Fine Tune"), -50, 50,
                                    [](int v) { return QStringLiteral("%1 Hz").arg(v); });
    ringModMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                               [](int v) { return QStringLiteral("%1%").arg(v); });

    lay->addStretch(1);
    return page;
}

// --------------------------------------------------------- Envelope Filter

QWidget *CreativeFxPanel::buildEnvFilterTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.envFilter = EnvelopeFilterInfo{};
        setValues(v);
        emit changed();
    });

    envFilterOn_ = addEffectHeader(
        lay, tr("Envelope Filter"),
        tr("Auto-wah: a peak follower sweeps a resonant bandpass filter with how "
           "hard you play."));
    auto *body = static_cast<QVBoxLayout *>(envFilterOn_.body->layout());

    envFilterSens_ = addSliderRow(body, tr("Sensitivity"), 0, 100,
                                  [](int v) { return QStringLiteral("%1%").arg(v); });
    envFilterAttack_ = addSliderRow(body, tr("Attack"), 1, 100,
                                    [](int v) { return QStringLiteral("%1 ms").arg(v); });
    envFilterRelease_ = addSliderRow(body, tr("Release"), 10, 500,
                                     [](int v) { return QStringLiteral("%1 ms").arg(v); });
    envFilterResonance_ = addSliderRow(body, tr("Resonance"), 0, 95,
                                       [](int v) { return QStringLiteral("%1%").arg(v); });
    envFilterMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                                 [](int v) { return QStringLiteral("%1%").arg(v); });

    lay->addStretch(1);
    return page;
}

// ------------------------------------------------------------------- Pitch

QWidget *CreativeFxPanel::buildPitchTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.pitch = PitchShifterInfo{};
        setValues(v);
        emit changed();
    });

    pitchOn_ = addEffectHeader(
        lay, tr("Pitch Shifter"),
        tr("Time-domain grain-delay pitch shift, for octave lines or wide detune."));
    auto *body = static_cast<QVBoxLayout *>(pitchOn_.body->layout());

    pitchSemitones_ = addSliderRow(body, tr("Pitch"), -12, 12,
                                   [](int v) { return QStringLiteral("%1 st").arg(v); });
    pitchDetune_ = addSliderRow(body, tr("Detune"), -50, 50,
                                [](int v) { return QStringLiteral("%1 ct").arg(v); });
    pitchMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                             [](int v) { return QStringLiteral("%1%").arg(v); });

    lay->addStretch(1);
    return page;
}

// ---------------------------------------------------------- Reverse Delay

QWidget *CreativeFxPanel::buildReverseDelayTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.reverseDelay = ReverseDelayInfo{};
        setValues(v);
        emit changed();
    });

    reverseDelayOn_ = addEffectHeader(
        lay, tr("Reverse Delay"),
        tr("Records into a rolling buffer and plays each block back reversed."));
    auto *body = static_cast<QVBoxLayout *>(reverseDelayOn_.body->layout());

    reverseDelayTime_ = addSliderRow(body, tr("Time"), 50, 2000,
                                     [](int v) { return QStringLiteral("%1 ms").arg(v); });
    reverseDelayFeedback_ = addSliderRow(body, tr("Feedback"), 0, 95,
                                         [](int v) { return QStringLiteral("%1%").arg(v); });
    reverseDelaySmoothing_ = addSliderRow(body, tr("Fader"), 0, 100,
                                          [](int v) { return QStringLiteral("%1%").arg(v); });
    reverseDelaySmoothing_.slider->setToolTip(
        tr("Crossfade width at each block edge, smoothing the splice."));
    reverseDelayMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                                    [](int v) { return QStringLiteral("%1%").arg(v); });

    lay->addStretch(1);
    return page;
}

// ------------------------------------------------------------- Tape Sat

QWidget *CreativeFxPanel::buildTapeSatTab() {
    auto *page = new QWidget(this);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    addResetRow(lay, [this] {
        CreativeFxInfo v = values();
        v.tapeSat = TapeSaturatorInfo{};
        setValues(v);
        emit changed();
    });

    tapeSatOn_ = addEffectHeader(
        lay, tr("Tape Saturator"),
        tr("Soft saturation, wow/flutter wobble and bandwidth narrowing for instant "
           "vintage tape or vinyl texture."));
    auto *body = static_cast<QVBoxLayout *>(tapeSatOn_.body->layout());

    tapeSatDrive_ = addSliderRow(body, tr("Drive"), 0, 100,
                                 [](int v) { return QStringLiteral("%1%").arg(v); });
    tapeSatFlutter_ = addSliderRow(body, tr("Flutter"), 0, 100,
                                   [](int v) { return QStringLiteral("%1%").arg(v); });
    tapeSatFlutter_.slider->setToolTip(tr("Wow/flutter wobble depth."));
    tapeSatAge_ = addSliderRow(body, tr("Age"), 0, 100,
                               [](int v) { return QStringLiteral("%1%").arg(v); });
    tapeSatAge_.slider->setToolTip(
        tr("Narrows the bandwidth from both ends, like a worn tape or vinyl."));
    tapeSatMix_ = addSliderRow(body, tr("Mix"), 0, 100,
                               [](int v) { return QStringLiteral("%1%").arg(v); });

    lay->addStretch(1);
    return page;
}

// -------------------------------------------------------------- values I/O

void CreativeFxPanel::setValues(const CreativeFxInfo &v) {
    updating_ = true;

    bitOn_.enabled->setChecked(v.bitcrusher.enabled);
    bitOn_.body->setEnabled(v.bitcrusher.enabled);
    bitDepth_.slider->setValue(int(std::lround(v.bitcrusher.bitDepth)));
    bitSrReduction_.slider->setValue(scaled(v.bitcrusher.sampleRateReduction, 100.0));
    bitMix_.slider->setValue(scaled(v.bitcrusher.mix, 100.0));

    odOn_.enabled->setChecked(v.overdrive.enabled);
    odOn_.body->setEnabled(v.overdrive.enabled);
    odDrive_.slider->setValue(scaled(v.overdrive.drive, 100.0));
    odTone_.slider->setValue(scaled(v.overdrive.tone, 100.0));
    odOutput_.slider->setValue(scaled(v.overdrive.outputDb, 10.0));
    odMix_.slider->setValue(scaled(v.overdrive.mix, 100.0));

    chorusOn_.enabled->setChecked(v.chorus.enabled);
    chorusOn_.body->setEnabled(v.chorus.enabled);
    chorusRate_.slider->setValue(scaled(v.chorus.rateHz, 100.0));
    chorusDepth_.slider->setValue(scaled(v.chorus.depthMs, 10.0));
    chorusFeedback_.slider->setValue(scaled(v.chorus.feedback, 100.0));
    chorusMix_.slider->setValue(scaled(v.chorus.mix, 100.0));

    flangerOn_.enabled->setChecked(v.flanger.enabled);
    flangerOn_.body->setEnabled(v.flanger.enabled);
    flangerRate_.slider->setValue(scaled(v.flanger.rateHz, 100.0));
    flangerDepth_.slider->setValue(scaled(v.flanger.depthMs, 10.0));
    flangerFeedback_.slider->setValue(scaled(v.flanger.feedback, 100.0));
    flangerMix_.slider->setValue(scaled(v.flanger.mix, 100.0));

    phaserOn_.enabled->setChecked(v.phaser.enabled);
    phaserOn_.body->setEnabled(v.phaser.enabled);
    phaserRate_.slider->setValue(scaled(v.phaser.rateHz, 100.0));
    phaserDepth_.slider->setValue(scaled(v.phaser.depth, 100.0));
    phaserFeedback_.slider->setValue(scaled(v.phaser.feedback, 100.0));
    phaserMix_.slider->setValue(scaled(v.phaser.mix, 100.0));

    tremOn_.enabled->setChecked(v.tremolo.enabled);
    tremOn_.body->setEnabled(v.tremolo.enabled);
    tremRate_.slider->setValue(scaled(v.tremolo.rateHz, 10.0));
    tremDepth_.slider->setValue(scaled(v.tremolo.depth, 100.0));
    if (tremShape_) tremShape_->setCurrentIndex(std::clamp(v.tremolo.shape, 0, 2));
    tremMix_.slider->setValue(scaled(v.tremolo.mix, 100.0));

    delayOn_.enabled->setChecked(v.delay.enabled);
    delayOn_.body->setEnabled(v.delay.enabled);
    delayTime_.slider->setValue(int(std::lround(v.delay.timeMs)));
    delayFeedback_.slider->setValue(scaled(v.delay.feedback, 100.0));
    delayMix_.slider->setValue(scaled(v.delay.mix, 100.0));
    delayDamping_.slider->setValue(scaled(v.delay.damping, 100.0));
    if (delayPingPong_) delayPingPong_->setChecked(v.delay.pingPong);

    reverbOn_.enabled->setChecked(v.reverb.enabled);
    reverbOn_.body->setEnabled(v.reverb.enabled);
    reverbSize_.slider->setValue(scaled(v.reverb.size, 100.0));
    reverbDamping_.slider->setValue(scaled(v.reverb.damping, 100.0));
    reverbPredelay_.slider->setValue(int(std::lround(v.reverb.predelayMs)));
    reverbMix_.slider->setValue(scaled(v.reverb.mix, 100.0));

    eqOn_.enabled->setChecked(v.eq.enabled);
    eqOn_.body->setEnabled(v.eq.enabled);
    eqGain_.slider->setValue(scaled(v.eq.gain, 10.0));
    eqBass_.slider->setValue(scaled(v.eq.bass, 10.0));
    eqMid_.slider->setValue(scaled(v.eq.mid, 10.0));
    eqTreble_.slider->setValue(scaled(v.eq.treble, 10.0));
    eqPresence_.slider->setValue(scaled(v.eq.presence, 10.0));
    eqMaster_.slider->setValue(scaled(v.eq.master, 10.0));

    ringModOn_.enabled->setChecked(v.ringMod.enabled);
    ringModOn_.body->setEnabled(v.ringMod.enabled);
    ringModFreq_.slider->setValue(int(std::lround(v.ringMod.frequencyHz)));
    ringModFineTune_.slider->setValue(int(std::lround(v.ringMod.fineTuneHz)));
    ringModMix_.slider->setValue(scaled(v.ringMod.mix, 100.0));

    envFilterOn_.enabled->setChecked(v.envFilter.enabled);
    envFilterOn_.body->setEnabled(v.envFilter.enabled);
    envFilterSens_.slider->setValue(scaled(v.envFilter.sensitivity, 100.0));
    envFilterAttack_.slider->setValue(int(std::lround(v.envFilter.attackMs)));
    envFilterRelease_.slider->setValue(int(std::lround(v.envFilter.releaseMs)));
    envFilterResonance_.slider->setValue(scaled(v.envFilter.resonance, 100.0));
    envFilterMix_.slider->setValue(scaled(v.envFilter.mix, 100.0));

    pitchOn_.enabled->setChecked(v.pitch.enabled);
    pitchOn_.body->setEnabled(v.pitch.enabled);
    pitchSemitones_.slider->setValue(int(std::lround(v.pitch.semitones)));
    pitchDetune_.slider->setValue(int(std::lround(v.pitch.detuneCents)));
    pitchMix_.slider->setValue(scaled(v.pitch.mix, 100.0));

    reverseDelayOn_.enabled->setChecked(v.reverseDelay.enabled);
    reverseDelayOn_.body->setEnabled(v.reverseDelay.enabled);
    reverseDelayTime_.slider->setValue(int(std::lround(v.reverseDelay.timeMs)));
    reverseDelayFeedback_.slider->setValue(scaled(v.reverseDelay.feedback, 100.0));
    reverseDelaySmoothing_.slider->setValue(scaled(v.reverseDelay.smoothing, 100.0));
    reverseDelayMix_.slider->setValue(scaled(v.reverseDelay.mix, 100.0));

    tapeSatOn_.enabled->setChecked(v.tapeSat.enabled);
    tapeSatOn_.body->setEnabled(v.tapeSat.enabled);
    tapeSatDrive_.slider->setValue(scaled(v.tapeSat.drive, 100.0));
    tapeSatFlutter_.slider->setValue(scaled(v.tapeSat.flutter, 100.0));
    tapeSatAge_.slider->setValue(scaled(v.tapeSat.age, 100.0));
    tapeSatMix_.slider->setValue(scaled(v.tapeSat.mix, 100.0));

    updating_ = false;
}

CreativeFxInfo CreativeFxPanel::values() const {
    CreativeFxInfo v;
    v.bitcrusher = {bitOn_.enabled->isChecked(), double(bitDepth_.slider->value()),
                    bitSrReduction_.slider->value() / 100.0, bitMix_.slider->value() / 100.0};
    v.overdrive = {odOn_.enabled->isChecked(), odDrive_.slider->value() / 100.0,
                   odTone_.slider->value() / 100.0, odOutput_.slider->value() / 10.0,
                   odMix_.slider->value() / 100.0};
    v.chorus = {chorusOn_.enabled->isChecked(), chorusRate_.slider->value() / 100.0,
               chorusDepth_.slider->value() / 10.0, chorusFeedback_.slider->value() / 100.0,
               chorusMix_.slider->value() / 100.0};
    v.flanger = {flangerOn_.enabled->isChecked(), flangerRate_.slider->value() / 100.0,
                flangerDepth_.slider->value() / 10.0, flangerFeedback_.slider->value() / 100.0,
                flangerMix_.slider->value() / 100.0};
    v.phaser = {phaserOn_.enabled->isChecked(), phaserRate_.slider->value() / 100.0,
               phaserDepth_.slider->value() / 100.0, phaserFeedback_.slider->value() / 100.0,
               phaserMix_.slider->value() / 100.0};
    v.tremolo = {tremOn_.enabled->isChecked(), tremRate_.slider->value() / 10.0,
                tremDepth_.slider->value() / 100.0, tremShape_ ? tremShape_->currentIndex() : 0,
                tremMix_.slider->value() / 100.0};
    v.delay = {delayOn_.enabled->isChecked(),
              double(delayTime_.slider->value()),
              delayFeedback_.slider->value() / 100.0,
              delayMix_.slider->value() / 100.0,
              delayDamping_.slider->value() / 100.0,
              delayPingPong_ && delayPingPong_->isChecked()};
    v.reverb = {reverbOn_.enabled->isChecked(), reverbSize_.slider->value() / 100.0,
               reverbDamping_.slider->value() / 100.0, double(reverbPredelay_.slider->value()),
               reverbMix_.slider->value() / 100.0};
    v.eq = {eqOn_.enabled->isChecked(),   eqGain_.slider->value() / 10.0,
           eqBass_.slider->value() / 10.0,     eqMid_.slider->value() / 10.0,
           eqTreble_.slider->value() / 10.0,   eqPresence_.slider->value() / 10.0,
           eqMaster_.slider->value() / 10.0};
    v.ringMod = {ringModOn_.enabled->isChecked(), double(ringModFreq_.slider->value()),
                double(ringModFineTune_.slider->value()), ringModMix_.slider->value() / 100.0};
    v.envFilter = {envFilterOn_.enabled->isChecked(),
                  envFilterSens_.slider->value() / 100.0,
                  double(envFilterAttack_.slider->value()),
                  double(envFilterRelease_.slider->value()),
                  envFilterResonance_.slider->value() / 100.0,
                  envFilterMix_.slider->value() / 100.0};
    v.pitch = {pitchOn_.enabled->isChecked(), double(pitchSemitones_.slider->value()),
              double(pitchDetune_.slider->value()), pitchMix_.slider->value() / 100.0};
    v.reverseDelay = {reverseDelayOn_.enabled->isChecked(),
                      double(reverseDelayTime_.slider->value()),
                      reverseDelayFeedback_.slider->value() / 100.0,
                      reverseDelaySmoothing_.slider->value() / 100.0,
                      reverseDelayMix_.slider->value() / 100.0};
    v.tapeSat = {tapeSatOn_.enabled->isChecked(), tapeSatDrive_.slider->value() / 100.0,
                tapeSatFlutter_.slider->value() / 100.0, tapeSatAge_.slider->value() / 100.0,
                tapeSatMix_.slider->value() / 100.0};
    return v;
}

void CreativeFxPanel::setStereoCapable(bool on) {
    stereoCapable_ = on;
    if (pingPongRow_) pingPongRow_->setVisible(on);
}

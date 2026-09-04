// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The Creative FX pedalboard: fourteen effects, one sub-tab each, full
// parameter sets on sliders. A self-contained widget rather than a free
// function like the old three-effect version, because it now owns real
// state (60-odd sliders, an encode/decode round trip) that four call sites
// would otherwise have to duplicate.

#pragma once

#include <QWidget>

#include <functional>

#include "mixerclient.h"

class QComboBox;
class QLabel;
class QTabWidget;
class QVBoxLayout;
class TrackSlider;
class ToggleSwitch;

class CreativeFxPanel : public QWidget {
    Q_OBJECT

public:
    explicit CreativeFxPanel(QWidget *parent = nullptr);

    // Does not emit changed() -- refreshing from the daemon should never
    // look like the user just moved every slider at once.
    void setValues(const CreativeFxInfo &v);
    CreativeFxInfo values() const;

    // The mic-input Creative tab runs its CreativeFxFilter instance mono, so
    // Ping-Pong is structurally inert there; hidden rather than shown
    // disabled, since a control that cannot ever do anything is not a
    // control worth explaining.
    void setStereoCapable(bool on);

signals:
    void changed();

private:
    struct EffectRowUi {
        ToggleSwitch *enabled = nullptr;
        QWidget *body = nullptr;  // the rows below the toggle; hidden when off
    };
    struct SliderUi {
        TrackSlider *slider = nullptr;
        QLabel *readout = nullptr;
    };

    QWidget *buildBitcrusherTab();
    QWidget *buildOverdriveTab();
    QWidget *buildChorusTab();
    QWidget *buildFlangerTab();
    QWidget *buildPhaserTab();
    QWidget *buildTremoloTab();
    QWidget *buildDelayTab();
    QWidget *buildReverbTab();
    QWidget *buildEqTab();
    QWidget *buildRingModTab();
    QWidget *buildEnvFilterTab();
    QWidget *buildPitchTab();
    QWidget *buildReverseDelayTab();
    QWidget *buildTapeSatTab();

    // A right-aligned "Default" button at the top of one effect's tab page,
    // resetting only that effect -- not a shared row for the whole panel,
    // since resetting Bitcrusher should never touch Reverb's dialled-in
    // settings.
    void addResetRow(QVBoxLayout *pageLay, const std::function<void()> &onDefault);
    // Builds one "enable this effect" toggle row plus a body widget that the
    // caller fills with sliderRow()s; wires the toggle to show/hide the body
    // and to fire changed() -- every sub-tab's local dependency handling
    // that used to be applyCreativeControlDependencies() lives here instead,
    // once, rather than once per call site.
    EffectRowUi addEffectHeader(QVBoxLayout *pageLay, const QString &title,
                               const QString &tip);
    // One labeled slider + numeric readout row. `format` turns the slider's
    // raw int value into the text the readout shows.
    SliderUi addSliderRow(QVBoxLayout *lay, const QString &label, int min, int max,
                         const std::function<QString(int)> &format);

    void emitChanged();

    QTabWidget *tabs_ = nullptr;
    bool updating_ = false;
    bool stereoCapable_ = true;

    EffectRowUi bitOn_;
    SliderUi bitDepth_, bitSrReduction_, bitMix_;

    EffectRowUi odOn_;
    SliderUi odDrive_, odTone_, odOutput_, odMix_;

    EffectRowUi chorusOn_;
    SliderUi chorusRate_, chorusDepth_, chorusFeedback_, chorusMix_;

    EffectRowUi flangerOn_;
    SliderUi flangerRate_, flangerDepth_, flangerFeedback_, flangerMix_;

    EffectRowUi phaserOn_;
    SliderUi phaserRate_, phaserDepth_, phaserFeedback_, phaserMix_;

    EffectRowUi tremOn_;
    SliderUi tremRate_, tremDepth_, tremMix_;
    QComboBox *tremShape_ = nullptr;

    EffectRowUi delayOn_;
    SliderUi delayTime_, delayFeedback_, delayMix_, delayDamping_;
    QWidget *pingPongRow_ = nullptr;
    ToggleSwitch *delayPingPong_ = nullptr;

    EffectRowUi reverbOn_;
    SliderUi reverbSize_, reverbDamping_, reverbPredelay_, reverbMix_;

    EffectRowUi eqOn_;
    SliderUi eqGain_, eqBass_, eqMid_, eqTreble_, eqPresence_, eqMaster_;

    EffectRowUi ringModOn_;
    SliderUi ringModFreq_, ringModFineTune_, ringModMix_;

    EffectRowUi envFilterOn_;
    SliderUi envFilterSens_, envFilterAttack_, envFilterRelease_, envFilterResonance_,
        envFilterMix_;

    EffectRowUi pitchOn_;
    SliderUi pitchSemitones_, pitchDetune_, pitchMix_;

    EffectRowUi reverseDelayOn_;
    SliderUi reverseDelayTime_, reverseDelayFeedback_, reverseDelaySmoothing_, reverseDelayMix_;

    EffectRowUi tapeSatOn_;
    SliderUi tapeSatDrive_, tapeSatFlutter_, tapeSatAge_, tapeSatMix_;
};

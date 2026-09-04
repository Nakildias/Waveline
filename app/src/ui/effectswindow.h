// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>

#include "mixerclient.h"

class MixerClient;
class LevelMeter;
class ProEqWindow;
class ToggleSwitch;
class IconToggle;
class CreativeFxPanel;
class QComboBox;
class QSlider;
class QLabel;
class QTabWidget;
class QVBoxLayout;
class QPushButton;
class QListWidget;
class QFileDialog;

struct MicDynamicsInfo;

// The Easy/Advanced switch and everything it swaps in and out. Kept together
// because the two modes share one enclosing layout: Advanced hides the three
// slider rows in place and puts a button where they were, so the section does
// not jump around when the mode changes.
struct EqAdvancedControls {
    ToggleSwitch *mode = nullptr;   // off = Easy, on = Advanced
    QWidget *openRow = nullptr;
    QPushButton *open = nullptr;
    QWidget *lowRow = nullptr;
    QWidget *midRow = nullptr;
    QWidget *highRow = nullptr;
};

struct FxStageControls {
    ToggleSwitch *noise = nullptr;
    QSlider *intensity = nullptr;
    QLabel *intensityLabel = nullptr;
    ToggleSwitch *lowCut = nullptr;
    QComboBox *lowCutHz = nullptr;
    ToggleSwitch *eq = nullptr;
    QSlider *lowDb = nullptr;
    QSlider *midDb = nullptr;
    QSlider *highDb = nullptr;
    QLabel *lowReadout = nullptr;
    QLabel *midReadout = nullptr;
    QLabel *highReadout = nullptr;
    EqAdvancedControls adv;
    ToggleSwitch *gate = nullptr;
    QSlider *gateThreshold = nullptr;
    QLabel *gateThresholdReadout = nullptr;
    ToggleSwitch *compressor = nullptr;
    QSlider *compThreshold = nullptr;
    QSlider *compRatio = nullptr;
    QLabel *compThresholdReadout = nullptr;
    QLabel *compRatioReadout = nullptr;
    ToggleSwitch *autoMakeup = nullptr;
    ToggleSwitch *limiter = nullptr;
    QSlider *limitThreshold = nullptr;
    QLabel *limitThresholdReadout = nullptr;
    ToggleSwitch *deEsser = nullptr;
    QSlider *deEsserAmount = nullptr;
    QLabel *deEsserAmountReadout = nullptr;
};

struct DuckingSourceRow {
    QWidget *row = nullptr;
    QComboBox *kind = nullptr;
    QComboBox *channel = nullptr;
    QPushButton *remove = nullptr;
};

struct DuckingControls {
    ToggleSwitch *enabled = nullptr;
    QSlider *intensity = nullptr;
    QLabel *intensityLabel = nullptr;
    // Tenths of a second, 0..100.
    QSlider *hold = nullptr;
    QLabel *holdLabel = nullptr;
    QWidget *sourcesHost = nullptr;
    QVBoxLayout *sourcesLayout = nullptr;
    QPushButton *addSource = nullptr;
    QList<DuckingSourceRow> sourceRows;
};

struct LufsLimiterControls {
    ToggleSwitch *enabled = nullptr;
    QSlider *maxLufs = nullptr;
    QLabel *maxLufsReadout = nullptr;
};

// Microphone-wide settings: RNNoise, mic EQ, centre-mono, meters.
class GlobalEffectsWindow : public QWidget {
    Q_OBJECT

public:
    explicit GlobalEffectsWindow(MixerClient *client,
                                 const QString &masterId = QStringLiteral("mic"),
                                 QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;

private slots:
    void refresh();
    void refreshLevels();
    void pushMicFx();
    void pushMicDynamics();
    void pushOutputSettings();
    void pushMicCreativeSettings();
    void pushOutputCreativeSettings();
    void pushDuckingSettings();
    void pushLufsLimiterSettings();
    void resetMicProcessing();
    void resetMicDynamics();
    void onHardwareMonitorMoved(int value);
    void onHardwareMonitorReleased();
    void onHardwareMicGainReleased();

private:
    void refreshOutput();
    void refreshDucking();
    void refreshLufsLimiter();
    void refreshMicCreative();
    void refreshOutputCreative();
    void refreshCaptureDevices();
    void refreshMidiDevices();
    void refreshSoundfonts();
    bool isMidiMaster() const;
    void buildSoundfontUi(QWidget *parent, QTabWidget *effectTabs);
    void refreshHardware();
    void buildHardwareUi(QWidget *parent, QTabWidget *effectTabs);
    void updateWindowTitle();
    bool isPrimary() const { return masterId_ == QLatin1String("mic"); }
    QString levelInKey() const;
    QString levelOutKey() const;
    void buildMasterDuckingUi(QWidget *parent, QVBoxLayout *lay, bool inTab = false);
    void buildMasterLufsLimiterUi(QWidget *parent, QVBoxLayout *lay, bool inTab = false);
    void clearMasterDuckingSourceRows();
    void addMasterDuckingSourceRow(const DuckingSourceInfo &src = DuckingSourceInfo{});
    void removeMasterDuckingSourceRow(int index);
    void populateMasterDuckingChannelCombo(DuckingSourceRow &row);
    void updateMasterDuckingAddButton();
    QString encodeMasterDuckingSourcesFromUi() const;
    void setMasterDuckingSourcesFromConfig(const QList<DuckingSourceInfo> &sources);
    static MicDynamicsInfo dynamicsFromStage(const FxStageControls &ui);
    void applyMicDependencies();
    void applyOutputDependencies();

    MixerClient *client_ = nullptr;
    QString masterId_;
    QTabWidget *tabs_ = nullptr;
    QTabWidget *micEffectTabs_ = nullptr;
    QComboBox *captureDevice_ = nullptr;
    QComboBox *midiPort_ = nullptr;
    QWidget *captureRowWidget_ = nullptr;
    QWidget *midiPortRowWidget_ = nullptr;
    QListWidget *soundfontList_ = nullptr;
    QPushButton *addSoundfontBtn_ = nullptr;
    QPushButton *removeSoundfontBtn_ = nullptr;
    QWidget *hardwareSection_ = nullptr;
    QSlider *hwMicGain_ = nullptr;
    QLabel *hwMicGainLabel_ = nullptr;
    bool hwMicGainHeld_ = false;
    IconToggle *hwMicMute_ = nullptr;
    ToggleSwitch *clipguard_ = nullptr;
    QSlider *hwMonitor_ = nullptr;
    QLabel *hwMonitorLabel_ = nullptr;
    bool hwMonitorHeld_ = false;
    QSlider *hpVolume_ = nullptr;
    QLabel *hpVolumeLabel_ = nullptr;
    bool hpVolumeHeld_ = false;
    IconToggle *hpMute_ = nullptr;
    QLabel *deviceLabel_ = nullptr;
    QLabel *firmwareLabel_ = nullptr;
    QWidget *firmwareRow_ = nullptr;
    ToggleSwitch *noise_ = nullptr;
    QSlider *intensity_ = nullptr;
    QLabel *intensityLabel_ = nullptr;
    LevelMeter *noiseIn_ = nullptr;
    LevelMeter *noiseOut_ = nullptr;
    ToggleSwitch *micStereo_ = nullptr;
    ToggleSwitch *lowCut_ = nullptr;
    QComboBox *lowCutHz_ = nullptr;
    ToggleSwitch *eq_ = nullptr;
    QSlider *lowDb_ = nullptr;
    QSlider *midDb_ = nullptr;
    QSlider *highDb_ = nullptr;
    QLabel *lowReadout_ = nullptr;
    QLabel *midReadout_ = nullptr;
    QLabel *highReadout_ = nullptr;
    EqAdvancedControls micAdv_;
    // Kept rather than recreated, so a panel someone has left open beside the
    // mixer keeps its selected band and its position when they come back to
    // this window. QPointer because the panels are parented here and go with
    // it, and nothing below may be left holding a dangling one.
    QPointer<ProEqWindow> micProEq_;
    QPointer<ProEqWindow> outputProEq_;
    ToggleSwitch *gate_ = nullptr;
    QSlider *gateThreshold_ = nullptr;
    QLabel *gateThresholdReadout_ = nullptr;
    ToggleSwitch *compressor_ = nullptr;
    QSlider *compThreshold_ = nullptr;
    QSlider *compRatio_ = nullptr;
    QLabel *compThresholdReadout_ = nullptr;
    QLabel *compRatioReadout_ = nullptr;
    ToggleSwitch *autoMakeup_ = nullptr;
    ToggleSwitch *limiter_ = nullptr;
    QSlider *limitThreshold_ = nullptr;
    ToggleSwitch *deEsser_ = nullptr;
    QSlider *deEsserAmount_ = nullptr;
    QLabel *deEsserAmountReadout_ = nullptr;
    QLabel *limitThresholdReadout_ = nullptr;
    FxStageControls output_;
    CreativeFxPanel *micCreative_ = nullptr;
    // Shown instead of hidden when Rack Mode is on: a greyed-out tab with no
    // explanation reads as broken, not as "look elsewhere."
    QLabel *micCreativeRackNotice_ = nullptr;
    CreativeFxPanel *outputCreative_ = nullptr;
    DuckingControls ducking_;
    LufsLimiterControls lufsLimiter_;
    QString fxSignature_;
    bool updating_ = false;
};

// Per-channel input/output effect chains (NC, EQ, low-cut, dynamics).
class ChannelEffectsWindow : public QWidget {
    Q_OBJECT

public:
    using StageControls = FxStageControls;

    explicit ChannelEffectsWindow(const QString &channelId, const QString &title,
                                  MixerClient *client, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *e) override;

private slots:
    void refresh();
    void refreshInputLevels();
    void pushInputSettings();
    void pushOutputSettings();
    void pushInputCreativeSettings();
    void pushOutputCreativeSettings();
    void pushDuckingSettings();
    void pushLufsLimiterSettings();

private:
    void refreshStage(const QString &stage, StageControls &ui);
    void refreshStageFrom(const QString &channelId, const QString &stage,
                          StageControls &ui);
    void refreshDucking();
    void refreshLufsLimiter();
    void refreshInputCreative();
    void refreshOutputCreative();
    void refreshCreativeFrom(const QString &channelId, const QString &stage,
                             CreativeFxPanel *ui);
    void buildDuckingUi(QWidget *parent, QVBoxLayout *lay, bool inTab = false);
    void buildLufsLimiterUi(QWidget *parent, QVBoxLayout *lay, bool inTab = false);
    void clearDuckingSourceRows();
    void addDuckingSourceRow(const DuckingSourceInfo &src = DuckingSourceInfo{});
    void removeDuckingSourceRow(int index);
    void populateDuckingChannelCombo(DuckingSourceRow &row);
    void updateDuckingAddButton();
    QString encodeDuckingSourcesFromUi() const;
    void setDuckingSourcesFromConfig(const QList<DuckingSourceInfo> &sources);
    void setMicTabEnabled(bool on);
    void setStageControlsEnabled(StageControls &ui, bool on);
    void setDuckingControlsEnabled(bool on);
    void setLufsLimiterControlsEnabled(bool on);
    void updateEffectSourceUi();
    void applyInputDependencies();
    void applyOutputDependencies();
    void applyDuckingDependencies();
    void applyLufsLimiterDependencies();
    static MicDynamicsInfo dynamicsFromStage(const StageControls &ui);

    QString channelId_;
    MixerClient *client_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QComboBox *inputEffectSource_ = nullptr;
    QComboBox *outputEffectSource_ = nullptr;
    StageControls input_;
    StageControls output_;
    QPointer<ProEqWindow> inputProEq_;
    QPointer<ProEqWindow> outputProEq_;
    CreativeFxPanel *inputCreative_ = nullptr;
    CreativeFxPanel *outputCreative_ = nullptr;
    DuckingControls ducking_;
    LufsLimiterControls lufsLimiter_;
    LevelMeter *inputNoiseIn_ = nullptr;
    LevelMeter *inputNoiseOut_ = nullptr;
    ToggleSwitch *micSource_ = nullptr;
    // One row per input device (see below) feeding this channel's published microphone.
    // Rebuilt from the daemon whenever the selection or the available devices
    // change; the first row adds, the rest remove themselves.
    void rebuildMasterMicRows(const QStringList &selected);
    void pushMasterMics();
    QStringList selectedMasterMics() const;
    bool micSourcesIncludeMidi() const;
    // Greys out input noise suppression while a MIDI instrument feeds this
    // channel: the engine routes the mic chain around the filter there.
    void applyMidiNoiseRule();
    QString inputNoiseTip_;
    QVBoxLayout *masterMicRows_ = nullptr;
    QList<QComboBox *> masterMicBoxes_;
    QString masterMicSignature_;
    QString signature_;
    bool updating_ = false;
};

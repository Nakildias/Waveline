// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#pragma once

#include <QHash>
#include <QWidget>

class MixerClient;
class QCheckBox;
class QComboBox;
class QSlider;
class QLabel;
class LevelMeter;
class QTableWidget;
class ToggleSwitch;
class TrackSlider;

class SoundSharingTab : public QWidget {
    Q_OBJECT

public:
    explicit SoundSharingTab(MixerClient *client, QWidget *parent = nullptr);

    // Each application's level before its sliders, already on the meters'
    // curve, by the node id Apps() lists it under. Every row's two tracks show
    // it through their own slider -- what it plays at for you, and what goes
    // into the microphone it is shared to.
    void setAppLevels(const QHash<uint, double> &levels);

private slots:
    void refresh();

private:
    MixerClient *client_ = nullptr;
    ToggleSwitch *routing_ = nullptr;
    ToggleSwitch *enable_ = nullptr;
    QTableWidget *table_ = nullptr;
    QLabel *empty_ = nullptr;
    QString signature_;

    struct RowMeters {
        uint nodeId = 0;
        TrackSlider *app = nullptr;
        LevelMeter *appMeter = nullptr;
        TrackSlider *share = nullptr;
        LevelMeter *shareMeter = nullptr;
        bool shared = false;
    };
    // Rebuilt with the rows; the widgets belong to the table.
    QList<RowMeters> meters_;
};

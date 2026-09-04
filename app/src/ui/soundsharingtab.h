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
class QTableWidget;
class ToggleSwitch;

class SoundSharingTab : public QWidget {
    Q_OBJECT

public:
    explicit SoundSharingTab(MixerClient *client, QWidget *parent = nullptr);

private slots:
    void refresh();
    void onNoiseEnginePicked(int index);

private:
    // Refills the engine selector from the daemon. Not free -- it asks the
    // daemon to go looking for DeepFilterNet -- so it runs on Changed() rather
    // than on the fast level poll.
    void refreshNoiseEngines();

    MixerClient *client_ = nullptr;
    ToggleSwitch *routing_ = nullptr;
    ToggleSwitch *enable_ = nullptr;
    QComboBox *ncEngine_ = nullptr;
    QTableWidget *table_ = nullptr;
    QLabel *empty_ = nullptr;
    QString signature_;
};

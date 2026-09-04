// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Every application currently playing audio, with the channel it is on.
//
// Lists all streams, not only ones the router has moved, so it is still useful
// with automatic routing switched off.

#pragma once

#include <QHash>
#include <QWidget>

#include "mixerclient.h"

class QLabel;
class QTableWidget;

class AppsTab : public QWidget {
    Q_OBJECT

public:
    explicit AppsTab(MixerClient *client, QWidget *parent = nullptr);

public slots:
    void refresh();

private:
    MixerClient *client_ = nullptr;
    QTableWidget *table_ = nullptr;
    QLabel *empty_ = nullptr;
    // What the table is currently showing, so it is only rebuilt when the set
    // of applications actually changes. Rebuilding on every poll would close
    // the combo box the moment the user opened it.
    QString signature_;
};

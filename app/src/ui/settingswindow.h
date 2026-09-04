// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Latency & Diagnostics: the two latency controls, the warnings that can be
// silenced, and everything a bug report needs.
//
// These used to be three things in the header bar -- two dropdowns and an info
// button -- next to a status dot, a profile button, a tuner and a companion
// button. The bar could not fit them and a status line, and the two dropdowns
// were the least likely of the lot to be touched twice in a session: they are
// machine setup, not mixing. So they live behind one gear, in tabs, and the bar
// is back to controls you reach for while using the mixer.
//
// The two latency controls sit in one tab because they are halves of the same
// number and nowhere else says so. One is the graph cycle, paid by everything
// and applied instantly; the other is slack in the buffer of output devices
// that are not the graph clock, and it costs a WirePlumber restart. Putting
// them side by side is the only place that distinction is visible.

#pragma once

#include <QWidget>

class MixerClient;
class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QTableWidget;
class QTimer;
class QVBoxLayout;

class SettingsWindow : public QWidget {
    Q_OBJECT

public:
    explicit SettingsWindow(MixerClient *client, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;

private:
    QWidget *buildLatencyTab();
    // What a desktop shell's audio applet is allowed to show. Its own tab
    // rather than a row in Latency: nothing here is about this machine keeping
    // up with anything, and the two would only confuse each other.
    QWidget *buildDesktopTab();
    QWidget *buildWarningsTab();
    QWidget *buildDiagnosticsTab();

    void onLatencyPicked(int index);
    void onHeadroomPicked(int index);
    // Asks before it changes anything, and puts the box back if the answer is
    // no: turning this off costs a graph rebuild, so unlike the two combos the
    // click is a proposal rather than the setting.
    void onRealtimeToggled(bool on);
    // Nothing to ask about and nothing to put back: this one applies live and
    // rebuilds nothing, so the click *is* the setting.
    void onDspProfilingToggled(bool on);
    // Applies live and rebuilds nothing -- a shell re-reads the count on its
    // next poll -- so the change *is* the setting, same as the box above.
    void onShellNcInputsChanged(int count);
    // Puts the daemon's answer into a combo without that write being mistaken
    // for the user choosing something -- see syncing_ below.
    void syncLatencyCombo();
    void syncHeadroomCombo();
    void syncRealtimeCheck();
    void syncDspProfilingCheck();
    void syncDesktopTab();

    void refreshWarnings();
    void refreshServices();
    void refreshDiagnostics();
    void refresh();

    MixerClient *client_ = nullptr;

    QComboBox *latencyCombo_ = nullptr;
    QComboBox *headroomCombo_ = nullptr;
    QLabel *headroomPending_ = nullptr;
    QCheckBox *realtimeCheck_ = nullptr;
    QCheckBox *dspProfilingCheck_ = nullptr;
    QSpinBox *shellNcSpin_ = nullptr;
    QLabel *shellStatus_ = nullptr;
    // Set while a sync is writing into a combo, so the resulting activated
    // signal does not read back as a user choice and send the value we just
    // displayed straight back to the daemon.
    bool syncing_ = false;

    QVBoxLayout *warningsLay_ = nullptr;
    QLabel *warningsEmpty_ = nullptr;
    QString warningsSignature_;

    QVBoxLayout *servicesLay_ = nullptr;
    QString servicesSignature_;

    QTableWidget *table_ = nullptr;
    QLabel *tableEmpty_ = nullptr;
    QStringList rows_;
    QString rowsSignature_;

    // Only while the window is on screen. Every tick is D-Bus calls plus a
    // systemctl, and a panel nobody is looking at should cost nothing.
    QTimer *poll_ = nullptr;
};

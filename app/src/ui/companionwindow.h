// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The Web Companion panel: turn the tablet controller on, off, and on at login.
//
// The server itself lives in the daemon -- the point of the feature is that it
// keeps working with this window closed -- so everything here is a D-Bus call
// and a display of what came back. There is deliberately nothing in this panel
// that configures the *mixer*: it configures the door into it.
//
// The addresses are the panel's real work. A user who has just switched the
// server on needs a URL to type into a phone, and neither "0.0.0.0" nor a
// hostname that only resolves on the machine itself is one -- so the daemon
// enumerates its own interfaces and this lists what came back, ready to copy.

#pragma once

#include <QWidget>

class MixerClient;
class QLabel;
class QPushButton;
class QSpinBox;
class QVBoxLayout;
class StatusDot;
class ToggleSwitch;

class CompanionWindow : public QWidget {
    Q_OBJECT

public:
    explicit CompanionWindow(MixerClient *client, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *) override;

private slots:
    void refresh();
    void onAvailabilityChanged(bool available);

private:
    void toggleServer();
    void applyPort();
    void rebuildAddresses(const QStringList &urls);
    // Failures are shown in the panel rather than in a message box: "port 8787
    // is already in use" is something the user fixes in the spin box two rows
    // up, and a modal dialog puts itself between them and it.
    void showError(const QString &text);

    MixerClient *client_ = nullptr;

    StatusDot *dot_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *clientsLabel_ = nullptr;
    QPushButton *toggleBtn_ = nullptr;
    QSpinBox *portBox_ = nullptr;
    QPushButton *applyPortBtn_ = nullptr;
    ToggleSwitch *autoStart_ = nullptr;
    QLabel *errorLabel_ = nullptr;
    QVBoxLayout *addressLay_ = nullptr;
    QLabel *addressHint_ = nullptr;

    // What rebuildAddresses() last drew, so a 400 ms refresh does not take a
    // copy button out from under the cursor on its way down.
    QStringList shownAddresses_;
    // The port the daemon last reported. Typing in the spin box must not be
    // overwritten by a refresh mid-edit, and this is how the panel tells a
    // value the user is changing from one it has not touched.
    int daemonPort_ = 0;
};

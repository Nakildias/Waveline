// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QNetworkAccessManager;
class QNetworkReply;

class AboutWindow : public QWidget {
    Q_OBJECT

public:
    explicit AboutWindow(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onUpdateReplyFinished();
    void openRepository();
    void copyInstallCommand();

private:
    void startUpdateCheck();

    QLabel *updateStatus_ = nullptr;
    QLabel *updateHint_ = nullptr;
    QPushButton *updateBtn_ = nullptr;
    QNetworkAccessManager *nam_ = nullptr;
    QNetworkReply *pendingReply_ = nullptr;
};

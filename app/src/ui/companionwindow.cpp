// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "companionwindow.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "mixerclient.h"
#include "theme.h"
#include "widgets.h"

namespace {

QLabel *dimLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(p);
    return l;
}

}  // namespace

CompanionWindow::CompanionWindow(MixerClient *client, QWidget *parent)
    : QWidget(parent, Qt::Window), client_(client) {
    setWindowTitle(tr("Web Companion"));
    resize(520, 560);
    setMinimumSize(430, 470);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(12);

    auto *title = new QLabel(tr("Web Companion"), this);
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() * 1.2);
    title->setFont(tf);
    outer->addWidget(title);

    auto *blurb = dimLabel(
        tr("Use a phone or tablet as a second control surface. Open one of the "
           "addresses below in its browser and you get the inputs, the "
           "channels, the application settings and both mixes, live."),
        this);
    blurb->setWordWrap(true);
    outer->addWidget(blurb);

    // ------------------------------------------------------------ status
    auto *statusCard = new Section(tr("Server"), this);
    auto *statusLay = statusCard->contentLayout();

    auto *statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    dot_ = new StatusDot(statusCard);
    statusRow->addWidget(dot_);
    statusLabel_ = new QLabel(tr("Stopped"), statusCard);
    QFont sf = statusLabel_->font();
    sf.setBold(true);
    statusLabel_->setFont(sf);
    statusRow->addWidget(statusLabel_);
    clientsLabel_ = dimLabel(QString(), statusCard);
    statusRow->addWidget(clientsLabel_);
    statusRow->addStretch();

    toggleBtn_ = new QPushButton(tr("Start"), statusCard);
    toggleBtn_->setMinimumWidth(100);
    connect(toggleBtn_, &QPushButton::clicked, this,
            &CompanionWindow::toggleServer);
    statusRow->addWidget(toggleBtn_);
    statusLay->addLayout(statusRow);

    errorLabel_ = new QLabel(statusCard);
    errorLabel_->setWordWrap(true);
    {
        QPalette p = errorLabel_->palette();
        p.setColor(QPalette::WindowText, Theme::Danger);
        errorLabel_->setPalette(p);
    }
    errorLabel_->hide();
    statusLay->addWidget(errorLabel_);
    outer->addWidget(statusCard);

    // ----------------------------------------------------------- settings
    auto *settings = new Section(tr("Settings"), this);
    auto *setLay = settings->contentLayout();

    auto *portRow = new QHBoxLayout;
    portRow->setSpacing(8);
    portRow->addWidget(new QLabel(tr("Port"), settings));
    portBox_ = new QSpinBox(settings);
    // Below 1024 needs privileges a user unit does not have and should not get.
    portBox_->setRange(1024, 65535);
    portBox_->setValue(8787);
    portBox_->setFixedWidth(96);
    // No keyboard tracking: the box would otherwise report 8, then 87, then 878
    // on the way to 8787, and each of those is a port the panel would offer to
    // move a running server onto.
    portBox_->setKeyboardTracking(false);
    portRow->addWidget(portBox_);
    applyPortBtn_ = new QPushButton(tr("Apply"), settings);
    connect(applyPortBtn_, &QPushButton::clicked, this,
            &CompanionWindow::applyPort);
    portRow->addWidget(applyPortBtn_);
    portRow->addStretch();
    setLay->addLayout(portRow);

    auto *portHint = dimLabel(
        tr("Changing the port while the server is running moves it. Devices "
           "already on the old address will need the new one."),
        settings);
    portHint->setWordWrap(true);
    setLay->addWidget(portHint);

    auto *autoRow = new QHBoxLayout;
    autoRow->setSpacing(8);
    auto *autoText = new QVBoxLayout;
    autoText->setSpacing(1);
    auto *autoTitle = new QLabel(tr("Start with the mixer"), settings);
    autoText->addWidget(autoTitle);
    auto *autoHint = dimLabel(
        tr("Bring the server up whenever wavelined starts, so the tablet works "
           "without opening this window first."),
        settings);
    autoHint->setWordWrap(true);
    autoText->addWidget(autoHint);
    autoRow->addLayout(autoText, 1);
    autoStart_ = new ToggleSwitch(settings);
    connect(autoStart_, &QAbstractButton::toggled, this, [this](bool on) {
        client_->setCompanionAutoStart(on);
    });
    autoRow->addWidget(autoStart_, 0, Qt::AlignTop);
    setLay->addLayout(autoRow);
    outer->addWidget(settings);

    // ---------------------------------------------------------- addresses
    auto *addresses = new Section(tr("Addresses"), this);
    addressLay_ = addresses->contentLayout();
    addressHint_ = dimLabel(
        tr("Start the server to see the addresses it answers on."), addresses);
    addressHint_->setWordWrap(true);
    addressLay_->addWidget(addressHint_);
    outer->addWidget(addresses);

    // The one thing about this feature a user has to be told rather than shown.
    auto *warning = dimLabel(
        tr("Anyone who can reach this machine on your network can open the "
           "companion and change your mixer — there is no password. Leave it "
           "stopped on networks you do not control."),
        this);
    warning->setWordWrap(true);
    {
        QPalette p = warning->palette();
        p.setColor(QPalette::WindowText, Theme::Warn);
        warning->setPalette(p);
    }
    outer->addWidget(warning);

    outer->addStretch();

    connect(client_, &MixerClient::changed, this, &CompanionWindow::refresh);
    connect(client_, &MixerClient::availabilityChanged, this,
            &CompanionWindow::onAvailabilityChanged);
    onAvailabilityChanged(client_->available());
    refresh();
}

void CompanionWindow::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    refresh();
}

void CompanionWindow::onAvailabilityChanged(bool available) {
    toggleBtn_->setEnabled(available);
    portBox_->setEnabled(available);
    applyPortBtn_->setEnabled(available);
    autoStart_->setEnabled(available);
    if (!available) {
        dot_->setColor(Theme::TextFaint);
        statusLabel_->setText(tr("wavelined is not running"));
        clientsLabel_->clear();
        rebuildAddresses({});
    }
}

void CompanionWindow::refresh() {
    if (!client_->available()) return;
    const CompanionInfo info = client_->companion();

    dot_->setColor(info.running ? Theme::Accent : Theme::TextFaint);
    statusLabel_->setText(info.running ? tr("Running") : tr("Stopped"));
    toggleBtn_->setText(info.running ? tr("Stop") : tr("Start"));

    if (!info.running)
        clientsLabel_->clear();
    else
        clientsLabel_->setText(info.clients == 1
                                   ? tr("· 1 device connected")
                                   : tr("· %n device(s) connected", "", info.clients));

    // Never while it is being edited: the panel refreshes every 400 ms and
    // would otherwise reset the number under the user's fingers.
    if (info.port != daemonPort_ && !portBox_->hasFocus()) {
        QSignalBlocker block(portBox_);
        portBox_->setValue(info.port);
    }
    daemonPort_ = info.port;

    {
        QSignalBlocker block(autoStart_);
        autoStart_->setChecked(info.autoStart);
    }

    rebuildAddresses(info.addresses);
}

void CompanionWindow::toggleServer() {
    errorLabel_->hide();
    const CompanionInfo info = client_->companion();
    if (info.running) {
        client_->companionStop();
    } else {
        // The port the box is showing, not the one the daemon has stored: a
        // number typed and then Start pressed has to mean what it looks like.
        if (portBox_->value() != daemonPort_) {
            const QString error = client_->setCompanionPort(portBox_->value());
            if (!error.isEmpty()) {
                showError(error);
                return;
            }
        }
        const QString error = client_->companionStart();
        if (!error.isEmpty()) showError(error);
    }
    client_->refresh();
    refresh();
}

void CompanionWindow::applyPort() {
    errorLabel_->hide();
    const QString error = client_->setCompanionPort(portBox_->value());
    if (!error.isEmpty()) showError(error);
    client_->refresh();
    refresh();
}

void CompanionWindow::showError(const QString &text) {
    errorLabel_->setText(text);
    errorLabel_->show();
}

void CompanionWindow::rebuildAddresses(const QStringList &urls) {
    if (urls == shownAddresses_) return;
    shownAddresses_ = urls;

    // Everything but the hint, which is reused rather than recreated so its
    // wrapping does not resize the panel each time the server is toggled.
    while (addressLay_->count() > 1) {
        QLayoutItem *item = addressLay_->takeAt(1);
        if (QWidget *w = item->widget()) w->deleteLater();
        delete item;
    }

    if (urls.isEmpty()) {
        addressHint_->setText(
            tr("Start the server to see the addresses it answers on."));
        addressHint_->show();
        return;
    }

    addressHint_->setText(
        tr("Open any of these on the device. They are the same page; more than "
           "one means this machine is on more than one network."));

    for (const QString &url : urls) {
        auto *row = new QHBoxLayout;
        row->setSpacing(8);

        auto *label = new QLabel(url, this);
        // Selectable so the address can be dragged out as text, which is what
        // people reach for before they notice the button.
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont mono = label->font();
        mono.setFamily(QStringLiteral("monospace"));
        label->setFont(mono);
        row->addWidget(label, 1);

        auto *copy = new QPushButton(tr("Copy"), this);
        copy->setFixedWidth(72);
        connect(copy, &QPushButton::clicked, this, [url] {
            QApplication::clipboard()->setText(url);
        });
        row->addWidget(copy);

        auto *holder = new QWidget(this);
        holder->setLayout(row);
        addressLay_->addWidget(holder);
    }
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "appstab.h"

#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

#include "theme.h"
#include "widgets.h"

AppsTab::AppsTab(MixerClient *client, QWidget *parent)
    : QWidget(parent), client_(client) {
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    auto *hint = new QLabel(tr("Pick a channel to move an application."), this);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    lay->addWidget(hint);

    table_ = new QTableWidget(0, 2, this);
    table_->setMinimumHeight(120);
    table_->setHorizontalHeaderLabels({tr("Application"), tr("Channel")});
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    if (auto *h = table_->horizontalHeaderItem(0))
        h->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // Channel sits hard right, over the badges it labels.
    if (auto *h = table_->horizontalHeaderItem(1))
        h->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    // Only a badge: the channel names live in the popup.
    table_->horizontalHeader()->resizeSection(1, 46);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(table_, 1);

    empty_ = new QLabel(tr("Nothing is playing audio right now."), this);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setEnabled(false);
    lay->addWidget(empty_);

    connect(client_, &MixerClient::changed, this, &AppsTab::refresh);
    refresh();
}

void AppsTab::refresh() {
    if (!client_->available()) return;

    const auto apps = client_->apps();
    const auto channels = client_->channels();

    // Rebuild only when the set of applications or their channels changes: the
    // client polls several times a second for the meters, and recreating the
    // combo boxes each time would make them impossible to use.
    QString sig;
    for (const auto &a : apps)
        sig += QStringLiteral("%1:%2:%3;").arg(a.nodeId).arg(a.name, a.channelId);
    for (const auto &c : channels) sig += c.id + QLatin1Char(',');
    // Recolouring a card changes no application and no channel id, so without
    // this the badges would keep whatever colour they were built with.
    sig += QStringLiteral("looks%1").arg(Theme::cardLooksRevision());
    if (sig == signature_) return;
    signature_ = sig;

    table_->setRowCount(0);
    empty_->setVisible(apps.isEmpty());
    table_->setVisible(!apps.isEmpty());

    for (const auto &a : apps) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(row, 0, new QTableWidgetItem(a.name));

        auto *combo = new IconCombo(table_);
        combo->addItem(Theme::noneBadge(), tr("Not routed"), QString());
        for (const auto &c : channels)
            combo->addItem(Theme::channelBadge(c.id), c.name, c.id);

        int index = 0;
        for (int i = 0; i < combo->count(); ++i)
            if (combo->itemData(i).toString() == a.channelId) { index = i; break; }
        combo->setCurrentIndex(index);

        const uint nodeId = a.nodeId;
        connect(combo, &QComboBox::activated, this, [this, combo, nodeId](int i) {
            const QString channel = combo->itemData(i).toString();
            // "(not routed)" is not an instruction we can carry out: PipeWire
            // has no "put it back where it was" once we have moved it, so the
            // entry is a status, not a destination.
            if (channel.isEmpty()) return;
            client_->moveApp(nodeId, channel);
            signature_.clear();  // force a rebuild on the next update
        });
        table_->setCellWidget(row, 1, combo);
    }
}

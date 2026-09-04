// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "soundsharingtab.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QAbstractItemView>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardItemModel>
#include <QStylePainter>
#include <QTableWidget>
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

QWidget *makeVolumeRow(QWidget *parent, TrackSlider **outSlider, QLabel **outPct,
                       int maxPct, double value, const QColor &accent, bool enabled,
                       const QString &tip) {
    auto *row = new QWidget(parent);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);

    auto *slider = new TrackSlider(row);
    slider->setRange(0, maxPct);
    slider->setValue(int(value * 100.0 + 0.5));
    slider->setAccent(enabled ? accent : Theme::Line);
    slider->setEnabled(enabled);
    slider->setToolTip(tip);
    if (!enabled) slider->setCursor(Qt::ArrowCursor);

    auto *pct = dimLabel(QStringLiteral("%1%").arg(slider->value()), row);
    pct->setMinimumWidth(40);
    pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (!enabled) {
        QPalette p = pct->palette();
        p.setColor(QPalette::WindowText, Theme::TextFaint);
        pct->setPalette(p);
    }

    lay->addWidget(slider, 1);
    lay->addWidget(pct);
    if (outSlider) *outSlider = slider;
    if (outPct) *outPct = pct;
    return row;
}

}  // namespace

struct SharedAppInfo {
    uint nodeId = 0;
    QString name;
    QString target;   // "" = not shared, else channel id or "mic"
    double level = 1.0;
    double volume = 1.0;
    QString channelId;
};

SoundSharingTab::SoundSharingTab(MixerClient *client, QWidget *parent)
    : QWidget(parent), client_(client) {
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);

    auto *routeRow = new QHBoxLayout;
    routeRow->addWidget(dimLabel(tr("Auto Routing"), this));
    routeRow->addStretch();
    routing_ = new ToggleSwitch(this);
    routing_->setToolTip(
        tr("Move applications onto channels by name (Discord to Voice, Spotify "
           "to Music, and so on).\n"
           "Off by default: enabling it moves audio that is already playing."));
    routeRow->addWidget(routing_);
    lay->addLayout(routeRow);
    connect(routing_, &QAbstractButton::toggled, client_,
            &MixerClient::setRoutingEnabled);

    auto *enableRow = new QHBoxLayout;
    enableRow->addWidget(dimLabel(tr("Audio Sharing"), this));
    enableRow->addStretch();
    enable_ = new ToggleSwitch(this);
    enable_->setToolTip(tr("Route chosen applications into the Stream and Monitor "
                           "mixes alongside your microphone."));
    enableRow->addWidget(enable_);
    lay->addLayout(enableRow);
    connect(enable_, &QAbstractButton::toggled, client_,
            &MixerClient::setSoundSharingEnabled);

    // Noise suppression engine. Not an application setting as such, but it is
    // the other thing in this panel that applies to everything at once, and it
    // is one line rather than a section of its own.
    auto *ncRow = new QHBoxLayout;
    auto *ncName = dimLabel(tr("NC Engine"), this);
    ncRow->addWidget(ncName);
    ncEngine_ = new QComboBox(this);
    ncEngine_->setCursor(Qt::PointingHandCursor);
    ncRow->addWidget(ncEngine_, 1);
    lay->addLayout(ncRow);

    const QString ncTip =
        tr("Which model removes background noise, for the microphone and every "
           "channel.\n"
           "DeepFilterNet is far better on steady noise (fans, keyboards, "
           "traffic)\nbut costs several times the CPU.\n"
           "RNNoise is small and fast, and is always available.");
    ncName->setToolTip(ncTip);
    ncEngine_->setToolTip(ncTip);
    connect(ncEngine_, &QComboBox::activated, this,
            &SoundSharingTab::onNoiseEnginePicked);
    refreshNoiseEngines();

    table_ = new QTableWidget(0, 3, this);
    table_->setMinimumHeight(120);
    table_->setHorizontalHeaderLabels(
        {tr("APPLICATIONS"), tr("CH \u24D8"), tr("AS \u24D8")});
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    if (auto *h = table_->horizontalHeaderItem(0))
        h->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // The two badge columns are abbreviated to keep them narrow, so each
    // carries its own explanation rather than relying on the reader guessing.
    if (auto *h = table_->horizontalHeaderItem(1)) {
        h->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        h->setToolTip(tr("Channel this app will use."));
    }
    if (auto *h = table_->horizontalHeaderItem(2)) {
        h->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        h->setToolTip(tr("Sends the Audio of the application to a Microphone, "
                         "Good for Soundboards."));
    }
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table_->horizontalHeader()->resizeSection(1, 46);
    table_->horizontalHeader()->resizeSection(2, 46);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    lay->addWidget(table_, 1);

    empty_ = new QLabel(tr("Nothing is playing audio right now."), this);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setEnabled(false);
    lay->addWidget(empty_);

    connect(client_, &MixerClient::changed, this, &SoundSharingTab::refresh);
    refresh();
}

void SoundSharingTab::refresh() {
    if (!client_->available()) return;

    {
        QSignalBlocker b(routing_);
        routing_->setChecked(client_->routingEnabled());
    }
    {
        QSignalBlocker b(enable_);
        enable_->setChecked(client_->soundSharingEnabled());
    }
    refreshNoiseEngines();

    QHash<uint, double> volumes;
    QHash<uint, QString> routed;
    for (const auto &a : client_->apps()) {
        routed.insert(a.nodeId, a.channelId);
        volumes.insert(a.nodeId, a.volume);
    }

    QList<SharedAppInfo> apps;
    for (const QString &row : client_->soundSharingApps()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 3) continue;
        SharedAppInfo a;
        a.nodeId = f[0].toUInt();
        a.name = f[1];
        a.target = f[2];
        if (f.size() > 3) a.level = f[3].toDouble();
        a.volume = volumes.value(a.nodeId, 1.0);
        a.channelId = routed.value(a.nodeId);
        apps.append(a);
    }

    const QStringList targets = client_->soundSharingTargets();
    const auto channels = client_->channels();
    // Volumes stay out of the signature so dragging a slider does not rebuild
    // the row under the mouse (same trick as share level).
    QString sig = targets.join(QLatin1Char('|')) + QLatin1Char('#');
    for (const auto &c : channels) sig += c.id + QLatin1Char(',');
    for (const auto &a : apps)
        sig += QStringLiteral("%1:%2:%3:%4;")
                   .arg(a.nodeId)
                   .arg(a.name, a.target, a.channelId);
    // The sliders and badges here are drawn in their channel's colour, and a
    // recolour changes nothing else in this signature.
    sig += QStringLiteral("looks%1").arg(Theme::cardLooksRevision());
    if (sig == signature_) return;
    signature_ = sig;

    table_->setRowCount(0);
    empty_->setVisible(apps.isEmpty());
    table_->setVisible(!apps.isEmpty());

    for (int i = 0; i < apps.size(); ++i) {
        const SharedAppInfo &a = apps[i];
        const int row = table_->rowCount();
        table_->insertRow(row);

        auto *cell = new QWidget(table_);
        auto *cl = new QVBoxLayout(cell);
        cl->setContentsMargins(2, 4, 2, 4);
        cl->setSpacing(4);
        cl->addWidget(new QLabel(a.name, cell));

        const uint nodeId = a.nodeId;
        const QColor channelAccent = Theme::channelColor(a.channelId);
        TrackSlider *appSlider = nullptr;
        QLabel *appPct = nullptr;
        cl->addWidget(makeVolumeRow(
            cell, &appSlider, &appPct, 150, a.volume, channelAccent, true,
            tr("How loud this application plays for you.\n"
               "100% is unity; up to 150% for a boost.")));
        if (appSlider) {
            connect(appSlider, &QSlider::valueChanged, this,
                    [this, appPct, nodeId](int v) {
                        appPct->setText(QStringLiteral("%1%").arg(v));
                        client_->setAppVolume(nodeId, v / 100.0);
                    });
        }

        const bool shared = !a.target.isEmpty();
        const QColor shareAccent =
            shared ? Theme::channelColor(a.target) : Theme::Line;
        TrackSlider *shareSlider = nullptr;
        QLabel *sharePct = nullptr;
        cl->addWidget(makeVolumeRow(
            cell, &shareSlider, &sharePct, 100, a.level, shareAccent, shared,
            shared ? tr("How loud this application is in the microphone.\n"
                        "Does not change how loudly it plays for you.")
                   : tr("Pick an Audio Sharing target to adjust this level.")));
        if (shareSlider) {
            connect(shareSlider, &QSlider::valueChanged, this,
                    [this, sharePct, nodeId](int v) {
                        sharePct->setText(QStringLiteral("%1%").arg(v));
                        client_->setSoundSharingAppLevel(nodeId, v / 100.0);
                    });
        }

        if (i + 1 < apps.size()) {
            auto *sep = new QFrame(cell);
            sep->setFrameShape(QFrame::HLine);
            sep->setFrameShadow(QFrame::Plain);
            sep->setFixedHeight(1);
            sep->setStyleSheet(
                QStringLiteral("background: %1; border: none;").arg(Theme::Line.name()));
            cl->addSpacing(2);
            cl->addWidget(sep);
        }

        table_->setCellWidget(row, 0, cell);
        table_->setRowHeight(row, cell->sizeHint().height() + 4);

        auto *box = new IconCombo(table_);
        box->addItem(Theme::noneBadge(), tr("Not shared"), QString());
        for (const QString &t : targets) {
            const int tab = t.indexOf(QLatin1Char('\t'));
            if (tab < 0) continue;
            const QString id = t.left(tab);
            box->addItem(Theme::channelBadge(id), t.mid(tab + 1), id);
        }
        const int at = box->findData(a.target);
        box->setCurrentIndex(at < 0 ? 0 : at);
        box->setToolTip(
            tr("Add this application's audio to a microphone, so whoever is\n"
               "listening to it hears the application alongside your voice.\n"
               "The application keeps playing to you as well.\n"
               "Only microphones that are published can be chosen."));
        connect(box, &QComboBox::activated, this, [this, box, nodeId](int idx) {
            client_->setSoundSharingAppTarget(nodeId, box->itemData(idx).toString());
        });
        table_->setCellWidget(row, 2, box);

        auto *ch = new IconCombo(table_);
        ch->addItem(Theme::noneBadge(), tr("Not routed"), QString());
        for (const auto &c : channels)
            ch->addItem(Theme::channelBadge(c.id), c.name, c.id);
        const int chAt = ch->findData(a.channelId);
        ch->setCurrentIndex(chAt < 0 ? 0 : chAt);
        ch->setToolTip(tr("Channel this app will use."));
        connect(ch, &QComboBox::activated, this, [this, ch, nodeId](int idx) {
            const QString channel = ch->itemData(idx).toString();
            // "Not routed" is a state, not a destination: PipeWire cannot put a
            // stream back where it came from once it has been moved.
            if (channel.isEmpty()) return;
            client_->moveApp(nodeId, channel);
            signature_.clear();
        });
        table_->setCellWidget(row, 1, ch);
    }
}

void SoundSharingTab::refreshNoiseEngines() {
    if (!ncEngine_) return;

    const QString current = client_->noiseEngine();
    const auto engines = client_->noiseEngines();

    QSignalBlocker block(ncEngine_);
    ncEngine_->clear();
    for (const auto &e : engines) {
        // Unavailable engines are listed but greyed, not hidden.
        // "DeepFilterNet is not installed" is far more useful to see than an
        // absence the user has no way to explain.
        ncEngine_->addItem(e.available ? e.label
                                       : tr("%1 (not installed)").arg(e.label),
                           e.id);
        const int i = ncEngine_->count() - 1;
        if (!e.available) {
            auto *model = qobject_cast<QStandardItemModel *>(ncEngine_->model());
            if (model && model->item(i)) model->item(i)->setEnabled(false);
            ncEngine_->setItemData(i, e.reason, Qt::ToolTipRole);
        }
    }

    const int idx = ncEngine_->findData(current);
    if (idx >= 0) ncEngine_->setCurrentIndex(idx);
}

void SoundSharingTab::onNoiseEnginePicked(int index) {
    if (index < 0 || !ncEngine_) return;
    const QString id = ncEngine_->itemData(index).toString();
    if (id.isEmpty() || id == client_->noiseEngine()) return;

    const QString error = client_->setNoiseEngine(id);
    if (error.isEmpty()) return;

    // The daemon kept the old engine running, so put the selector back rather
    // than leaving it claiming something that is not true.
    refreshNoiseEngines();
    QMessageBox::warning(this, tr("Noise suppression"),
                         tr("Could not switch to %1.\n\n%2")
                             .arg(ncEngine_->itemText(index), error));
}

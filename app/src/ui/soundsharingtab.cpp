// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "soundsharingtab.h"

#include <algorithm>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QAbstractItemView>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QSlider>
#include <QStylePainter>
#include <QTableWidget>
#include <QVBoxLayout>

#include "levelmeter.h"
#include "mixerclient.h"
#include "monarchy/desktop.h"
#include "theme.h"
#include "widgets.h"

namespace {

// The application icon beside each name, on Monarchy.
constexpr int kAppIconPx = 18;

QLabel *dimLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    Theme::setTextColor(l, Theme::TextDim);
    return l;
}

QWidget *makeVolumeRow(QWidget *parent, TrackSlider **outSlider, QLabel **outPct,
                       int maxPct, double value, const QColor &accent, bool enabled,
                       const QString &tip, const QString &glyph) {
    auto *row = new QWidget(parent);
    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);

    // Side by side, the two have to say which is which without a caption.
    auto *icon = new QLabel(row);
    Theme::setPixmap(icon, glyph, enabled ? Theme::TextDim : Theme::TextFaint, 14);
    icon->setToolTip(tip);
    lay->addWidget(icon);

    auto *slider = new TrackSlider(row);
    slider->setFaderStyle(true);
    slider->setRange(0, maxPct);
    slider->setValue(int(value * 100.0 + 0.5));
    slider->setAccent(enabled ? accent : Theme::Inactive);
    slider->setEnabled(enabled);
    slider->setToolTip(tip);
    if (!enabled) slider->setCursor(Qt::ArrowCursor);

    auto *pct = dimLabel(QStringLiteral("%1%").arg(slider->value()), row);
    pct->setMinimumWidth(40);
    pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (!enabled) {
        Theme::setTextColor(pct, Theme::TextFaint);
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
    QString icon;     // the launcher entry's Icon=, empty when unknown
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

    // One column: each application's channel and sharing badges are on its
    // own name row, so the separators between applications run edge to edge.
    table_ = new QTableWidget(0, 1, this);
    table_->setMinimumHeight(120);
    table_->setHorizontalHeaderLabels({tr("APPLICATIONS")});
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
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
    Theme::onChange(this, [this] { refresh(); });
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
    QHash<uint, double> volumes;
    QHash<uint, QString> routed;
    QHash<uint, QString> icons;
    for (const auto &a : client_->apps()) {
        routed.insert(a.nodeId, a.channelId);
        volumes.insert(a.nodeId, a.volume);
        icons.insert(a.nodeId, a.icon);
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
        a.icon = icons.value(a.nodeId);
        apps.append(a);
    }

    const QStringList targets = client_->soundSharingTargets();
    const auto channels = client_->channels();
    // Volumes stay out of the signature so dragging a slider does not rebuild
    // the row under the mouse (same trick as share level).
    QString sig = targets.join(QLatin1Char('|')) + QLatin1Char('#');
    for (const auto &c : channels) sig += c.id + QLatin1Char(',');
    for (const auto &a : apps)
        sig += QStringLiteral("%1:%2:%3:%4:%5;")
                   .arg(a.nodeId)
                   .arg(a.name, a.target, a.channelId, a.icon);
    // The sliders and badges here are drawn in their channel's colour, and a
    // recolour changes nothing else in this signature.
    sig += QStringLiteral("looks%1").arg(Theme::cardLooksRevision());
    // And the scheme: the rows copy its colours when they are built.
    sig += QStringLiteral("light%1").arg(Theme::Light ? 1 : 0);
    if (sig == signature_) return;
    signature_ = sig;

    table_->setRowCount(0);
    meters_.clear();
    empty_->setVisible(apps.isEmpty());
    table_->setVisible(!apps.isEmpty());

    for (int i = 0; i < apps.size(); ++i) {
        const SharedAppInfo &a = apps[i];
        const int row = table_->rowCount();
        table_->insertRow(row);

        auto *cell = new QWidget(table_);
        auto *cl = new QVBoxLayout(cell);
        cl->setContentsMargins(2, 4, 2, 4);
        cl->setSpacing(6);
        const uint nodeId = a.nodeId;

        // ------------------------------------------- name, channel, sharing
        auto *nameRow = new QHBoxLayout;
        nameRow->setContentsMargins(0, 0, 0, 0);
        nameRow->setSpacing(6);
        if (Monarchy::isActive()) {
            // On Monarchy, the application's own icon to the left of its name,
            // from the desktop's icon theme. The universal look stays text
            // only: with no known theme to resolve against, some rows would
            // have an icon and some a gap.
            auto *iconLabel = new QLabel(cell);
            // Held at its size even without an icon, so every name starts at
            // the same place.
            iconLabel->setFixedSize(kAppIconPx, kAppIconPx);
            const QIcon icon = Monarchy::appIcon(a.icon);
            if (!icon.isNull()) iconLabel->setPixmap(icon.pixmap(kAppIconPx, kAppIconPx));
            nameRow->addWidget(iconLabel);
        }
        nameRow->addWidget(new QLabel(a.name, cell), 1);

        auto *ch = new IconCombo(cell);
        ch->addItem(Theme::noneBadge(), tr("Not routed"), QString());
        for (const auto &c : channels)
            ch->addItem(Theme::channelBadge(c.id), c.name, c.id);
        const int chAt = ch->findData(a.channelId);
        ch->setCurrentIndex(chAt < 0 ? 0 : chAt);
        ch->setToolTip(tr("Channel this application uses."));
        connect(ch, &QComboBox::activated, this, [this, ch, nodeId](int idx) {
            const QString channel = ch->itemData(idx).toString();
            // "Not routed" is a state, not a destination: PipeWire cannot put a
            // stream back where it came from once it has been moved.
            if (channel.isEmpty()) return;
            client_->moveApp(nodeId, channel);
            signature_.clear();
        });
        nameRow->addWidget(ch);

        auto *box = new IconCombo(cell);
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
            tr("Audio Sharing: add this application's audio to a microphone, so\n"
               "whoever is listening to it hears the application alongside your\n"
               "voice. Good for soundboards. The application keeps playing to you\n"
               "as well. Only microphones that are published can be chosen."));
        connect(box, &QComboBox::activated, this, [this, box, nodeId](int idx) {
            client_->setSoundSharingAppTarget(nodeId, box->itemData(idx).toString());
        });
        nameRow->addWidget(box);
        cl->addLayout(nameRow);

        // ------------------------------------- the two levels, side by side
        auto *levels = new QHBoxLayout;
        levels->setContentsMargins(0, 0, 0, 0);
        levels->setSpacing(14);

        const QColor channelAccent = Theme::channelColor(a.channelId);
        TrackSlider *appSlider = nullptr;
        QLabel *appPct = nullptr;
        levels->addWidget(makeVolumeRow(
            cell, &appSlider, &appPct, 150, a.volume, channelAccent, true,
            tr("How loud this application plays for you.\n"
               "100% is unity; up to 150% for a boost."),
            QStringLiteral("speaker-high")), 1);
        if (appSlider) {
            connect(appSlider, &QSlider::valueChanged, this,
                    [this, appPct, nodeId](int v) {
                        appPct->setText(QStringLiteral("%1%").arg(v));
                        client_->setAppVolume(nodeId, v / 100.0);
                    });
        }

        const bool shared = !a.target.isEmpty();
        const QColor shareAccent =
            shared ? Theme::channelColor(a.target) : Theme::Inactive;
        TrackSlider *shareSlider = nullptr;
        QLabel *sharePct = nullptr;
        levels->addWidget(makeVolumeRow(
            cell, &shareSlider, &sharePct, 100, a.level, shareAccent, shared,
            shared ? tr("How loud this application is in the microphone.\n"
                        "Does not change how loudly it plays for you.")
                   : tr("Pick an Audio Sharing target to adjust this level."),
            QStringLiteral("microphone")), 1);
        if (shareSlider) {
            connect(shareSlider, &QSlider::valueChanged, this,
                    [this, sharePct, nodeId](int v) {
                        sharePct->setText(QStringLiteral("%1%").arg(v));
                        client_->setSoundSharingAppLevel(nodeId, v / 100.0);
                    });
        }
        cl->addLayout(levels);

        // Each slider's track is its meter: the application's level through
        // that slider, in the slider's own colour.
        RowMeters m;
        m.nodeId = nodeId;
        m.shared = shared;
        if (appSlider) {
            m.app = appSlider;
            m.appMeter = attachTrackMeter(appSlider);
            m.appMeter->setTint(channelAccent);
        }
        if (shareSlider) {
            m.share = shareSlider;
            m.shareMeter = attachTrackMeter(shareSlider);
            m.shareMeter->setTint(shareAccent);
        }
        meters_.append(m);

        // The table is one column now, so this runs the whole width.
        if (i + 1 < apps.size()) {
            auto *sep = new QFrame(cell);
            sep->setFrameShape(QFrame::HLine);
            sep->setFrameShadow(QFrame::Plain);
            sep->setFixedHeight(1);
            Theme::follow(sep, [sep] {
                sep->setStyleSheet(QStringLiteral("background: %1; border: none;")
                                       .arg(Theme::css(Theme::Line)));
            });
            cl->addSpacing(2);
            cl->addWidget(sep);
        }

        table_->setCellWidget(row, 0, cell);
        table_->setRowHeight(row, cell->sizeHint().height() + 4);
    }
}

void SoundSharingTab::setAppLevels(const QHash<uint, double> &levels) {
    for (const RowMeters &m : std::as_const(meters_)) {
        const double pre = levels.value(m.nodeId, 0.0);
        // The playback slider goes to 150%; the bar stops at full.
        if (m.appMeter)
            m.appMeter->setLevel(std::min(1.0, pre * m.app->value() / 100.0));
        if (m.shareMeter)
            m.shareMeter->setLevel(m.shared ? pre * m.share->value() / 100.0 : 0.0);
    }
}

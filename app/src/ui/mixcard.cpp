// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "mixcard.h"

#include <algorithm>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "levelmeter.h"
#include "theme.h"

namespace {

// Half a channel card, near enough: a centred fader, with room either side of
// it for the "+" and "-" on the right without covering the fader.
constexpr int kMixCardWidth = 80;
constexpr int kButtonInset = 3;
constexpr int kButtonGap = 4;
// The meter standing in for the fader's track: wider than the bare groove, so
// the level reads, and narrower than the handle, so the handle covers it.
constexpr int kMeterTrackWidth = 8;

}  // namespace

MixCard::MixCard(Kind kind, QWidget *parent) : CardBase(parent), kind_(kind) {
    // A floor: with room to spare the window widens it with the rest.
    setMinimumWidth(kMixCardWidth);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setRadius(12);
    // The channel cards' lighter fill, and like them an outline in the card's
    // own colour -- see setIdentity().
    setFillColor(Theme::CardHover);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 12, 6, 10);
    lay->setSpacing(8);

    // The tile, then the fader with the level meter as its track.
    tile_ = new IconTile(QString(), Theme::Inactive, this);
    if (kind_ == Kind::Stream) {
        tile_->setOnClicked([this] { emit identityClicked(); });
    } else {
        // The window takes the clicks and drags (see handle()); the tile only
        // has to look like it can be pressed.
        tile_->setCursor(Qt::PointingHandCursor);
    }
    lay->addWidget(tile_, 0, Qt::AlignHCenter);

    // The level meter is the fader's track: the bar rises behind the handle,
    // so one strip says both where the level is set and what is coming out.
    // Centred like everything else on the card. The "+" and "-" sit in the
    // card's right margin, clear of it: see kMixCardWidth.
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->addStretch(1);
    fader_ = new Fader(this);
    fader_->setAccent(Theme::Accent);
    fader_->setToolTip(kind_ == Kind::Monitor ? tr("Level for this Monitor mix output.")
                                              : tr("Level of the Stream mix virtual device."));
    body->addWidget(fader_);
    body->addStretch(1);
    lay->addLayout(body, 1);

    meter_ = attachTrackMeter(fader_, kMeterTrackWidth);

    value_ = new QLabel(QStringLiteral("100"), this);
    value_->setAlignment(Qt::AlignCenter);
    QFont vf = value_->font();
    vf.setPointSizeF(vf.pointSizeF() * 0.85);
    value_->setFont(vf);
    Theme::setTextColor(value_, Theme::TextDim);
    lay->addWidget(value_);

    mute_ = kind_ == Kind::Monitor
                ? new IconToggle(QStringLiteral("speaker-muted"), QStringLiteral("speaker-high"), this)
                : new IconToggle(QStringLiteral("stream-off"), QStringLiteral("stream"), this);
    // Checked means muted, so it lights in the warning colour, as the channel
    // cards' mutes do.
    mute_->setAccent(Theme::Danger);
    mute_->setIconSize(17);
    mute_->setToolTip(kind_ == Kind::Monitor ? tr("Mute this Monitor mix output only.")
                                             : tr("Mute the Stream mix virtual device."));
    lay->addWidget(mute_, 0, Qt::AlignHCenter);

    connect(fader_, &QSlider::valueChanged, this, [this](int v) {
        value_->setText(QString::number(v));
        emit volumeChanged(v / 100.0);
    });
    connect(mute_, &QAbstractButton::toggled, this, [this](bool muted) {
        fader_->setMuted(muted);
        emit muteToggled(muted);
    });

    applyLook();
}

void MixCard::setIdentity(const QString &icon, const QColor &color) {
    icon_ = icon;
    color_ = color;
    applyLook();
}

void MixCard::setAddButton(QPushButton *button) {
    if (add_ == button) return;
    add_ = button;
    if (add_) {
        add_->setParent(this);
        add_->show();
    }
    placeButtons();
    // Again once the card is laid out, in case it is not yet: the place is
    // worked out from where the fader is.
    QTimer::singleShot(0, this, &MixCard::placeButtons);
}

void MixCard::setRemoveButton(QPushButton *button) {
    if (remove_ == button) return;
    remove_ = button;
    if (remove_) {
        remove_->setParent(this);
        remove_->show();
    }
    placeButtons();
    // Again once the card is laid out, in case it is not yet: the place is
    // worked out from where the fader is.
    QTimer::singleShot(0, this, &MixCard::placeButtons);
}

void MixCard::resizeEvent(QResizeEvent *event) {
    CardBase::resizeEvent(event);
    placeButtons();
}

void MixCard::placeButtons() {
    // Centred on the fader: a "+" and "-" as a pair, so a lone "-" on the other
    // cards lands level with the middle of it and the row of them lines up.
    const QPoint faderTop = fader_->mapTo(this, QPoint(0, 0));
    const int middle = faderTop.y() + fader_->height() / 2;
    if (add_ && remove_) {
        const int side = add_->width();
        const int top = middle - side - kButtonGap / 2;
        add_->move(width() - side - kButtonInset, top);
        remove_->move(width() - remove_->width() - kButtonInset, top + side + kButtonGap);
    } else if (QPushButton *only = add_ ? add_ : remove_) {
        only->move(width() - only->width() - kButtonInset, middle - only->height() / 2);
    }
    if (add_) add_->raise();
    if (remove_) remove_->raise();
}

void MixCard::setDescription(const QString &text) {
    description_ = text;
    applyLook();
}

void MixCard::setConnected(bool connected) {
    if (connected == connected_) return;
    connected_ = connected;
    applyLook();
}

void MixCard::applyLook() {
    // Grey while the device is unplugged, as an input card goes.
    const QColor shown = connected_ && color_.isValid() ? color_ : Theme::Inactive;
    tile_->setAppearance(icon_, shown);
    setBorderColor(shown);
    fader_->setAccent(color_.isValid() ? shown : Theme::Accent);
    // The level in the card's colour, as a channel card's is.
    meter_->setTint(shown);
    fader_->setEnabled(connected_);
    mute_->setEnabled(connected_);
    meter_->setEnabled(connected_);
    if (!connected_) meter_->setLevel(0.0);

    QString tip = description_;
    if (kind_ == Kind::Monitor) {
        if (!connected_) tip += tr(" (disconnected)");
        tip += QLatin1Char('\n') +
               tr("Click to choose the device, colour and icon. Drag onto another "
                  "Monitor mix to swap their devices.");
    } else {
        tip += QLatin1Char('\n') + tr("Click to change its colour.");
    }
    tile_->setToolTip(tip);
}

void MixCard::setVolume(double volume) {
    if (fader_->isSliderDown() || fader_->hasFocus()) return;
    const int v = std::clamp(int(volume * 100.0 + 0.5), 0, 100);
    if (v == fader_->value()) return;
    QSignalBlocker block(fader_);
    fader_->setValue(v);
    value_->setText(QString::number(v));
}

void MixCard::setMuted(bool muted) {
    if (muted == mute_->isChecked()) return;
    QSignalBlocker block(mute_);
    mute_->setChecked(muted);
    fader_->setMuted(muted);
}

double MixCard::volume() const { return fader_->value() / 100.0; }
bool MixCard::muted() const { return mute_->isChecked(); }

void MixCard::setLevel(double preFader) {
    if (!connected_ || muted()) {
        meter_->setLevel(0.0);
        return;
    }
    meter_->setLevel(preFader * volume());
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// A mix as a card in the row, after the channels: a Monitor mix output, or the
// Stream mix. The same idea as an input device's card, cut down to what a mix
// has -- a level, a mute and a meter -- so it is half a channel card wide.
//
// It carries no name and no number. A tile says what it plays on --
// headphones, speakers, a headset, earbuds, in a colour of its own, both the
// user's to change. On a Monitor mix the tile is also the handle: clicked, it
// opens the menu of devices (and the colour and icon, and Remove). The card,
// by its tile or its body, can be moved along the row to reorder the mixes.
// The device is in its tooltip. The window owns everything about which device that is: the card
// only reports clicks, drags of its tile, and its own fader and mute.

#pragma once

#include "widgets.h"

class Fader;
class IconToggle;
class LevelMeter;
class QLabel;
class QPushButton;

class MixCard : public CardBase {
    Q_OBJECT

public:
    enum class Kind { Monitor, Stream };

    MixCard(Kind kind, QWidget *parent = nullptr);

    Kind kind() const { return kind_; }

    // The tile's glyph and colour, which the outline wears too.
    void setIdentity(const QString &icon, const QColor &color);
    // What the tile's tooltip says, above the hint for what it does.
    void setDescription(const QString &text);
    // An unplugged device keeps its card -- the assignment survives -- but the
    // card goes grey and its controls stop answering, as an input's does.
    void setConnected(bool connected);
    bool connected() const { return connected_; }

    // From the daemon. Ignored while the fader is in the hand: the daemon is a
    // poll behind and would pull it back to where it was.
    void setVolume(double volume);
    void setMuted(bool muted);
    double volume() const;
    bool muted() const;

    // Pre-fader level of what this mix carries, 0..1, already on the meter's
    // curve. The card applies its own fader and mute, so the bar shows what
    // leaves for the device rather than the shared bus.
    void setLevel(double preFader);

    // A Monitor mix's tile, which the window watches: a click opens the
    // device menu, a drag swaps. The Stream mix's tile edits its colour itself.
    QWidget *handle() const { return tile_; }

    // The "+" that adds a Monitor mix, on the last one, and the "-" that
    // removes this one, on every one but #1. Pinned to the card's right edge
    // level with the fader, the "-" under the "+", the way an input device's
    // card carries its own. The card takes them over; nullptr takes one away.
    void setAddButton(QPushButton *button);
    void setRemoveButton(QPushButton *button);

signals:
    // The Stream mix's tile was clicked: open its colour.
    void identityClicked();
    void volumeChanged(double volume);
    void muteToggled(bool muted);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void applyLook();
    void placeButtons();

    Kind kind_;
    bool connected_ = true;
    IconTile *tile_ = nullptr;
    QString icon_;
    QColor color_;
    QString description_;
    LevelMeter *meter_ = nullptr;
    Fader *fader_ = nullptr;
    QLabel *value_ = nullptr;
    IconToggle *mute_ = nullptr;
    QPushButton *add_ = nullptr;
    QPushButton *remove_ = nullptr;
};

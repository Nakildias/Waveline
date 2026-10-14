// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Moving a card along its row, inside the window: the card lifts out of the
// layout and follows the pointer, and each neighbour it passes slides over
// into the gap it left. On release it settles into the gap where it ended up.
//
// This used to be a QDrag. A drag hands the pointer to the compositor, which
// draws a picture of the card where the card was, so the row could only jump
// from one order to the next; here the card itself is what moves, and nothing
// leaves the window.
//
// The caller owns the mouse: it starts, moves and ends a shuffle from its own
// press, move and release, and is told when the card has settled so it can
// keep the new order. The cards a card may trade places with -- the input
// devices, the channels, the Monitor mixes -- sit side by side in one layout
// and are handed over as a group; nothing outside the group moves.

#pragma once

#include <functional>

#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>

class QBoxLayout;
class QScrollArea;
class QTimer;
class QWidget;

class CardShuffle : public QObject {
public:
    explicit CardShuffle(QObject *parent = nullptr);

    // Scrolled while the pointer holds near either end of its viewport, so a
    // card can be taken past what is on screen.
    void setScrollArea(QScrollArea *area);

    // True from begin() until the card has settled.
    bool busy() const;

    // Lifts `card` out of `layout`. `group` is every card it may trade places
    // with, `card` among them, all in `layout`. `globalPress` is where the
    // pointer went down, so the card stays held by the same spot. `settled`
    // runs once the card is back in the layout, in its new place -- or in its
    // old one, if it never moved.
    void begin(QBoxLayout *layout, QWidget *card, const QList<QWidget *> &group,
               const QPoint &globalPress, std::function<void()> settled);
    void move(const QPoint &globalPos);
    void end();

private:
    // The group as the layout has it now, with the gap standing in for the
    // card being moved.
    QList<QWidget *> sequence() const;
    // Lays the row out again and slides every card that moved from where it
    // was to where it now belongs.
    void relayoutAnimated();
    void slide(QWidget *w, const QPoint &from, const QPoint &to, int ms);
    void autoScroll();
    void finish();
    // The card went away mid-move (its channel was removed): drop the gap and
    // forget it, without telling anyone it settled.
    void abort();

    QPointer<QBoxLayout> layout_;
    QPointer<QWidget> card_;
    QPointer<QWidget> gap_;
    QList<QPointer<QWidget>> group_;
    std::function<void()> settled_;
    QPoint grab_;   // where on the card it is held
    QPoint last_;   // the pointer, globally, at the last move
    int stretch_ = 0;
    bool settling_ = false;
    QMetaObject::Connection destroyed_;

    QPointer<QScrollArea> scroll_;
    QTimer *scrollTimer_ = nullptr;
    int scrollStep_ = 0;
};

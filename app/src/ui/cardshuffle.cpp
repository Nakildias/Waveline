// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "cardshuffle.h"

#include <algorithm>

#include <QBoxLayout>
#include <QEasingCurve>
#include <QHash>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QWidget>

namespace {
constexpr int kSlideMs = 180;
constexpr int kSettleMs = 160;
// How close to the viewport's end the pointer has to be before the row
// scrolls, and how fast it goes right at the edge.
constexpr int kScrollEdge = 56;
constexpr int kScrollMaxStep = 18;
const char kSlideName[] = "cardShuffleSlide";
}  // namespace

CardShuffle::CardShuffle(QObject *parent) : QObject(parent) {
    scrollTimer_ = new QTimer(this);
    scrollTimer_->setInterval(16);
    connect(scrollTimer_, &QTimer::timeout, this, [this] { autoScroll(); });
}

void CardShuffle::setScrollArea(QScrollArea *area) { scroll_ = area; }

bool CardShuffle::busy() const { return card_ || settling_; }

void CardShuffle::begin(QBoxLayout *layout, QWidget *card, const QList<QWidget *> &group,
                        const QPoint &globalPress, std::function<void()> settled) {
    if (busy() || !layout || !card || group.size() < 2) return;
    const int at = layout->indexOf(card);
    QWidget *host = card->parentWidget();
    if (at < 0 || !host) return;

    layout_ = layout;
    card_ = card;
    group_.clear();
    for (QWidget *w : group) group_ << w;
    settled_ = std::move(settled);
    grab_ = card->mapFromGlobal(globalPress);
    last_ = globalPress;
    stretch_ = layout->stretch(at);

    // A gap the card's size holds its place. Fixed, so the cards either side
    // of it lay out exactly as they did with the card there.
    const QRect start = card->geometry();
    gap_ = new QWidget(host);
    gap_->setAttribute(Qt::WA_TransparentForMouseEvents);
    gap_->setFixedSize(start.size());
    layout->insertWidget(at, gap_, 0);
    layout->removeWidget(card);
    gap_->show();
    card->setGeometry(start);
    card->raise();

    destroyed_ = connect(card, &QObject::destroyed, this, [this] { abort(); });
}

QList<QWidget *> CardShuffle::sequence() const {
    QList<QWidget *> out;
    if (!layout_) return out;
    for (int i = 0; i < layout_->count(); ++i) {
        QWidget *w = layout_->itemAt(i)->widget();
        if (!w) continue;
        if (w == gap_) {
            out << w;
            continue;
        }
        for (const QPointer<QWidget> &g : group_)
            if (g == w) {
                out << w;
                break;
            }
    }
    return out;
}

void CardShuffle::move(const QPoint &globalPos) {
    if (!card_ || settling_ || !gap_ || !layout_) return;
    last_ = globalPos;
    QWidget *host = card_->parentWidget();

    // Where each card belongs, rather than where it is: one sliding past is
    // still headed for its slot, and that slot is what the card is compared
    // against.
    const QList<QWidget *> seq = sequence();
    if (seq.isEmpty()) return;
    QHash<QWidget *, QRect> home;
    for (QWidget *w : seq) {
        QRect r = w->geometry();
        if (auto *a = w->findChild<QPropertyAnimation *>(QLatin1String(kSlideName),
                                                         Qt::FindDirectChildrenOnly);
            a && a->state() == QAbstractAnimation::Running)
            r.moveTopLeft(a->endValue().toPoint());
        home.insert(w, r);
    }

    // Along the row only, and no further than the group reaches.
    int lo = home.value(seq.first()).left();
    int hi = home.value(seq.last()).right() + 1 - card_->width();
    int x = host->mapFromGlobal(globalPos).x() - grab_.x();
    x = std::clamp(x, lo, std::max(lo, hi));
    card_->move(x, home.value(gap_).top());

    // Once the card's leading edge is past a neighbour's middle, the two trade
    // places. The edge rather than the card's own middle: the card cannot go
    // past the end of the group, so with its middle it could never pass the
    // last card. As many times as it takes: a quick throw can pass several.
    const int left = x;
    const int right = x + card_->width();
    QList<QWidget *> order = seq;
    int idx = order.indexOf(gap_);
    const int was = idx;
    // One direction per move. The homes are where the cards were before this
    // move, so a card just passed still sits on the far side by them -- asked
    // both ways, the card would pass it back and forth forever.
    while (idx + 1 < order.size() && right > home.value(order[idx + 1]).center().x()) {
        order.swapItemsAt(idx, idx + 1);
        ++idx;
    }
    if (idx == was) {
        while (idx > 0 && left < home.value(order[idx - 1]).center().x()) {
            order.swapItemsAt(idx, idx - 1);
            --idx;
        }
    }
    if (idx != was) {
        layout_->removeWidget(gap_);
        const int to = idx == 0 ? layout_->indexOf(order[1])
                                : layout_->indexOf(order[idx - 1]) + 1;
        layout_->insertWidget(to, gap_, 0);
        relayoutAnimated();
    }

    if (scroll_) {
        const int px = scroll_->viewport()->mapFromGlobal(globalPos).x();
        const int w = scroll_->viewport()->width();
        if (px < kScrollEdge)
            scrollStep_ = -std::max(1, kScrollMaxStep * (kScrollEdge - px) / kScrollEdge);
        else if (px > w - kScrollEdge)
            scrollStep_ = std::max(1, kScrollMaxStep * (px - (w - kScrollEdge)) / kScrollEdge);
        else
            scrollStep_ = 0;
        if (scrollStep_ && !scrollTimer_->isActive()) scrollTimer_->start();
        else if (!scrollStep_) scrollTimer_->stop();
    }
}

void CardShuffle::relayoutAnimated() {
    QHash<QWidget *, QPoint> before;
    for (const QPointer<QWidget> &w : group_) {
        if (!w || w == card_) continue;
        // One already sliding starts again from wherever it has got to.
        if (auto *a = w->findChild<QPropertyAnimation *>(QLatin1String(kSlideName),
                                                         Qt::FindDirectChildrenOnly))
            a->stop();
        before.insert(w, w->pos());
    }
    // Lays the row out now rather than on the next pass, so where every card
    // belongs is known here.
    QWidget *host = gap_->parentWidget();
    if (host && host->layout()) host->layout()->activate();
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        QWidget *w = it.key();
        const QPoint to = w->pos();
        if (to == it.value()) continue;
        w->move(it.value());
        slide(w, it.value(), to, kSlideMs);
    }
    if (card_) card_->raise();
}

void CardShuffle::slide(QWidget *w, const QPoint &from, const QPoint &to, int ms) {
    auto *a = w->findChild<QPropertyAnimation *>(QLatin1String(kSlideName),
                                                 Qt::FindDirectChildrenOnly);
    if (!a) {
        // A child of the card, so it goes with it.
        a = new QPropertyAnimation(w, "pos", w);
        a->setObjectName(QLatin1String(kSlideName));
        a->setEasingCurve(QEasingCurve::OutCubic);
    }
    a->stop();
    a->setDuration(ms);
    a->setStartValue(from);
    a->setEndValue(to);
    a->start();
}

void CardShuffle::autoScroll() {
    if (!scroll_ || !card_ || settling_ || !scrollStep_) {
        scrollTimer_->stop();
        return;
    }
    QScrollBar *bar = scroll_->horizontalScrollBar();
    const int before = bar->value();
    bar->setValue(before + scrollStep_);
    if (bar->value() == before) {
        scrollTimer_->stop();
        return;
    }
    // The row moved under a pointer that did not: the card goes with the
    // pointer, past whatever has just scrolled into reach.
    move(last_);
}

void CardShuffle::end() {
    if (!card_ || settling_ || !gap_) return;
    scrollTimer_->stop();
    scrollStep_ = 0;
    settling_ = true;
    QWidget *card = card_;
    slide(card, card->pos(), gap_->pos(), kSettleMs);
    auto *a = card->findChild<QPropertyAnimation *>(QLatin1String(kSlideName),
                                                    Qt::FindDirectChildrenOnly);
    connect(a, &QAbstractAnimation::finished, this, [this] { finish(); },
            Qt::SingleShotConnection);
}

void CardShuffle::finish() {
    if (!card_ || !gap_ || !layout_) {
        abort();
        return;
    }
    disconnect(destroyed_);
    const int at = layout_->indexOf(gap_);
    layout_->insertWidget(at, card_, stretch_);
    layout_->removeWidget(gap_);
    gap_->deleteLater();

    auto settled = std::move(settled_);
    settled_ = nullptr;
    card_ = nullptr;
    gap_ = nullptr;
    group_.clear();
    settling_ = false;
    if (settled) settled();
}

void CardShuffle::abort() {
    disconnect(destroyed_);
    scrollTimer_->stop();
    scrollStep_ = 0;
    if (gap_) {
        if (layout_) layout_->removeWidget(gap_);
        gap_->deleteLater();
    }
    card_ = nullptr;
    gap_ = nullptr;
    group_.clear();
    settled_ = nullptr;
    settling_ = false;
}

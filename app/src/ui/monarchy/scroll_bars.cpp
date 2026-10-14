// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "scroll_bars.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QEvent>

#include "desktop.h"
#include "scroll_bar_auto_hide.h"

namespace Monarchy {

namespace {

class ScrollBarFollower : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::Polish) return false;
        auto *area = qobject_cast<QAbstractScrollArea *>(watched);
        if (!area) return false;
        // A combo box's list and the like live in popups, which come and go
        // with a click and are styled by the shell's own menus: left alone.
        if (area->window()->windowFlags().testFlag(Qt::Popup)) return false;
        ScrollBarAutoHide::attach(area, Qt::Vertical);
        ScrollBarAutoHide::attach(area, Qt::Horizontal);
        return false;
    }
};

}  // namespace

void followScrollBarSetting() {
    if (!isActive() || !qApp) return;
    qApp->installEventFilter(new ScrollBarFollower(qApp));
}

}  // namespace Monarchy

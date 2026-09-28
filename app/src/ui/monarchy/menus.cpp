// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "menus.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProxyStyle>
#include <QScreen>
#include <QSet>
#include <QStyleOptionMenuItem>
#include <QVariantAnimation>
#include <QWindow>

#include <algorithm>
#include <cmath>

#include "desktop.h"
#include "../theme.h"

#ifdef WAVELINE_HAVE_KWINDOWSYSTEM
#include <KWindowEffects>
#endif

namespace Monarchy {

namespace {

// The shell menu's layout, value for value -- DesktopMenu's in Monarchy.
constexpr int kMenuMargin = 6;
constexpr int kRowLeft = 10;
constexpr int kRowRight = 8;
constexpr int kRowPaddingV = 8;
constexpr int kRowSpacing = 6;
constexpr int kRowGap = 2;
constexpr int kCheckColumn = 16;
constexpr int kArrowColumn = 14;
constexpr int kIconSize = 16;
constexpr int kSeparatorHeight = 9;
constexpr int kSubmenuOverlap = 6;
constexpr int kSeparatorInset = 10;
constexpr int kShortcutGap = 24;
// A context menu of short labels is never narrower than this.
constexpr int kContextMenuMinWidth = 190;
// How far under its control a dropdown's card starts.
constexpr int kDropGap = 4;

const QColor kCheckMark(0x34, 0x78, 0xf6);
constexpr qreal kDisabledOpacity = 0.35;

// The opening: out of a smaller menu at the pointer (or out of the control a
// dropdown hangs under), past full size and back.
constexpr int kPopDurationMs = 460;
constexpr qreal kPopOvershoot = 1.70158;
constexpr qreal kPopStartScale = 0.6;
// Room past the card for the overshoot, as the popup's contents margins:
// towards the pointer's right and down, or even on both sides for a dropdown
// that grows out of the middle of its control.
constexpr int kPopRoomLeft = 1;
constexpr int kPopRoomTop = 1;
constexpr int kPopRoomRight = 12;
constexpr int kPopRoomBottom = 18;
constexpr int kPopRoomCentredLeft = kPopRoomRight;

constexpr char kRowsOnlyProperty[] = "_shell_menu_rows_only";
constexpr char kPopOriginProperty[] = "_shell_menu_pop_origin";
constexpr char kStyledProperty[] = "_shell_menu_styled";
constexpr char kPopProperty[] = "_shell_menu_pop";

enum class CheckedRow { Tick, AccentText };

int cornerRadiusFor(int width, int height) {
    const int r = Settings::instance().kwinCornerRadius();
    if (r <= 0 || width <= 0 || height <= 0) return 0;
    return std::min(r, std::min(width, height) / 2);
}

QString withoutMnemonics(const QString &text) {
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('&')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('&')) {
                out.append(QLatin1Char('&'));
                ++i;
            }
            continue;
        }
        out.append(text.at(i));
    }
    return out;
}

void splitLabel(const QString &packed, QString *label, QString *shortcut) {
    const int tab = packed.indexOf(QLatin1Char('\t'));
    *label = withoutMnemonics(tab < 0 ? packed : packed.left(tab));
    *shortcut = tab < 0 ? QString() : packed.mid(tab + 1);
}

// ------------------------------------------------------------------ surface
// Monarchy's shellstyle/shell_surface_paint.h: the fill with its sheen, and
// the rim grown inwards from the edge along the shell's fade curve.

struct Ink {
    QColor text, secondary, hover, separator;
};

Ink inkFor(const QColor &fill) {
    const bool light = 0.299 * fill.redF() + 0.587 * fill.greenF() + 0.114 * fill.blueF() > 0.5;
    if (light)
        return {QColor(0x1d, 0x1d, 0x1f), QColor(0x6e, 0x6e, 0x73), QColor(0, 0, 0, 22),
                QColor(0, 0, 0, 45)};
    return {QColor(0xf5, 0xf5, 0xf7), QColor(0xd0, 0xd0, 0xd4), QColor(255, 255, 255, 30),
            QColor(255, 255, 255, 90)};
}

QBrush fillBrush(const QRectF &box, const Settings::MenuSurface &look) {
    if (look.fadeTop <= 0.0 && look.fadeBottom <= 0.0) return QBrush(look.fill);
    const auto stop = [&look](qreal amount) {
        QColor c = look.fill;
        const qreal keep = 1.0 - amount;
        c.setRed(qRound(c.red() * keep + 255 * amount));
        c.setGreen(qRound(c.green() * keep + 255 * amount));
        c.setBlue(qRound(c.blue() * keep + 255 * amount));
        return c;
    };
    QLinearGradient g(box.left(), box.top(), box.left(), box.bottom());
    g.setColorAt(0.0, stop(look.fadeTop));
    g.setColorAt(1.0, stop(look.fadeBottom));
    return QBrush(g);
}

QBrush rimBrush(const QRectF &box, const Settings::MenuSurface &look) {
    const qreal top = look.borderTop;
    const qreal span = look.borderBottom - top;
    const auto stop = [&](qreal t) {
        QColor c = look.borderColour;
        c.setAlphaF(float(top + span * t));
        return c;
    };
    QLinearGradient g(box.left(), box.top(), box.left(), box.bottom());
    g.setColorAt(0.0, stop(0.0));
    g.setColorAt(0.15, stop(0.428571));
    g.setColorAt(0.5, stop(0.771429));
    g.setColorAt(1.0, stop(1.0));
    return QBrush(g);
}

void paintSurface(QPainter &p, const QRectF &box, qreal radius) {
    const Settings::MenuSurface &look = Settings::instance().menuSurface();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath body;
    body.addRoundedRect(box, radius, radius);
    p.fillPath(body, fillBrush(box, look));
    if (look.borderWidth > 0.0) {
        const qreal inset = look.borderWidth / 2.0;
        const QRectF stroke = box.adjusted(inset, inset, -inset, -inset);
        if (!stroke.isEmpty()) {
            QPainterPath rim;
            const qreal r = std::max(qreal(0.0), radius - inset);
            rim.addRoundedRect(stroke, r, r);
            p.setPen(QPen(rimBrush(box, look), look.borderWidth));
            p.setBrush(Qt::NoBrush);
            p.drawPath(rim);
        }
    }
    p.restore();
}

// The pixels a rounded surface covers, row by row off the arc, taking a pixel
// when its centre is inside -- the blur region and input mask. Not a flattened
// QPainterPath, whose faceted corners chew the rim.
QRegion roundedRegion(const QRect &rect, int cornerRadius) {
    if (!rect.isValid()) return {};
    const int radius = std::min(cornerRadius, std::min(rect.width(), rect.height()) / 2);
    if (radius <= 0) return QRegion(rect);
    const auto insetAt = [radius](double d) {
        if (d < 0) return 0;
        if (d >= radius) return radius;
        const double inner = double(radius) * radius - d * d;
        if (inner <= 0.0) return radius;
        return std::max(0, int(std::ceil(double(radius) - std::sqrt(inner) - 0.5)));
    };
    QRegion region;
    for (int row = 0; row < rect.height(); ++row) {
        int cut = 0;
        if (row < radius) cut = std::max(cut, insetAt(radius - row - 0.5));
        if (row >= rect.height() - radius)
            cut = std::max(cut, insetAt(row - (rect.height() - radius) + 0.5));
        const int span = rect.width() - 2 * cut;
        if (span > 0) region += QRect(rect.x() + cut, rect.y() + row, span, 1);
    }
    return region;
}

// ---------------------------------------------------------------- the style

class ShellMenuStyle final : public QProxyStyle {
public:
    // Proxying Fusion, which the mixer draws everything else with anyway.
    //
    // Only the metrics are taken from here in practice. The mixer sets an
    // application style sheet, and Qt wraps any style set on a widget in its
    // style sheet style, whose rules for QWidget then paint the panel and the
    // rows. So MenuPop paints the whole menu itself, through paintChrome()
    // and paintRow() below, and the style's own drawing is the fallback.
    ShellMenuStyle(CheckedRow checked, int minimumWidth, QObject *parent)
        : QProxyStyle(QStringLiteral("Fusion")), checked_(checked),
          minimumWidth_(minimumWidth) {
        setParent(parent);
    }

    void polish(QWidget *) override {}
    void unpolish(QWidget *) override {}

    int pixelMetric(PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override {
        switch (metric) {
        case PM_MenuPanelWidth:
        case PM_MenuBarPanelWidth:
            return 0;   // the rim is painted inside the popup
        case PM_MenuHMargin:
        case PM_MenuVMargin:
            return kMenuMargin;
        case PM_SubMenuOverlap:
            return kSubmenuOverlap - kPopRoomLeft;
        default:
            return QProxyStyle::pixelMetric(metric, option, widget);
        }
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size,
                           const QWidget *widget) const override {
        const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
        if (type != CT_MenuItem || !item)
            return QProxyStyle::sizeFromContents(type, option, size, widget);
        if (item->menuItemType == QStyleOptionMenuItem::Separator)
            return QSize(size.width(), kSeparatorHeight);

        QString label, shortcut;
        splitLabel(item->text, &label, &shortcut);
        const QFontMetrics metrics(item->font);
        // Rounded up: a label 79.4 px wide handed a 79 px row is elided.
        const QFontMetricsF exact(item->font);
        const auto advance = [&exact](const QString &t) {
            return int(std::ceil(exact.horizontalAdvance(t)));
        };
        int width = kRowLeft + advance(label) + kRowRight;
        if (item->menuHasCheckableItems && checked_ == CheckedRow::Tick)
            width += kCheckColumn + kRowSpacing;
        if (!item->icon.isNull()) width += kIconSize + kRowSpacing;
        if (!shortcut.isEmpty()) width += kShortcutGap + advance(shortcut);
        if (item->menuItemType == QStyleOptionMenuItem::SubMenu)
            width += kRowSpacing + kArrowColumn;
        const int height = std::max(metrics.height(), kIconSize) + 2 * kRowPaddingV + kRowGap;
        // The floor on the row, not the widget: QMenu lays every row out to
        // the widest, so the hover pill reaches the edge of a widened menu.
        return QSize(std::max(width, minimumWidth_ - 2 * kMenuMargin), height);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override {
        if (element == PE_PanelMenu) {
            if (widget && widget->property(kRowsOnlyProperty).toBool()) return;
            paintChrome(*painter, widget ? widget->contentsRect() : option->rect);
            return;
        }
        if (element == PE_FrameMenu) return;
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget) const override {
        if (element == CE_MenuEmptyArea) return;
        const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
        if (element != CE_MenuItem || !item) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }
        paintRow(*item, painter);
    }

    void paintChrome(QPainter &painter, const QRect &frame) const {
        const QRectF box = QRectF(frame).adjusted(0.0, 0.0, -1.0, -1.0);
        if (box.isEmpty()) return;
        paintSurface(painter, box, cornerRadiusFor(frame.width(), frame.height()));
    }

    void paintRow(const QStyleOptionMenuItem &item, QPainter *painter) const {
        const Ink ink = inkFor(Settings::instance().menuSurface().fill);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        if (item.menuItemType == QStyleOptionMenuItem::Separator) {
            painter->setPen(QPen(ink.separator, 1));
            const int y = item.rect.center().y();
            painter->drawLine(item.rect.left() + kSeparatorInset, y,
                              item.rect.right() - kSeparatorInset, y);
            painter->restore();
            return;
        }

        const bool enabled = item.state & QStyle::State_Enabled;
        if ((item.state & QStyle::State_Selected) && enabled) {
            const QRectF pill = QRectF(item.rect.adjusted(2, 1, -2, -1));
            painter->setPen(Qt::NoPen);
            painter->setBrush(ink.hover);
            painter->drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
        }

        QRect row = item.rect.adjusted(kRowLeft, kRowPaddingV, -kRowRight,
                                       -kRowPaddingV - kRowGap);
        if (!enabled) painter->setOpacity(kDisabledOpacity);

        if (item.menuHasCheckableItems && checked_ == CheckedRow::Tick) {
            const QRect box(row.left(), row.top(), kCheckColumn, row.height());
            if (item.checked) {
                painter->setPen(kCheckMark);
                painter->drawText(box, Qt::AlignCenter, QStringLiteral("✓"));
            }
            row.setLeft(box.right() + 1 + kRowSpacing);
        }
        if (!item.icon.isNull()) {
            const QRect box(row.left(), row.top() + (row.height() - kIconSize) / 2,
                            kIconSize, kIconSize);
            item.icon.paint(painter, box, Qt::AlignCenter,
                            enabled ? QIcon::Normal : QIcon::Disabled);
            row.setLeft(box.right() + 1 + kRowSpacing);
        }
        if (item.menuItemType == QStyleOptionMenuItem::SubMenu) {
            const QRect box(row.right() - kArrowColumn + 1, row.top(), kArrowColumn,
                            row.height());
            painter->setPen(ink.secondary);
            painter->drawText(box, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("›"));
            row.setRight(box.left() - 1 - kRowSpacing);
        }

        QString label, shortcut;
        splitLabel(item.text, &label, &shortcut);
        if (!shortcut.isEmpty()) {
            painter->setPen(ink.secondary);
            painter->drawText(row, Qt::AlignRight | Qt::AlignVCenter, shortcut);
            row.setRight(row.right()
                         - int(std::ceil(QFontMetricsF(item.font).horizontalAdvance(shortcut)))
                         - kShortcutGap);
        }

        // A dropdown marks the chosen value by drawing it in the accent: every
        // row is a value, so a column of ticks would be empty space on all
        // but one.
        const bool accentRow = item.checked && checked_ == CheckedRow::AccentText;
        painter->setPen(accentRow ? Theme::Accent : ink.text);
        painter->setFont(item.font);
        painter->drawText(row, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(item.font).elidedText(label, Qt::ElideRight, row.width()));
        painter->restore();
    }

private:
    CheckedRow checked_;
    int minimumWidth_;
};

// ------------------------------------------------------------------ opening

class MenuPop final : public QObject {
public:
    explicit MenuPop(QMenu *menu, ShellMenuStyle *style)
        : QObject(menu), menu_(menu), style_(style) {
        animation_ = new QVariantAnimation(this);
        animation_->setDuration(kPopDurationMs);
        animation_->setStartValue(0.0);
        animation_->setEndValue(1.0);
        QEasingCurve curve(QEasingCurve::OutBack);
        curve.setOvershoot(kPopOvershoot);
        animation_->setEasingCurve(curve);
        connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            progress_ = v.toDouble();
            applyBlur();
            menu_->update();
        });
        connect(animation_, &QVariantAnimation::finished, this, [this] {
            stop();
            applyBlur();
            menu_->update();
        });
        menu->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched != menu_) {
            // A widget row of the menu: held back while the picture of it pops.
            return active_ && event->type() == QEvent::Paint
                   && held_.contains(static_cast<QWidget *>(watched));
        }
        switch (event->type()) {
        case QEvent::Show:
            // Again at every opening: a style reload clears these.
            menu_->setAttribute(Qt::WA_TranslucentBackground, true);
            menu_->setAttribute(Qt::WA_NoSystemBackground, true);
            if (!menu_->property(kPopOriginProperty).isValid())
                setRoom(false);
            start();
            applyBlur();
            break;
        case QEvent::Paint:
            // All of it painted here, not by QMenu -- see ShellMenuStyle.
            paint();
            return true;
        case QEvent::Hide:
            stop();
            break;
        case QEvent::Resize:
            applyBlur();
            break;
        default:
            break;
        }
        return false;
    }

public:
    // Centred room for a dropdown, the pointer's side otherwise.
    void setRoom(bool centred) {
        const QMargins room(centred ? kPopRoomCentredLeft : kPopRoomLeft, kPopRoomTop,
                            kPopRoomRight, kPopRoomBottom);
        if (menu_->contentsMargins() == room) return;
        menu_->setContentsMargins(room);
        // A hidden QMenu keeps its rows where the old margins put them until
        // it is shown, so sizeHint() would measure the old room.
        QEvent relayout(QEvent::StyleChange);
        QCoreApplication::sendEvent(menu_, &relayout);
    }

private:
    void start() {
        // A picture of the rows alone: the chrome is painted fresh at every
        // size, so its rim and corners keep their width while the rows scale.
        const QRect rest = menu_->contentsRect();
        const qreal dpr = menu_->devicePixelRatioF();
        snapshot_ = QPixmap(rest.size() * dpr);
        snapshot_.setDevicePixelRatio(dpr);
        snapshot_.fill(Qt::transparent);
        {
            QPainter p(&snapshot_);
            p.translate(-rest.topLeft());
            paintRows(p);
        }

        for (QWidget *child : menu_->findChildren<QWidget *>()) {
            if (child->window() != menu_ || held_.contains(child)) continue;
            held_.insert(child);
            child->installEventFilter(this);
            connect(child, &QObject::destroyed, this,
                    [this](QObject *gone) { held_.remove(static_cast<QWidget *>(gone)); });
        }

        const QVariant anchored = menu_->property(kPopOriginProperty);
        menu_->setProperty(kPopOriginProperty, QVariant());
        const QPoint from = menu_->mapFromGlobal(anchored.isValid() ? anchored.toPoint()
                                                                    : QCursor::pos());
        origin_ = QPoint(std::clamp(from.x(), rest.left(), rest.right() + 1),
                         std::clamp(from.y(), rest.top(), rest.bottom() + 1));
        progress_ = 0.0;
        active_ = true;
        animation_->stop();
        animation_->start();
    }

    void stop() {
        animation_->stop();
        active_ = false;
        snapshot_ = QPixmap();
    }

    QRect frame() const {
        const QRect rest = menu_->contentsRect();
        if (!active_) return rest;
        const qreal scale = kPopStartScale + (1.0 - kPopStartScale) * progress_;
        const auto at = [scale](qreal edge, qreal origin) {
            return qRound(origin + (edge - origin) * scale);
        };
        const int left = at(rest.x(), origin_.x());
        const int top = at(rest.y(), origin_.y());
        const int right = at(rest.x() + rest.width(), origin_.x());
        const int bottom = at(rest.y() + rest.height(), origin_.y());
        return QRect(QPoint(left, top), QPoint(right - 1, bottom - 1)).intersected(menu_->rect());
    }

    void paint() {
        QPainter painter(menu_);
        const QRect current = frame();
        style_->paintChrome(painter, current);
        if (!active_) {
            paintRows(painter);
        } else if (!snapshot_.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawPixmap(QRectF(current), snapshot_, QRectF(snapshot_.rect()));
        }
    }

    // What QMenu::paintEvent() does for its rows, with the option each one is
    // drawn from built here: QMenu::initStyleOption() is protected.
    void paintRows(QPainter &painter) {
        const QList<QAction *> actions = menu_->actions();
        const bool anyCheckable = std::any_of(actions.begin(), actions.end(),
                                              [](QAction *a) { return a->isCheckable(); });
        for (QAction *action : actions) {
            if (!action->isVisible()) continue;
            const QRect rect = menu_->actionGeometry(action);
            if (rect.isEmpty()) continue;
            QStyleOptionMenuItem o;
            o.initFrom(menu_);
            o.rect = rect;
            o.font = action->font().resolve(menu_->font());
            o.menuHasCheckableItems = anyCheckable;
            o.state = QStyle::State_None;
            if (action->isEnabled() && menu_->isEnabled()) o.state |= QStyle::State_Enabled;
            if (action == menu_->activeAction() && !action->isSeparator())
                o.state |= QStyle::State_Selected;
            if (action->isSeparator())
                o.menuItemType = QStyleOptionMenuItem::Separator;
            else if (action->menu())
                o.menuItemType = QStyleOptionMenuItem::SubMenu;
            else
                o.menuItemType = QStyleOptionMenuItem::Normal;
            o.checkType = action->isCheckable() ? QStyleOptionMenuItem::NonExclusive
                                                : QStyleOptionMenuItem::NotCheckable;
            o.checked = action->isChecked();
            o.icon = action->icon();
            o.text = action->text();
            if (!action->shortcut().isEmpty())
                o.text += QLatin1Char('\t')
                        + action->shortcut().toString(QKeySequence::NativeText);
            style_->paintRow(o, &painter);
        }
    }

    // The blur behind the card's own outline, following it through the
    // bounce; and on Wayland the input shape too, so a click in the bounce
    // room belongs to whatever is underneath.
    void applyBlur() {
        QWindow *window = menu_->windowHandle();
        if (!window) return;
        const QRect current = frame();
        const QRect painted = current.adjusted(0, 0, -1, -1);
        if (painted.isEmpty()) return;
#ifdef WAVELINE_HAVE_KWINDOWSYSTEM
        KWindowEffects::enableBlurBehind(
            window, true, roundedRegion(painted, cornerRadiusFor(painted.width(), painted.height())));
#endif
        if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland")))
            window->setMask(roundedRegion(current, cornerRadiusFor(current.width(), current.height())));
    }

    QMenu *menu_;
    ShellMenuStyle *style_;
    QVariantAnimation *animation_ = nullptr;
    QPixmap snapshot_;
    QSet<QWidget *> held_;
    QPoint origin_;
    qreal progress_ = 1.0;
    bool active_ = false;
};

MenuPop *popOf(QMenu *menu) {
    return static_cast<MenuPop *>(menu->property(kPopProperty).value<QObject *>());
}

void styleMenu(QMenu *menu, CheckedRow checked, int minimumWidth) {
    if (!menu || menu->property(kStyledProperty).toBool()) return;
    menu->setProperty(kStyledProperty, true);
    menu->setAttribute(Qt::WA_TranslucentBackground, true);
    menu->setAttribute(Qt::WA_NoSystemBackground, true);
    // Owned by the menu, so both go when it does.
    auto *style = new ShellMenuStyle(checked, minimumWidth, menu);
    menu->setStyle(style);
    menu->setContentsMargins(kPopRoomLeft, kPopRoomTop, kPopRoomRight, kPopRoomBottom);
    menu->setProperty(kPopProperty,
                      QVariant::fromValue(static_cast<QObject *>(new MenuPop(menu, style))));
    // Submenus are drawn the same way; one added later is caught when it is
    // polished, like any other menu.
    for (QMenu *child : menu->findChildren<QMenu *>()) styleMenu(child, checked, minimumWidth);
}

// Where to open `menu` so its card drops centred under `anchor`; its opening
// then grows out of the middle of the anchor's bottom edge.
QPoint dropUnder(QMenu *menu, QWidget *anchor, int gap) {
    menu->ensurePolished();
    if (MenuPop *pop = popOf(menu)) pop->setRoom(true);
    const QSize size = menu->sizeHint();
    const QMargins room = menu->contentsMargins();
    const int cardWidth = size.width() - room.left() - room.right();
    const QPoint below = anchor->mapToGlobal(QPoint(anchor->width() / 2, anchor->height()));
    QPoint at(below.x() - cardWidth / 2 - room.left(), below.y() + gap - room.top());
    if (const QScreen *screen = anchor->screen()) {
        const QRect area = screen->availableGeometry();
        at.setX(std::clamp(at.x(), area.left() - room.left(),
                           std::max(area.left() - room.left(),
                                    area.right() + 1 - size.width() + room.right())));
    }
    menu->setProperty(kPopOriginProperty, below);
    return at;
}

// --------------------------------------------------------------- the hooks

class ShellMenuHooks final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        switch (event->type()) {
        case QEvent::Polish:
            // Every menu, before it is measured and shown.
            if (auto *menu = qobject_cast<QMenu *>(watched))
                styleMenu(menu, CheckedRow::Tick, kContextMenuMinWidth);
            break;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: {
            auto *combo = qobject_cast<QComboBox *>(watched);
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (combo && !combo->isEditable() && mouse->button() == Qt::LeftButton) {
                showComboMenu(combo);
                return true;
            }
            break;
        }
        case QEvent::KeyPress: {
            auto *combo = qobject_cast<QComboBox *>(watched);
            if (!combo || combo->isEditable()) break;
            const auto *key = static_cast<QKeyEvent *>(event);
            const bool alt = key->modifiers() & Qt::AltModifier;
            // The keys QComboBox opens its list on.
            if (key->key() == Qt::Key_Space || key->key() == Qt::Key_F4
                || (alt && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up))) {
                showComboMenu(combo);
                return true;
            }
            break;
        }
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }
};

}  // namespace

void showComboMenu(QComboBox *combo) {
    // Built fresh each time and deleted on close: the items are the combo's
    // as they are now, and the combo's model changes under it at poll rate.
    auto *menu = new QMenu(combo);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    // Styled before anything polishes it, so it gets the dropdown's look --
    // the chosen value in the accent, at least as wide as the control -- and
    // not the context menu's.
    // The popup's own floor counts too: an icon-only combo (IconCombo) is
    // narrower than the names in its list.
    styleMenu(menu, CheckedRow::AccentText,
              std::max(combo->width(), combo->view()->minimumWidth()));
    menu->setFont(combo->font());

    const QAbstractItemModel *model = combo->model();
    const int column = combo->modelColumn();
    for (int i = 0; i < combo->count(); ++i) {
        const QModelIndex index = model->index(i, column, combo->rootModelIndex());
        // QComboBox::insertSeparator() marks its rows this way.
        if (index.data(Qt::AccessibleDescriptionRole).toString() == QLatin1String("separator")) {
            menu->addSeparator();
            continue;
        }
        QAction *action = menu->addAction(combo->itemIcon(i), combo->itemText(i));
        action->setCheckable(true);
        action->setChecked(i == combo->currentIndex());
        action->setEnabled(model->flags(index) & Qt::ItemIsEnabled);
        action->setToolTip(index.data(Qt::ToolTipRole).toString());
        QPointer<QComboBox> guard(combo);
        QObject::connect(action, &QAction::triggered, combo, [guard, i] {
            if (!guard) return;
            // What choosing from the list does: set it, then say the user did.
            guard->setCurrentIndex(i);
            emit guard->activated(i);
            emit guard->textActivated(guard->itemText(i));
        });
    }
    if (menu->actions().isEmpty()) {
        delete menu;
        return;
    }
    menu->popup(dropUnder(menu, combo, kDropGap));
}

void useShellMenus() {
    if (!isActive() || qApp->property("_waveline_shell_menus").toBool()) return;
    qApp->setProperty("_waveline_shell_menus", true);
    qApp->installEventFilter(new ShellMenuHooks(qApp));
    // A change in Appearance repaints any menu that is open.
    QObject::connect(&Settings::instance(), &Settings::changed, qApp, [] {
        for (QWidget *w : QApplication::topLevelWidgets())
            if (qobject_cast<QMenu *>(w) && w->isVisible()) w->update();
    });
}

}  // namespace Monarchy

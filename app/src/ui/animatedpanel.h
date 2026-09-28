// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QEvent>
#include <QResizeEvent>
#include <QElapsedTimer>
#include <QEasingCurve>
#include <QScreen>
#include <cmath>
#include <QWidget>
#include <QPushButton>
#include <QPainter>
#include <QTimer>
#include "theme.h"

class PanelEdgeButton : public QPushButton {
public:
    PanelEdgeButton(Qt::Orientation direction, QWidget *parent)
        : QPushButton(parent), direction_(direction) {
        setCheckable(true);
        setFixedSize(direction == Qt::Horizontal ? QSize(18, 36) : QSize(36, 18));
        setCursor(Qt::PointingHandCursor);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // Solid, not the Monarchy wash: it sits across a panel's edge and has
        // to hide the line it covers.
        p.setBrush(Theme::Popup);
        p.setPen(QPen(hasFocus() || underMouse() ? Theme::Accent : Theme::TextDim, 0.7));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
        p.setPen(QPen(Theme::Text, 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF c = QRectF(rect()).center();
        const qreal sign = isChecked() ? 1.0 : -1.0;
        QPolygonF arrow;
        if (direction_ == Qt::Horizontal)
            arrow << c + QPointF(-2 * sign, -4) << c + QPointF(2 * sign, 0) << c + QPointF(-2 * sign, 4);
        else
            arrow << c + QPointF(-4, -2 * sign) << c + QPointF(0, 2 * sign) << c + QPointF(4, -2 * sign);
        p.drawPolyline(arrow);
    }
private:
    Qt::Orientation direction_;
};

// Clips the content while the surrounding layout gives its space back. The
// content keeps its natural size during the transition, so controls do not
// squash or reflow on every animation frame.
class AnimatedPanel : public QWidget {
public:
    AnimatedPanel(QWidget *content, Qt::Orientation direction, QWidget *parent = nullptr)
        : QWidget(parent), content_(content), direction_(direction), frameTimer_(this) {
        content_->setParent(this);
        content_->installEventFilter(this);
        setSizePolicy(direction == Qt::Horizontal ? QSizePolicy::Fixed : QSizePolicy::Preferred,
                      direction == Qt::Vertical ? QSizePolicy::Maximum : QSizePolicy::Expanding);
        // The shared Qt animation driver normally ticks at about 60 Hz.
        // Use a precise, display-aware timer and elapsed time so high-refresh
        // screens get more frames without changing the transition duration.
        frameTimer_.setTimerType(Qt::PreciseTimer);
        connect(&frameTimer_, &QTimer::timeout, this, [this] {
            updateFrameInterval();
            const qreal progress = qMin(1.0, clock_.nsecsElapsed() / 220000000.0);
            const qreal eased = easing_.valueForProgress(progress);
            setExtent(qRound(startExtent_ + (endExtent_ - startExtent_) * eased));
            if (progress >= 1.0) {
                frameTimer_.stop();
                finish();
            }
        });
    }

    void setEdgeButton(PanelEdgeButton *button, QWidget *anchor) {
        button_ = button;
        anchor_ = anchor;
        button_->parentWidget()->installEventFilter(this);
        anchor_->installEventFilter(this);
        positionButton();
    }

    QSize sizeHint() const override { return content_->sizeHint(); }
    QSize minimumSizeHint() const override {
        QSize size = content_->minimumSizeHint();
        if (direction_ == Qt::Horizontal) size.setWidth(0);
        else size.setHeight(0);
        return size;
    }

    void setExpanded(bool expanded, bool animate = true) {
        frameTimer_.stop();
        expanded_ = expanded;
        const int current = direction_ == Qt::Horizontal ? width() : height();
        const int target = expanded ? naturalExtent() : 0;
        show();
        content_->show();
        // Hidden controls must not receive keyboard focus during collapse.
        content_->setEnabled(expanded);
        if (!animate) {
            setExtent(target);
            finish();
            return;
        }
        startExtent_ = current;
        endExtent_ = target;
        clock_.restart();
        updateFrameInterval();
        frameTimer_.start();
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        placeContent();
        positionButton();
    }
    void moveEvent(QMoveEvent *event) override {
        QWidget::moveEvent(event);
        positionButton();
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == content_ && event->type() == QEvent::LayoutRequest) {
            placeContent();
            updateGeometry();
        }
        if (button_ && (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
                        event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show))
            QTimer::singleShot(0, this, [this] { positionButton(); });
        return QWidget::eventFilter(object, event);
    }

private:
    void updateFrameInterval() {
        const QScreen *display = screen();
        const qreal reported = display ? display->refreshRate() : 60.0;
        const qreal hz = std::isfinite(reported) && reported > 1.0 ? reported : 60.0;
        // Round down to avoid undersampling 144/240 Hz with integer-ms timers.
        const int interval = qMax(1, int(1000.0 / qBound(30.0, hz, 1000.0)));
        if (frameTimer_.interval() != interval) frameTimer_.setInterval(interval);
    }
    void positionButton() {
        if (!button_ || !anchor_) return;
        QWidget *host = button_->parentWidget();
        const QRect area(anchor_->mapTo(host, QPoint(0, 0)), anchor_->size());
        const QPoint origin = mapTo(host, QPoint(0, 0));
        QPoint center;
        if (direction_ == Qt::Horizontal)
            // Collapsed, it sits on the window's edge, the way the vertical
            // one does on the bottom edge.
            center = QPoint(isHidden() ? area.right() + 1 : origin.x(), area.center().y());
        else
            center = QPoint(area.center().x(), isHidden() ? area.bottom() : origin.y());
        button_->move(center.x() - button_->width() / 2, center.y() - button_->height() / 2);
        button_->raise();
    }
    int naturalExtent() const {
        const QSize hint = content_->sizeHint().expandedTo(content_->minimumSizeHint());
        return direction_ == Qt::Horizontal ? qBound(content_->minimumWidth(), hint.width(), content_->maximumWidth())
                                           : qMax(content_->minimumHeight(), hint.height());
    }
    void setExtent(int extent) {
        if (direction_ == Qt::Horizontal) setFixedWidth(extent);
        else setFixedHeight(extent);
    }
    void placeContent() {
        if (direction_ == Qt::Horizontal)
            content_->setGeometry(0, 0, naturalExtent(), height());
        else
            content_->setGeometry(0, 0, width(), naturalExtent());
    }
    void finish() {
        content_->setVisible(expanded_);
        setVisible(expanded_);
        if (expanded_ && direction_ == Qt::Vertical) {
            setMinimumHeight(0);
            setMaximumHeight(QWIDGETSIZE_MAX);
        }
        placeContent();
        updateGeometry();
        positionButton();
    }

    PanelEdgeButton *button_ = nullptr;
    QWidget *anchor_ = nullptr;
    QWidget *content_;
    Qt::Orientation direction_;
    QTimer frameTimer_;
    QElapsedTimer clock_;
    QEasingCurve easing_{QEasingCurve::InOutCubic};
    int startExtent_ = 0;
    int endExtent_ = 0;
    bool expanded_ = true;
};

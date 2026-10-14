// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "channelstrip.h"

#include <algorithm>

#include <QApplication>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVariantAnimation>
#include <QVBoxLayout>


#include "levelmeter.h"
#include "theme.h"

namespace {

constexpr int kCardWidth = 122;
constexpr int kWideCardWidth = 168;

// The gap between two faders, made into a handle for both. Only the band
// around the two handles answers the mouse (the rest of the gap is the card's,
// for dragging it along the row); there a line joins the two handles while
// the pointer is over it, and a drag moves both by the same amount, so the
// balance between them is kept.
class GangGrip : public QWidget {
public:
    GangGrip(Fader *a, Fader *b, QWidget *parent) : QWidget(parent), a_(a), b_(b) {
        setMouseTracking(true);
        setCursor(Qt::SizeVerCursor);
        setToolTip(QObject::tr("Drag between the two handles to move both faders."));
        for (Fader *f : {a_, b_}) {
            QObject::connect(f, &QSlider::valueChanged, this, [this] { sync(); });
        }
    }

    void setAccent(const QColor &c) {
        accent_ = c;
        update();
    }

    // Laid in the gap between the two faders, and masked to the band around
    // their handles.
    void sync() {
        QWidget *host = parentWidget();
        if (!host || !a_->isVisibleTo(host) || !b_->isVisibleTo(host)) {
            hide();
            return;
        }
        const QRect ra(a_->mapTo(host, QPoint(0, 0)), a_->size());
        const QRect rb(b_->mapTo(host, QPoint(0, 0)), b_->size());
        const int left = ra.right() + 1;
        const int right = rb.left() - 1;
        if (right - left < 4) {
            hide();
            return;
        }
        const int top = std::min(ra.top(), rb.top());
        const int bottom = std::max(ra.bottom(), rb.bottom());
        setGeometry(QRect(QPoint(left, top), QPoint(right, bottom)));
        ya_ = qRound(ra.top() - top + a_->handleCenter());
        yb_ = qRound(rb.top() - top + b_->handleCenter());
        const int from = std::min(ya_, yb_) - kReach;
        const int to = std::max(ya_, yb_) + kReach;
        setMask(QRegion(0, from, width(), to - from));
        show();
        raise();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (!hover_ && !dragging_) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor c = accent_;
        c.setAlpha(dragging_ ? 230 : 160);
        p.setPen(QPen(c, 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(0, ya_), QPointF(width(), yb_));
    }
    void enterEvent(QEnterEvent *) override {
        hover_ = true;
        update();
    }
    void leaveEvent(QEvent *) override {
        hover_ = false;
        update();
    }
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) return;
        dragging_ = true;
        startY_ = e->position().y();
        startA_ = a_->value();
        startB_ = b_->value();
        for (Fader *f : {a_, b_}) f->setSliderDown(true);
        update();
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (!dragging_) return;
        // Up is louder, as on the faders themselves.
        const double dy = startY_ - e->position().y();
        a_->setValue(startA_ + qRound(dy * (a_->maximum() - a_->minimum()) / a_->travel()));
        b_->setValue(startB_ + qRound(dy * (b_->maximum() - b_->minimum()) / b_->travel()));
    }
    void mouseReleaseEvent(QMouseEvent *) override {
        dragging_ = false;
        for (Fader *f : {a_, b_}) f->setSliderDown(false);
        update();
    }

private:
    static constexpr int kReach = 9;
    Fader *a_;
    Fader *b_;
    QColor accent_ = Theme::Accent;
    int ya_ = 0;
    int yb_ = 0;
    bool hover_ = false;
    bool dragging_ = false;
    double startY_ = 0;
    int startA_ = 0;
    int startB_ = 0;
};

// The same colour, softened, for anything sitting behind text.
QColor accentTint(const QColor &c, int alpha) {
    QColor out = c;
    out.setAlpha(alpha);
    return out;
}

}  // namespace

ChannelStrip::ChannelStrip(const QString &id, const QString &name, QWidget *parent)
    : CardBase(parent), id_(id), color_(Theme::channelColor(id)) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setBaseWidth(kCardWidth);
    setRadius(12);
    // A shade lighter than the cards elsewhere, and outlined in the channel's
    // colour where a strip of it used to run along the top.
    setFillColor(Theme::CardHover);
    setBorderColor(color_);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 12, 10, 10);
    lay->setSpacing(8);

    // ------------------------------------------------------------- header
    // The badge is centred; whatever the card adds to its header (the ear)
    // sits hard right in the same cell, so it never pulls the badge over.
    auto *head = new QGridLayout;
    head->setContentsMargins(0, 0, 0, 0);
    badgeTile_ = new IconTile(Theme::channelIconName(id), color_, this);
    iconName_ = Theme::channelIconName(id);
    badgeTile_->setToolTip(tr("Name, colour and icon for this card."));
    static_cast<IconTile *>(badgeTile_)
        ->setOnClicked([this] { emit identityClicked(); });
    head->addWidget(badgeTile_, 0, 0, Qt::AlignHCenter | Qt::AlignVCenter);

    head_ = new QHBoxLayout;
    head_->setSpacing(7);
    head->addLayout(head_, 0, 0, Qt::AlignRight | Qt::AlignVCenter);
    lay->addLayout(head);

    // No meter across the top: each fader's track is its meter -- see
    // buildColumn() and setLevel().

    // --------------------------------------------------------------- name
    // Above the faders, centred: where the card says what it is.
    nameRow_ = new QHBoxLayout;
    auto *title = new QLabel(name, this);
    titleLabel_ = title;
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);
    title->setToolTip(name);
    nameRow_->addWidget(title, 1);
    lay->addLayout(nameRow_);

    // ------------------------------------------------------------- faders
    cols_ = new QHBoxLayout;
    auto *cols = cols_;
    cols->setSpacing(4);
    cols->addWidget(buildColumn(QStringLiteral("monitor"), QStringLiteral("headphones-off"),
                                QStringLiteral("headphones"),
                                tr("Mute this channel in the Monitor mix -- what "
                                   "you hear. Your audience still hears it."),
                                monitor_),
                    1);
    cols->addWidget(buildColumn(QStringLiteral("stream"), QStringLiteral("stream-off"),
                                QStringLiteral("stream"),
                                tr("Mute this channel in the Stream mix -- what "
                                   "your audience hears. You still hear it."),
                                stream_),
                    1);
    lay->addLayout(cols, 1);

    auto *grip = new GangGrip(monitor_.fader, stream_.fader, this);
    grip->setAccent(color_);
    gangGrip_ = grip;

    // Heartbeat is centred in the full footer width; slot number and gear sit
    // in fixed-width side boxes so they do not pull the toggle off centre.
    footerHost_ = new QWidget(this);
    footerHost_->setFixedHeight(30);
    footer_ = new QHBoxLayout(footerHost_);
    footer_->setContentsMargins(0, 2, 0, 0);
    footer_->setSpacing(0);
    constexpr int kFooterSideWidth = 22;
    footerLeftBox_ = new QWidget(footerHost_);
    footerLeftBox_->setFixedWidth(kFooterSideWidth);
    auto *leftBoxLay = new QHBoxLayout(footerLeftBox_);
    leftBoxLay->setContentsMargins(0, 0, 0, 0);
    leftBoxLay->setSpacing(0);
    slotLabel_ = new QLabel(footerLeftBox_);
    slotLabel_->setAlignment(Qt::AlignCenter);
    QFont slotFont = slotLabel_->font();
    slotFont.setPointSizeF(slotFont.pointSizeF() * 0.9);
    slotFont.setBold(true);
    slotLabel_->setFont(slotFont);
    Theme::setTextColor(slotLabel_, Theme::TextDim);
    slotLabel_->setVisible(false);
    leftBoxLay->addWidget(slotLabel_);
    footerLeftSpacer_ = new QWidget(footerHost_);
    footerLeftSpacer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    footerRightSpacer_ = new QWidget(footerHost_);
    footerRightSpacer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    footerRightBox_ = new QWidget(footerHost_);
    footerRightBox_->setFixedWidth(kFooterSideWidth);
    footer_->addWidget(footerLeftBox_, 0);
    footer_->addWidget(footerLeftSpacer_, 1);
    footer_->addWidget(footerRightSpacer_, 1);
    footer_->addWidget(footerRightBox_, 0);
    lay->addWidget(footerHost_);
}

void ChannelStrip::setDisplayName(const QString &name) {
    if (titleEdit_ && titleEdit_->isVisible()) return;
    if (!titleLabel_) return;
    titleLabel_->setText(name);
    titleLabel_->setToolTip(titleEditable_
                                ? tr("%1\n\nClick to rename.").arg(name)
                                : name);
}

namespace {

constexpr int kFooterSideWidth = 22;

}  // namespace

void ChannelStrip::setMasterSlotNumber(int slot) {
    if (!slotLabel_) return;
    masterSlot_ = slot;
    if (slot < 1) {
        slotLabel_->setVisible(false);
        if (rackButton_) rackButton_->setVisible(false);
        if (rebuildButton_) rebuildButton_->setVisible(false);
        return;
    }
    slotLabel_->setText(QString::number(slot));
    updateSlotTooltip();
    slotLabel_->setVisible(true);
    if (rackButton_) rackButton_->setVisible(true);
    if (rebuildButton_) rebuildButton_->setVisible(true);
    positionOverlays();
}

void ChannelStrip::setInputLatencyUs(qint64 us) {
    // -2 is meaningful and must survive: it is "this cannot be measured", which
    // the tooltip renders differently from "not measured yet".
    if (us < -2) us = -1;
    if (us == inputLatencyUs_) return;
    inputLatencyUs_ = us;
    updateSlotTooltip();
}

void ChannelStrip::updateSlotTooltip() {
    if (!slotLabel_ || masterSlot_ < 1) return;
    QString tip = tr("Input #%1").arg(masterSlot_);
    if (inputLatencyUs_ == kLatencyHidden) {
        tip += QLatin1Char('\n') + tr("Latency: N/A");
        tip += QLatin1Char('\n') +
               tr("This device processes audio inside itself, so the delay you "
                  "hear is decided somewhere this machine cannot see. Any "
                  "number here would be a fraction of it.");
    } else if (inputLatencyUs_ >= 0) {
        const double ms = inputLatencyUs_ / 1000.0;
        // Always one decimal. The figure is a median of readings rather than a
        // computed round number, and rounding a measurement to whole
        // milliseconds makes 21.2 and 21.9 look like the same device -- which
        // is exactly the false equivalence this number exists to break.
        tip += QLatin1Char('\n') + tr("%1 ms capture delay").arg(ms, 0, 'f', 1);
        // Said plainly, because the alternative is a user concluding the
        // number is broken. It is measured from ALSA, so it covers everything
        // from the device handing audio to the host onwards -- and nothing the
        // device did before that. A camera or headset doing its own noise
        // suppression can spend far more than this figure upstream of anything
        // Linux can see. See scripts/latency-offset-test.sh.
        tip += QLatin1Char('\n') +
               tr("Measured from ALSA. Does not include processing inside the "
                  "device itself.");
    }
    slotLabel_->setToolTip(tip);
}

void ChannelStrip::enableMasterRack(bool on) {
    masterRackEnabled_ = on;
    if (on && !rackButton_) {
        rackButton_ = new IconToggle(QStringLiteral("rack"),
                                     QStringLiteral("rack"), footerHost_);
        rackButton_->setCheckable(false);
        rackButton_->setIconSize(14);
        rackButton_->setToolTip(tr("Open the Creative FX Virtual Rack."));
        connect(rackButton_, &QAbstractButton::clicked, this,
                &ChannelStrip::rackRequested);
        positionOverlays();
    }
    if (rackButton_) rackButton_->setVisible(on && slotLabel_ && slotLabel_->isVisible());
}

void ChannelStrip::enableChannelRack(bool on) {
    if (on && !channelRackButton_) {
        channelRackButton_ = new IconToggle(QStringLiteral("rack"), QStringLiteral("rack"),
                                            footerLeftBox_);
        channelRackButton_->setCheckable(false);
        channelRackButton_->setIconSize(14);
        channelRackButton_->setToolTip(tr("Open the Creative FX Virtual Rack."));
        connect(channelRackButton_, &QAbstractButton::clicked, this,
                &ChannelStrip::rackRequested);
        // A channel shows no slot number, so the box on the gear's opposite
        // side is free for it.
        footerLeftBox_->layout()->addWidget(channelRackButton_);
    }
    if (channelRackButton_) channelRackButton_->setVisible(on);
}

void ChannelStrip::enableMasterRebuild(bool on) {
    masterRebuildEnabled_ = on;
    if (on && !rebuildButton_) {
        rebuildButton_ = new IconToggle(QStringLiteral("build"),
                                        QStringLiteral("build"), footerHost_);
        rebuildButton_->setCheckable(false);
        rebuildButton_->setIconSize(14);
        rebuildButton_->setToolTip(
            tr("Rebuild this microphone's capture path.\n"
               "Use if the audio sounds robotic or glitchy after plugging in."));
        connect(rebuildButton_, &QAbstractButton::clicked, this,
                &ChannelStrip::masterRebuildRequested);
        positionOverlays();
    }
    if (rebuildButton_)
        rebuildButton_->setVisible(on && slotLabel_ && slotLabel_->isVisible());
}

void ChannelStrip::setAddMasterOverlayButton(QPushButton *btn) {
    addMasterOverlay_ = btn;
    if (btn) {
        btn->setParent(this);
        btn->hide();
        scheduleOverlayLayout();
    }
}

void ChannelStrip::setRemoveMasterOverlayButton(QPushButton *btn) {
    removeMasterOverlay_ = btn;
    if (btn) {
        btn->setParent(this);
        btn->hide();
        scheduleOverlayLayout();
    }
}

void ChannelStrip::scheduleOverlayLayout() {
    if (overlayLayoutScheduled_) return;
    overlayLayoutScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        overlayLayoutScheduled_ = false;
        if (layout()) layout()->activate();
        positionOverlays();
        if (addMasterOverlay_) addMasterOverlay_->show();
        if (removeMasterOverlay_) removeMasterOverlay_->show();
    });
}

void ChannelStrip::showEvent(QShowEvent *event) {
    CardBase::showEvent(event);
    if (addMasterOverlay_ || rackButton_ || rebuildButton_ || gangGrip_) scheduleOverlayLayout();
}

void ChannelStrip::positionRackButton() {
    if (!rackButton_ || !footerHost_ || !footerLeftBox_) return;
    const QSize sh = rackButton_->sizeHint();
    const QRect left = footerLeftBox_->geometry();
    const int x = left.right() + 1;
    const int y = left.y() + (left.height() - sh.height()) / 2;
    rackButton_->setFixedSize(sh);
    rackButton_->move(x, y);
    rackButton_->raise();
}

void ChannelStrip::positionRebuildButton() {
    if (!rebuildButton_ || !footerHost_ || !footerRightBox_) return;
    const QSize sh = rebuildButton_->sizeHint();
    const QRect right = footerRightBox_->geometry();
    const int x = right.left() - sh.width() - 1;
    const int y = right.y() + (right.height() - sh.height()) / 2;
    rebuildButton_->setFixedSize(sh);
    rebuildButton_->move(x, y);
    rebuildButton_->raise();
}

void ChannelStrip::positionAddMasterButton() {
    if (!addMasterOverlay_) return;
    const int side = addMasterOverlay_->width();
    constexpr int kRightInset = 2;
    const int x = width() - side - kRightInset;

    int y = (height() - side) / 2;
    if (gain_.fader && gain_.host && gain_.host->isVisible()) {
        const QPoint faderTop = gain_.fader->mapTo(this, QPoint(0, 0));
        y = faderTop.y() + (gain_.fader->height() - side) / 2;
    } else if (monitor_.fader && monitor_.host) {
        const QPoint faderTop = monitor_.fader->mapTo(this, QPoint(0, 0));
        y = faderTop.y() + (monitor_.fader->height() - side) / 2;
    }

    addMasterOverlay_->setFixedSize(side, side);
    addMasterOverlay_->move(x, y);
    addMasterOverlay_->raise();

    if (removeMasterOverlay_) {
        constexpr int kGap = 4;
        removeMasterOverlay_->setFixedSize(side, side);
        removeMasterOverlay_->move(x, y + side + kGap);
        removeMasterOverlay_->raise();
    }
}

void ChannelStrip::positionOverlays() {
    if (gangGrip_) static_cast<GangGrip *>(gangGrip_)->sync();
    positionRackButton();
    positionRebuildButton();
    positionAddMasterButton();
}

void ChannelStrip::resizeEvent(QResizeEvent *event) {
    CardBase::resizeEvent(event);
    positionOverlays();
}

void ChannelStrip::enableEditableTitle(bool on) {
    titleEditable_ = on;
    if (!titleLabel_) return;
    if (on) {
        titleLabel_->setCursor(Qt::PointingHandCursor);
        titleLabel_->installEventFilter(this);
        titleLabel_->setToolTip(
            tr("%1\n\nClick to rename.").arg(titleLabel_->text()));
    } else {
        titleLabel_->setCursor(Qt::ArrowCursor);
        titleLabel_->removeEventFilter(this);
    }
}

void ChannelStrip::enableDragHandle(bool on) {
    dragHandle_ = on;
    // An open hand rather than a closed one: the card is not held yet. The
    // controls on it keep their own cursors.
    if (on) setCursor(Qt::OpenHandCursor);
    else unsetCursor();
}

void ChannelStrip::mousePressEvent(QMouseEvent *event) {
    if (dragHandle_ && event->button() == Qt::LeftButton) {
        dragArmed_ = true;
        dragPress_ = event->pos();
        event->accept();
        return;
    }
    CardBase::mousePressEvent(event);
}

void ChannelStrip::mouseMoveEvent(QMouseEvent *event) {
    if (dragging_) {
        emit reorderMoved(event->globalPosition().toPoint());
        return;
    }
    if (dragArmed_ && (event->pos() - dragPress_).manhattanLength() >=
                          QApplication::startDragDistance()) {
        // Past the threshold this is a move, not a click.
        dragArmed_ = false;
        dragging_ = true;
        setCursor(Qt::ClosedHandCursor);
        emit reorderStarted(mapToGlobal(dragPress_),
                            event->modifiers() & Qt::ControlModifier);
        emit reorderMoved(event->globalPosition().toPoint());
        return;
    }
    CardBase::mouseMoveEvent(event);
}

void ChannelStrip::mouseReleaseEvent(QMouseEvent *event) {
    if (dragging_) {
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        emit reorderEnded();
        return;
    }
    // A press on the name that never became a move: the click renames, as it
    // did before the whole card could be grabbed. The card took the press, so
    // the name never sees this release itself.
    const bool wasClick = dragArmed_;
    dragArmed_ = false;
    if (wasClick && titleEditable_ && titleLabel_ && titleLabel_->isVisible() &&
        QRect(titleLabel_->mapTo(this, QPoint(0, 0)), titleLabel_->size())
            .contains(event->pos())) {
        beginTitleEdit();
        return;
    }
    CardBase::mouseReleaseEvent(event);
}

bool ChannelStrip::eventFilter(QObject *obj, QEvent *ev) {
    if (titleEditable_ && obj == titleLabel_ && ev->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(ev);
        if (me->button() == Qt::LeftButton) beginTitleEdit();
        return true;
    }
    return CardBase::eventFilter(obj, ev);
}

void ChannelStrip::beginTitleEdit() {
    if (!titleLabel_) return;
    if (!titleEdit_) {
        titleEdit_ = new QLineEdit(this);
        titleEdit_->setFrame(false);
        QFont f = titleLabel_->font();
        titleEdit_->setFont(f);
        titleEdit_->setPlaceholderText(tr("Name"));
        nameRow_->insertWidget(0, titleEdit_, 1);
        titleEdit_->setAlignment(Qt::AlignCenter);
        connect(titleEdit_, &QLineEdit::editingFinished, this,
                &ChannelStrip::finishTitleEdit);
    }
    applyTitleEditAccent();
    titleEdit_->setText(titleLabel_->text());
    titleLabel_->hide();
    titleEdit_->show();
    titleEdit_->setFocus(Qt::OtherFocusReason);
    titleEdit_->selectAll();
}

void ChannelStrip::finishTitleEdit() {
    if (!titleEdit_ || !titleEdit_->isVisible()) return;
    const QString prev = titleLabel_ ? titleLabel_->text().trimmed() : QString();
    const QString name = titleEdit_->text().trimmed();
    titleEdit_->hide();
    titleLabel_->show();
    if (name.isEmpty()) {
        emit displayNameEdited(QString());
        return;
    }
    if (name == prev) return;
    setDisplayName(name);
    emit displayNameEdited(name);
}

void ChannelStrip::setIdentityBadge(const QString &iconName, const QColor &color) {
    color_ = color;
    iconName_ = iconName;
    // Re-applied on every refresh pass, so it has to honour the grey a
    // disconnected device is wearing rather than paint over it.
    const QColor shown = deviceConnected_ ? color : Theme::Inactive;
    setBorderColor(shown);
    applyMeterTints();
    if (monitor_.fader) monitor_.fader->setAccent(shown);
    if (stream_.fader) stream_.fader->setAccent(shown);
    if (gain_.fader) gain_.fader->setAccent(shown);
    if (micSend_.fader) micSend_.fader->setAccent(shown);
    if (badgeTile_)
        static_cast<IconTile *>(badgeTile_)->setAppearance(iconName, shown);
    applyAccentToToggles();
}

// The toggles that mean "on, for this card" -- link and the monitor ear --
// light in the card's colour. The heartbeat does not: its three states are
// distinguished by colour, and the window sets that accent as it changes them.
// The rename box belongs to the card, so it is ringed and underlined in the
// card's colour rather than the global focus green -- which on a wall of cards
// says only "something has focus", not which card you are renaming.
void ChannelStrip::applyTitleEditAccent() {
    if (!titleEdit_) return;
    Theme::follow(titleEdit_, [this] {
        const QColor accent = deviceConnected_ ? color_ : Theme::Inactive;
        titleEdit_->setStyleSheet(
            QStringLiteral("QLineEdit { background: %1; border: 1px solid %2; "
                           "border-radius: 6px; padding: 2px 6px; "
                           "selection-background-color: %3; }"
                           "QLineEdit:focus { border-color: %4; }")
                .arg(Theme::css(Theme::Well), Theme::css(Theme::Line),
                     accentTint(accent, 90).name(QColor::HexArgb), accent.name()));
    }, "title-edit");
}

void ChannelStrip::applyAccentToToggles() {
    const QColor shown = deviceConnected_ ? color_ : Theme::Inactive;
    if (gangGrip_) static_cast<GangGrip *>(gangGrip_)->setAccent(shown);
    if (micMonitorButton_)
        micMonitorButton_->setAccent(micMonitorBlocked_ ? Theme::Danger : shown);
    applyTitleEditAccent();
}

void ChannelStrip::setDeviceConnected(bool on) {
    if (on == deviceConnected_) return;
    deviceConnected_ = on;

    const QColor shown = on ? color_ : Theme::Inactive;
    setBorderColor(shown);
    if (badgeTile_)
        static_cast<IconTile *>(badgeTile_)->setAppearance(iconName_, shown);
    if (monitor_.fader) monitor_.fader->setAccent(shown);
    if (stream_.fader) stream_.fader->setAccent(shown);
    if (gain_.fader) gain_.fader->setAccent(shown);
    if (micSend_.fader) micSend_.fader->setAccent(shown);
    applyAccentToToggles();
    if (titleLabel_) {
        Theme::setTextColor(titleLabel_, on ? Theme::Text : Theme::TextFaint);
    }
    // Tints only. The levels themselves are left alone: an unplugged
    // microphone reads silence on its own, and a bus that is still carrying
    // shared application audio should go on showing it.
    applyMeterTints();
}

void ChannelStrip::enableGainFader(bool on) {
    if (on && !gain_.fader) {
        setBaseWidth(kWideCardWidth);
        QHBoxLayout *cols = cols_;
        if (cols) {
            cols->addWidget(
                buildColumn(QStringLiteral("gain"), QStringLiteral("microphone-off"),
                            QStringLiteral("microphone"),
                            tr("Hardware microphone gain (0–40 dB). Distinct from "
                               "the Stream and Monitor mix levels."),
                            gain_),
                1);
        }
        gain_.fader->setRange(0, 80);
        // Detach the generic handler: it would emit muteToggled("gain"), which
        // the daemon parses as the Stream mix.
        disconnect(gain_.mute, nullptr, this, nullptr);
        gain_.mute->setToolTip(tr("Mute the microphone."));
        connect(gain_.mute, &QAbstractButton::toggled, this,
                [this](bool muted) { emit hardwareMuteToggled(muted); });
        connect(gain_.fader, &QSlider::valueChanged, this, [this](int v) {
            if (gainStyle_ == GainStyle::PreampDb) {
                gain_.value->setText(QStringLiteral("+%1").arg(v / 2.0, 0, 'f', 1));
                emit gainChanged(v / 2.0);
            } else {
                gain_.value->setText(QStringLiteral("%1%").arg(v));
                emit gainChanged(v);
            }
        });
    }
    // The whole column, not its three widgets: the caption above them is part
    // of it, and left behind it labelled empty space. The card also has to go
    // back to its normal width, or a microphone with no preamp control gets a
    // card a third wider than every other one for no visible reason.
    if (gain_.host) gain_.host->setVisible(on);
    if (!on && !micSend_.fader) setBaseWidth(kCardWidth);
    if (on && addMasterOverlay_) positionOverlays();
}

void ChannelStrip::enableMicSendFader(bool on) {
    if (on && !micSend_.fader) {
        setBaseWidth(kWideCardWidth);
        QHBoxLayout *cols = cols_;
        if (cols) {
            cols->addWidget(
                buildColumn(QStringLiteral("micsend"), QStringLiteral("microphone-off"),
                            QStringLiteral("microphone"),
                            tr("Gain of this channel's own microphone -- what an "
                               "application recording from it hears.\n"
                               "Independent of the input devices. 100% is unity."),
                            micSend_),
                1);
        }
        micSend_.fader->setRange(0, 100);
        disconnect(micSend_.mute, nullptr, this, nullptr);
        micSend_.mute->setToolTip(tr("Mute this channel's microphone."));
        connect(micSend_.mute, &QAbstractButton::toggled, this,
                [this](bool muted) { emit micMuteToggled(id_, muted); });
        connect(micSend_.fader, &QSlider::valueChanged, this, [this](int v) {
            micSend_.value->setText(QStringLiteral("%1%").arg(v));
            emit micSendChanged(id_, v / 100.0);
        });
    }
    if (micSend_.fader) micSend_.fader->setVisible(on);
    if (micSend_.value) micSend_.value->setVisible(on);
    if (micSend_.mute) micSend_.mute->setVisible(on);
}

void ChannelStrip::setMicMuted(bool muted) {
    if (!micSend_.mute) return;
    QSignalBlocker b(micSend_.mute);
    micSend_.mute->setChecked(muted);
    if (micSend_.fader) micSend_.fader->setMuted(muted);
}

// Greyed when the channel publishes no microphone: there is nothing for the
// gain to act on, and a live-looking control that does nothing is worse than a
// disabled one.
void ChannelStrip::setMicControlsActive(bool on) {
    if (micSend_.fader) micSend_.fader->setEnabled(on);
    if (micSend_.mute) micSend_.mute->setEnabled(on);
    if (micSend_.value) micSend_.value->setEnabled(on);
}

void ChannelStrip::setMicColumnShown(bool on) {
    if (!micSend_.host) return;
    const int was = micColumnShown_;
    micColumnShown_ = on ? 1 : 0;
    if (was == micColumnShown_) return;
    const int target = on ? kWideCardWidth : kCardWidth;
    if (was < 0 || !isVisible()) {
        micSend_.host->setVisible(on);
        setBaseWidth(target);
        return;
    }
    // Growing, the column is there from the start and the three share the
    // widening card; shrinking, it stays until the card is narrow again. Three
    // faders fit in the narrow width, so nothing is clipped on the way.
    if (on) micSend_.host->show();
    animateWidthTo(target);
}

void ChannelStrip::setBaseWidth(int width) {
    if (width == minimumWidth()) return;
    // A floor, not a fixed size: with room to spare the window shares it out
    // among the cards in proportion to this -- see MainWindow::updateRowStretch().
    setMinimumWidth(width);
    emit baseWidthChanged();
}

void ChannelStrip::animateWidthTo(int width) {
    if (!widthAnim_) {
        widthAnim_ = new QVariantAnimation(this);
        widthAnim_->setDuration(220);
        widthAnim_->setEasingCurve(QEasingCurve::OutCubic);
        connect(widthAnim_, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &v) { setBaseWidth(v.toInt()); });
        connect(widthAnim_, &QVariantAnimation::finished, this, [this] {
            if (micColumnShown_ == 0 && micSend_.host) micSend_.host->hide();
            positionOverlays();
        });
    }
    widthAnim_->stop();
    widthAnim_->setStartValue(minimumWidth());
    widthAnim_->setEndValue(width);
    widthAnim_->start();
}

void ChannelStrip::setHardwareControlsEnabled(bool on) {
    if (gain_.fader) gain_.fader->setEnabled(on);
    if (gain_.value) gain_.value->setEnabled(on);
    if (hwMute_) hwMute_->setEnabled(on);
}

void ChannelStrip::setMicSend(double level) {
    if (!micSend_.fader || micSend_.fader->isSliderDown()) return;
    QSignalBlocker b(micSend_.fader);
    // Clamped to the fader's own range, so a value saved when this went to
    // 400% cannot leave the readout disagreeing with where the handle sits.
    const int v = std::clamp(int(level * 100.0 + 0.5), micSend_.fader->minimum(),
                             micSend_.fader->maximum());
    micSend_.fader->setValue(v);
    micSend_.value->setText(QStringLiteral("%1%").arg(v));
    micSend_.last = v;
}

void ChannelStrip::enableEffectsButton(bool on) {
    if (on && !fxButton_) {
        fxButton_ = new IconToggle(QStringLiteral("heartbeat"),
                                   QStringLiteral("heartbeat"), this);
        fxButton_->setShape(IconToggle::Pill);
        fxButton_->setIconSize(16);
        fxButton_->setToolTip(
            tr("Effects on/off for this channel (noise suppression, EQ, low cut).\n"
               "Your settings are kept, so turning it back on restores the chain.\n"
               "The gear on the right opens the effects panel.\n"
               "Right-click cycles the states in reverse."));
        // Between two equal spacers inside matching side boxes.
        footer_->insertWidget(2, fxButton_, 0, Qt::AlignVCenter);
    }
    if (fxButton_) fxButton_->setVisible(on);
}

void ChannelStrip::enableSettingsButton(bool on) {
    if (on && !settingsButton_) {
        // Not checkable: it opens a window, it does not hold a state. The
        // painted "checked" look would be a lie the moment the panel was
        // closed from its own title bar.
        settingsButton_ = new IconToggle(QStringLiteral("gear"),
                                         QStringLiteral("gear"), this);
        settingsButton_->setCheckable(false);
        settingsButton_->setIconSize(16);
        settingsButton_->setToolTip(tr("Open the effects panel for this card."));
        auto *rightLay = new QHBoxLayout(footerRightBox_);
        rightLay->setContentsMargins(0, 0, 0, 0);
        rightLay->setSpacing(0);
        rightLay->addStretch();
        rightLay->addWidget(settingsButton_, 0, Qt::AlignRight | Qt::AlignVCenter);
    }
    if (settingsButton_) settingsButton_->setVisible(on);
}

void ChannelStrip::enableMicMonitorButton(bool on) {
    if (on && !micMonitorButton_) {
        micMonitorButton_ = new IconToggle(QStringLiteral("ear"),
                                           QStringLiteral("ear-off"), this);
        micMonitorButton_->setIconSize(17);
        // The card's own colour, so "I am hearing this one" is answered by the
        // same hue as the card it belongs to.
        micMonitorButton_->setAccent(deviceConnected_ ? color_ : Theme::Inactive);
        micMonitorButton_->setToolTip(
            tr("Hear this channel's published microphone in the Monitor mix.\n"
               "Software only: there is no hardware monitor for a channel mic."));
        micMonitorButton_->setVisible(false);
        addHeaderWidget(micMonitorButton_);
    }
}

void ChannelStrip::setMicMonitorButtonVisible(bool on) {
    if (micMonitorButton_) micMonitorButton_->setVisible(on);
}

void ChannelStrip::setMicMonitorChecked(bool on) {
    if (!micMonitorButton_) return;
    QSignalBlocker b(micMonitorButton_);
    micMonitorButton_->setChecked(on);
}

void ChannelStrip::setMicMonitorBlocked(bool blocked) {
    if (!micMonitorButton_ || micMonitorBlocked_ == blocked) return;
    micMonitorBlocked_ = blocked;
    if (blocked) {
        micMonitorTip_ = micMonitorButton_->toolTip();
        micMonitorButton_->setToolTip(
            tr("Monitoring is cut: a Monitor mix is playing on speakers, which "
               "would feed this microphone back into itself.\n"
               "Mute that mix, or turn this off in Settings > General."));
    } else if (!micMonitorTip_.isEmpty()) {
        micMonitorButton_->setToolTip(micMonitorTip_);
    }
    applyAccentToToggles();
}

void ChannelStrip::addHeaderWidget(QWidget *w) {
    if (head_ && w) head_->addWidget(w);
}

void ChannelStrip::makeMonitorColumnMaster(const QString &tip) {
    setMonitorColumnTip(
        tip,
        tr("Mute everything you hear through the Monitor mix.\n"
           "Your audience is unaffected."));
}

void ChannelStrip::setMonitorColumnTip(const QString &faderTip,
                                       const QString &muteTip) {
    if (monitor_.fader) monitor_.fader->setToolTip(faderTip);
    if (monitor_.mute) monitor_.mute->setToolTip(muteTip);
    if (monitor_.value) monitor_.value->setToolTip(faderTip);
}

void ChannelStrip::setMixFadersVisible(bool on, const QString &disabledTip) {
    if (monitor_.host) monitor_.host->setVisible(on);
    if (stream_.host) stream_.host->setVisible(on);
    // The grip between them goes with them; laid out again once they are.
    if (gangGrip_) QTimer::singleShot(0, this, [this] { positionOverlays(); });
    if (!on && !disabledTip.isEmpty()) setToolTip(disabledTip);
    else if (on) setToolTip(QString());
    if (!on) setBaseWidth(kCardWidth);
    else if (gain_.host && gain_.host->isVisible()) setBaseWidth(kWideCardWidth);
    else setBaseWidth(kCardWidth);
}

void ChannelStrip::setMixColumnEnabled(const QString &mix, bool on,
                                       const QString &disabledTip) {
    MixColumn *col = nullptr;
    if (mix == QLatin1String("monitor")) col = &monitor_;
    else if (mix == QLatin1String("stream")) col = &stream_;
    if (!col || !col->host) return;
    if (col->fader) col->fader->setEnabled(on);
    if (col->mute) col->mute->setEnabled(on);
    if (col->value) col->value->setEnabled(on);
    col->host->setToolTip(on ? QString() : disabledTip);
    const qreal opacity = on ? 1.0 : 0.45;
    col->host->setGraphicsEffect(nullptr);
    if (!on) {
        auto *fx = new QGraphicsOpacityEffect(col->host);
        fx->setOpacity(opacity);
        col->host->setGraphicsEffect(fx);
    }
}

void ChannelStrip::enableHardwareMute(bool on) {
    if (on && !hwMute_) {
        hwMute_ = new IconToggle(QStringLiteral("microphone-off"),
                                 QStringLiteral("microphone"), this);
        hwMute_->setAccent(Theme::Danger);
        hwMute_->setIconSize(17);
        hwMute_->setToolTip(tr("Hardware microphone mute (the pad or button on "
                               "the microphone itself)."));
        connect(hwMute_, &QAbstractButton::toggled, this,
                &ChannelStrip::hardwareMuteToggled);
        auto *head = layout()->itemAt(0)->layout();
        if (head) head->addWidget(hwMute_);
    }
    if (hwMute_) hwMute_->setVisible(on);
}

void ChannelStrip::setGainStyle(GainStyle style) {
    gainStyle_ = style;
    if (!gain_.fader) return;
    QSignalBlocker b(gain_.fader);
    // Half-decibel steps over 0..40 dB, against plain percent. Working in the
    // hardware's own steps keeps the dB readout exact rather than rounding a
    // percentage back and forth.
    if (style == GainStyle::PreampDb) {
        gain_.fader->setRange(0, 80);
        gain_.mute->setToolTip(tr("Mute the microphone."));
    } else {
        gain_.fader->setRange(0, 100);
        gain_.mute->setToolTip(
            tr("Mute the microphone.\nThis is the input device's own mute, so "
               "nothing recording from it hears you -- not just this mixer."));
    }
    if (gain_.fader->parentWidget())
        gain_.fader->parentWidget()->setToolTip(
            style == GainStyle::PreampDb
                ? tr("Hardware microphone gain (0-40 dB). Distinct from the "
                     "Stream and Monitor mix levels.")
                : tr("Input level of the microphone itself, the same control "
                     "your volume settings show.\nDistinct from the Stream and "
                     "Monitor mix levels, which only change this mixer."));
}

void ChannelStrip::setGainValue(double value) {
    if (gainStyle_ == GainStyle::PreampDb) { setGainDb(value); return; }
    if (!gain_.fader || gain_.fader->isSliderDown()) return;
    QSignalBlocker b(gain_.fader);
    const int v = std::clamp(int(value + 0.5), 0, 100);
    gain_.fader->setValue(v);
    gain_.value->setText(QStringLiteral("%1%").arg(v));
    gain_.last = v;
}

void ChannelStrip::setGainDb(double db) {
    if (!gain_.fader || gain_.fader->isSliderDown()) return;
    QSignalBlocker b(gain_.fader);
    const int v = int(db * 2.0 + 0.5);
    gain_.fader->setValue(v);
    gain_.value->setText(QStringLiteral("+%1").arg(db, 0, 'f', 1));
    gain_.last = v;
}

void ChannelStrip::setHardwareMuted(bool muted) {
    // Two buttons now mute the microphone -- the one under the gain fader,
    // where people look for it, and the original in the footer. They must never
    // disagree about the state they both control.
    if (hwMute_) {
        QSignalBlocker b(hwMute_);
        hwMute_->setChecked(muted);
    }
    if (gain_.mute) {
        QSignalBlocker b(gain_.mute);
        gain_.mute->setChecked(muted);
        if (gain_.fader) gain_.fader->setMuted(muted);
    }
}

QWidget *ChannelStrip::buildColumn(const QString &mix, const QString &iconOn,
                                   const QString &iconOff, const QString &tip,
                                   MixColumn &out) {
    auto *col = new QWidget(this);
    out.host = col;
    auto *lay = new QVBoxLayout(col);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(5);

    out.fader = new Fader(col);
    out.fader->setAccent(color_);
    lay->addWidget(out.fader, 1, Qt::AlignHCenter);
    // The level through this fader, as its track: see setLevel().
    out.meter = attachTrackMeter(out.fader);
    out.meter->setTint(color_);

    out.value = new QLabel(QStringLiteral("100"), col);
    out.value->setAlignment(Qt::AlignCenter);
    QFont vf = out.value->font();
    vf.setPointSizeF(vf.pointSizeF() * 0.85);
    out.value->setFont(vf);
    Theme::setTextColor(out.value, Theme::TextDim);
    lay->addWidget(out.value);

    out.mute = new IconToggle(iconOn, iconOff, col);
    // Checked means muted, so the highlight is a warning colour, not the
    // channel's own. A red badge under a fader is unambiguous; the channel
    // colour would read as "selected".
    out.mute->setAccent(Theme::Danger);
    out.mute->setIconSize(17);
    out.mute->setToolTip(tip);
    lay->addWidget(out.mute, 0, Qt::AlignHCenter);

    // By pointer, not by reference: `out` is a reference parameter and would
    // be gone by the time the lambda runs.
    MixColumn *column = &out;
    connect(out.fader, &QSlider::valueChanged, this,
            [this, mix](int v) { onFaderMoved(mix, v); });
    connect(out.mute, &QAbstractButton::toggled, this,
            [this, mix, column](bool muted) {
                column->fader->setMuted(muted);
                emit muteToggled(id_, mix, muted);
            });
    return col;
}

void ChannelStrip::onFaderMoved(const QString &mix, int value) {
    // Only the two mix faders belong here. buildColumn() wires every column to
    // this slot, and the gain and microphone columns used to fall through the
    // isMonitor test into the Stream branch -- moving the hardware gain rewrote
    // the Stream readout, dragged the linked fader and emitted a Stream volume
    // change. They have their own handlers.
    if (mix != QLatin1String("monitor") && mix != QLatin1String("stream")) return;
    const bool isMonitor = (mix == QLatin1String("monitor"));
    auto &self = isMonitor ? monitor_ : stream_;

    self.value->setText(QString::number(value));
    self.last = value;
    emit volumeChanged(id_, mix, value / 100.0);
}

void ChannelStrip::setValues(double streamVol, bool streamMuted, double monitorVol,
                             bool monitorMuted) {
    // A refresh landing mid-drag would fight the hand on the fader: the daemon
    // is always a poll behind, so it answers with the value from before the
    // last move.
    if (stream_.fader->isSliderDown() || monitor_.fader->isSliderDown()) return;

    // Blocked so refreshing from the daemon does not look like user input and
    // bounce straight back as a write -- and so it does not drag the linked
    // fader along with it.
    QSignalBlocker b1(stream_.fader), b2(monitor_.fader);
    QSignalBlocker b3(stream_.mute), b4(monitor_.mute);

    const int s = int(streamVol * 100.0 + 0.5);
    const int m = int(monitorVol * 100.0 + 0.5);
    stream_.fader->setValue(s);
    monitor_.fader->setValue(m);
    stream_.last = s;
    monitor_.last = m;

    stream_.mute->setChecked(streamMuted);
    monitor_.mute->setChecked(monitorMuted);
    stream_.fader->setMuted(streamMuted);
    monitor_.fader->setMuted(monitorMuted);

    stream_.value->setText(QString::number(s));
    monitor_.value->setText(QString::number(m));
}

void ChannelStrip::setLevel(double fraction) {
    level_ = fraction;
    // Each mix's track shows what this card sends into that mix: the level at
    // its fader, nothing while it is muted. On an input device the Monitor
    // send also needs the device to be monitored (setMainMeterActive()).
    const auto through = [this](const MixColumn &col, bool live) {
        if (!col.meter) return;
        const bool on = live && col.fader && col.fader->isEnabled() &&
                        !(col.mute && col.mute->isChecked());
        col.meter->setLevel(on ? level_ * col.fader->value() / 100.0 : 0.0);
    };
    through(monitor_, mainMeterActive_);
    through(stream_, true);
}

void ChannelStrip::setMainMeterActive(bool on) {
    if (on == mainMeterActive_) return;
    mainMeterActive_ = on;
    applyMeterTints();
    setLevel(level_);
}

void ChannelStrip::setMicLevel(double fraction) {
    // A channel's own microphone, through its send fader; an input device's
    // published signal, behind its gain fader as it is.
    if (micSend_.meter) {
        const bool on = micMeterActive_ && !(micSend_.mute && micSend_.mute->isChecked());
        micSend_.meter->setLevel(on ? fraction * micSend_.fader->value() / 100.0 : 0.0);
    } else if (gain_.meter) {
        gain_.meter->setLevel(micMeterActive_ ? fraction : 0.0);
    }
}

void ChannelStrip::setMicMeterActive(bool on) {
    if (on == micMeterActive_) return;
    micMeterActive_ = on;
    applyMeterTints();
    if (!on) setMicLevel(0.0);
}

void ChannelStrip::applyMeterTints() {
    // The card's colour while live; grey while the device is unplugged, or
    // while that track has nothing it could be carrying.
    const auto tint = [this](const MixColumn &col, bool live) {
        if (col.meter) col.meter->setTint(live && deviceConnected_ ? color_ : Theme::Inactive);
    };
    tint(monitor_, mainMeterActive_);
    tint(stream_, true);
    tint(micSend_, micMeterActive_);
    tint(gain_, micMeterActive_);
}

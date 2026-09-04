// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "cardidentity.h"

#include <QApplication>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "theme.h"

namespace {

constexpr int kPreviewPx = 44;
constexpr int kIconButtonPx = 38;
constexpr int kIconColumns = 8;

// Around the wheel, red through violet, then the neutrals. Ordered by hue
// rather than by when each colour was wanted, so picking "a bit more orange
// than that one" means moving one square, and each family sits together where
// it can be compared with itself.
//
// Two rows per family -- a bright and a deep -- because a card colour is doing
// two jobs at once: a tile you can find at a glance, and a fader accent that
// must not glow on a dark strip.
const QColor kSwatches[] = {
    // reds → oranges → yellows
    QColor(0xe8, 0x45, 0x5a), QColor(0xb9, 0x2b, 0x3e),
    QColor(0xf9, 0x73, 0x16), QColor(0xc2, 0x41, 0x0c),
    QColor(0xf5, 0x9e, 0x0b), QColor(0xd9, 0x77, 0x06),
    QColor(0xfa, 0xcc, 0x15), QColor(0xa1, 0x6e, 0x07),
    // greens
    QColor(0x84, 0xcc, 0x16), QColor(0x4d, 0x7c, 0x0f),
    QColor(0x3d, 0xd6, 0x8c), QColor(0x10, 0xb9, 0x81),
    QColor(0x0f, 0x76, 0x6e), QColor(0x14, 0xb8, 0xa6),
    // cyans → blues
    QColor(0x22, 0xd3, 0xee), QColor(0x06, 0xb6, 0xd4),
    QColor(0x3b, 0x82, 0xf6), QColor(0x1d, 0x4e, 0xd8),
    QColor(0x5b, 0x6c, 0xf0), QColor(0x43, 0x38, 0xca),
    // violets → pinks
    QColor(0xa9, 0x6d, 0xf5), QColor(0x7c, 0x3a, 0xed),
    QColor(0xd9, 0x46, 0xef), QColor(0x9d, 0x4a, 0xe0),
    QColor(0xec, 0x48, 0x99), QColor(0xbe, 0x18, 0x5d),
    // neutrals, pure white last
    QColor(0x8b, 0x5c, 0x2e), QColor(0x6a, 0x6a, 0x76),
    QColor(0x9a, 0x9a, 0xa4), QColor(0xd4, 0xd4, 0xdc),
    QColor(0xff, 0xff, 0xff),
};

// Icons that are part of the mixer's own furniture rather than something a
// card would be identified by. Offering an arrow or a trash can as a channel
// icon is offering a mistake.
bool isChromeIcon(const QString &name) {
    static const QStringList kChrome = {
        QStringLiteral("arrow-down"),  QStringLiteral("arrow-left"),
        QStringLiteral("arrow-right"), QStringLiteral("arrow-up"),
        QStringLiteral("build"),       QStringLiteral("chevron"),
        QStringLiteral("ear"),         QStringLiteral("ear-off"),
        QStringLiteral("equal"),
        QStringLiteral("gear"),        QStringLiteral("info"),
        QStringLiteral("minus"),       QStringLiteral("plus"),
        QStringLiteral("trash"),       QStringLiteral("heartbeat"),
        QStringLiteral("edit"),        QStringLiteral("export"),
        QStringLiteral("import"),      QStringLiteral("save"),
    };
    return kChrome.contains(name) || name.endsWith(QStringLiteral("-off"));
}

QPixmap tilePreview(const QColor &fill, const QString &icon, int px) {
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(px, px) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, px, px), px * 0.29, px * 0.29);
    p.fillPath(path, fill);
    const int ip = int(px * 0.62);
    p.drawPixmap(QPointF((px - ip) / 2.0, (px - ip) / 2.0),
                 Theme::iconPixmap(icon, Theme::glyphOn(fill), ip));
    return pm;
}

QLabel *caption(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(pal);
    return l;
}

}  // namespace

CardIdentityDialog::CardIdentityDialog(const QString &title, const QString &name,
                                       const QColor &color, const QString &icon,
                                       const QColor &defaultColor,
                                       const QString &defaultIcon,
                                       QWidget *parent)
    : QDialog(parent),
      color_(color.isValid() ? color : defaultColor),
      icon_(icon.isEmpty() ? defaultIcon : icon),
      defaultColor_(defaultColor),
      defaultIcon_(defaultIcon),
      usingDefaults_(!color.isValid() && icon.isEmpty()) {
    setWindowTitle(title);
    setMinimumWidth(430);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 16, 16, 16);
    lay->setSpacing(12);

    // ------------------------------------------------------ name + preview
    auto *top = new QHBoxLayout;
    top->setSpacing(12);
    preview_ = new QLabel(this);
    preview_->setFixedSize(kPreviewPx, kPreviewPx);
    top->addWidget(preview_);

    auto *nameCol = new QVBoxLayout;
    nameCol->setSpacing(4);
    nameCol->addWidget(caption(tr("Name"), this));
    name_ = new QLineEdit(name, this);
    name_->setPlaceholderText(tr("Card name"));
    nameCol->addWidget(name_);
    top->addLayout(nameCol, 1);
    lay->addLayout(top);

    // ------------------------------------------------------------- colour
    lay->addWidget(caption(tr("Colour"), this));
    auto *swatches = new QGridLayout;
    swatches->setSpacing(6);
    buildColorRow(swatches, 0);
    lay->addLayout(swatches);

    // --------------------------------------------------------------- icon
    auto *iconHead = new QHBoxLayout;
    iconHead->addWidget(caption(tr("Icon"), this), 1);
    auto *addSvg = new QPushButton(tr("Add SVG…"), this);
    addSvg->setToolTip(
        tr("Copies an SVG into %1 and offers it here from now on.\n"
           "Icons drawn with currentColor take the card's colour; one with "
           "colours of its own keeps them.")
            .arg(Theme::userIconDir()));
    connect(addSvg, &QPushButton::clicked, this, &CardIdentityDialog::importIcon);
    iconHead->addWidget(addSvg);
    lay->addLayout(iconHead);

    buildIconGrid();
    lay->addWidget(iconScroll_, 1);

    // ------------------------------------------------------------ buttons
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *reset = buttons->addButton(tr("Use default"), QDialogButtonBox::ResetRole);
    reset->setToolTip(tr("Hand the colour and the icon back to the theme. The "
                         "name is left as it is."));
    connect(reset, &QPushButton::clicked, this, &CardIdentityDialog::useDefaults);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    updatePreview();
    name_->setFocus();
    name_->selectAll();
}

QString CardIdentityDialog::name() const { return name_->text().trimmed(); }

void CardIdentityDialog::buildColorRow(QGridLayout *grid, int row) {
    const int count = int(sizeof(kSwatches) / sizeof(kSwatches[0]));
    int col = 0;
    for (int i = 0; i < count; ++i) {
        const QColor c = kSwatches[i];
        auto *b = new QToolButton(this);
        b->setFixedSize(26, 26);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(c.name(QColor::HexRgb));
        b->setIconSize(QSize(22, 22));
        b->setAutoRaise(true);
        b->setProperty("swatch", c);
        swatches_ << b;
        connect(b, &QToolButton::clicked, this, [this, c] { setColor(c); });
        grid->addWidget(b, row + col / kIconColumns, col % kIconColumns);
        ++col;
    }
    auto *custom = new QPushButton(tr("Custom…"), this);
    custom->setToolTip(tr("Any colour, from the system colour picker."));
    connect(custom, &QPushButton::clicked, this,
            &CardIdentityDialog::pickCustomColor);
    // Its own row under the swatches: sharing the last one leaves it as wide
    // as whatever is left over, which is a different width for every count.
    grid->addWidget(custom, row + (col + kIconColumns - 1) / kIconColumns, 0, 1,
                    kIconColumns);
}

void CardIdentityDialog::buildIconGrid() {
    iconScroll_ = new QScrollArea(this);
    iconScroll_->setWidgetResizable(true);
    iconScroll_->setMinimumHeight(180);
    iconScroll_->setFrameShape(QFrame::NoFrame);
    iconHost_ = new QWidget(iconScroll_);
    new QGridLayout(iconHost_);
    iconScroll_->setWidget(iconHost_);
    refreshIconGrid();
}

void CardIdentityDialog::refreshIconGrid() {
    auto *grid = static_cast<QGridLayout *>(iconHost_->layout());
    while (QLayoutItem *item = grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    grid->setSpacing(6);
    grid->setContentsMargins(0, 0, 0, 0);

    QStringList names;
    for (const QString &n : Theme::builtinIconNames())
        if (!isChromeIcon(n)) names << n;
    // The user's own last, so an imported icon lands at the end of the grid
    // where it can be found rather than sorted into the middle of the set.
    names += Theme::userIconNames();

    int i = 0;
    for (const QString &n : names) {
        auto *b = new QToolButton(iconHost_);
        b->setFixedSize(kIconButtonPx, kIconButtonPx);
        b->setCheckable(true);
        b->setChecked(n == icon_);
        b->setAutoRaise(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(n);
        b->setIcon(QIcon(Theme::iconPixmap(n, Theme::Text, 22)));
        b->setIconSize(QSize(22, 22));
        connect(b, &QToolButton::clicked, this, [this, n] { setIcon(n); });
        grid->addWidget(b, i / kIconColumns, i % kIconColumns);
        ++i;
    }
    grid->setRowStretch(i / kIconColumns + 1, 1);
}

void CardIdentityDialog::setColor(const QColor &c, bool fromUser) {
    if (!c.isValid()) return;
    color_ = c;
    if (fromUser) usingDefaults_ = false;
    updatePreview();
}

void CardIdentityDialog::setIcon(const QString &name, bool fromUser) {
    if (name.isEmpty()) return;
    icon_ = name;
    if (fromUser) usingDefaults_ = false;
    // The checked button is whichever one matches now, so the grid never shows
    // two selections after a pick.
    for (QToolButton *b : iconHost_->findChildren<QToolButton *>())
        b->setChecked(b->toolTip() == icon_);
    updatePreview();
}

void CardIdentityDialog::updatePreview() {
    preview_->setPixmap(tilePreview(color_, icon_, kPreviewPx));
    // The name box wears the colour being chosen, for the same reason the card
    // itself does: this is the one field on screen that belongs to that card.
    QColor selection = color_;
    selection.setAlpha(90);
    name_->setStyleSheet(
        QStringLiteral("QLineEdit { background: %1; border: 1px solid %2; "
                       "border-radius: 7px; padding: 6px 9px; "
                       "selection-background-color: %3; }"
                       "QLineEdit:focus { border-color: %4; }")
            .arg(Theme::Well.name(), Theme::Line.name(),
                 selection.name(QColor::HexArgb), color_.name()));
    for (QToolButton *b : swatches_) {
        const QColor c = b->property("swatch").value<QColor>();
        b->setIcon(QIcon(tilePreview(c, icon_, 22)));
    }
    preview_->setToolTip(usingDefaults_
                             ? tr("Following the theme.")
                             : tr("%1, %2").arg(color_.name(QColor::HexRgb), icon_));
}

void CardIdentityDialog::pickCustomColor() {
    const QColor c = QColorDialog::getColor(color_, this, tr("Card colour"));
    if (c.isValid()) setColor(c);
}

void CardIdentityDialog::importIcon() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Add an SVG icon"), QDir::homePath(),
        tr("SVG icons (*.svg);;All files (*)"));
    if (path.isEmpty()) return;

    QString error;
    const QString name = Theme::importUserIcon(path, &error);
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("Add an SVG icon"),
                             error.isEmpty() ? tr("Could not add that icon.")
                                             : error);
        return;
    }
    refreshIconGrid();
    setIcon(name);
}

void CardIdentityDialog::useDefaults() {
    color_ = defaultColor_;
    icon_ = defaultIcon_;
    usingDefaults_ = true;
    for (QToolButton *b : iconHost_->findChildren<QToolButton *>())
        b->setChecked(b->toolTip() == icon_);
    updatePreview();
}

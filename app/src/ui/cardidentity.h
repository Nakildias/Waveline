// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// What a card is called, what colour it is, and which glyph sits on its tile.
//
// One dialog for both kinds of card: an input device and an app channel differ
// in what they route, not in how they are identified, and a second dialog that
// looked the same would be a second dialog to keep in step.

#pragma once

#include <QColor>
#include <QDialog>
#include <QList>
#include <QString>

class QLabel;
class QLineEdit;
class QGridLayout;
class QScrollArea;
class QToolButton;

class CardIdentityDialog : public QDialog {
    Q_OBJECT

public:
    // `name` is the card's current title; `defaultColor` / `defaultIcon` are
    // what the theme would use, so "Use default" has something to go back to.
    // `color` / `icon` are what the card is using now.
    CardIdentityDialog(const QString &title, const QString &name,
                       const QColor &color, const QString &icon,
                       const QColor &defaultColor, const QString &defaultIcon,
                       QWidget *parent = nullptr);

    QString name() const;
    // Invalid / empty when the card should follow the theme again.
    QColor color() const { return usingDefaults_ ? QColor() : color_; }
    QString icon() const { return usingDefaults_ ? QString() : icon_; }

private:
    void buildColorRow(QGridLayout *grid, int row);
    void buildIconGrid();
    void refreshIconGrid();
    void setColor(const QColor &c, bool fromUser = true);
    void setIcon(const QString &name, bool fromUser = true);
    void updatePreview();
    void pickCustomColor();
    void importIcon();
    void useDefaults();

    QLineEdit *name_ = nullptr;
    QLabel *preview_ = nullptr;
    // Kept so the swatches can be redrawn with whichever icon is selected:
    // seeing the glyph on the colour is how the contrast flip shows itself
    // before the card is committed to it.
    QList<QToolButton *> swatches_;
    QScrollArea *iconScroll_ = nullptr;
    QWidget *iconHost_ = nullptr;

    QColor color_;
    QString icon_;
    QColor defaultColor_;
    QString defaultIcon_;
    // Set while the card carries no override at all, and cleared the moment
    // the user picks anything: a card that looks default must also *be*
    // default, or it would stop following the palette.
    bool usingDefaults_ = true;
};

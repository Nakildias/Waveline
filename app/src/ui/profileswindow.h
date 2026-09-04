// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Everything you can do to a saved mixer setup, in one place.
//
// The header used to carry a "Save as..." and a "Delete" button beside the
// profile picker. Two buttons is what fits in a title bar, and it was the wrong
// two: they were the destructive half of profile management with no way to
// rename anything and no way to move a setup between machines. The picker stays
// -- switching profiles is a one-click thing and belongs where it is -- and
// everything else moved here, where a profile can be a row with its own actions
// instead of an entry in a combo box that is only editable when it is selected.

#pragma once

#include <QStringList>
#include <QWidget>

#include "widgets.h"

class MixerClient;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace profileswitch {
// Everything that has to happen before the mixer is replaced by a stored
// profile: if the live settings have drifted from the profile they came from,
// say so and offer to save them first. Returns false when the user backed out,
// in which case the switch must not happen.
//
// Shared rather than living in the panel because there are two ways into a
// profile switch -- the picker in the header and the list here -- and a warning
// you can avoid by using the other one is not a warning.
bool confirmSwitch(QWidget *parent, MixerClient *client, const QString &target);
}  // namespace profileswitch

// One profile in the list: a click loads it, the buttons on the right do
// everything else. A row rather than a list item because a QListWidget row
// cannot hold four buttons without a delegate, and a delegate cannot be hovered
// the way a card can.
class ProfileRow : public CardBase {
    Q_OBJECT

public:
    explicit ProfileRow(QWidget *parent = nullptr);

signals:
    void clicked();

protected:
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    // Both, and the press only to accept it: QWidget ignores a press it does
    // not handle, which hands the whole gesture to the parent and means the
    // release never arrives here at all.
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    void applyFill();

    bool hover_ = false;
};

class ProfilesWindow : public QWidget {
    Q_OBJECT

public:
    explicit ProfilesWindow(MixerClient *client, QWidget *parent = nullptr);

private slots:
    // Rebuilds the list, but only when it actually differs -- the client emits
    // changed() every 400 ms and a list that rebuilt on each one would take the
    // button out from under the cursor on its way down.
    void refresh();
    void onAvailabilityChanged(bool available);

private:
    void saveCurrentAs();
    void importFromFile();
    void loadProfile(const QString &name);
    void overwriteProfile(const QString &name);
    void renameProfile(const QString &name);
    void exportProfile(const QString &name);
    void deleteProfile(const QString &name);

    void rebuild(const QStringList &names, const QString &active);
    QWidget *buildRow(const QString &name, bool active);
    // Asks for a profile name, refusing empties and, unless it is `allowed`,
    // names already taken. With `allowReplace` a clash is offered as a replace
    // instead -- which is what "save as" over an existing name means, and is
    // not something a rename can do. Returns empty when the user backed out.
    QString askForName(const QString &title, const QString &label,
                       const QString &initial, const QString &allowed = {},
                       bool allowReplace = false);
    // "Studio" -> "Studio (2)" when "Studio" is taken. Used for imports, where
    // the name comes from a file rather than from someone who can see the list.
    QString freeName(const QString &wanted) const;
    void warn(const QString &title, const QString &text);

    MixerClient *client_ = nullptr;

    QVBoxLayout *listLay_ = nullptr;
    QLabel *emptyLabel_ = nullptr;
    QPushButton *saveAsBtn_ = nullptr;
    QPushButton *importBtn_ = nullptr;

    // What rebuild() last drew. See refresh().
    QStringList shownNames_;
    QString shownActive_;
    // Where the last export or import went, so a second one starts there
    // instead of back at $HOME.
    QString lastDir_;
};

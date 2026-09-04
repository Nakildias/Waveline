// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "profileswindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QVBoxLayout>

#include "mixerclient.h"
#include "theme.h"

namespace {

// The extension exported profiles get. The contents are JSON and .json is
// accepted on the way back in, but a name of its own is what makes the file
// recognisable in a downloads folder six months later.
constexpr const char *kSuffix = ".wlprofile";

QLabel *dimLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(p);
    return l;
}

// A flat icon button, sized so four of them fit on a row without crowding the
// name. Tinted rather than themed: `tint` carries the only warning this panel
// gives that the trash can is not like the other three.
QPushButton *actionButton(const QString &icon, const QString &tip,
                          const QColor &tint, QWidget *parent) {
    auto *b = new QPushButton(parent);
    b->setIcon(Theme::icon(icon, tint, 17));
    b->setIconSize(QSize(17, 17));
    b->setFixedSize(30, 28);
    b->setFlat(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setToolTip(tip);
    return b;
}

// The strip of colour down the left edge of a row.
QWidget *accentBar(const QColor &c, QWidget *parent) {
    auto *w = new QWidget(parent);
    w->setFixedWidth(3);
    // Set here rather than through the palette: the application stylesheet sets
    // `background: transparent` on QWidget, and a stylesheet beats a palette.
    w->setStyleSheet(QStringLiteral("background: %1; border-radius: 1px;")
                         .arg(c.name(QColor::HexRgb)));
    return w;
}

QLabel *activePill(QWidget *parent) {
    auto *l = new QLabel(QObject::tr("ACTIVE"), parent);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.78);
    f.setBold(true);
    l->setFont(f);
    l->setStyleSheet(
        QStringLiteral("color: %1; background: %2; border-radius: 6px; "
                       "padding: 2px 7px;")
            .arg(Theme::Accent.name(), Theme::AccentDim.name()));
    return l;
}

// A profile name that is safe as a file name, for the export dialog's
// suggestion. Only the suggestion: the user can call the file anything.
QString fileStemFor(const QString &name) {
    static const QRegularExpression unsafe(QStringLiteral(R"([^\w.\- ]+)"));
    QString s = name;
    s.replace(unsafe, QStringLiteral("_"));
    s = s.trimmed();
    return s.isEmpty() ? QStringLiteral("profile") : s;
}

}  // namespace

// =============================================================== confirmSwitch

namespace profileswitch {

bool confirmSwitch(QWidget *parent, MixerClient *client, const QString &target) {
    if (!client->available()) return false;
    const QString active = client->activeProfile();
    if (client->profileMatchesLive(active)) return true;

    QMessageBox box(parent);
    box.setWindowTitle(QObject::tr("Switch profile"));
    box.setIcon(QMessageBox::Warning);
    box.setText(
        active.isEmpty()
            ? QObject::tr("Your current mixer settings are not saved in any "
                          "profile.")
            : QObject::tr("Your current mixer settings no longer match the "
                          "profile “%1”.")
                  .arg(active));
    box.setInformativeText(
        QObject::tr("Loading “%1” replaces them. Anything you have changed "
                    "since will be lost.")
            .arg(target));

    // Named for what it does rather than "Yes": the destructive answer is the
    // one that has to be unmistakable.
    QPushButton *save = box.addButton(
        active.isEmpty() ? QObject::tr("Save as…")
                         : QObject::tr("Save to “%1” first").arg(active),
        QMessageBox::AcceptRole);
    QPushButton *discard =
        box.addButton(QObject::tr("Discard and switch"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(save);
    box.exec();

    if (box.clickedButton() == discard) return true;
    if (box.clickedButton() != save) return false;

    if (!active.isEmpty()) {
        client->saveProfile(active);
        client->refresh();
        return true;
    }

    bool ok = false;
    const QString name =
        QInputDialog::getText(parent, QObject::tr("Save profile"),
                              QObject::tr("Name for this setup:"),
                              QLineEdit::Normal, {}, &ok)
            .trimmed();
    if (!ok || name.isEmpty()) return false;
    if (client->profiles().contains(name) &&
        QMessageBox::question(
            parent, QObject::tr("Save profile"),
            QObject::tr("There is already a profile called “%1”. Replace it?")
                .arg(name)) != QMessageBox::Yes)
        return false;
    client->saveProfile(name);
    client->refresh();
    return true;
}

}  // namespace profileswitch

// ================================================================ ProfileRow

ProfileRow::ProfileRow(QWidget *parent) : CardBase(parent) {
    setRadius(10);
    setCursor(Qt::PointingHandCursor);
    applyFill();
}

void ProfileRow::applyFill() {
    setFillColor(hover_ ? Theme::CardHover : Theme::Card);
}

void ProfileRow::enterEvent(QEnterEvent *) {
    hover_ = true;
    applyFill();
}

void ProfileRow::leaveEvent(QEvent *) {
    hover_ = false;
    applyFill();
}

void ProfileRow::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) e->accept();
    else CardBase::mousePressEvent(e);
}

void ProfileRow::mouseReleaseEvent(QMouseEvent *e) {
    // Only when the release lands on the row itself. The action buttons are
    // children and consume their own clicks, so this cannot fire behind them.
    if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()))
        emit clicked();
}

// ============================================================ ProfilesWindow

ProfilesWindow::ProfilesWindow(MixerClient *client, QWidget *parent)
    : QWidget(parent, Qt::Window), client_(client) {
    setWindowTitle(tr("Manage Profiles"));
    resize(580, 540);
    setMinimumSize(460, 380);

    lastDir_ = QDir::homePath();

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(12);

    // ------------------------------------------------------------ preamble
    auto *title = new QLabel(tr("Profiles"), this);
    QFont tf = title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() * 1.2);
    title->setFont(tf);
    outer->addWidget(title);

    auto *blurb = dimLabel(
        tr("A profile is a whole mixer setup: levels, mutes, routing and "
           "effects. Click one to load it. Your day-to-day changes are kept "
           "separately and never overwrite a saved profile on their own."),
        this);
    blurb->setWordWrap(true);
    outer->addWidget(blurb);

    // ------------------------------------------------------------- toolbar
    auto *tools = new QHBoxLayout;
    tools->setSpacing(8);

    saveAsBtn_ = new QPushButton(tr("Save current setup as…"), this);
    saveAsBtn_->setIcon(Theme::icon(QStringLiteral("save"), Theme::Text, 16));
    saveAsBtn_->setToolTip(
        tr("Takes a snapshot of the mixer as it is right now and stores it "
           "under a new name."));
    connect(saveAsBtn_, &QPushButton::clicked, this,
            &ProfilesWindow::saveCurrentAs);
    tools->addWidget(saveAsBtn_);

    importBtn_ = new QPushButton(tr("Import…"), this);
    importBtn_->setIcon(Theme::icon(QStringLiteral("import"), Theme::Text, 16));
    importBtn_->setToolTip(
        tr("Adds a profile from a file someone exported. It is filed away, not "
           "loaded -- your current mixer is left alone."));
    connect(importBtn_, &QPushButton::clicked, this,
            &ProfilesWindow::importFromFile);
    tools->addWidget(importBtn_);

    tools->addStretch();
    outer->addLayout(tools);

    // ---------------------------------------------------------------- list
    // The same recessed well the input cards sit in, so a list of profiles
    // reads as a group of things rather than as a stack of loose panels.
    auto *well = new CardBase(this);
    well->setFillColor(Theme::Well);
    well->setRadius(14);
    auto *wellLay = new QVBoxLayout(well);
    wellLay->setContentsMargins(10, 10, 10, 10);

    auto *scroll = new QScrollArea(well);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);

    auto *host = new QWidget(scroll);
    host->setAutoFillBackground(false);
    listLay_ = new QVBoxLayout(host);
    listLay_->setContentsMargins(0, 0, 0, 0);
    listLay_->setSpacing(8);

    scroll->setWidget(host);
    wellLay->addWidget(scroll);
    outer->addWidget(well, 1);

    // ------------------------------------------------------------- buttons
    auto *bottom = new QHBoxLayout;
    bottom->addStretch();
    auto *close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    bottom->addWidget(close);
    outer->addLayout(bottom);

    connect(client_, &MixerClient::changed, this, &ProfilesWindow::refresh);
    connect(client_, &MixerClient::availabilityChanged, this,
            &ProfilesWindow::onAvailabilityChanged);

    saveAsBtn_->setEnabled(client_->available());
    importBtn_->setEnabled(client_->available());
    // Forced rather than left to refresh(): shownNames_ starts empty, and so
    // does the profile list on a machine with none saved, so the placeholder
    // would never be built.
    rebuild(client_->profiles(), client_->activeProfile());
}

void ProfilesWindow::onAvailabilityChanged(bool available) {
    saveAsBtn_->setEnabled(available);
    importBtn_->setEnabled(available);
    // The rows carry the same gating, and a daemon that came or went changes it
    // without changing the list of names -- so rebuild rather than refresh.
    rebuild(client_->profiles(), client_->activeProfile());
}

void ProfilesWindow::refresh() {
    const QStringList names = client_->profiles();
    const QString active = client_->activeProfile();
    if (names == shownNames_ && active == shownActive_) return;
    rebuild(names, active);
}

void ProfilesWindow::rebuild(const QStringList &names, const QString &active) {
    // Never while a confirmation or a file chooser is up. Every one of those is
    // opened from a row's own button, and that click is still on the stack
    // underneath the dialog -- while deleteLater() posted from inside a nested
    // event loop is processed by *that* loop, so the button would be freed
    // before its own handler returned. Nothing is lost by waiting: shownNames_
    // is left alone, so the next poll finds the same difference and redraws.
    if (QApplication::activeModalWidget()) return;

    shownNames_ = names;
    shownActive_ = active;

    // deleteLater(), not delete: a rebuild is routinely triggered from inside
    // one of these rows' own button handlers, and tearing the button down while
    // it is still emitting is a crash.
    while (QLayoutItem *item = listLay_->takeAt(0)) {
        if (QWidget *w = item->widget()) w->deleteLater();
        delete item;
    }

    if (names.isEmpty()) {
        emptyLabel_ = dimLabel(
            tr("No profiles saved yet.\n\nSet the mixer up the way you like "
               "it, then use “Save current setup as…”."),
            listLay_->parentWidget());
        emptyLabel_->setAlignment(Qt::AlignCenter);
        emptyLabel_->setWordWrap(true);
        listLay_->addWidget(emptyLabel_);
    } else {
        emptyLabel_ = nullptr;
        for (const QString &n : names)
            listLay_->addWidget(buildRow(n, n == active));
    }
    listLay_->addStretch();
}

QWidget *ProfilesWindow::buildRow(const QString &name, bool active) {
    auto *row = new ProfileRow;
    row->setToolTip(tr("Load “%1” into the mixer.").arg(name));
    connect(row, &ProfileRow::clicked, this, [this, name] { loadProfile(name); });

    auto *lay = new QHBoxLayout(row);
    lay->setContentsMargins(10, 8, 8, 8);
    lay->setSpacing(10);

    lay->addWidget(accentBar(active ? Theme::Accent : Theme::Line, row));

    auto *label = new QLabel(name, row);
    QFont lf = label->font();
    lf.setBold(active);
    label->setFont(lf);
    lay->addWidget(label, 1);

    if (active) lay->addWidget(activePill(row));

    auto *save = actionButton(QStringLiteral("save"),
                              tr("Overwrite “%1” with the mixer's current "
                                 "settings.").arg(name),
                              Theme::TextDim, row);
    connect(save, &QPushButton::clicked, this,
            [this, name] { overwriteProfile(name); });
    lay->addWidget(save);

    auto *edit = actionButton(QStringLiteral("edit"),
                              tr("Rename “%1”.").arg(name), Theme::TextDim, row);
    connect(edit, &QPushButton::clicked, this,
            [this, name] { renameProfile(name); });
    lay->addWidget(edit);

    auto *exp = actionButton(QStringLiteral("export"),
                             tr("Export “%1” to a file.").arg(name),
                             Theme::TextDim, row);
    connect(exp, &QPushButton::clicked, this,
            [this, name] { exportProfile(name); });
    lay->addWidget(exp);

    auto *del = actionButton(QStringLiteral("trash"),
                             tr("Delete “%1”.").arg(name), Theme::Danger, row);
    connect(del, &QPushButton::clicked, this,
            [this, name] { deleteProfile(name); });
    lay->addWidget(del);

    const bool available = client_->available();
    for (QPushButton *b : {save, edit, exp, del}) b->setEnabled(available);
    row->setEnabled(available);

    return row;
}

// ================================================================== actions

void ProfilesWindow::loadProfile(const QString &name) {
    if (!client_->available() || name == client_->activeProfile()) return;
    if (!profileswitch::confirmSwitch(this, client_, name)) return;
    client_->loadProfile(name);
    client_->refresh();
}

void ProfilesWindow::saveCurrentAs() {
    const QString name =
        askForName(tr("Save profile"), tr("Name for this setup:"),
                   client_->activeProfile(), {}, /*allowReplace=*/true);
    if (name.isEmpty()) return;
    client_->saveProfile(name);
    client_->refresh();
}

void ProfilesWindow::overwriteProfile(const QString &name) {
    if (QMessageBox::question(
            this, tr("Overwrite profile"),
            tr("Replace what is saved in “%1” with the mixer's current "
               "settings?\n\nThe settings currently stored in it are lost.")
                .arg(name)) != QMessageBox::Yes)
        return;
    // SaveProfile is a save-as that happens to reuse an existing name, so this
    // also makes the profile active -- which is right: what is saved in it and
    // what you are listening to are now the same thing.
    client_->saveProfile(name);
    client_->refresh();
}

void ProfilesWindow::renameProfile(const QString &name) {
    const QString wanted =
        askForName(tr("Rename profile"), tr("New name:"), name, name);
    if (wanted.isEmpty() || wanted == name) return;
    if (!client_->renameProfile(name, wanted)) {
        warn(tr("Rename profile"),
             tr("Could not rename “%1”.").arg(name));
        return;
    }
    client_->refresh();
}

void ProfilesWindow::exportProfile(const QString &name) {
    const QString json = client_->exportProfile(name);
    if (json.isEmpty()) {
        warn(tr("Export profile"),
             tr("Could not read “%1” back from wavelined.").arg(name));
        return;
    }

    QString path = QFileDialog::getSaveFileName(
        this, tr("Export profile"),
        QDir(lastDir_).filePath(fileStemFor(name) + QLatin1String(kSuffix)),
        tr("Waveline profile (*.wlprofile);;JSON (*.json);;All files (*)"));
    if (path.isEmpty()) return;
    // Only when the user typed no extension at all: someone who asked for
    // "backup.json" meant it.
    if (QFileInfo(path).suffix().isEmpty()) path += QLatin1String(kSuffix);
    lastDir_ = QFileInfo(path).absolutePath();

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        warn(tr("Export profile"),
             tr("Could not write %1:\n%2").arg(path, f.errorString()));
        return;
    }
    f.write(json.toUtf8());
    f.close();
}

void ProfilesWindow::importFromFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import profile"), lastDir_,
        tr("Waveline profiles (*.wlprofile *.json);;All files (*)"));
    if (path.isEmpty()) return;
    lastDir_ = QFileInfo(path).absolutePath();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        warn(tr("Import profile"),
             tr("Could not read %1:\n%2").arg(path, f.errorString()));
        return;
    }
    const QByteArray blob = f.readAll();
    f.close();

    // Parsed here as well as in the daemon, for the name only: the panel has to
    // know what the file calls itself before it can ask about a clash, and the
    // daemon has no way to ask. The daemon still validates it properly -- what
    // is done with a bad file is refuse it there, not trust this.
    QString suggested;
    const QJsonDocument doc = QJsonDocument::fromJson(blob);
    if (doc.isObject())
        suggested = doc.object().value(QStringLiteral("name")).toString().trimmed();
    if (suggested.isEmpty()) suggested = QFileInfo(path).completeBaseName();

    QString name = suggested;
    if (client_->profiles().contains(name)) {
        QMessageBox box(this);
        box.setWindowTitle(tr("Import profile"));
        box.setIcon(QMessageBox::Question);
        box.setText(tr("There is already a profile called “%1”.").arg(name));
        box.setInformativeText(
            tr("Replace it with the one in this file, or keep both?"));
        QPushButton *replace =
            box.addButton(tr("Replace"), QMessageBox::DestructiveRole);
        QPushButton *both = box.addButton(tr("Keep both"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(both);
        box.exec();
        if (box.clickedButton() == both) name = freeName(name);
        else if (box.clickedButton() != replace) return;
    }

    if (!client_->importProfile(name, QString::fromUtf8(blob))) {
        const QString why = client_->lastError();
        warn(tr("Import profile"),
             why.isEmpty()
                 ? tr("%1 is not a Waveline profile.").arg(QFileInfo(path).fileName())
                 : tr("Could not import %1:\n%2")
                       .arg(QFileInfo(path).fileName(), why));
        return;
    }
    client_->refresh();
}

void ProfilesWindow::deleteProfile(const QString &name) {
    if (QMessageBox::question(
            this, tr("Delete profile"),
            tr("Delete the profile “%1”?\n\nYour current mixer settings are "
               "kept -- only the saved snapshot is removed.")
                .arg(name)) != QMessageBox::Yes)
        return;
    client_->deleteProfile(name);
    client_->refresh();
}

// ================================================================== helpers

QString ProfilesWindow::askForName(const QString &title, const QString &label,
                                   const QString &initial,
                                   const QString &allowed, bool allowReplace) {
    QString seed = initial;
    for (;;) {
        bool ok = false;
        const QString typed =
            QInputDialog::getText(this, title, label, QLineEdit::Normal, seed, &ok)
                .trimmed();
        if (!ok) return {};
        if (typed.isEmpty()) {
            warn(title, tr("A profile needs a name."));
            seed = initial;
            continue;
        }
        // Asked again rather than refused outright: the answer to a clash is
        // almost always "then call it something else", and dropping the user
        // back to the list to start over is a worse way to say so.
        if (typed != allowed && client_->profiles().contains(typed)) {
            if (allowReplace &&
                QMessageBox::question(
                    this, title,
                    tr("There is already a profile called “%1”. Replace it?")
                        .arg(typed)) == QMessageBox::Yes)
                return typed;
            if (!allowReplace)
                warn(title,
                     tr("There is already a profile called “%1”.").arg(typed));
            seed = typed;
            continue;
        }
        return typed;
    }
}

QString ProfilesWindow::freeName(const QString &wanted) const {
    const QStringList taken = client_->profiles();
    if (!taken.contains(wanted)) return wanted;
    for (int i = 2; i < 1000; ++i) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(wanted).arg(i);
        if (!taken.contains(candidate)) return candidate;
    }
    return wanted;
}

void ProfilesWindow::warn(const QString &title, const QString &text) {
    QMessageBox::warning(this, title, text);
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "aboutwindow.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QFrame>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "theme.h"
#include "version.h"
#include "widgets.h"

namespace {

constexpr const char *kAuthor = "Nakildias";
constexpr const char *kEmail = "nakildiaspro@gmail.com";
constexpr const char *kRepoUrl = "https://github.com/Nakildias/Waveline";
constexpr const char *kVersionCheckUrl =
    "https://raw.githubusercontent.com/Nakildias/Waveline/main/VERSION";

// The same one-liner the README documents. Updating is a root shell command, so
// the button hands the user the command rather than running it: a GUI that
// silently elevates and rewrites the system it is running on is not a thing
// anyone asked for, and the terminal shows them what it is doing.
constexpr const char *kInstallCommand =
    "sudo bash -c \"$(curl -fsSL "
    "https://raw.githubusercontent.com/Nakildias/Waveline/main/scripts/"
    "install-waveline.sh)\"";

// Light blue — readable on the dark card without the heaviness of default link blue.
constexpr QColor kLinkColor(0x6e, 0xe7, 0xff);

QLabel *dimLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, Theme::TextDim);
    l->setPalette(p);
    l->setAlignment(Qt::AlignCenter);
    l->setWordWrap(true);
    return l;
}

QLabel *bodyLabel(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    l->setAlignment(Qt::AlignCenter);
    l->setWordWrap(true);
    return l;
}

QLabel *linkLabel(const QString &html, QWidget *parent) {
    auto *l = new QLabel(html, parent);
    l->setOpenExternalLinks(true);
    l->setAlignment(Qt::AlignCenter);
    l->setTextFormat(Qt::RichText);
    l->setTextInteractionFlags(Qt::TextBrowserInteraction);
    QPalette p = l->palette();
    p.setColor(QPalette::Link, kLinkColor);
    p.setColor(QPalette::LinkVisited, kLinkColor);
    l->setPalette(p);
    return l;
}

QString accentLink(const QString &url, const QString &label) {
    // QLabel rich text ignores QSS anchor rules; colour has to live on the tag.
    return QStringLiteral("<a href=\"%1\" style=\"color: %2; text-decoration: none;\">%3</a>")
        .arg(url, kLinkColor.name(), label);
}

struct VersionParts {
    int year = 0;
    int month = 0;
    int day = 0;
    int build = 0;
    bool valid = false;
};

VersionParts parseVersion(const QString &text) {
    VersionParts parts;
    const QString trimmed = text.trimmed();
    const int dash = trimmed.indexOf(QLatin1Char('-'));
    const QString datePart = dash >= 0 ? trimmed.left(dash) : trimmed;
    if (dash >= 0)
        parts.build = trimmed.mid(dash + 1).toInt();

    const QStringList dateFields = datePart.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (dateFields.size() != 3)
        return parts;

    parts.year = dateFields[0].toInt();
    parts.month = dateFields[1].toInt();
    parts.day = dateFields[2].toInt();
    parts.valid = parts.year > 0 && parts.month >= 1 && parts.month <= 12
                  && parts.day >= 1 && parts.day <= 31 && parts.build >= 0;
    return parts;
}

bool isVersionNewer(const QString &remote, const QString &local) {
    const VersionParts r = parseVersion(remote);
    const VersionParts l = parseVersion(local);
    if (!r.valid || !l.valid)
        return false;

    if (r.year != l.year) return r.year > l.year;
    if (r.month != l.month) return r.month > l.month;
    if (r.day != l.day) return r.day > l.day;
    return r.build > l.build;
}

}  // namespace

AboutWindow::AboutWindow(QWidget *parent)
    : QWidget(parent, Qt::Window), nam_(new QNetworkAccessManager(this)) {
    setWindowTitle(tr("About Waveline"));
    setFixedSize(500, 430);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);

    auto *card = new CardBase(this);
    card->setFillColor(Theme::Card);
    card->setRadius(14);
    card->setTopStripe(Theme::Accent);
    outer->addWidget(card);

    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(28, 28, 28, 28);
    lay->setSpacing(8);
    lay->setAlignment(Qt::AlignHCenter);

    auto *icon = new QLabel(card);
    icon->setPixmap(
        QIcon(QStringLiteral(":/waveline/waveline-mixer.png")).pixmap(64));
    icon->setAlignment(Qt::AlignCenter);
    lay->addWidget(icon, 0, Qt::AlignHCenter);
    lay->addSpacing(4);

    auto *mark = new QLabel(QStringLiteral("WAVELINE"), card);
    QFont mf = mark->font();
    mf.setBold(true);
    mf.setPointSizeF(mf.pointSizeF() * 1.2);
    mf.setLetterSpacing(QFont::AbsoluteSpacing, 1.6);
    mark->setFont(mf);
    mark->setAlignment(Qt::AlignCenter);
    lay->addWidget(mark, 0, Qt::AlignHCenter);

    lay->addWidget(
        dimLabel(tr("Version %1").arg(QStringLiteral(WAVELINE_VERSION)), card),
        0, Qt::AlignHCenter);

    lay->addSpacing(10);

    updateStatus_ = dimLabel(tr("Checking for updates..."), card);
    lay->addWidget(updateStatus_, 0, Qt::AlignHCenter);

    updateBtn_ = new QPushButton(tr("Copy install command"), card);
    updateBtn_->hide();
    updateBtn_->setToolTip(QString::fromLatin1(kInstallCommand));
    updateBtn_->setCursor(Qt::PointingHandCursor);
    updateBtn_->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: %1;"
        "  border: 1px solid %2;"
        "  color: %3;"
        "  padding: 7px 18px;"
        "  border-radius: 7px;"
        "}"
        "QPushButton:hover { background: %2; }"
        "QPushButton:pressed { background: %4; }")
                                    .arg(Theme::AccentDim.name(), Theme::Accent.name(),
                                         Theme::Text.name(), Theme::Accent.name()));
    connect(updateBtn_, &QPushButton::clicked, this, &AboutWindow::copyInstallCommand);
    lay->addWidget(updateBtn_, 0, Qt::AlignHCenter);

    updateHint_ = dimLabel(tr("Paste it in a terminal to update."), card);
    updateHint_->hide();
    lay->addWidget(updateHint_, 0, Qt::AlignHCenter);

    lay->addSpacing(6);

    auto *line = new QFrame(card);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setFixedHeight(1);
    line->setStyleSheet(
        QStringLiteral("background: %1; border: none;").arg(Theme::Line.name()));
    line->setFixedWidth(380);
    lay->addWidget(line, 0, Qt::AlignHCenter);

    lay->addSpacing(6);

    lay->addWidget(bodyLabel(tr("Copyright \u00a9 2026 %1")
                                 .arg(QString::fromLatin1(kAuthor)),
                           card),
                   0, Qt::AlignHCenter);

    lay->addWidget(
        linkLabel(accentLink(QStringLiteral("mailto:%1").arg(QString::fromLatin1(kEmail)),
                             QString::fromLatin1(kEmail)),
                  card),
        0, Qt::AlignHCenter);

    auto *license = dimLabel(
        tr("Licensed under the GNU General Public License v2.0 or later"), card);
    license->setWordWrap(false);
    lay->addWidget(license, 0, Qt::AlignHCenter);

    lay->addWidget(
        linkLabel(accentLink(QString::fromLatin1(kRepoUrl),
                             QStringLiteral("github.com/Nakildias/Waveline")),
                  card),
        0, Qt::AlignHCenter);
}

void AboutWindow::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    startUpdateCheck();
}

void AboutWindow::startUpdateCheck() {
    if (pendingReply_) {
        pendingReply_->abort();
        pendingReply_->deleteLater();
        pendingReply_ = nullptr;
    }

    updateStatus_->setText(tr("Checking for updates..."));
    updateBtn_->hide();
    updateHint_->hide();

    QNetworkRequest req(QUrl(QString::fromLatin1(kVersionCheckUrl)));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    pendingReply_ = nam_->get(req);
    connect(pendingReply_, &QNetworkReply::finished, this,
            &AboutWindow::onUpdateReplyFinished);
}

void AboutWindow::onUpdateReplyFinished() {
    auto *reply = pendingReply_;
    pendingReply_ = nullptr;
    if (!reply)
        return;

    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        updateStatus_->setText(tr("Unable to check for update"));
        updateBtn_->hide();
        updateHint_->hide();
        return;
    }

    const QString remote = QString::fromUtf8(reply->readAll()).trimmed();
    const VersionParts remoteParts = parseVersion(remote);
    if (!remoteParts.valid) {
        updateStatus_->setText(tr("Unable to check for update"));
        updateBtn_->hide();
        updateHint_->hide();
        return;
    }

    if (isVersionNewer(remote, QStringLiteral(WAVELINE_VERSION))) {
        updateStatus_->setText(tr("Update available: %1").arg(remote));
        // Reset in case the window is reopened after a previous copy left the
        // button reading "Copied".
        updateBtn_->setText(tr("Copy install command"));
        updateBtn_->show();
        updateHint_->show();
    } else {
        updateStatus_->setText(tr("Up to date"));
        updateBtn_->hide();
        updateHint_->hide();
    }
}

void AboutWindow::openRepository() {
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kRepoUrl)));
}

void AboutWindow::copyInstallCommand() {
    QClipboard *clip = QGuiApplication::clipboard();
    if (!clip) {
        updateHint_->setText(tr("No clipboard available on this desktop."));
        return;
    }
    clip->setText(QString::fromLatin1(kInstallCommand));

    updateBtn_->setText(tr("Copied"));
    // `this` as the context object, so a pending timer is dropped if the window
    // goes away before it fires.
    QTimer::singleShot(2000, this, [this]() {
        if (updateBtn_ && updateBtn_->isVisible())
            updateBtn_->setText(tr("Copy install command"));
    });
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
#include "desktopnames.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>

namespace waveline {

namespace {

// A save is a write to a temporary file and a rename over the target, which
// the watcher sees as a delete. Re-arming a beat later catches the new file
// rather than the gap.
constexpr int kRewatchDelayMs = 120;

QString namesPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("desktop-environment/audio-device-names.json"));
}

}  // namespace

DesktopNames &DesktopNames::instance() {
    static DesktopNames names;
    return names;
}

DesktopNames::DesktopNames() {
    reload();

    watcher_ = new QFileSystemWatcher(this);
    connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString &) {
        reload();
        QTimer::singleShot(kRewatchDelayMs, this, [this] { watch(); });
        emit changed();
    });
    // The directory as well: on a machine where nothing has ever been renamed
    // the file does not exist yet, and there would be nothing to watch.
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this](const QString &) {
        reload();
        watch();
        emit changed();
    });
    watch();
}

void DesktopNames::watch() {
    if (!watcher_) return;
    const QString file = namesPath();
    const QString dir = QFileInfo(file).absolutePath();
    if (!watcher_->directories().contains(dir) && QFileInfo::exists(dir))
        watcher_->addPath(dir);
    if (!watcher_->files().contains(file) && QFileInfo::exists(file))
        watcher_->addPath(file);
}

void DesktopNames::reload() {
    QHash<QString, QString> names;
    QFile file(namesPath());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const QString value = it.value().toString().trimmed();
            if (!value.isEmpty()) names.insert(it.key(), value);
        }
    }
    names_ = std::move(names);
}

QString DesktopNames::apply(const QString &nodeName, const QString &fallback) const {
    if (nodeName.isEmpty()) return fallback;
    const QString name = names_.value(nodeName);
    return name.isEmpty() ? fallback : name;
}

}  // namespace waveline

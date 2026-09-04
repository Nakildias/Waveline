// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "rackpresetstore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace {
constexpr int kFileVersion = 1;
}  // namespace

RackPresetStore &RackPresetStore::instance() {
    static RackPresetStore store;
    return store;
}

RackPresetStore::RackPresetStore() { load(); }

QString RackPresetStore::filePath() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
        QStringLiteral("/waveline");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/rack-presets.json");
}

void RackPresetStore::load() {
    presets_.clear();
    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject presets = root.value(QStringLiteral("presets")).toObject();
    for (auto it = presets.constBegin(); it != presets.constEnd(); ++it)
        presets_.insert(it.key(), it.value().toString());
}

// Written with QSaveFile (temp file + atomic rename), the same durability
// ConfigStore's own config.json write uses -- a crash or power loss mid-write
// leaves the previous file intact rather than a half-written, unparsable one.
void RackPresetStore::persist() const {
    QJsonObject presets;
    for (auto it = presets_.constBegin(); it != presets_.constEnd(); ++it)
        presets.insert(it.key(), it.value());

    QJsonObject root;
    root[QStringLiteral("version")] = kFileVersion;
    root[QStringLiteral("presets")] = presets;

    QSaveFile f(filePath());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.commit();
}

QStringList RackPresetStore::names() const { return presets_.keys(); }

bool RackPresetStore::contains(const QString &name) const { return presets_.contains(name); }

QString RackPresetStore::spec(const QString &name) const { return presets_.value(name); }

void RackPresetStore::save(const QString &name, const QString &spec) {
    presets_.insert(name, spec);
    persist();
}

bool RackPresetStore::rename(const QString &from, const QString &to) {
    if (from == to || !presets_.contains(from) || presets_.contains(to)) return false;
    presets_.insert(to, presets_.take(from));
    persist();
    return true;
}

bool RackPresetStore::remove(const QString &name) {
    if (presets_.remove(name) == 0) return false;
    persist();
    return true;
}

QString RackPresetStore::freeName(const QString &wanted) const {
    if (!presets_.contains(wanted)) return wanted;
    for (int i = 2; i < 1000; ++i) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(wanted).arg(i);
        if (!presets_.contains(candidate)) return candidate;
    }
    return wanted;
}

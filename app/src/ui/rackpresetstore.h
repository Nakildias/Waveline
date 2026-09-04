// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Application-wide library of named Creative FX Rack presets, shared by
// every Virtual Rack window regardless of which input device it's driving --
// a preset is just a named engine/creativefxspec.h wire-format string (see
// mixerclient.h's encodeCreativeFxSpec/decodeCreativeFxSpec), so "save this
// rack" and "load it onto a different device's rack" are the same
// operation. Persisted client-side (~/.config/waveline/rack-presets.json)
// rather than through the daemon: a preset is inert data until loaded,
// unlike a Profile, which the daemon must apply live, so there's nothing
// here that needs a D-Bus round trip.

#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

class RackPresetStore {
public:
    static RackPresetStore &instance();

    QStringList names() const;
    bool contains(const QString &name) const;
    QString spec(const QString &name) const;

    void save(const QString &name, const QString &spec);
    bool rename(const QString &from, const QString &to);
    bool remove(const QString &name);

    // "Name" if free, otherwise "Name (2)", "Name (3)", ... -- for offering
    // "keep both" on an import that collides with an existing preset.
    QString freeName(const QString &wanted) const;

private:
    RackPresetStore();
    void load();
    void persist() const;
    static QString filePath();

    // QMap keeps keys sorted, which is exactly the order names() and the
    // Presets menu want -- no separate sort step needed.
    QMap<QString, QString> presets_;
};

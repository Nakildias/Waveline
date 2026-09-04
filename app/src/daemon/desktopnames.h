// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
#pragma once

// Device names a desktop shell has been told to use, if one has.
//
// Some desktops let a user rename an audio device without touching the graph:
// the name is an overlay the shell applies when it draws a device, so it takes
// effect while audio is playing instead of costing a restart of WirePlumber.
// Where such a file exists, the mixer reads it, so one device does not go by
// two names on one machine.
//
//     $XDG_CONFIG_HOME/desktop-environment/audio-device-names.json
//     { "<node.name>": "<what to call it>" }
//
// Entirely optional, and deliberately one-way. Waveline never writes this file
// and does not require it: with no file, or no entry for a device, every name
// is exactly what it was before this existed. Nothing else in the daemon
// depends on it, and a machine running no desktop shell at all behaves
// identically.
//
// Read once and watched, so a rename shows up here without a poll and without
// the shell having to tell us anything.

#include <QHash>
#include <QObject>
#include <QString>

class QFileSystemWatcher;

namespace waveline {

class DesktopNames : public QObject {
    Q_OBJECT

public:
    static DesktopNames &instance();

    // The name to show for the node called `nodeName`, or `fallback` when the
    // desktop has nothing to say about it -- which is the ordinary case.
    QString apply(const QString &nodeName, const QString &fallback) const;

    // std::string convenience, since most of the daemon's node names are one.
    QString apply(const std::string &nodeName, const std::string &fallback) const {
        return apply(QString::fromStdString(nodeName), QString::fromStdString(fallback));
    }

signals:
    // A name changed. The service emits its own Changed() from this, so a
    // client that is showing a device redraws it.
    void changed();

private:
    DesktopNames();

    void reload();
    void watch();

    QHash<QString, QString> names_;
    QFileSystemWatcher *watcher_ = nullptr;
};

}  // namespace waveline

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Ported unchanged from Monarchy's src/common/scroll (same author, same
// licence) rather than linked, for the reason chrome.h gives: that is a static
// library Monarchy's applications compile in from that tree, and the mixer is
// built on machines that have never seen it. Keep it in step with the
// original -- the point is that the rack scrolls like every other Monarchy
// view, and reads the same ~/.config tuning.

#include "scroll_settings.h"

#include <algorithm>
#include <map>
#include <memory>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace desktop {


namespace {

// Nested under one object so the global config stays legible as it grows.
constexpr char kScrollObjectKey[] = "scrolling";

QString configDirPath() {
  const QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
  return base + QStringLiteral("/monarchy");
}

QString stripJsonComments(const QString& text) {
  QString result;
  result.reserve(text.size());

  bool in_string = false;
  bool escape = false;
  for (int i = 0; i < text.size(); ++i) {
    const QChar ch = text.at(i);
    if (in_string) {
      result.append(ch);
      if (escape) {
        escape = false;
      } else if (ch == QLatin1Char('\\')) {
        escape = true;
      } else if (ch == QLatin1Char('"')) {
        in_string = false;
      }
      continue;
    }

    if (ch == QLatin1Char('"')) {
      in_string = true;
      result.append(ch);
      continue;
    }

    if (ch == QLatin1Char('/') && i + 1 < text.size()) {
      const QChar next = text.at(i + 1);
      if (next == QLatin1Char('/')) {
        while (i < text.size() && text.at(i) != QLatin1Char('\n')) {
          ++i;
        }
        if (i < text.size()) {
          result.append(QLatin1Char('\n'));
        }
        continue;
      }
      if (next == QLatin1Char('*')) {
        i += 2;
        while (i + 1 < text.size() &&
               !(text.at(i) == QLatin1Char('*') && text.at(i + 1) == QLatin1Char('/'))) {
          ++i;
        }
        i += 1;
        continue;
      }
    }

    result.append(ch);
  }

  return result;
}

QJsonObject readJsonObject(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return {};
  }
  const QString text = QString::fromUtf8(file.readAll());
  const QJsonDocument doc = QJsonDocument::fromJson(stripJsonComments(text).toUtf8());
  return doc.isObject() ? doc.object() : QJsonObject{};
}

QJsonObject readConfigObject() { return readJsonObject(scrollSettingsPath()); }

void readInto(const QJsonObject& obj, const QString& key, bool* out) {
  if (obj.contains(key)) {
    *out = obj.value(key).toBool(*out);
  }
}

void readInto(const QJsonObject& obj, const QString& key, double* out) {
  if (obj.contains(key)) {
    *out = obj.value(key).toDouble(*out);
  }
}

void readInto(const QJsonObject& obj, const QString& key, ScrollBarVisibility* out) {
  if (obj.contains(key)) {
    *out = scrollBarVisibilityFromString(obj.value(key).toString(), *out);
  }
}

void readScrollObject(const QJsonObject& obj, const QString& prefix, ScrollSettings* settings) {
  readInto(obj, prefix + QStringLiteral("smooth-scroll"), &settings->smooth);
  readInto(obj, prefix + QStringLiteral("trackpad-scroll"), &settings->trackpad);
  readInto(obj, prefix + QStringLiteral("scroll-bars"), &settings->scrollbars);
  readInto(obj, prefix + QStringLiteral("trackpad-speed"), &settings->trackpad_speed);
  readInto(obj, prefix + QStringLiteral("trackpad-momentum"), &settings->trackpad_momentum);
  readInto(obj, prefix + QStringLiteral("overscroll-spring-k"), &settings->overscroll_spring_k);
  readInto(obj, prefix + QStringLiteral("overscroll-spring-damp"),
           &settings->overscroll_spring_damp);
  readInto(obj, prefix + QStringLiteral("overscroll-spring-edge-pull"),
           &settings->overscroll_edge_pull);
}

QJsonObject writeScrollObject(const ScrollSettings& settings) {
  QJsonObject obj;
  obj.insert(QStringLiteral("smooth-scroll"), settings.smooth);
  obj.insert(QStringLiteral("trackpad-scroll"), settings.trackpad);
  obj.insert(QStringLiteral("scroll-bars"), scrollBarVisibilityToString(settings.scrollbars));
  obj.insert(QStringLiteral("trackpad-speed"), settings.trackpad_speed);
  obj.insert(QStringLiteral("trackpad-momentum"), settings.trackpad_momentum);
  obj.insert(QStringLiteral("overscroll-spring-k"), settings.overscroll_spring_k);
  obj.insert(QStringLiteral("overscroll-spring-damp"), settings.overscroll_spring_damp);
  obj.insert(QStringLiteral("overscroll-spring-edge-pull"), settings.overscroll_edge_pull);
  return obj;
}

struct Cache {
  ScrollSettings settings;
  bool valid = false;
  std::unique_ptr<QFileSystemWatcher> watcher;
  std::map<int, std::function<void()>> listeners;
  int next_token = 1;
};

Cache& cache() {
  static Cache instance;
  return instance;
}

// Over a copy: a listener is free to drop itself while being run.
void notifyListeners() {
  const auto listeners = cache().listeners;
  for (const auto& entry : listeners) {
    if (entry.second) {
      entry.second();
    }
  }
}

// Watches the directory as well as the file: a config rewritten via a temp file
// and rename replaces the inode, which drops a file-only watch on the floor.
void ensureWatcher() {
  Cache& c = cache();
  if (c.watcher) {
    return;
  }
  c.watcher = std::make_unique<QFileSystemWatcher>();
  const QString path = scrollSettingsPath();
  const QString dir = QFileInfo(path).absolutePath();
  if (QFile::exists(dir)) {
    c.watcher->addPath(dir);
  }
  if (QFile::exists(path)) {
    c.watcher->addPath(path);
  }

  const auto on_change = [](const QString&) {
    Cache& inner = cache();
    inner.valid = false;
    notifyListeners();
    // Re-arm: the path may have been dropped when the file was replaced.
    const QString p = scrollSettingsPath();
    if (inner.watcher && QFile::exists(p) && !inner.watcher->files().contains(p)) {
      inner.watcher->addPath(p);
    }
  };
  QObject::connect(c.watcher.get(), &QFileSystemWatcher::fileChanged, on_change);
  QObject::connect(c.watcher.get(), &QFileSystemWatcher::directoryChanged, on_change);
}

}  // namespace

void ScrollSettings::clamp() {
  trackpad_speed = std::clamp(trackpad_speed, 0.25, 4.0);
  overscroll_spring_k = std::clamp(overscroll_spring_k, 0.01, 1.0);
  overscroll_spring_damp = std::clamp(overscroll_spring_damp, 0.10, 0.99);
  overscroll_edge_pull = std::clamp(overscroll_edge_pull, 0.10, 1.0);
}

QString scrollBarVisibilityToString(ScrollBarVisibility visibility) {
  switch (visibility) {
    case ScrollBarVisibility::Automatic:
      return QStringLiteral("automatic");
    case ScrollBarVisibility::WhenScrolling:
      return QStringLiteral("when-scrolling");
    case ScrollBarVisibility::Always:
      break;
  }
  return QStringLiteral("always");
}

ScrollBarVisibility scrollBarVisibilityFromString(const QString& text,
                                                  ScrollBarVisibility fallback) {
  const QString value = text.trimmed().toLower();
  if (value == QLatin1String("automatic")) {
    return ScrollBarVisibility::Automatic;
  }
  if (value == QLatin1String("when-scrolling")) {
    return ScrollBarVisibility::WhenScrolling;
  }
  if (value == QLatin1String("always")) {
    return ScrollBarVisibility::Always;
  }
  return fallback;
}

int addScrollSettingsListener(std::function<void()> callback) {
  Cache& c = cache();
  // The watcher is what makes a listener useful; a view may register before
  // anything has read the settings.
  ensureWatcher();
  const int token = c.next_token++;
  c.listeners.insert({token, std::move(callback)});
  return token;
}

void removeScrollSettingsListener(int token) { cache().listeners.erase(token); }

QString scrollSettingsPath() {
  return configDirPath() + QStringLiteral("/interface.jsonc");
}

ScrollSettings loadScrollSettingsFromDisk() {
  const QJsonObject root = readConfigObject();
  ScrollSettings settings;

  readScrollObject(root.value(QLatin1StringView(kScrollObjectKey)).toObject(), QString(),
                   &settings);

  settings.clamp();
  return settings;
}

const ScrollSettings& globalScrollSettings() {
  Cache& c = cache();
  ensureWatcher();
  if (!c.valid) {
    c.settings = loadScrollSettingsFromDisk();
    c.valid = true;
  }
  return c.settings;
}

void invalidateGlobalScrollSettings() {
  cache().valid = false;
  notifyListeners();
}

bool saveGlobalScrollSettings(const ScrollSettings& settings) {
  ScrollSettings clamped = settings;
  clamped.clamp();

  QDir().mkpath(configDirPath());

  QJsonObject root = readConfigObject();
  root.insert(QLatin1StringView(kScrollObjectKey), writeScrollObject(clamped));

  QFile file(scrollSettingsPath());
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    return false;
  }
  if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0) {
    return false;
  }
  file.close();

  invalidateGlobalScrollSettings();
  return true;
}

}  // namespace desktop

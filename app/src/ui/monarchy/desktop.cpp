// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "desktop.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <optional>

namespace Monarchy {

namespace {

std::optional<bool> g_forced;

// os-release, as systemd documents it: /etc first, /usr/lib as the fallback.
// Only ID and ID_LIKE matter, and neither is ever more than a word list, so
// the quoting rules are handled only as far as stripping them.
bool osReleaseSaysMonarchy() {
    for (const char *path : {"/etc/os-release", "/usr/lib/os-release"}) {
        QFile f(QString::fromLatin1(path));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq <= 0) continue;
            const QString key = line.left(eq);
            if (key != QLatin1String("ID") && key != QLatin1String("ID_LIKE")) continue;
            QString value = line.mid(eq + 1);
            value.remove(QLatin1Char('"')).remove(QLatin1Char('\''));
            if (value.split(QLatin1Char(' '), Qt::SkipEmptyParts)
                    .contains(QLatin1String("monarchy"), Qt::CaseInsensitive))
                return true;
        }
        return false;   // the first file that exists is the answer
    }
    return false;
}

QString configDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
}

QString titlebarPath() { return configDir() + QStringLiteral("/titlebar-rc"); }
QString kdeglobalsPath() { return configDir() + QStringLiteral("/kdeglobals"); }
QString kwinrcPath() { return configDir() + QStringLiteral("/kwinrc"); }
QString monarchyConfigPath() {
    return configDir() + QStringLiteral("/monarchy/config.jsonc");
}

// JSONC: the same JSON with // and /* */ comments, which QJsonDocument will
// not take. Strings are skipped over so a URL's // survives.
QString stripJsonComments(const QString &in) {
    QString out;
    out.reserve(in.size());
    bool inString = false;
    for (int i = 0; i < in.size(); ++i) {
        const QChar c = in[i];
        if (inString) {
            out += c;
            if (c == QLatin1Char('\\') && i + 1 < in.size()) out += in[++i];
            else if (c == QLatin1Char('"')) inString = false;
            continue;
        }
        if (c == QLatin1Char('"')) {
            inString = true;
            out += c;
        } else if (c == QLatin1Char('/') && i + 1 < in.size() && in[i + 1] == QLatin1Char('/')) {
            while (i < in.size() && in[i] != QLatin1Char('\n')) ++i;
            out += QLatin1Char('\n');
        } else if (c == QLatin1Char('/') && i + 1 < in.size() && in[i + 1] == QLatin1Char('*')) {
            i += 2;
            while (i + 1 < in.size() && !(in[i] == QLatin1Char('*') && in[i + 1] == QLatin1Char('/'))) ++i;
            ++i;
        } else {
            out += c;
        }
    }
    return out;
}

int gridUnit() {
    // The width of an M, not its height -- KDecoration's grid unit is meant to
    // be about a millimetre, and the width is what lands there.
    const int w = QFontMetrics(QGuiApplication::font())
                      .boundingRect(QStringLiteral("M")).width();
    return w > 0 ? w : 10;
}

qreal buttonSizeFactor(const QString &name) {
    if (name == QLatin1String("ButtonTiny")) return 1.0;
    if (name == QLatin1String("ButtonSmall")) return 1.5;
    if (name == QLatin1String("ButtonLarge")) return 2.5;
    if (name == QLatin1String("ButtonVeryLarge")) return 3.5;
    return 2.0;
}

Settings::ButtonStyle buttonStyleFor(const QString &name) {
    using S = Settings::ButtonStyle;
    static const QHash<QString, S> byName = {
        {QStringLiteral("plasma"), S::Plasma},
        {QStringLiteral("gnome"), S::Gnome},
        {QStringLiteral("macSierra"), S::MacSierra},
        {QStringLiteral("macDarkAurorae"), S::MacDarkAurorae},
        {QStringLiteral("sbeSierra"), S::SbeSierra},
        {QStringLiteral("sbeSierraActive"), S::SbeSierraActive},
        {QStringLiteral("sbeSierraInactive"), S::SbeSierraInactive},
        {QStringLiteral("sbeDarkAurorae"), S::SbeDarkAurorae},
        {QStringLiteral("sbeDarkAuroraeActive"), S::SbeDarkAuroraeActive},
        {QStringLiteral("sbeDarkAuroraeInactive"), S::SbeDarkAuroraeInactive},
        {QStringLiteral("sierraColorSymbols"), S::SierraColorSymbols},
        {QStringLiteral("darkAuroraeColorSymbols"), S::DarkAuroraeColorSymbols},
        {QStringLiteral("sierraMonochromeSymbols"), S::SierraMonochromeSymbols},
        {QStringLiteral("darkAuroraeMonochromeSymbols"), S::DarkAuroraeMonochromeSymbols},
    };
    // KConfig writes an enum by name; a hand-edited file may hold the index.
    bool numeric = false;
    const int index = name.toInt(&numeric);
    if (numeric && index >= 0 && index <= int(S::DarkAuroraeMonochromeSymbols))
        return S(index);
    return byName.value(name, S::MacDarkAurorae);
}

bool readBool(const QSettings &c, const QString &key, bool fallback) {
    const QString t = c.value(key).toString().trimmed().toLower();
    if (t == QLatin1String("true") || t == QLatin1String("1")) return true;
    if (t == QLatin1String("false") || t == QLatin1String("0")) return false;
    return fallback;
}

int readShadowSize(const QSettings &c) {
    static const QStringList names = {
        QStringLiteral("ShadowNone"), QStringLiteral("ShadowSmall"),
        QStringLiteral("ShadowMedium"), QStringLiteral("ShadowLarge"),
        QStringLiteral("ShadowVeryLarge")};
    const QString name = c.value(QStringLiteral("ShadowSize")).toString().trimmed();
    bool numeric = false;
    const int index = name.toInt(&numeric);
    if (numeric) return (index >= 0 && index < 5) ? index : 3;
    const int found = names.indexOf(name);
    return found >= 0 ? found : 3;
}

// KConfig stores a colour as "r,g,b[,a]", which QSettings reads as a list.
QColor readColour(const QSettings &c, const QString &key) {
    const QVariant v = c.value(key);
    const QStringList parts = v.typeId() == QMetaType::QStringList
                                  ? v.toStringList()
                                  : v.toString().split(QLatin1Char(','));
    if (parts.size() < 3) return Qt::black;
    QColor colour(parts[0].trimmed().toInt(), parts[1].trimmed().toInt(),
                  parts[2].trimmed().toInt());
    if (parts.size() > 3) colour.setAlpha(parts[3].trimmed().toInt());
    return colour;
}

QJsonObject readAppearance() {
    QFile f(monarchyConfigPath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument doc =
        QJsonDocument::fromJson(stripJsonComments(QString::fromUtf8(f.readAll())).toUtf8());
    return doc.object().value(QStringLiteral("appearance")).toObject();
}

// The scheme's window colour, which every shell surface is filled with before
// anything tints it.
QColor schemeWindowColour() {
    QSettings c(kdeglobalsPath(), QSettings::IniFormat);
    const QVariant v = c.value(QStringLiteral("Colors:Window/BackgroundNormal"));
    const QStringList parts = v.typeId() == QMetaType::QStringList
                                  ? v.toStringList()
                                  : v.toString().split(QLatin1Char(','));
    if (parts.size() < 3) return QColor(18, 18, 18);
    return QColor(parts[0].trimmed().toInt(), parts[1].trimmed().toInt(),
                  parts[2].trimmed().toInt());
}

// Monarchy's MonarchyConfig::effectiveShellComponentStyle(), for the menus:
// their own tier if something is stored for it, "all" otherwise, and the
// shell menu's built-in look when neither is -- from the light set in the
// light scheme, as Monarchy's own menus do. Tints are not followed.
Settings::MenuSurface readMenuSurface(const QJsonObject &appearance) {
    Settings::MenuSurface m;
    m.fill = schemeWindowColour();
    m.fill.setAlpha(153);
    const QJsonObject tiers =
        appearance.value(isLight() ? QStringLiteral("shellComponentsLight")
                                   : QStringLiteral("shellComponents")).toObject();
    QJsonObject tier;
    for (const char *name : {"all-context-menus", "all"}) {
        const QJsonObject t = tiers.value(QLatin1String(name)).toObject();
        if (t.value(QStringLiteral("configured")).toBool()) {
            tier = t;
            break;
        }
    }
    if (tier.isEmpty()) return m;
    const auto percent = [&tier](const char *key, double fallback) {
        return std::clamp(tier.value(QLatin1String(key)).toDouble(fallback) / 100.0, 0.0, 1.0);
    };
    m.fill.setAlpha(std::clamp(qRound(percent("backgroundOpacity", 60) * 255.0), 0, 255));
    m.fadeTop = percent("backgroundFadeTop", 0);
    m.fadeBottom = percent("backgroundFadeBottom", 0);
    m.borderWidth = std::clamp(tier.value(QStringLiteral("borderWidth")).toDouble(2.5), 0.0, 10.0);
    m.borderTop = percent("borderFadeTop", 14);
    m.borderBottom = percent("borderFadeBottom", 1);
    const QString colour = tier.value(QStringLiteral("borderColour")).toString();
    if (colour.startsWith(QLatin1Char('#')) && QColor(colour).isValid())
        m.borderColour = QColor(colour);
    return m;
}

}  // namespace

QColor accentColour() {
    QSettings c(kdeglobalsPath(), QSettings::IniFormat);
    // "AccentColor", not "General/AccentColor": QSettings folds an INI file's
    // [General] group into the top level.
    for (const QString &key : {QStringLiteral("AccentColor"),
                               QStringLiteral("Colors:Selection/BackgroundNormal")}) {
        const QVariant v = c.value(key);
        const QStringList parts = v.typeId() == QMetaType::QStringList
                                      ? v.toStringList()
                                      : v.toString().split(QLatin1Char(','));
        if (parts.size() < 3) continue;
        // Opaque on purpose: kdeglobals often stores these with alpha 0.
        const QColor colour(parts[0].trimmed().toInt(), parts[1].trimmed().toInt(),
                            parts[2].trimmed().toInt());
        if (colour.isValid()) return colour;
    }
    return QColor();
}

bool isLight() {
    QSettings c(kdeglobalsPath(), QSettings::IniFormat);
    // "ColorScheme", not "General/ColorScheme": QSettings folds [General]
    // into the top level.
    return c.value(QStringLiteral("ColorScheme")).toString() == QLatin1String("monarchy-light");
}

QIcon appIcon(const QString &iconName) {
    if (iconName.isEmpty()) return {};
    if (QFileInfo(iconName).isAbsolute())
        return QFile::exists(iconName) ? QIcon(iconName) : QIcon();
    // The desktop's theme, whatever Qt's platform theme did or did not tell
    // us: without it only hicolor is searched, and most application icons on
    // Monarchy are its own theme's.
    static const bool themed = [] {
        QSettings c(kdeglobalsPath(), QSettings::IniFormat);
        const QString theme = c.value(QStringLiteral("Icons/Theme")).toString();
        if (!theme.isEmpty() && QIcon::themeName() != theme) QIcon::setThemeName(theme);
        // A session with no platform theme (the offscreen platform the
        // screenshots use, a bare compositor) searches only Qt's resources.
        QStringList paths = QIcon::themeSearchPaths();
        if (!paths.contains(QStringLiteral("/usr/share/icons"))) {
            for (const QString &dir : QStandardPaths::standardLocations(
                     QStandardPaths::GenericDataLocation))
                paths << dir + QStringLiteral("/icons");
            QIcon::setThemeSearchPaths(paths);
        }
        return true;
    }();
    (void)themed;
    QIcon icon = QIcon::fromTheme(iconName);
    if (icon.isNull() && iconName != iconName.toLower())
        icon = QIcon::fromTheme(iconName.toLower());
    return icon;
}

bool isActive() {
    if (g_forced) return *g_forced;
    const QByteArray env = qgetenv("WAVELINE_LOOK").trimmed().toLower();
    if (env == "monarchy") g_forced = true;
    else if (env == "universal") g_forced = false;
    else g_forced = osReleaseSaysMonarchy();
    return *g_forced;
}

void setActive(bool active) { g_forced = active; }

Settings &Settings::instance() {
    static Settings s;
    return s;
}

int Settings::smallSpacing() { return std::max(2, gridUnit() / 4); }

int Settings::windowAlpha(bool dark) const {
    const int base = qRound(opacityPercent_ * 255.0 / 100.0);
    // 11: how much more opaque Monarchy draws its light scheme than its dark
    // one, 228 against 217 at the setting's Normal.
    if (dark || base <= 0) return std::clamp(base, 0, 255);
    return std::clamp(base + 11, 0, 255);
}

Settings::Settings() {
    read();
    refresh_.setSingleShot(true);
    refresh_.setInterval(120);
    connect(&refresh_, &QTimer::timeout, this, [this] {
        const QString before = signature_;
        read();
        if (signature_ != before) emit changed();
    });

    watcher_ = new QFileSystemWatcher(this);
    watch();
    // Both files are saved by replacing them, which takes a file watch with
    // it, so the directories are watched too and the files re-added.
    connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this] {
        watch();
        refresh_.start();
    });
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this] {
        watch();
        refresh_.start();
    });
}

void Settings::watch() {
    for (const QString &path : {titlebarPath(), monarchyConfigPath(), kdeglobalsPath(),
                                kwinrcPath()}) {
        const QString dir = QFileInfo(path).absolutePath();
        if (!watcher_->directories().contains(dir) && QDir(dir).exists())
            watcher_->addPath(dir);
        if (!watcher_->files().contains(path) && QFile::exists(path))
            watcher_->addPath(path);
    }
}

void Settings::read() {
    QSettings c(titlebarPath(), QSettings::IniFormat);

    c.beginGroup(QStringLiteral("Windeco"));
    buttonSize_ = qRound(buttonSizeFactor(c.value(QStringLiteral("ButtonSize")).toString())
                         * gridUnit());
    buttonSpacing_ = qRound(0.5 * smallSpacing() * c.value(QStringLiteral("ButtonSpacing"), 2).toInt());
    buttonPadding_ = qRound(0.5 * smallSpacing() * c.value(QStringLiteral("ButtonPadding"), 4).toInt());
    animationsEnabled_ = readBool(c, QStringLiteral("AnimationsEnabled"), true);
    animationsDuration_ = c.value(QStringLiteral("AnimationsDuration"), 150).toInt();
    buttonStyle_ = buttonStyleFor(c.value(QStringLiteral("ButtonStyle")).toString());
    cornerRadius_ = c.value(QStringLiteral("CornerRadius"), 0).toString().toInt();
    c.endGroup();

    c.beginGroup(QStringLiteral("Common"));
    shadowSize_ = readShadowSize(c);
    bool ok = false;
    const int strength = c.value(QStringLiteral("ShadowStrength")).toString().toInt(&ok);
    shadowStrength_ = ok ? std::clamp(strength, 25, 255) : 255;
    shadowColor_ = readColour(c, QStringLiteral("ShadowColor"));
    c.endGroup();

    const QJsonObject appearance = readAppearance();
    opacityPercent_ = std::clamp(appearance.value(QStringLiteral("windowOpacity")).toInt(85), 0, 100);
    menuSurface_ = readMenuSurface(appearance);

    QSettings kwin(kwinrcPath(), QSettings::IniFormat);
    kwinCornerRadius_ = std::max(0, kwin.value(QStringLiteral("Round-Corners/Size"), 14).toInt());

    signature_ = QStringLiteral("%1/%2/%3/%4/%5/%6/%7/%8/%9/%10/%11")
                     .arg(buttonSize_).arg(buttonSpacing_).arg(buttonPadding_)
                     .arg(animationsEnabled_).arg(animationsDuration_)
                     .arg(int(buttonStyle_)).arg(cornerRadius_).arg(shadowSize_)
                     .arg(shadowStrength_).arg(shadowColor_.name(QColor::HexArgb))
                     .arg(opacityPercent_)
                 + accentColour().name() + (isLight() ? QStringLiteral("/light") : QString())
                 + QStringLiteral("/%1/%2/%3/%4/%5/%6/%7/%8")
                       .arg(menuSurface_.fill.name(QColor::HexArgb)).arg(menuSurface_.fadeTop)
                       .arg(menuSurface_.fadeBottom).arg(menuSurface_.borderWidth)
                       .arg(menuSurface_.borderTop).arg(menuSurface_.borderBottom)
                       .arg(menuSurface_.borderColour.name(QColor::HexArgb))
                       .arg(kwinCornerRadius_);
}

}  // namespace Monarchy

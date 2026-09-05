// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSvgRenderer>

namespace Theme {
namespace {

QString hex(const QColor &c) { return c.name(QColor::HexRgb); }

// Pushed in from the window on every refresh; read by everything that draws a
// channel. A plain hash rather than anything cleverer: it is written once per
// refresh and read a few dozen times, all on the GUI thread.
QHash<QString, CardLook> gLooks;
int gLooksRevision = 0;

}  // namespace

void setCardLooks(const QHash<QString, CardLook> &looks) {
    gLooks = looks;
    ++gLooksRevision;
}

int cardLooksRevision() { return gLooksRevision; }

void setCardLook(const QString &cardKey, const CardLook &look) {
    if (!look.color.isValid() && look.icon.isEmpty())
        gLooks.remove(cardKey);
    else
        gLooks.insert(cardKey, look);
    ++gLooksRevision;
}

QString channelCardKey(const QString &channelId) {
    return QStringLiteral("channel:") + channelId;
}

QString masterCardKey(const QString &masterId) {
    return QStringLiteral("master:") + masterId;
}

QColor cardColor(const QString &cardKey, const QColor &fallback) {
    const auto it = gLooks.constFind(cardKey);
    if (it != gLooks.constEnd() && it->color.isValid()) return it->color;
    return fallback;
}

QString cardIcon(const QString &cardKey, const QString &fallback) {
    const auto it = gLooks.constFind(cardKey);
    if (it != gLooks.constEnd() && !it->icon.isEmpty()) return it->icon;
    return fallback;
}

QColor glyphOn(const QColor &fill) {
    // Perceived brightness, not plain lightness: a saturated yellow and a
    // saturated blue at the same HSL lightness are nowhere near as bright as
    // each other, and it is the yellow that needs the dark glyph.
    const double y = 0.2126 * fill.redF() + 0.7152 * fill.greenF() +
                     0.0722 * fill.blueF();
    return y > 0.55 ? QColor(0x10, 0x10, 0x14) : QColor(Qt::white);
}

QColor channelColorDefault(const QString &channelId) {
    static const QHash<QString, QColor> kColors = {
        {QStringLiteral("mic"),     QColor(0x5b, 0x6c, 0xf0)},  // indigo
        {QStringLiteral("system"),  QColor(0x2f, 0x86, 0xe8)},  // blue
        {QStringLiteral("voice"),   QColor(0xc9, 0xd4, 0x3a)},  // lime
        {QStringLiteral("music"),   QColor(0xe0, 0x47, 0x9e)},  // pink
        {QStringLiteral("video"),   QColor(0x22, 0xd3, 0xee)},  // cyan (brighter than input-device cyan)
        {QStringLiteral("browser"), QColor(0x9d, 0x4a, 0xe0)},  // purple
        {QStringLiteral("game"),    QColor(0xe8, 0x45, 0x5a)},  // red
        {QStringLiteral("sfx"),     QColor(0xc2, 0x41, 0x0c)},  // deep burnt orange (≠ input #f97316)
    };
    return kColors.value(channelId, QColor(0x6a, 0x6a, 0x76));
}

QColor channelColor(const QString &channelId) {
    return cardColor(channelCardKey(channelId), channelColorDefault(channelId));
}

QColor masterBusColor(int slotIndex) {
    static const QColor kPalette[] = {
        QColor(0x5b, 0x6c, 0xf0),  // 1 indigo
        QColor(0x10, 0xb9, 0x81),  // 2 emerald
        QColor(0xf9, 0x73, 0x16),  // 3 orange
        QColor(0x06, 0xb6, 0xd4),  // 4 cyan
        QColor(0xa9, 0x6d, 0xf5),  // 5 violet
        QColor(0xec, 0x48, 0x99),  // 6 rose
        QColor(0xf5, 0x9e, 0x0b),  // 7 amber
        QColor(0x84, 0xcc, 0x16),  // 8 lime
        QColor(0x3b, 0x82, 0xf6),  // 9 blue
    };
    const int n = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
    const int i = ((slotIndex % n) + n) % n;
    return kPalette[i];
}

QString channelIconNameDefault(const QString &channelId) {
    static const QHash<QString, QString> kIcons = {
        {QStringLiteral("mic"),     QStringLiteral("microphone")},
        {QStringLiteral("system"),  QStringLiteral("system")},
        {QStringLiteral("voice"),   QStringLiteral("voice")},
        {QStringLiteral("music"),   QStringLiteral("music")},
        {QStringLiteral("video"),   QStringLiteral("video")},
        {QStringLiteral("browser"), QStringLiteral("browser")},
        {QStringLiteral("game"),    QStringLiteral("game")},
        {QStringLiteral("sfx"),     QStringLiteral("sfx")},
    };
    return kIcons.value(channelId, QStringLiteral("speaker-high"));
}

QString channelIconName(const QString &channelId) {
    return cardIcon(channelCardKey(channelId), channelIconNameDefault(channelId));
}

QPixmap iconPixmap(const QString &name, const QColor &color, int px) {
    // Keyed on the rendered size in device pixels, so a cached icon is never
    // reused at the wrong scale on a mixed-DPI setup.
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(name, color.name(QColor::HexArgb))
                            .arg(px)
                            .arg(dpr);

    static QHash<QString, QPixmap> cache;
    const auto it = cache.constFind(key);
    if (it != cache.constEnd()) return *it;

    // Bundled first, then the user's own directory, so a custom icon can never
    // shadow one the mixer's own widgets ask for by name.
    QFile f(QStringLiteral(":/waveline/%1.svg").arg(name));
    if (!f.open(QIODevice::ReadOnly)) {
        f.setFileName(userIconDir() + QLatin1Char('/') + name +
                      QStringLiteral(".svg"));
        if (!f.open(QIODevice::ReadOnly)) return {};
    }
    QString svg = QString::fromUtf8(f.readAll());
    // Tabler icons stroke with currentColor and fill nothing. Substituting the
    // literal is enough; there is no CSS engine involved.
    svg.replace(QLatin1String("currentColor"), hex(color));

    QSvgRenderer renderer(svg.toUtf8());
    QPixmap pm(QSize(px, px) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    renderer.render(&p, QRectF(0, 0, px, px));
    p.end();

    cache.insert(key, pm);
    return pm;
}

QString tintedIconFile(const QString &name, const QColor &color, int px) {
    // Qt's stylesheets have no notion of currentColor, so an icon drawn into a
    // rule -- the combo box chevron -- comes out in whatever colour the file
    // literally names, which for a Tabler outline is nothing at all. Render it
    // the way every other icon here is rendered, write it beside the cache, and
    // point the rule at that. Written at twice the drawn size so it stays sharp
    // where the rest of the window is scaled.
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString path = QStringLiteral("%1/%2-%3-%4.png")
                             .arg(dir, name, color.name(QColor::HexRgb))
                             .arg(px);

    QPixmap pm = iconPixmap(name, color, px * 2);
    if (pm.isNull()) return path;
    // Rewritten every run rather than reused blindly: the file is small, and a
    // stale one from an older palette would outlive the colour that made it.
    pm.save(path, "PNG");
    return path;
}

QString userIconDir() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
        QStringLiteral("/waveline/icons");
    QDir().mkpath(dir);
    return dir;
}

QStringList builtinIconNames() {
    // From our own resource prefix rather than a list kept by hand, so an
    // icon added to CMakeLists turns up in the picker by itself -- and so that
    // nothing else's resources turn up with it.
    QStringList out;
    for (const QFileInfo &fi : QDir(QStringLiteral(":/waveline"))
                                   .entryInfoList({QStringLiteral("*.svg")},
                                                  QDir::Files, QDir::Name)) {
        out << fi.completeBaseName();
    }
    return out;
}

QStringList userIconNames() {
    QStringList out;
    for (const QFileInfo &fi : QDir(userIconDir())
                                   .entryInfoList({QStringLiteral("*.svg")},
                                                  QDir::Files, QDir::Name)) {
        out << fi.completeBaseName();
    }
    return out;
}

QString importUserIcon(const QString &sourcePath, QString *error) {
    const auto fail = [error](const QString &why) {
        if (error) *error = why;
        return QString();
    };
    QFileInfo src(sourcePath);
    if (!src.exists() || !src.isFile())
        return fail(QObject::tr("%1 does not exist.").arg(sourcePath));

    // Reject anything that is not actually an SVG before it is copied in: a
    // file that fails to render would otherwise become a card icon that draws
    // nothing, with no way to tell why from the picker.
    QFile in(sourcePath);
    if (!in.open(QIODevice::ReadOnly))
        return fail(QObject::tr("Cannot read %1.").arg(sourcePath));
    const QByteArray data = in.readAll();
    in.close();
    if (!QSvgRenderer(data).isValid())
        return fail(QObject::tr("%1 is not an SVG the mixer can draw.")
                        .arg(src.fileName()));

    // A name of its own, so importing a second "icon.svg" cannot silently
    // replace the first -- and so that every card pointing at the old one
    // keeps pointing at the icon it was given.
    QString stem = src.completeBaseName();
    stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")),
                 QStringLiteral("-"));
    if (stem.isEmpty()) stem = QStringLiteral("icon");
    const QStringList taken = builtinIconNames() + userIconNames();
    QString name = stem;
    for (int n = 2; taken.contains(name); ++n)
        name = stem + QStringLiteral("-") + QString::number(n);

    QFile out(userIconDir() + QLatin1Char('/') + name + QStringLiteral(".svg"));
    if (!out.open(QIODevice::WriteOnly))
        return fail(QObject::tr("Cannot write into %1.").arg(userIconDir()));
    out.write(data);
    out.close();
    return name;
}

namespace {
QPixmap tilePixmap(const QColor &fill, int px) {
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(QSize(px, px) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, px, px), px * 0.3, px * 0.3);
    p.fillPath(path, fill);
    return pm;
}
}  // namespace

QIcon channelBadge(const QString &channelId, int px) {
    const QColor fill = channelColor(channelId);
    QPixmap pm = tilePixmap(fill, px);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const int ip = int(px * 0.62);
    p.drawPixmap(QPointF((px - ip) / 2.0, (px - ip) / 2.0),
                 iconPixmap(channelIconName(channelId), glyphOn(fill), ip));
    return QIcon(pm);
}

QIcon noneBadge(int px) {
    // Grey is the fallback channel colour, so "nothing" sits in the same family
    // as the real channels without pretending to be one.
    QPixmap pm = tilePixmap(QColor(0x6a, 0x6a, 0x76), px);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal m = px * 0.32;
    p.setPen(QPen(Qt::white, px * 0.11, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(m, m), QPointF(px - m, px - m));
    p.drawLine(QPointF(px - m, m), QPointF(m, px - m));
    return QIcon(pm);
}

QIcon icon(const QString &name, const QColor &color, int px) {
    const QPixmap pm = iconPixmap(name, color, px);
    return pm.isNull() ? QIcon() : QIcon(pm);
}

QString styleSheet() {
    return QStringLiteral(R"(
QWidget {
    background: transparent;
    color: %TEXT%;
    font-size: 13px;
}
QMainWindow, QDialog { background: %BG%; }
QToolTip {
    background: %CARD%;
    color: %TEXT%;
    border: 1px solid %LINE%;
    border-radius: 6px;
    padding: 6px 8px;
}

/* ---------------------------------------------------------------- inputs */
QComboBox {
    background: %CARD%;
    border: 1px solid %LINE%;
    border-radius: 7px;
    padding: 5px 10px;
    min-height: 20px;
    color: %TEXT%;
    /* Plain list, not a menu-style popup. Fusion's menu-style dropdown puts a
       five pixel margin around the list and paints nothing in it, and the
       popup is its own opaque top-level window -- so those pixels come out as
       a black band across the top and bottom of every dropdown. Turning the
       menu style off makes the popup exactly the list, with nothing left
       unpainted. */
    combobox-popup: 0;
}
QComboBox:hover { background: %CARDHOVER%; }
QComboBox:disabled { color: %TEXTFAINT%; }

/* A destination that is fixed rather than chosen -- the Stream mix's virtual
   device. Same box as the picker above it so the two Outputs rows line up,
   but no hover and no chevron: there is nothing to open. Geometry is kept in
   step with QComboBox by hand; the two rules sit together for that reason. */
QLabel#fixedOutput {
    background: %CARD%;
    border: 1px solid %LINE%;
    border-radius: 7px;
    padding: 5px 10px;
    min-height: 20px;
    color: %TEXT%;
}
QComboBox::drop-down { border: none; width: 26px; }
QComboBox::down-arrow {
    image: url("%CHEVRON%");
    width: 18px; height: 18px;
}
QComboBox::down-arrow:disabled { image: url("%CHEVRONDIM%"); }
QComboBox QAbstractItemView {
    background: %CARD%;
    border: 1px solid %LINE%;
    border-radius: 7px;
    padding: 4px;
    outline: none;
    selection-background-color: %ACCENTDIM%;
    selection-color: %TEXT%;
}

QLineEdit {
    background: %WELL%;
    border: 1px solid %LINE%;
    border-radius: 7px;
    padding: 6px 9px;
    selection-background-color: %ACCENTDIM%;
}
QLineEdit:focus { border-color: %ACCENT%; }

QPushButton {
    background: %CARD%;
    border: 1px solid %LINE%;
    border-radius: 7px;
    padding: 6px 14px;
    color: %TEXT%;
}
QPushButton:hover  { background: %CARDHOVER%; }
QPushButton:pressed{ background: %WELL%; }
QPushButton:disabled { color: %TEXTFAINT%; border-color: %WELL%; }

/* A segmented choice -- the tuner's Auto/Manual pair. One of the two is always
   down, so an unstyled :checked state would read as "neither is selected".
   Scoped by object name rather than styling every checkable QPushButton,
   because a lone toggle button wants a quieter treatment than this. */
QPushButton#segment { padding: 6px 18px; }
QPushButton#segment:checked {
    background: %ACCENTDIM%;
    border-color: %ACCENT%;
    color: %TEXT%;
}
QPushButton#segment:checked:hover { background: %ACCENT%; }

/* A call to action -- the Soundboard's "+ Add Sound", and anywhere else one
   accent-filled button is the obvious next thing to click. Scoped by object
   name for the same reason #segment is: most buttons want the quiet
   Card-coloured treatment, and a form filled with primary buttons has no
   primary button at all. */
QPushButton#primary {
    background: %ACCENT%;
    border-color: %ACCENT%;
    color: %BG%;
    font-weight: 600;
}
QPushButton#primary:hover { background: %ACCENTHOVER%; border-color: %ACCENTHOVER%; }
QPushButton#primary:pressed { background: %ACCENTDIM%; border-color: %ACCENTDIM%; }

/* --------------------------------------------------------------- scrolling */
QScrollArea { border: none; background: transparent; }
QScrollBar:vertical {
    background: transparent; width: 10px; margin: 2px;
}
QScrollBar:horizontal {
    background: transparent; height: 10px; margin: 2px;
}
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
    background: %LINE%; border-radius: 4px; min-height: 28px; min-width: 28px;
}
QScrollBar::handle:hover { background: %TEXTFAINT%; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

/* ------------------------------------------------------------------ table */
QTableWidget {
    background: %WELL%;
    border: 1px solid %LINE%;
    border-radius: 8px;
    gridline-color: transparent;
    outline: none;
}
QTableWidget::item { padding: 4px 8px; border: none; }
QHeaderView::section {
    background: transparent;
    color: %TEXTFAINT%;
    border: none;
    border-bottom: 1px solid %LINE%;
    padding: 6px 8px;
    font-size: 11px;
    text-transform: uppercase;
}

/* --------------------------------------------------------- horizontal slider
   Vertical faders are painted by Fader, not styled here. */
QSlider::groove:horizontal {
    background: %WELL%;
    border: none;
    height: 5px;
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: %ACCENT%;
    border: none;
    height: 5px;
    border-radius: 2px;
}
/* The unfilled part. Without this Fusion paints its own light grey bar over
   the groove, which is where the washed-out background on these sliders came
   from -- styling the groove alone is not enough. */
QSlider::add-page:horizontal {
    background: %WELL%;
    border: none;
    height: 5px;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    background: %FADER%;
    width: 13px;
    margin: -5px 0;
    border-radius: 6px;
}
QSlider::handle:horizontal:hover { background: white; }
/* Sub-control first, THEN the state. Written the other way round
   (`QSlider:disabled::handle`) Qt does not scope the state to the sub-control,
   so the disabled handle colour was painted on every slider whether or not it
   was disabled -- a 15px grey block behind the 5px groove, which is exactly
   the washed-out background these sliders had. */
QSlider::sub-page:horizontal:disabled { background: %LINE%; }
QSlider::add-page:horizontal:disabled { background: %WELL%; }
QSlider::handle:horizontal:disabled { background: %TEXTFAINT%; }
)")
        .replace(QLatin1String("%BG%"), hex(Bg))
        .replace(QLatin1String("%WELL%"), hex(Well))
        .replace(QLatin1String("%CARDHOVER%"), hex(CardHover))
        .replace(QLatin1String("%CARD%"), hex(Card))
        .replace(QLatin1String("%LINE%"), hex(Line))
        .replace(QLatin1String("%TEXTFAINT%"), hex(TextFaint))
        .replace(QLatin1String("%TEXTDIM%"), hex(TextDim))
        .replace(QLatin1String("%TEXT%"), hex(Text))
        .replace(QLatin1String("%ACCENTDIM%"), hex(AccentDim))
        .replace(QLatin1String("%ACCENTHOVER%"), hex(Accent.lighter(112)))
        .replace(QLatin1String("%ACCENT%"), hex(Accent))
        .replace(QLatin1String("%CHEVRONDIM%"), tintedIconFile(
                                                    QStringLiteral("chevron"),
                                                    TextFaint, 18))
        .replace(QLatin1String("%CHEVRON%"), tintedIconFile(
                                                 QStringLiteral("chevron"),
                                                 Text, 18))
        .replace(QLatin1String("%FADER%"), hex(Fader));
}

void apply() {
    QPalette pal;
    pal.setColor(QPalette::Window, Bg);
    pal.setColor(QPalette::WindowText, Text);
    pal.setColor(QPalette::Base, Well);
    pal.setColor(QPalette::AlternateBase, Card);
    pal.setColor(QPalette::Text, Text);
    pal.setColor(QPalette::Button, Card);
    pal.setColor(QPalette::ButtonText, Text);
    pal.setColor(QPalette::Highlight, AccentDim);
    pal.setColor(QPalette::HighlightedText, Text);
    pal.setColor(QPalette::ToolTipBase, Card);
    pal.setColor(QPalette::ToolTipText, Text);
    pal.setColor(QPalette::PlaceholderText, TextFaint);
    pal.setColor(QPalette::Disabled, QPalette::Text, TextFaint);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, TextFaint);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, TextFaint);
    qApp->setPalette(pal);

    // Fusion, not the platform style: the desktop style is free to ignore
    // half of the stylesheet above, and on a GTK desktop the result is a
    // half-light, half-dark window.
    qApp->setStyle(QStringLiteral("Fusion"));
    qApp->setStyleSheet(styleSheet());
}

}  // namespace Theme

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// One place for every colour, radius and icon in the mixer.
//
// The widgets deliberately do not carry stylesheets of their own. A Qt
// application that sets QSS in twenty constructors becomes impossible to
// restyle, and the parts that are painted by hand (meters, faders, the toggle)
// then drift out of step with the parts that are not. Everything reads its
// colours from here instead.

#pragma once

#include <QColor>
#include <QHash>
#include <QIcon>
#include <QString>
#include <QStringList>

namespace Theme {

// ------------------------------------------------------------------ palette
// Near-black rather than pure black: an OLED-black panel next to a dark grey
// card reads as a rendering fault, not as depth.
inline const QColor Bg            {0x12, 0x12, 0x14};  // window
inline const QColor Well          {0x0c, 0x0c, 0x0e};  // recessed areas
inline const QColor Card          {0x1e, 0x1e, 0x22};  // channel cards, panels
inline const QColor CardHover     {0x26, 0x26, 0x2b};
inline const QColor Line          {0x2e, 0x2e, 0x34};  // hairline separators
inline const QColor Text          {0xe8, 0xe8, 0xec};
inline const QColor TextDim       {0x9a, 0x9a, 0xa4};
inline const QColor TextFaint     {0x66, 0x66, 0x70};  // section captions
inline const QColor Accent        {0x3d, 0xd6, 0x8c};  // "on", signal present
inline const QColor AccentDim     {0x24, 0x7f, 0x54};
inline const QColor Warn          {0xff, 0xb3, 0x2e};
inline const QColor Danger        {0xe8, 0x4b, 0x4b};
// "on, and routed somewhere unusual" -- currently the channel whose monitor
// mix is fed from its FX chain instead of the dry sink.
inline const QColor Violet        {0xa9, 0x6d, 0xf5};
inline const QColor Fader         {0xb4, 0xb4, 0xbe};  // fader handles

// Per-channel identity colour, used for the icon tile and the fader accent.
// Wave Link gives every input its own hue and it is the single thing that
// makes a wall of identical strips scannable, so it is worth copying.
QColor channelColor(const QString &channelId);

// Input Device strip colour by visual slot (0 = primary). Nine hues, then
// the sequence repeats.
QColor masterBusColor(int slotIndex);

// ------------------------------------------------------- card appearance
// What the user chose, if anything, for each card. Cards are keyed
// "channel:<id>" and "master:<id>"; the daemon owns the table and the window
// pushes it here on every refresh, so that every place a channel's colour or
// icon is drawn -- cards, the Apps tab, the sharing pickers, the combo
// badges -- picks the choice up without knowing it exists.
struct CardLook {
    QColor color;    // invalid = use the palette
    QString icon;    // empty = use the built-in icon
};
void setCardLooks(const QHash<QString, CardLook> &looks);
// Bumped whenever the table changes. Views that only rebuild when their
// contents change -- the Apps and Sound Sharing tables, which must not be
// recreated under the user's cursor four times a second -- fold this into
// their signature, so a recoloured card is a reason to redraw and a poll that
// changed nothing still is not.
int cardLooksRevision();
// One card, applied before the daemon has confirmed it: the card the user just
// edited must be wearing its new colour when the dialog closes, not one poll
// later. An empty colour and icon drops the entry.
void setCardLook(const QString &cardKey, const CardLook &look);
QString channelCardKey(const QString &channelId);
QString masterCardKey(const QString &masterId);
// fallback is what the theme would have picked; the override wins if there is
// one. Never returns an invalid colour.
QColor cardColor(const QString &cardKey, const QColor &fallback);
QString cardIcon(const QString &cardKey, const QString &fallback);
// What the theme would have used, ignoring any override -- what the identity
// dialog offers as "use default".
QColor channelColorDefault(const QString &channelId);
QString channelIconNameDefault(const QString &channelId);

// Black or white, whichever stays readable on top of `fill`. A user who picks
// a pale yellow for a card must not end up with a white glyph on it.
QColor glyphOn(const QColor &fill);

// Icons the identity dialog offers: the ones built into the mixer, and the
// SVGs the user has added under ~/.config/waveline/icons.
QStringList builtinIconNames();
QStringList userIconNames();
// Where userIconNames() reads from, created on demand. Custom SVGs are copied
// in here so a card's icon survives the file being moved or the mixer being
// started from somewhere else.
QString userIconDir();
// Copies an SVG in under a name derived from its own, and returns that name
// ("piano"), or empty on failure.
QString importUserIcon(const QString &sourcePath, QString *error = nullptr);

// ------------------------------------------------------------------- icons
// The bundled icons are Tabler outline SVGs, which paint with `currentColor`.
// Qt has no notion of that, so the colour is substituted into the markup
// before rendering. Results are cached: the channel cards ask for the same
// handful of icons on every repaint.
//
// `name` is the file stem, e.g. "microphone" for :/waveline/microphone.svg,
// falling back to ~/.config/waveline/icons for the user's own SVGs.
QIcon icon(const QString &name, const QColor &color, int px = 20);
// The same rendering, written out as a file for the places that can only take
// a path: Qt stylesheet rules, which cannot tint an SVG themselves.
QString tintedIconFile(const QString &name, const QColor &color, int px);
QPixmap iconPixmap(const QString &name, const QColor &color, int px = 20);

// The icon that matches a channel id, e.g. "music" -> music.svg. Falls back to
// a speaker for ids we do not recognise, so a channel added later still draws.
QString channelIconName(const QString &channelId);

// The channel's rounded, colour-filled icon tile, as the channel cards draw it.
// Keep it generous: these are thin stroked glyphs and they turn to mush below
// about 20 px of tile.
QIcon channelBadge(const QString &channelId, int px = 24);
// The same tile for "no channel": grey ground, white cross.
QIcon noneBadge(int px = 24);

// -------------------------------------------------------------- stylesheet
// Applied once to QApplication. Covers only the stock widgets; anything drawn
// by hand takes its colours from the constants above.
QString styleSheet();

// Applies styleSheet() plus a matching QPalette. The palette matters because
// several widgets (tooltips, combo popups, text selection) paint from it
// rather than from QSS.
void apply();

}  // namespace Theme

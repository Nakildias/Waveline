// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Monarchy's window chrome, for the mixer's window when it runs there.
//
// Ported from Monarchy's own src/common/windowchrome and src/common/titlebar
// (same author, same licence) rather than linked: those are static libraries
// each Monarchy application compiles in from that tree, and the mixer is built
// on machines that have never seen it. Keep the numbers in step with the
// originals -- the point of all this is that a mixer window beside a Files
// window looks like one desktop, and a copy that drifts defeats it.
//
//   - WindowButtons: close / minimise / maximise, drawn in the header because
//     a frameless window has no titlebar to put them in. The glass bead and
//     glyphs are the desktop decoration's own, and follow its settings.
//   - ChromeGroup: the rounded pill the toolbar's buttons sit in, with the rim
//     of light along its edge.
//   - The grab strip round a frameless window, the drag that makes a header a
//     titlebar, and the blur and shadow a decoration would otherwise provide.

#pragma once

#include <QColor>
#include <QWidget>

#include "../theme.h"

#include <initializer_list>

class QAbstractButton;
class QBoxLayout;
class QEvent;
class QHBoxLayout;
class QLabel;
class QPainter;
class QRectF;
class QToolButton;
class QWindow;

namespace Monarchy {

// ------------------------------------------------------------ window buttons
class WindowButtons : public QWidget {
    Q_OBJECT

public:
    explicit WindowButtons(QWidget *parent = nullptr);

    // A window that cannot be resized has nothing to maximise to, so it does
    // not show the button -- the other two close up rather than leave a gap.
    void setMaximiseVisible(bool visible);

private:
    void applySettings();
    QWidget *maximise_ = nullptr;
};

// -------------------------------------------------------------- chrome group
// The rim light, stroked along a rounded shape's whole outline and faded by a
// top-to-bottom gradient: bright where the shape faces up, gone by its widest
// point, a fainter bounce underneath. `chrome` is the colour behind the shape,
// which decides whether that bounce is a white or a shadow.
void paintRimHighlight(QPainter *painter, const QRectF &bounds, qreal radius,
                       const QColor &chrome);

class ChromeGroup : public QWidget {
    Q_OBJECT

public:
    // `radius` has to agree with the border-radius the style sheet gives the
    // group, or the light will not sit on the shape.
    explicit ChromeGroup(qreal radius, QWidget *parent = nullptr);

    // Handed in rather than read from the palette: a style sheet that gives a
    // widget a background rewrites that widget's palette with it, so the group
    // asked what is behind it would answer with its own fill.
    void setChromeColour(const QColor &chrome);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    qreal radius_;
    Theme::Live chrome_;
};

// A toolbar button: a transparent circle inside a ChromeGroup that lights up
// under the pointer, showing `glyph` (one of the bundled icons) in the dimmed
// text colour. The pill's look is in the application style sheet (see
// Theme::styleSheet), so it is the same in both of the mixer's looks.
QToolButton *toolButton(const QString &glyph, const QString &tip, QWidget *parent);

// The pill round one or more toolbar buttons, which it reparents. `chrome` is
// the colour behind the pill -- see ChromeGroup::setChromeColour.
ChromeGroup *pillGroup(QWidget *parent, std::initializer_list<QAbstractButton *> buttons,
                       const QColor &chrome);

// ------------------------------------------------------------------ frameless
// How wide the grab strip round a frameless window is.
inline constexpr int kResizeMargin = 4;
// Monarchy's toolbar geometry: 36 px pills in a 46 px row.
inline constexpr int kGroupHeight = 36;
inline constexpr int kHeaderHeight = kGroupHeight + 10;

// Makes `window` frameless and translucent, with the desktop's blur behind it
// and the decoration's shadow round it. Call before the window is shown.
void makeFrameless(QWidget *window);

// Sets `central` up as the grab strip, once its children exist: it tracks the
// pointer, and its direct children get a cursor of their own so they do not
// inherit the resize shape.
void prepareResizeStrip(QWidget *central);

// The strip's cursor and the press that starts a resize, for a window that
// filters `central`'s events. True when the event was taken.
bool handleResizeStrip(QWidget *central, QWindow *handle, QEvent *event);

// A press on a header that stands in for a titlebar: drags the window, and a
// double-click maximises or restores it. True when the event was taken.
bool handleTitlebarEvent(QWidget *window, QEvent *event);

// ------------------------------------------------------------- window chrome
// Everything a secondary window needs to wear Monarchy's chrome: frameless and
// translucent with the tint painted behind it, and a header row carrying the
// traffic lights, the window's title (kept in step with windowTitle()) and a
// toolbar for the window's own buttons.
//
// Both entry points return nullptr in the universal look, so a window writes
//
//     if (auto *chrome = Monarchy::WindowChrome::adopt(this, outer))
//         heading->hide();
//
// and is otherwise untouched everywhere but Monarchy.
class WindowChrome : public QObject {
    Q_OBJECT

public:
    // For a window whose layout already exists: the header is laid over the
    // top of the window, `outer`'s margins grow to clear it and to leave the
    // grab strip round the edge, and a fixed-size window grows by as much.
    static WindowChrome *adopt(QWidget *window, QBoxLayout *outer);
    // For a window that builds its own title row: the caller adds header() to
    // its layout where the old bar went.
    static WindowChrome *create(QWidget *window);

    QWidget *header() const { return header_; }
    // The rest of the header row after the title, where the window's own
    // controls go. After adopt() it starts with a stretch, so they sit at the
    // right; after create() it is empty and the caller lays it out.
    QHBoxLayout *toolbar() const { return toolbar_; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    WindowChrome(QWidget *window, bool overlay);
    void placeHeader();

    QWidget *window_;
    QWidget *header_ = nullptr;
    QLabel *title_ = nullptr;
    WindowButtons *buttons_ = nullptr;
    QHBoxLayout *toolbar_ = nullptr;
    bool overlay_ = false;
};

}  // namespace Monarchy

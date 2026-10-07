// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// Whether the mixer is running on Monarchy, and the handful of that desktop's
// settings its window chrome has to follow when it is.
//
// The mixer has two looks. The universal one is what it has always been: a
// decorated window with the WAVELINE bar across the top, and it is what every
// other distribution gets. On Monarchy the window drops its decoration and
// wears the desktop's own toolbar instead -- the traffic lights in the header,
// the rounded pills with the light along their edges, a translucent blurred
// background, and a real menu bar for the global one at the top of the screen
// -- the way Monarchy's own applications do.
//
// Nothing here links against Monarchy. Its chrome lives in its own source tree
// as static libraries its applications compile in, which is no use to a
// program built anywhere else, so the pieces the mixer needs are carried in
// src/ui/monarchy/ and read the same configuration files Monarchy's copies do.
// On a machine with no Monarchy those files are simply absent and every value
// below is its default -- though on such a machine none of it is used anyway.

#pragma once

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QTimer>

class QFileSystemWatcher;

namespace Monarchy {

// True when the window should wear Monarchy's chrome: /etc/os-release names
// Monarchy (ID, or ID_LIKE for a derivative). WAVELINE_LOOK=universal or
// =monarchy overrides the answer either way, as does --look on the command
// line (see main.cpp) -- for a screenshot of the other look, or for someone
// who simply prefers it. Decided once per process.
bool isActive();

// Forces the answer, before the first window is built. main.cpp's --look.
void setActive(bool active);

// An application's icon, from the Icon= its launcher entry names, resolved
// in the desktop's icon theme (kdeglobals [Icons] Theme) the way Monarchy's
// dock and menus resolve it. Null when the theme has nothing by that name.
QIcon appIcon(const QString &iconName);

// True when the desktop is in Monarchy's light scheme (kdeglobals
// [General] ColorScheme=monarchy-light). Settings::changed() is emitted when
// it flips.
bool isLight();

// The desktop's accent colour, as Monarchy's Settings writes it into
// kdeglobals; invalid when there is none. Settings::changed() is emitted when
// it moves.
QColor accentColour();

// The part of Titlebar's configuration (titlebar-rc, the desktop's window
// decoration) that a window drawing its own buttons and shadow must follow, and
// Appearance > Window background opacity from Monarchy's own config. Watched,
// so a change in Settings reaches an open mixer without a restart.
//
// The arithmetic is Monarchy's, from common/titlebar/titlebar_button_settings
// and common/appearance/window_opacity, so a mixer window and a Files window
// side by side agree by construction.
class Settings : public QObject {
    Q_OBJECT

public:
    static Settings &instance();

    // The side of one traffic light's box, and the gaps between and around
    // them, in pixels.
    int buttonSize() const { return buttonSize_; }
    int buttonSpacing() const { return buttonSpacing_; }
    int buttonPadding() const { return buttonPadding_; }
    // [Buttons] Style=flat (plain discs rather than glass beads),
    // ZoomOnHover (the button under the pointer grows) and InactiveState (an
    // inactive window's buttons go grey). All off by default, as in Monarchy.
    bool flatButtons() const { return flatButtons_; }
    bool zoomOnHover() const { return zoomOnHover_; }
    bool inactiveState() const { return inactiveState_; }

    // One window shadow as titlebar-rc [Shadow] configures it: a size step
    // (0 none, 1 small, 2 medium, 3 large, 4 huge), a strength 25..255 and
    // a colour. Monarchy's common/titlebar/window_shadow_settings.
    struct ShadowStyle {
        int size = 3;
        int strength = 255;
        QColor colour = Qt::black;
    };
    const ShadowStyle &activeShadow() const { return activeShadow_; }
    // [Shadow] SeparateInactive: an inactive window wears inactiveShadow()
    // rather than activeShadow().
    bool separateInactiveShadow() const { return separateInactiveShadow_; }
    const ShadowStyle &inactiveShadow() const { return inactiveShadow_; }

    // The background's alpha byte for a window that paints its own, as
    // Monarchy's windows do. The light scheme is drawn a little more opaque,
    // as it is there.
    int windowAlpha(bool dark) const;

    // How the desktop's menus are painted: Appearance > Shell Components, for
    // "all context menus" or, where that tier is not configured, for "all".
    // The defaults are the shell menu's own, for a component nobody has set.
    struct MenuSurface {
        QColor fill{18, 18, 18, 153};   // the scheme's window colour, at 60%
        qreal fadeTop = 0.0;            // the white sheen over the fill, 0..1
        qreal fadeBottom = 0.0;
        qreal borderWidth = 2.5;
        qreal borderTop = 0.14;         // the rim's opacity at top and bottom
        qreal borderBottom = 0.01;
        QColor borderColour = Qt::white;
    };
    const MenuSurface &menuSurface() const { return menuSurface_; }

    // KWin's window corner radius (Round-Corners in kwinrc), which the menus
    // are rounded to so they match every window's corners.
    int kwinCornerRadius() const { return kwinCornerRadius_; }

    // KDecoration's small spacing: a quarter of the width of an M, which is how
    // a decoration's metrics are derived when there is no decoration to ask.
    static int smallSpacing();

signals:
    // Only when a value above actually moved.
    void changed();

private:
    Settings();
    void watch();
    void read();

    int buttonSize_ = 18;
    int buttonSpacing_ = 2;
    int buttonPadding_ = 2;
    bool flatButtons_ = false;
    bool zoomOnHover_ = false;
    bool inactiveState_ = false;
    ShadowStyle activeShadow_;
    bool separateInactiveShadow_ = false;
    ShadowStyle inactiveShadow_;
    int opacityPercent_ = 85;
    MenuSurface menuSurface_;
    int kwinCornerRadius_ = 14;
    QString signature_;

    QFileSystemWatcher *watcher_ = nullptr;
    // A save is several writes; wait for them to stop before re-reading.
    QTimer refresh_;
};

}  // namespace Monarchy

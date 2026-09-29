// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include <QApplication>
#include <QIcon>
#include <QTimer>

#include <cstdio>

#include "ui/mainwindow.h"
#include "ui/monarchy/desktop.h"
#include "ui/monarchy/menus.h"
#include "ui/theme.h"
#include "version.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("waveline-mixer"));
    QApplication::setApplicationVersion(QStringLiteral(WAVELINE_VERSION));
    QApplication::setOrganizationName(QStringLiteral("waveline"));

    // The launcher entry gets its icon from the .desktop file, but a *running*
    // window is a separate question and nothing above answers it -- which is
    // why the task manager and the alt-tab switcher were showing a placeholder
    // for a mixer whose menu entry looked perfectly fine.
    //
    // Two mechanisms because there are two display protocols. On Wayland the
    // window carries an app id and the compositor looks the .desktop file up
    // by that name; setDesktopFileName is what makes Qt send the right one.
    // On X11 the icon travels with the window itself, as pixels, which is what
    // setWindowIcon provides. Setting both leaves nothing to the session.
    QGuiApplication::setDesktopFileName(QStringLiteral("waveline-mixer"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("waveline-mixer")));

    // The look first: Theme::apply() already depends on it, for the accent.
    const QStringList args = QApplication::arguments();
    for (int i = 1; i + 1 < args.size(); ++i) {
        // --look universal, or --look monarchy. See ui/monarchy/desktop.h.
        // Not --style: QApplication takes that one for itself and removes it.
        if (args[i] == QLatin1String("--look"))
            Monarchy::setActive(args[i + 1] == QLatin1String("monarchy"));
    }

    Theme::apply();
    // On Monarchy, every menu and dropdown is the desktop's own.
    Monarchy::useShellMenus();

    // --screenshot renders the window once and exits. Works under
    // QT_QPA_PLATFORM=offscreen, so the layout can be checked without a
    // desktop session, and it is how the README image is produced.
    QString shot;
    QString shootWindow;
    int scroll = 0;
    int shotAfterMs = 700;
    int width = 0, height = 0;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--screenshot") && i + 1 < args.size())
            shot = args[i + 1];
        // The tuner is a window of its own, so it needs its own grab.
        else if (args[i] == QLatin1String("--screenshot-tuner") && i + 1 < args.size()) {
            shot = args[i + 1];
            shootWindow = QStringLiteral("tuner");
        }
        // --screenshot-window <name> <file>: one of the other windows instead
        // of the main one -- see MainWindow::openWindowForScreenshot().
        else if (args[i] == QLatin1String("--screenshot-window") && i + 2 < args.size()) {
            shootWindow = args[i + 1];
            shot = args[i + 2];
        }
        // How long to wait before the shot. Long enough to change the desktop's
        // scheme underneath it, which is how the in-place restyle is checked:
        // a switched window must match one started in the new scheme.
        else if (args[i] == QLatin1String("--screenshot-after") && i + 1 < args.size())
            shotAfterMs = args[i + 1].toInt();
        else if (args[i] == QLatin1String("--scroll") && i + 1 < args.size())
            scroll = args[i + 1].toInt();
        else if (args[i] == QLatin1String("--size") && i + 2 < args.size()) {
            width = args[i + 1].toInt();
            height = args[i + 2].toInt();
        }
    }

    MainWindow w;
    if (width > 0 && height > 0) w.resize(width, height);
    if (scroll) w.scrollSidebar(scroll);
    w.show();

    QWidget *subject = &w;
    if (!shootWindow.isEmpty()) {
        subject = w.openWindowForScreenshot(shootWindow);
        if (!subject) {
            std::fprintf(stderr, "unknown window %s\n", qPrintable(shootWindow));
            return 1;
        }
    }

    if (!shot.isEmpty()) {
        // One event-loop turn so the first poll lands and the widgets show
        // real values rather than their constructed defaults.
        QTimer::singleShot(shotAfterMs, &w, [subject, shot] {
            const bool ok = subject->grab().save(shot);
            std::fprintf(ok ? stdout : stderr, "%s %s\n",
                         ok ? "wrote" : "failed to write", qPrintable(shot));
            QApplication::exit(ok ? 0 : 1);
        });
    }

    return app.exec();
}

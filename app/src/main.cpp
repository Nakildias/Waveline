// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include <QApplication>
#include <QTimer>

#include <cstdio>

#include "ui/mainwindow.h"
#include "ui/theme.h"
#include "version.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("waveline-mixer"));
    QApplication::setApplicationVersion(QStringLiteral(WAVELINE_VERSION));
    QApplication::setOrganizationName(QStringLiteral("waveline"));
    Theme::apply();

    // --screenshot renders the window once and exits. Works under
    // QT_QPA_PLATFORM=offscreen, so the layout can be checked without a
    // desktop session, and it is how the README image is produced.
    QString shot;
    bool shootTuner = false;
    int scroll = 0;
    int width = 0, height = 0;
    const QStringList args = QApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--screenshot") && i + 1 < args.size())
            shot = args[i + 1];
        // The tuner is a window of its own, so it needs its own grab.
        else if (args[i] == QLatin1String("--screenshot-tuner") && i + 1 < args.size()) {
            shot = args[i + 1];
            shootTuner = true;
        }
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
    if (shootTuner) subject = w.openTunerWindow();

    if (!shot.isEmpty()) {
        // One event-loop turn so the first poll lands and the widgets show
        // real values rather than their constructed defaults.
        QTimer::singleShot(700, &w, [subject, shot] {
            const bool ok = subject->grab().save(shot);
            std::fprintf(ok ? stdout : stderr, "%s %s\n",
                         ok ? "wrote" : "failed to write", qPrintable(shot));
            QApplication::exit(ok ? 0 : 1);
        });
    }

    return app.exec();
}

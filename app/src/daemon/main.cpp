// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// wavelined -- owns the mixer graph and the Wave:3 hardware, and exposes them on
// the session bus as org.waveline.Mixer.
//
// Deliberately a QCoreApplication: the daemon must never depend on a display,
// so it can run from a systemd user unit at login with no session open yet.

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDebug>
#include <QSocketNotifier>

#include <csignal>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "engine/rtsched.h"
#include "mixerservice.h"

namespace {

// Unix signals cannot safely touch Qt objects, so route them through a
// signalfd and handle them on the event loop like any other input.
int installSignalHandler(QCoreApplication &app) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) return -1;
    const int fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (fd < 0) return -1;

    auto *notifier = new QSocketNotifier(fd, QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [&app, fd] {
        signalfd_siginfo si{};
        if (read(fd, &si, sizeof(si)) == sizeof(si))
            qInfo("received signal %u, shutting down", si.ssi_signo);
        app.quit();
    });
    return fd;
}

// Every filter node and every loopback path opens its own PipeWire connection,
// and each of those costs a socket, an eventfd, an eventpoll and a handful of
// memfds. A full graph -- five channels x six filters, twelve loopbacks, the
// meters -- settles at just over a thousand descriptors.
//
// systemd's user manager hands out a *soft* RLIMIT_NOFILE of 1024 no matter how
// large the hard limit is, so the daemon ran straight into the wall: filters
// failed to connect and, worse, module-loopback failed to load, which is why
// individual channel paths (waveline-music-monitor and friends) simply vanished
// mid-rewire and never came back. Nothing logged an error -- the allocation
// just returned null.
//
// Raising the soft limit to the hard limit is what every other audio daemon
// does, and it makes the graph deterministic instead of a race for descriptors.
void raiseFileLimit() {
    rlimit lim{};
    if (getrlimit(RLIMIT_NOFILE, &lim) != 0) return;
    if (lim.rlim_cur == lim.rlim_max) return;
    const rlim_t want = lim.rlim_max;
    rlimit next{want, lim.rlim_max};
    if (setrlimit(RLIMIT_NOFILE, &next) != 0) return;
    qInfo("raised open-file limit %ju -> %ju", uintmax_t(lim.rlim_cur),
          uintmax_t(want));
}

// The same move for RLIMIT_RTPRIO, and for the same reason: the hard limit is
// the system's answer to "may this daemon be real-time", the soft limit is
// merely where it starts, and every audio daemon lifts one to the other.
//
// It decides which of PipeWire's two routes to real-time scheduling this
// process takes -- direct, or via rtkit, which runs out of grants a quarter of
// the way through a full Waveline graph. See engine/rtsched.h for the whole
// mechanism. This must happen before the first PipeWire context exists, so it
// sits next to raiseFileLimit() and ahead of everything else.
void raiseRealtimeLimit() {
    const auto lim = waveline::raiseRealtimeLimits();
    if (!lim.realtimeAllowed()) {
        // Not fatal, and deliberately not a warning at startup: on a stock
        // system rtkit will still hand out its first two dozen grants and the
        // daemon runs. It is only at a small graph cycle, on a busy machine,
        // that the difference becomes audible -- so the honest place for this
        // is the diagnostics window, where it is reported next to the cycle
        // size that makes it matter.
        qInfo("real-time scheduling not granted (RLIMIT_RTPRIO is 0); "
              "PipeWire will fall back to rtkit, which stops at 25 threads "
              "per user -- run install.sh to grant it");
        return;
    }
    if (lim.raised)
        qInfo("raised real-time priority limit -> %ju", uintmax_t(lim.soft));
    else
        qInfo("real-time priority limit %ju", uintmax_t(lim.soft));
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("wavelined"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    installSignalHandler(app);
    // Both before anything opens a PipeWire connection: the first context to be
    // created reads these limits and every one after it inherits the decision.
    raiseFileLimit();
    raiseRealtimeLimit();

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical("cannot connect to the session bus");
        return 1;
    }

    // Claim the name before building anything: two daemons would each create a
    // full set of identically named PipeWire nodes, and links would then attach
    // to whichever was found first.
    if (!bus.registerService(QStringLiteral("org.waveline.Mixer"))) {
        qCritical("org.waveline.Mixer is already taken -- is wavelined already running?");
        return 1;
    }

    MixerService service;
    QString error;
    if (!service.start(error)) {
        qCritical("startup failed: %s", qPrintable(error));
        return 1;
    }

    if (!bus.registerObject(QStringLiteral("/org/waveline/Mixer"), &service,
                            QDBusConnection::ExportAllSlots |
                                QDBusConnection::ExportAllSignals)) {
        qCritical("cannot export /org/waveline/Mixer");
        return 1;
    }

    qInfo("wavelined ready on org.waveline.Mixer");
    qInfo("  channels: %s", qPrintable(service.ChannelIds().join(QLatin1String(", "))));
    qInfo("  device:   %s", service.DeviceConnected()
                                ? qPrintable(QStringLiteral("firmware %1")
                                                 .arg(service.DeviceFirmware()))
                                : "not connected");
    qInfo("  noise suppression: %s", service.NoiseSuppression() ? "on" : "off");
    qInfo("  app routing: %s", service.RoutingEnabled() ? "on" : "off");

    const int rc = app.exec();
    // Persist mute/volume before the debounced save timer can be dropped on SIGTERM.
    service.flushPendingSave();
    // Last thing, with the engine still up: mute the devices this mixer feeds,
    // so the gap between this daemon going away and the next one arriving is a
    // quiet one. The next start puts them back.
    service.muteOutputsForShutdown();
    return rc;
}

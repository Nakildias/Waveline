// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/mixerclient.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

class FakeMixer : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.waveline.Mixer")
public:
    int stateReads = 0;
    int levelReads = 0;
    QString name = QStringLiteral("Before");
public slots:
    QStringList Channels() {
        ++stateReads;
        return {QStringLiteral("test\t%1\t1\t1\t0\t0").arg(name)};
    }
    QStringList Levels() { ++levelReads; return {QStringLiteral("test\t0")}; }
signals:
    void Changed();
};

static void waitMs(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

static void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    FakeMixer service;
    auto bus = QDBusConnection::sessionBus();
    check(bus.registerService(QStringLiteral("org.waveline.Mixer")), "register private service");
    check(bus.registerObject(QStringLiteral("/org/waveline/Mixer"), &service,
                             QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals),
          "register fake mixer");
    MixerClient client;
    check(client.available(), "client connects");
    check(client.channels().size() == 1, "initial cache populated");

    const int initial = service.stateReads;
    waitMs(1300);
    check(service.stateReads == initial, "idle must not refresh all controls at 400 ms");
    check(service.levelReads > 30, "meter cadence preserved");

    service.name = QStringLiteral("After");
    emit service.Changed();
    waitMs(50);
    check(client.channels().front().name == service.name, "notification refreshes promptly");

    const int beforeBurst = service.stateReads;
    for (int i = 0; i < 50; ++i) emit service.Changed();
    waitMs(100);
    check(service.stateReads > beforeBurst && service.stateReads - beforeBurst < 50,
          "visible change bursts coalesce without dropping the update");

    client.setPollingEnabled(false);
    const int hiddenState = service.stateReads;
    const int hiddenLevels = service.levelReads;
    for (int i = 0; i < 50; ++i) emit service.Changed();
    waitMs(150);
    check(service.stateReads == hiddenState && service.levelReads == hiddenLevels,
          "hidden notifications and meters must not poll");
    service.name = QStringLiteral("Restored");
    client.setPollingEnabled(true);
    check(client.channels().front().name == service.name, "show immediately refreshes stale cache");

    // A legacy daemon or a missed signal still converges without interaction.
    service.name = QStringLiteral("Reconciled");
    waitMs(5500);
    check(client.channels().front().name == service.name, "fallback reconciliation remains active");

    client.setPollingEnabled(false);
    MixerClient hidden;
    hidden.setPollingEnabled(false);
    bus.unregisterService(QStringLiteral("org.waveline.Mixer"));
    waitMs(2300);
    check(!hidden.available(), "daemon disappearance detected");
    check(bus.registerService(QStringLiteral("org.waveline.Mixer")), "restart fake service");
    waitMs(2300);
    check(hidden.available(), "daemon restart detected while hidden");
    const int afterReconnect = service.levelReads;
    waitMs(150);
    check(service.levelReads == afterReconnect, "reconnect must respect hidden polling preference");
    hidden.setPollingEnabled(true);
    waitMs(100);
    check(service.levelReads > afterReconnect, "meters resume after hidden reconnect");
    std::puts("Idle polling, notifications, restore, reconciliation and reconnect passed.");
}

#include "idle_client_test.moc"

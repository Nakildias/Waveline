// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>

#include "companionserver.h"

#include "mixerservice.h"
#include "version.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>

namespace {

constexpr int kStateIntervalMs = 700;   // the slow refresh; Changed drives the rest
constexpr int kLevelIntervalMs = 50;    // 20 Hz, which is what a meter needs
constexpr int kChangedDebounceMs = 60;
// A request head larger than this is not one of ours. The page asks for five
// files and the WebSocket handshake is a few hundred bytes; anything past this
// is either a mistake or somebody probing, and neither deserves a buffer.
constexpr int kMaxRequestBytes = 16 * 1024;
// How long a socket may sit without producing a complete request head.
constexpr int kIdleHandshakeMs = 15000;

// The tri-state the effects button cycles through, as one word. Splitting it
// back into two booleans is the caller's job, and both of them are here so the
// two directions cannot drift apart.
QString fxMode(bool enabled, bool monitorFx) {
    if (!enabled) return QStringLiteral("off");
    return monitorFx ? QStringLiteral("monitor") : QStringLiteral("on");
}

QJsonObject rowsToAppearance(const QStringList &rows) {
    QJsonObject out;
    for (const QString &row : rows) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 3) continue;
        QJsonObject look;
        look[QStringLiteral("color")] = f[1];
        look[QStringLiteral("icon")] = f[2];
        out[f[0]] = look;
    }
    return out;
}

QByteArray contentTypeFor(const QString &path) {
    if (path.endsWith(QLatin1String(".html"))) return "text/html; charset=utf-8";
    if (path.endsWith(QLatin1String(".css"))) return "text/css; charset=utf-8";
    if (path.endsWith(QLatin1String(".js"))) return "application/javascript; charset=utf-8";
    if (path.endsWith(QLatin1String(".svg"))) return "image/svg+xml";
    if (path.endsWith(QLatin1String(".png"))) return "image/png";
    if (path.endsWith(QLatin1String(".webmanifest"))) return "application/manifest+json";
    return "application/octet-stream";
}

}  // namespace

CompanionServer::CompanionServer(MixerService *service, QObject *parent)
    : QObject(parent), service_(service) {
    stateTimer_.setInterval(kStateIntervalMs);
    connect(&stateTimer_, &QTimer::timeout, this, &CompanionServer::pushState);

    levelTimer_.setInterval(kLevelIntervalMs);
    connect(&levelTimer_, &QTimer::timeout, this, &CompanionServer::pushLevels);

    // Coalesces a fader drag's worth of Changed into one document.
    changedDebounce_.setSingleShot(true);
    changedDebounce_.setInterval(kChangedDebounceMs);
    connect(&changedDebounce_, &QTimer::timeout, this, &CompanionServer::pushState);
}

CompanionServer::~CompanionServer() { stop(); }

bool CompanionServer::running() const { return tcp_ != nullptr; }

int CompanionServer::clientCount() const { return int(clients_.size()); }

bool CompanionServer::start(int port, QString &error) {
    if (running()) {
        // Same port: already where the caller wants it. A different one is a
        // move, and a move is a stop and a start.
        if (port == port_) return true;
        stop();
    }

    if (port < 1024 || port > 65535) {
        error = QStringLiteral("port %1 is out of range (1024-65535)").arg(port);
        lastError_ = error;
        return false;
    }

    tcp_ = new QTcpServer(this);
    // Every interface, because the whole point is a device that is not this
    // machine. Loopback-only would make the feature unusable and is not the
    // safer default it looks like -- see the warning the panel carries.
    if (!tcp_->listen(QHostAddress::Any, quint16(port))) {
        error = tcp_->errorString();
        lastError_ = error;
        delete tcp_;
        tcp_ = nullptr;
        return false;
    }
    connect(tcp_, &QTcpServer::newConnection, this,
            &CompanionServer::onNewConnection);

    // Never listens itself: it only ever receives sockets this class has
    // already decided are handshakes.
    ws_ = new QWebSocketServer(QStringLiteral("Waveline Companion"),
                               QWebSocketServer::NonSecureMode, this);
    connect(ws_, &QWebSocketServer::newConnection, this,
            &CompanionServer::onWsConnected);

    port_ = port;
    lastError_.clear();
    error.clear();
    qInfo("companion server listening on port %d", port_);
    return true;
}

void CompanionServer::stop() {
    if (!running()) return;
    stateTimer_.stop();
    levelTimer_.stop();
    changedDebounce_.stop();

    for (QWebSocket *c : std::as_const(clients_)) {
        c->close(QWebSocketProtocol::CloseCodeGoingAway,
                 QStringLiteral("server stopping"));
        c->deleteLater();
    }
    clients_.clear();

    delete ws_;
    ws_ = nullptr;
    delete tcp_;
    tcp_ = nullptr;
    qInfo("companion server stopped");
    port_ = 0;
}

QStringList CompanionServer::addresses() const {
    QStringList out;
    if (!running()) return out;
    for (const QHostAddress &addr : QNetworkInterface::allAddresses()) {
        if (addr.isLoopback()) continue;
        // IPv4 only: this is a number somebody types into a phone.
        if (addr.protocol() != QAbstractSocket::IPv4Protocol) continue;
        out << QStringLiteral("http://%1:%2").arg(addr.toString()).arg(port_);
    }
    out.sort();
    return out;
}

// ============================================================ connections

void CompanionServer::onNewConnection() {
    while (tcp_ && tcp_->hasPendingConnections()) {
        QTcpSocket *sock = tcp_->nextPendingConnection();
        if (!sock) continue;
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] { classify(sock); });
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        // A connection that opens and then says nothing is not a browser. It
        // costs a descriptor for as long as it is held, and the graph is
        // already the thing on this machine most likely to run out of those.
        QTimer::singleShot(kIdleHandshakeMs, sock, [sock] {
            if (sock->property("waveline.classified").toBool()) return;
            sock->abort();
        });
    }
}

void CompanionServer::classify(QTcpSocket *sock) {
    // peek(), never read(): a handshake handed to QWebSocketServer has to
    // arrive with its request bytes still in the socket.
    const QByteArray head = sock->peek(kMaxRequestBytes);
    const int end = head.indexOf("\r\n\r\n");
    if (end < 0) {
        if (head.size() >= kMaxRequestBytes) {
            sendStatus(sock, 431, "Request Header Fields Too Large");
            sock->disconnectFromHost();
        }
        return;  // still arriving
    }

    // Whatever happens next, this socket is no longer ours to watch: HTTP is
    // answered in one shot, and a WebSocket's traffic belongs to QWebSocket.
    disconnect(sock, &QTcpSocket::readyRead, this, nullptr);
    sock->setProperty("waveline.classified", true);

    const QByteArray request = head.left(end);
    if (request.toLower().contains("upgrade: websocket")) {
        disconnect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        ws_->handleConnection(sock);
        return;
    }

    sock->read(end + 4);  // consume the head; nothing here has a body
    serveHttp(sock, request);
}

// ================================================================== HTTP

void CompanionServer::serveHttp(QTcpSocket *sock, const QByteArray &head) {
    const QByteArray line = head.left(head.indexOf('\r'));
    const QList<QByteArray> parts = line.split(' ');
    if (parts.size() < 2 || (parts[0] != "GET" && parts[0] != "HEAD")) {
        sendStatus(sock, 405, "Method Not Allowed");
        sock->disconnectFromHost();
        return;
    }

    QString path = QString::fromUtf8(parts[1]);
    const int query = path.indexOf(QLatin1Char('?'));
    if (query >= 0) path.truncate(query);
    if (path == QLatin1String("/")) path = QStringLiteral("/index.html");

    // The whole site is six known files. Mapping a request path onto the
    // resource tree would be a traversal question to get right; a table is not
    // one, and there is nothing here worth the generality.
    static const QHash<QString, QString> kFiles{
        {QStringLiteral("/index.html"), QStringLiteral(":/companion/index.html")},
        {QStringLiteral("/app.css"), QStringLiteral(":/companion/app.css")},
        {QStringLiteral("/app.js"), QStringLiteral(":/companion/app.js")},
        {QStringLiteral("/icon.svg"), QStringLiteral(":/companion/icon.svg")},
        {QStringLiteral("/app-icon.png"), QStringLiteral(":/companion/app-icon.png")},
        {QStringLiteral("/manifest.webmanifest"),
         QStringLiteral(":/companion/manifest.webmanifest")},
    };

    const auto it = kFiles.constFind(path);
    if (it == kFiles.constEnd()) {
        sendStatus(sock, 404, "Not Found");
        sock->disconnectFromHost();
        return;
    }
    sendFile(sock, it.value(), contentTypeFor(path));
    sock->disconnectFromHost();
}

void CompanionServer::sendFile(QTcpSocket *sock, const QString &resource,
                               const QByteArray &contentType) {
    QFile f(resource);
    if (!f.open(QIODevice::ReadOnly)) {
        sendStatus(sock, 500, "Internal Server Error");
        return;
    }
    const QByteArray body = f.readAll();

    QByteArray header = "HTTP/1.1 200 OK\r\n";
    header += "Content-Type: " + contentType + "\r\n";
    header += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    // The files are compiled into the daemon and change only with it, so a
    // cached copy on the tablet would survive an upgrade and talk the wrong
    // protocol at the new server.
    header += "Cache-Control: no-store\r\n";
    header += "Connection: close\r\n\r\n";
    sock->write(header);
    sock->write(body);
}

void CompanionServer::sendStatus(QTcpSocket *sock, int code,
                                 const QByteArray &reason) {
    QByteArray header = "HTTP/1.1 " + QByteArray::number(code) + " " + reason + "\r\n";
    header += "Content-Length: 0\r\n";
    header += "Connection: close\r\n\r\n";
    sock->write(header);
}

// ============================================================= WebSocket

void CompanionServer::onWsConnected() {
    while (ws_ && ws_->hasPendingConnections()) {
        QWebSocket *client = ws_->nextPendingConnection();
        if (!client) continue;
        clients_.append(client);
        connect(client, &QWebSocket::textMessageReceived, this,
                &CompanionServer::onTextMessage);
        connect(client, &QWebSocket::disconnected, this,
                &CompanionServer::onWsDisconnected);
        // Its own snapshot straight away rather than at the next tick: a page
        // that draws nothing for half a second on load reads as broken.
        send(client, buildState());
        send(client, buildLevels());
        qInfo("companion client connected from %s (%d total)",
              qPrintable(client->peerAddress().toString()), int(clients_.size()));
    }
    syncTimers();
}

void CompanionServer::onWsDisconnected() {
    auto *client = qobject_cast<QWebSocket *>(sender());
    if (!client) return;
    clients_.removeAll(client);
    client->deleteLater();
    syncTimers();
}

void CompanionServer::syncTimers() {
    const bool wanted = !clients_.isEmpty();
    if (wanted == stateTimer_.isActive()) return;
    if (wanted) {
        stateTimer_.start();
        levelTimer_.start();
    } else {
        stateTimer_.stop();
        levelTimer_.stop();
        changedDebounce_.stop();
    }
}

void CompanionServer::notifyChanged() {
    if (clients_.isEmpty()) return;
    if (!changedDebounce_.isActive()) changedDebounce_.start();
}

void CompanionServer::send(QWebSocket *client, const QJsonObject &msg) {
    client->sendTextMessage(
        QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void CompanionServer::broadcast(const QJsonObject &msg) {
    if (clients_.isEmpty()) return;
    const QString text =
        QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    for (QWebSocket *c : std::as_const(clients_)) c->sendTextMessage(text);
}

void CompanionServer::pushState() {
    changedDebounce_.stop();
    broadcast(buildState());
}

void CompanionServer::pushLevels() { broadcast(buildLevels()); }

// ================================================================= state

QJsonObject CompanionServer::buildState() const {
    QJsonObject o;
    o[QStringLiteral("t")] = QStringLiteral("state");
    o[QStringLiteral("version")] = QStringLiteral(WAVELINE_VERSION);
    o[QStringLiteral("brand")] = service_->DeviceBrand();

    QJsonArray profiles;
    for (const QString &name : service_->Profiles()) profiles.append(name);
    o[QStringLiteral("profiles")] = profiles;
    o[QStringLiteral("activeProfile")] = service_->ActiveProfile();

    o[QStringLiteral("routing")] = service_->RoutingEnabled();
    o[QStringLiteral("sharing")] = service_->SoundSharingEnabled();
    o[QStringLiteral("looks")] = rowsToAppearance(service_->CardAppearances());

    // ---- input devices
    QJsonArray inputs;
    for (const QString &row : service_->MasterBuses()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 8) continue;
        const QString id = f[0];
        QJsonObject in;
        in[QStringLiteral("id")] = id;
        in[QStringLiteral("name")] = f[1];
        in[QStringLiteral("busType")] = f[3];
        in[QStringLiteral("deviceLabel")] = f[6];
        in[QStringLiteral("connected")] = f[7] == QLatin1String("1");
        in[QStringLiteral("monitorVolume")] =
            service_->MasterMicVolume(id, QStringLiteral("monitor"));
        in[QStringLiteral("streamVolume")] =
            service_->MasterMicVolume(id, QStringLiteral("stream"));
        in[QStringLiteral("monitorMuted")] =
            service_->MasterMicMixMuted(id, QStringLiteral("monitor"));
        in[QStringLiteral("streamMuted")] =
            service_->MasterMicMixMuted(id, QStringLiteral("stream"));
        in[QStringLiteral("fx")] = fxMode(service_->MasterMicEffectsEnabled(id),
                                          service_->MasterMicMonitorFx(id));
        in[QStringLiteral("inputMuted")] = service_->MasterMicInputMuted(id);
        in[QStringLiteral("inputVolume")] = service_->MasterMicInputVolume(id);
        in[QStringLiteral("monitoring")] = service_->MasterSoftwareMonitor(id);
        inputs.append(in);
    }
    o[QStringLiteral("inputs")] = inputs;

    // ---- channels
    QJsonArray channels;
    for (const QString &row : service_->Channels()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 6) continue;
        const QString id = f[0];
        QJsonObject ch;
        ch[QStringLiteral("id")] = id;
        ch[QStringLiteral("name")] = f[1];
        ch[QStringLiteral("streamVolume")] = f[2].toDouble();
        ch[QStringLiteral("monitorVolume")] = f[3].toDouble();
        ch[QStringLiteral("streamMuted")] = f[4] == QLatin1String("1");
        ch[QStringLiteral("monitorMuted")] = f[5] == QLatin1String("1");
        ch[QStringLiteral("fx")] = fxMode(service_->ChannelEffectsEnabled(id),
                                          service_->ChannelMonitorFx(id));
        // The channel's own published microphone. `micSource` is what decides
        // whether the third fader is live: a channel that publishes nothing has
        // no gain to set, and a control that moves without acting on anything is
        // worse than one that is visibly greyed.
        ch[QStringLiteral("micSource")] = service_->ChannelMicSource(id);
        ch[QStringLiteral("micSend")] = service_->ChannelMicSend(id);
        ch[QStringLiteral("micMuted")] = service_->ChannelMicMuted(id);
        ch[QStringLiteral("micMonitor")] = service_->ChannelMicMonitor(id);
        channels.append(ch);
    }
    o[QStringLiteral("channels")] = channels;

    // ---- applications
    //
    // Two lists, one row each. Apps() carries the channel and the playback
    // volume, SoundSharingApps() carries where it joins a microphone -- and a
    // client that had to correlate them itself would be reimplementing the
    // Apps tab, so they are merged on node id here.
    QHash<uint, QJsonObject> shares;
    for (const QString &row : service_->SoundSharingApps()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 4) continue;
        QJsonObject s;
        s[QStringLiteral("shareTarget")] = f[2];
        s[QStringLiteral("shareLevel")] = f[3].toDouble();
        shares.insert(f[0].toUInt(), s);
    }

    QJsonArray apps;
    for (const QString &row : service_->Apps()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 4) continue;
        const uint nodeId = f[0].toUInt();
        QJsonObject a;
        a[QStringLiteral("nodeId")] = double(nodeId);
        a[QStringLiteral("name")] = f[1];
        a[QStringLiteral("channel")] = f[2];
        a[QStringLiteral("volume")] = f[3].toDouble();
        const QJsonObject s = shares.value(nodeId);
        a[QStringLiteral("shareTarget")] = s[QStringLiteral("shareTarget")];
        a[QStringLiteral("shareLevel")] =
            s.contains(QStringLiteral("shareLevel")) ? s[QStringLiteral("shareLevel")]
                                                     : QJsonValue(1.0);
        apps.append(a);
    }
    o[QStringLiteral("apps")] = apps;

    QJsonArray targets;
    for (const QString &row : service_->SoundSharingTargets()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 2) continue;
        QJsonObject t;
        t[QStringLiteral("id")] = f[0];
        t[QStringLiteral("label")] = f[1];
        targets.append(t);
    }
    o[QStringLiteral("shareTargets")] = targets;

    // ---- outputs
    QJsonArray monitors;
    for (const QString &row : service_->MonitorOutputStates()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 5) continue;
        QJsonObject m;
        m[QStringLiteral("sink")] = f[0];
        m[QStringLiteral("volume")] = f[1].toDouble();
        m[QStringLiteral("muted")] = f[2] == QLatin1String("1");
        m[QStringLiteral("connected")] = f[3] == QLatin1String("1");
        m[QStringLiteral("description")] = f[4];
        monitors.append(m);
    }
    QJsonObject outs;
    outs[QStringLiteral("monitor")] = monitors;
    QJsonObject stream;
    stream[QStringLiteral("volume")] = service_->StreamMixVolume();
    stream[QStringLiteral("muted")] = service_->StreamMixMuted();
    outs[QStringLiteral("stream")] = stream;
    QJsonObject master;
    master[QStringLiteral("volume")] = service_->MonitorOutputVolume();
    master[QStringLiteral("muted")] = service_->MonitorOutputMuted();
    outs[QStringLiteral("monitorMaster")] = master;
    o[QStringLiteral("outputs")] = outs;

    // ---- soundboard
    //
    // Play order is display order (SoundboardSounds()'s own contract), so the
    // array carries no separate ordering field: a client that renders it in
    // the order it arrived is already showing the board's real order, and
    // soundboard.reorder sends that same order straight back. Deliberately
    // thin -- no volume, trim or file path -- because the companion never
    // edits a sound, only plays and reorders it; see handleCommand()'s own
    // comment about what a performance surface is for.
    QJsonArray sounds;
    for (const QString &row : service_->SoundboardSounds()) {
        const QStringList f = row.split(QLatin1Char('\t'));
        if (f.size() < 7) continue;
        QJsonObject s;
        s[QStringLiteral("id")] = f[0];
        s[QStringLiteral("name")] = f[1];
        s[QStringLiteral("durationMs")] = f[5].toDouble();
        sounds.append(s);
    }
    QJsonObject soundboard;
    soundboard[QStringLiteral("sounds")] = sounds;
    o[QStringLiteral("soundboard")] = soundboard;

    return o;
}

QJsonObject CompanionServer::buildLevels() const {
    QJsonObject peaks;
    for (const QString &row : service_->Levels()) {
        const int tab = row.indexOf(QLatin1Char('\t'));
        if (tab < 0) continue;
        peaks[row.left(tab)] = row.mid(tab + 1).toDouble();
    }
    // Soundboard playback progress, at the same 20 Hz the meters get: a
    // progress ring drawn around a pad's border needs the same cadence a
    // level bar does, and riding the fast push spares this a timer of its
    // own. Only ids currently playing appear here -- absence *is* "not
    // playing", the same convention SoundboardProgress() itself uses.
    QJsonObject soundboard;
    for (const QString &row : service_->SoundboardProgress()) {
        const int tab = row.indexOf(QLatin1Char('\t'));
        if (tab < 0) continue;
        soundboard[row.left(tab)] = row.mid(tab + 1).toDouble();
    }
    QJsonObject o;
    o[QStringLiteral("t")] = QStringLiteral("levels");
    o[QStringLiteral("peaks")] = peaks;
    o[QStringLiteral("soundboard")] = soundboard;
    return o;
}

// =============================================================== commands

void CompanionServer::onTextMessage(const QString &text) {
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) return;
    handleCommand(doc.object());
}

void CompanionServer::handleCommand(const QJsonObject &msg) {
    const QString cmd = msg[QStringLiteral("cmd")].toString();
    const QString id = msg[QStringLiteral("id")].toString();
    const QString mix = msg[QStringLiteral("mix")].toString() == QLatin1String("monitor")
                            ? QStringLiteral("monitor")
                            : QStringLiteral("stream");
    const double value = msg[QStringLiteral("value")].toDouble();
    const bool on = msg[QStringLiteral("on")].toBool();
    const uint nodeId = uint(msg[QStringLiteral("nodeId")].toDouble());
    const int index = msg[QStringLiteral("index")].toInt();

    // Deliberately not a table of every method on the service. The companion
    // is a performance surface, not a second settings window: it can move what
    // a person moves mid-stream, and the things that rename, delete or rewire
    // are simply not reachable from here. Anything not listed is ignored.
    if (cmd == QLatin1String("profile.load")) {
        service_->LoadProfile(msg[QStringLiteral("name")].toString());
    } else if (cmd == QLatin1String("input.volume")) {
        service_->SetMasterMicVolume(id, mix, value);
    } else if (cmd == QLatin1String("input.mute")) {
        service_->SetMasterMicMixMuted(id, mix, on);
    } else if (cmd == QLatin1String("input.gate")) {
        service_->SetMasterMicInputMuted(id, on);
    } else if (cmd == QLatin1String("input.inputVolume")) {
        // The desktop's gain fader. Not a mix level: this is the device's own
        // capture volume, the one the system's sound settings show, so it is
        // heard by everything recording from the microphone and not only by us.
        service_->SetMasterMicInputVolume(id, value);
    } else if (cmd == QLatin1String("input.fx")) {
        // Both halves, always, and effects before monitoring: turning the chain
        // off while its monitor tap is still pointed at it is the one ordering
        // that leaves the graph describing something that is not happening.
        const QString mode = msg[QStringLiteral("mode")].toString();
        if (mode == QLatin1String("off")) {
            service_->SetMasterMicMonitorFx(id, false);
            service_->SetMasterMicEffectsEnabled(id, false);
        } else {
            service_->SetMasterMicEffectsEnabled(id, true);
            service_->SetMasterMicMonitorFx(id, mode == QLatin1String("monitor"));
        }
    } else if (cmd == QLatin1String("channel.volume")) {
        service_->SetChannelVolume(id, mix, value);
    } else if (cmd == QLatin1String("channel.mute")) {
        service_->SetChannelMuted(id, mix, on);
    } else if (cmd == QLatin1String("channel.fx")) {
        const QString mode = msg[QStringLiteral("mode")].toString();
        if (mode == QLatin1String("off")) {
            service_->SetChannelMonitorFx(id, false);
            service_->SetChannelEffectsEnabled(id, false);
        } else {
            service_->SetChannelEffectsEnabled(id, true);
            service_->SetChannelMonitorFx(id, mode == QLatin1String("monitor"));
        }
    } else if (cmd == QLatin1String("channel.micSend")) {
        service_->SetChannelMicSend(id, value);
    } else if (cmd == QLatin1String("channel.micMute")) {
        service_->SetChannelMicMuted(id, on);
    } else if (cmd == QLatin1String("channel.micMonitor")) {
        service_->SetChannelMicMonitor(id, on);
    } else if (cmd == QLatin1String("input.monitor")) {
        service_->SetMasterSoftwareMonitor(id, on);
    } else if (cmd == QLatin1String("routing.enabled")) {
        service_->SetRoutingEnabled(on);
    } else if (cmd == QLatin1String("sharing.enabled")) {
        service_->SetSoundSharingEnabled(on);
    } else if (cmd == QLatin1String("soundboard.play")) {
        // No stop-first: sounds can overlap themselves, and the button that
        // triggered this already reads as "stop" once playing starts, on the
        // client's own toggle logic -- see the desktop's identical PlayPad.
        service_->PlaySoundboardSound(id);
    } else if (cmd == QLatin1String("soundboard.stop")) {
        service_->StopSoundboardSound(id);
    } else if (cmd == QLatin1String("soundboard.reorder")) {
        QStringList order;
        for (const QJsonValue &v : msg[QStringLiteral("order")].toArray())
            order << v.toString();
        service_->ReorderSoundboardSounds(order);
    } else if (cmd == QLatin1String("app.channel")) {
        service_->MoveApp(nodeId, msg[QStringLiteral("channel")].toString());
    } else if (cmd == QLatin1String("app.volume")) {
        service_->SetAppVolume(nodeId, value);
    } else if (cmd == QLatin1String("app.share")) {
        service_->SetSoundSharingAppTarget(nodeId,
                                           msg[QStringLiteral("target")].toString());
    } else if (cmd == QLatin1String("app.shareLevel")) {
        service_->SetSoundSharingAppLevel(nodeId, value);
    } else if (cmd == QLatin1String("monitorOut.volume")) {
        service_->SetMonitorOutputVolumeAt(index, value);
    } else if (cmd == QLatin1String("monitorOut.mute")) {
        service_->SetMonitorOutputMutedAt(index, on);
    } else if (cmd == QLatin1String("monitorMix.volume")) {
        service_->SetMonitorOutputVolume(value);
    } else if (cmd == QLatin1String("monitorMix.mute")) {
        service_->SetMonitorOutputMuted(on);
    } else if (cmd == QLatin1String("streamMix.volume")) {
        service_->SetStreamMixVolume(value);
    } else if (cmd == QLatin1String("streamMix.mute")) {
        service_->SetStreamMixMuted(on);
    } else {
        return;
    }

    // The mutating calls above emit Changed(), which lands here debounced --
    // but the client that moved the control wants its own echo sooner than
    // that, and every other client wants it at the same time.
    notifyChanged();
}

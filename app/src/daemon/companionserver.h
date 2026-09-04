// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Nakildias <nakildiaspro@gmail.com>
//
// The Web Companion: a phone or tablet as a second surface for this mixer.
//
// It lives in the daemon rather than in the GUI, for the same reason everything
// else does. The point of a tablet controller is that it works while nothing is
// on screen -- during a stream, with the mixer window closed, or on a machine
// that never opened one -- and a server hanging off waveline-mixer would vanish
// the moment the window did.
//
// One TCP port serves both halves. The page is a handful of static files, and
// the live protocol is a WebSocket; running those on separate ports would mean
// two numbers for the user to know and two holes in their firewall, so this
// listens once and looks at the request: an Upgrade handshake is handed to
// QWebSocketServer, everything else is answered as HTTP. QWebSocketServer
// supports exactly this through handleConnection(), which is why the socket is
// only ever peeked at and never read from before the handoff.
//
// Clients get pushed state; they do not poll. A fader dragged on a tablet has
// to move the meter on the desktop inside a frame or two to feel connected to
// anything, and a request/response loop over Wi-Fi does not do that.

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

class MixerService;
class QTcpServer;
class QTcpSocket;
class QWebSocket;
class QWebSocketServer;

class CompanionServer : public QObject {
    Q_OBJECT

public:
    explicit CompanionServer(MixerService *service, QObject *parent = nullptr);
    ~CompanionServer() override;

    // Empty error string on success. Failing to bind is an ordinary outcome --
    // the port is routinely already taken -- so it is reported rather than
    // logged and swallowed.
    bool start(int port, QString &error);
    void stop();

    bool running() const;
    // The port actually bound, which is the requested one or nothing at all:
    // silently landing on a different port would leave every bookmarked tablet
    // pointing somewhere dead.
    int port() const { return port_; }
    int clientCount() const;
    // "http://192.168.1.20:8787", one per non-loopback IPv4 address, so the
    // panel can show something typeable rather than "0.0.0.0".
    QStringList addresses() const;
    QString lastError() const { return lastError_; }

public slots:
    // Hooked to MixerService::Changed. Debounced: a fader drag emits it per
    // step, and each push is a full state document.
    void notifyChanged();

private:
    void onNewConnection();
    // Decides between HTTP and a WebSocket handshake, once enough of the
    // request has arrived to tell.
    void classify(QTcpSocket *sock);
    void serveHttp(QTcpSocket *sock, const QByteArray &head);
    void sendFile(QTcpSocket *sock, const QString &resource,
                  const QByteArray &contentType);
    void sendStatus(QTcpSocket *sock, int code, const QByteArray &reason);

    void onWsConnected();
    void onWsDisconnected();
    void onTextMessage(const QString &text);
    void handleCommand(const QJsonObject &msg);

    // Everything a client draws with, except the meters. Sent on connect, on
    // Changed, and on a slow tick for the things Changed does not cover (the
    // application list, which follows PipeWire rather than any setting).
    QJsonObject buildState() const;
    // Meters only, at the frame rate. Kept apart from the state document
    // because it is sent twenty times as often and is a tenth the size.
    QJsonObject buildLevels() const;
    void pushState();
    void pushLevels();
    void send(QWebSocket *client, const QJsonObject &msg);
    void broadcast(const QJsonObject &msg);
    // Timers run only while somebody is connected: a daemon with the server up
    // and no tablet on it should cost nothing.
    void syncTimers();

    MixerService *service_ = nullptr;
    QTcpServer *tcp_ = nullptr;
    QWebSocketServer *ws_ = nullptr;
    QList<QWebSocket *> clients_;
    QTimer stateTimer_;
    QTimer levelTimer_;
    QTimer changedDebounce_;
    int port_ = 0;
    QString lastError_;
};

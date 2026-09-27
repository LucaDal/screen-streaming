#include "RoomClient.h"
#include "SignalingServer.h"
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QThread>
#include <QUrlQuery>
#include <QVideoFrameFormat>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTcpServer>
#include <algorithm>
#include <iostream>

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const auto addresses = QNetworkInterface::allAddresses();
    if (std::none_of(addresses.cbegin(), addresses.cend(), [](const auto& a) { return !a.isNull() && !a.isLoopback(); })) return 77;
    const QString serverKey = "test-server-secret-at-least-16";
    TurnService turn;
    SignalingServer server(serverKey, QWebSocketServer::NonSecureMode);
    WebRtcPeer::IceConfig ice;
    const bool withTurn = app.arguments().contains("--turn") || app.arguments().contains("--turn-tcp");
    QTemporaryDir turnDirectory;
    if (withTurn) {
        auto executable = qEnvironmentVariable("STREAMING_TEST_TURNSERVER");
        if (executable.isEmpty()) executable = QStandardPaths::findExecutable("turnserver");
        if (executable.isEmpty()) { std::cerr << "SKIP: install coturn or set STREAMING_TEST_TURNSERVER\n"; return 77; }
        QHostAddress relay;
        for (const auto& address : addresses)
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()) { relay = address; break; }
        if (relay.isNull()) return 77;
        QTcpServer portProbe;
        if (!portProbe.listen(QHostAddress::LocalHost, 0)) return 1;
        const auto turnPort = portProbe.serverPort(); portProbe.close();
        QFile config(turnDirectory.filePath("turnserver.conf"));
        if (!config.open(QIODevice::WriteOnly)) return 1;
        config.write(QString("listening-ip=%1\nrelay-ip=%1\nlistening-port=%2\n"
            "min-port=54000\nmax-port=54100\nrealm=streaming-test\nuse-auth-secret\n"
            "static-auth-secret=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
            "no-tls\nno-dtls\nno-cli\nno-tcp-relay\nrelay-threads=1\nverbose\n").arg(relay.toString()).arg(turnPort).toUtf8());
        config.close();
        const auto transport = app.arguments().contains("--turn-tcp") ? "tcp" : "udp";
        QString error;
        if (!turn.configure(config.fileName(), {QString("turn://%1:%2?transport=%3").arg(relay.toString()).arg(turnPort).arg(transport)}, error)
            || !turn.start(executable, error)) { std::cerr << error.toStdString() << '\n'; return 1; }
        server.setTurnService(&turn);
        // No manual URI: successful frames prove automatic server credentials
        // and real relay allocations on BOTH peers, including publisher handoff.
        ice.relayOnly = true;
    }
    if (!server.listen(QHostAddress::LocalHost, 0)) return 1;
    const QUrl url(QString("ws://127.0.0.1:%1").arg(server.port()));
    RoomClient first, second, intruder;
    first.setShareAudio(false); second.setShareAudio(false);
    VideoPipeline firstPipeline, secondPipeline;
    QString invitation, failure;
    bool rejected = false;
    QObject::connect(&first, &RoomClient::invitationReady, &app, [&](const QString& s) { invitation = s; });
    for (auto* room : {&first, &second}) {
        QObject::connect(room, &RoomClient::errorOccurred, &app, [&](const QString& e) { failure = e; });
        auto* pipeline = room == &first ? &firstPipeline : &secondPipeline;
        QObject::connect(room, &RoomClient::sessionStopped, &app, [pipeline] { pipeline->stop(); });
        QObject::connect(room, &RoomClient::captureRequested, &app, [&, room, pipeline](const VideoSettings& settings) {
            if (settings.fps != 30 || settings.maxSize != QSize(1280, 720) || settings.bitrateKbps != 4000)
                failure = "Receiver quality preference was not respected";
            QString error;
            if (!pipeline->start(settings, error, room->encodedHandler())) failure = error;
        });
    }
    QObject::connect(&intruder, &RoomClient::errorOccurred, &app, [&](const QString&) { rejected = true; });
    QVideoFrame frame(QVideoFrameFormat({320, 180}, QVideoFrameFormat::Format_RGBA8888));
    frame.map(QVideoFrame::WriteOnly);
    std::fill_n(frame.bits(0), frame.mappedBytes(0), 200); frame.unmap();
    auto wait = [&](auto done, int limit = 20000) {
        QElapsedTimer timer; timer.start();
        qint64 next = 0;
        while (!done() && failure.isEmpty() && timer.elapsed() < limit) {
            app.processEvents();
            if (timer.elapsed() >= next) {
                if (first.sending()) firstPipeline.submit(frame);
                if (second.sending()) secondPipeline.submit(frame);
                next = timer.elapsed() + 33;
            }
            QThread::msleep(2);
        }
        return done() && failure.isEmpty();
    };
    auto require = [&](bool okay, const char* step) {
        if (!okay) std::cerr << "FAIL " << step << ": " << failure.toStdString() << '\n';
        return okay;
    };
    first.create(url, serverKey, {}, ice);
    if (!require(wait([&] { return first.joined() && !invitation.isEmpty(); }), "create")) return 1;
    const QUrlQuery fields{QUrl(invitation)};
    if (!require(!fields.queryItemValue("room").isEmpty() && fields.queryItemValue("key") != serverKey
        && fields.queryItemValue("key").size() >= 40, "scoped invitation")) return 1;
    intruder.join(url, fields.queryItemValue("room"), serverKey, {}, {});
    if (!require(wait([&] { return rejected; }), "server key must not grant room access")) return 1;
    intruder.leave();
    second.joinInvitation(invitation, {30, {1280, 720}, 4000}, ice);
    if (!require(wait([&] { return first.participants() == 2 && second.participants() == 2; }), "join invitation")) return 1;
    first.publish();
    if (!require(wait([&] { return second.receivedFrames() >= 5; }), "first shares")) return 1;
    second.stopSharing();
    if (!require(wait([&] { return !first.busy() && !second.busy(); }), "receiver stops")) return 1;
    second.publish();
    if (!require(wait([&] { return first.receivedFrames() >= 5; }), "second shares")) return 1;
    first.leave(); second.leave();
    // Drain disconnects before attempting the now expired invitation.
    QElapsedTimer drain; drain.start();
    while (drain.elapsed() < 100) { app.processEvents(); QThread::msleep(2); }
    rejected = false; intruder.joinInvitation(invitation, {}, {});
    if (!require(wait([&] { return rejected; }), "expired invitation")) return 1;
    std::cout << "PASS: generated scoped invite, rejected server key, receiver quality, sharing both ways, stop and expiry\n";
}

#include "SignalingServer.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <QRandomGenerator>
#include <algorithm>

namespace {
bool validSettings(const QJsonObject &s) {
    const int fps = s.value("fps").toInt();
    const int width = s.value("width").toInt();
    const int height = s.value("height").toInt();
    const int bitrate = s.value("bitrate").toInt();
    return (fps == 30 || fps == 60) &&
           ((width == 1280 && height == 720) ||
            (width == 1920 && height == 1080)) &&
           bitrate >= 1000 && bitrate <= 50000;
}
} // namespace
SignalingServer::SignalingServer(QString token, QWebSocketServer::SslMode mode,
                                 QObject *parent)
    : QObject(parent),
      m_server(QStringLiteral("StreamingApp signaling"), mode, this),
      m_token(std::move(token)) {
    m_server.setMaxPendingConnections(32);
    m_server.setHandshakeTimeout(10000);
    connect(&m_server, &QWebSocketServer::newConnection, this,
            &SignalingServer::accept);
}
SignalingServer::~SignalingServer() {
    for (auto *socket : m_clients.keys()) {
        socket->disconnect(this);
        socket->abort();
        delete socket;
    }
}
bool SignalingServer::listen(const QHostAddress &address, quint16 port) {
    return m_server.listen(address, port);
}
void SignalingServer::send(QWebSocket *socket, const QJsonObject &message) {
    if (socket->bytesToWrite() > 512 * 1024) {
        socket->close();
        return;
    }
    socket->sendTextMessage(QString::fromUtf8(
        QJsonDocument(message).toJson(QJsonDocument::Compact)));
}
void SignalingServer::reject(QWebSocket *socket, const QString &message) {
    send(socket, {{"type", "error"}, {"message", message}});
}
void SignalingServer::accept() {
    while (m_server.hasPendingConnections()) {
        auto *socket = m_server.nextPendingConnection();
        if (m_clients.size() >= 64) {
            socket->close();
            socket->deleteLater();
            continue;
        }
        socket->setParent(this);
        socket->setMaxAllowedIncomingFrameSize(128 * 1024);
        socket->setMaxAllowedIncomingMessageSize(128 * 1024);
        Client client;
        client.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        client.rate.start();
        m_clients.insert(socket, client);
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString &text) { handle(socket, text); });
        connect(socket, &QWebSocket::binaryMessageReceived, socket,
                [socket] { socket->close(); });
        connect(socket, &QWebSocket::disconnected, this, [this, socket] {
            remove(socket);
            socket->deleteLater();
        });
        QTimer::singleShot(10000, socket, [this, socket] {
            if (m_clients.contains(socket) && m_clients[socket].room.isEmpty())
                socket->close();
        });
    }
}
void SignalingServer::presence(Room &room) {
    for (auto *member : room.members)
        send(member, {{"type", "presence"},
                      {"peers", room.members.size()},
                      {"busy", room.publisher != nullptr}});
}
void SignalingServer::stopSession(Room &room) {
    room.publisher = nullptr;
    room.session.clear();
    for (auto *member : room.members)
        send(member, {{"type", "stopped"}});
    presence(room);
}
void SignalingServer::remove(QWebSocket *socket) {
    if (!m_clients.contains(socket))
        return;
    const auto name = m_clients.take(socket).room;
    if (!m_rooms.contains(name))
        return;
    auto &room = m_rooms[name];
    room.members.removeAll(socket);
    if (room.publisher)
        stopSession(room);
    else
        presence(room);
    if (room.members.isEmpty())
        m_rooms.remove(name);
}
void SignalingServer::handle(QWebSocket *socket, const QString &text) {
    auto &client = m_clients[socket];
    if (client.rate.elapsed() > 1000) {
        client.rate.restart();
        client.messages = 0;
    }
    if (++client.messages > 200) {
        socket->close();
        return;
    }
    const auto document = QJsonDocument::fromJson(text.toUtf8());
    if (!document.isObject()) {
        reject(socket, "Messaggio JSON non valido.");
        return;
    }
    const auto message = document.object();
    const auto type = message.value("type").toString();
    if (type == "create" || type == "join") {
        QString name = message.value("room").toString();
        const auto token = message.value("token").toString();
        static const QRegularExpression pattern(QStringLiteral("^[a-zA-Z0-9_-]{1,64}$"));
        const bool creating = type == "create";
        if (!client.room.isEmpty() || !validSettings(message.value("settings").toObject())
            || (creating ? token != m_token
                : (!pattern.match(name).hasMatch() || !m_rooms.contains(name)
                   || token != m_rooms.value(name).key))) {
            reject(socket, "Accesso rifiutato: verifica l’invito o la chiave del server.");
            socket->close();
            return;
        }
        if (creating) {
            if (m_rooms.size() >= 32) {
                reject(socket, "Server pieno. Riprova più tardi."); socket->close(); return;
            }
            name = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QByteArray secret(32, '\0');
            for (int i = 0; i < secret.size(); i += 4) {
                const quint32 random = QRandomGenerator::system()->generate();
                for (int j = 0; j < 4; ++j) secret[i + j] = static_cast<char>(random >> (j * 8));
            }
            m_rooms[name].key = QString::fromLatin1(secret.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        }
        if (m_rooms.value(name).members.size() >= 2) {
            reject(socket, "Stanza piena: questa versione supporta due partecipanti.");
            socket->close(); return;
        }
        client.room = name;
        client.preferences = message.value("settings").toObject();
        auto &room = m_rooms[name];
        room.members.append(socket);
        send(socket, {{"type", "joined"}, {"id", client.id}, {"room", name}, {"key", room.key}});
        presence(room);
        return;
    }
    if (client.room.isEmpty()) {
        reject(socket, "Entra prima in una stanza.");
        return;
    }
    auto &room = m_rooms[client.room];
    if (type == "publish") {
        if (room.publisher || room.members.size() != 2) {
            reject(socket, "Attendi il secondo partecipante o la fine della "
                           "condivisione.");
            return;
        }
        room.publisher = socket;
        room.session = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QJsonObject settings = client.preferences;
        for (auto *member : room.members)
            for (const auto *key : {"fps", "width", "height", "bitrate"})
                settings[key] =
                    std::min(settings[key].toInt(),
                             m_clients[member].preferences[key].toInt());
        for (auto *member : room.members)
            send(member, {{"type", "session"},
                          {"session", room.session},
                          {"sender", member == socket},
                          {"audio", message.value("audio").toBool(false)},
                          {"settings", settings}});
        presence(room);
        return;
    }
    if (!room.publisher || message.value("session").toString() != room.session)
        return; // Ignore stale ICE.
    if (type == "stop") {
        stopSession(room);
        return;
    }
    if (type != "offer" && type != "answer" && type != "ice") {
        reject(socket, "Tipo di messaggio non valido.");
        return;
    }
    if ((type == "offer" && room.publisher != socket) ||
        (type == "answer" && room.publisher == socket)) {
        reject(socket, "Ruolo non valido per questo messaggio.");
        return;
    }
    if (type == "ice") {
        const int index = message.value("mline").toInt(-1);
        if (index < 0 || index > 8 ||
            message.value("candidate").toString().size() > 8192)
            return;
    } else if (message.value("sdp").toString().isEmpty() ||
               message.value("sdp").toString().size() > 100000)
        return;
    for (auto *member : room.members)
        if (member != socket)
            send(member, message);
}

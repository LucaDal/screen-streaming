#pragma once
#include <QHash>
#include <QList>
#include <QJsonObject>
#include <QString>
#include <QSslConfiguration>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QElapsedTimer>
#include "TurnService.h"

class SignalingServer final : public QObject
{
    Q_OBJECT
public:
    SignalingServer(QString token, QWebSocketServer::SslMode mode, QObject* parent = nullptr);
    ~SignalingServer() override;
    bool listen(const QHostAddress& address, quint16 port);
    quint16 port() const { return m_server.serverPort(); }
    QString errorString() const { return m_server.errorString(); }
    void setSslConfiguration(const QSslConfiguration& config) { m_server.setSslConfiguration(config); }
    void setTurnService(TurnService* service) { m_turn = service; }
private:
    struct Client { QString id; QString room; QJsonObject preferences; QElapsedTimer rate; int messages = 0; };
    struct Room { QString key; QList<QWebSocket*> members; QWebSocket* publisher = nullptr; QString session; };
    QWebSocketServer m_server;
    QString m_token;
    TurnService* m_turn = nullptr; // Owned by server_main; outlives this server.
    QHash<QWebSocket*, Client> m_clients;
    QHash<QString, Room> m_rooms;
    void accept();
    void handle(QWebSocket* socket, const QString& text);
    void remove(QWebSocket* socket);
    void presence(Room& room);
    void stopSession(Room& room);
    static void send(QWebSocket* socket, const QJsonObject& message);
    static void reject(QWebSocket* socket, const QString& message);
};

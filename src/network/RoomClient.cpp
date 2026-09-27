#include "RoomClient.h"
#include <QJsonDocument>
#include <QFile>
#include <QSslConfiguration>
#include <QSslCertificate>
#include <QHostAddress>
#include <QUrlQuery>

RoomClient::RoomClient(QObject* parent) : QObject(parent), m_peer(this) {
    m_socket.setMaxAllowedIncomingFrameSize(128 * 1024);
    m_socket.setMaxAllowedIncomingMessageSize(128 * 1024);
    m_timeout.setSingleShot(true); m_timeout.setInterval(20000);
    connect(&m_socket, &QWebSocket::connected, this, [this] { send(m_joinMessage); m_joinMessage = {}; });
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &RoomClient::receive);
    connect(&m_socket, &QWebSocket::disconnected, this, [this] {
        m_timeout.stop(); m_joined = false; m_connecting = false; m_participants = 0;
        resetSession(); emit changed(); emit statusChanged(tr("Fuori dalla stanza."));
    });
    connect(&m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        const auto error = tr("Signaling: %1").arg(m_socket.errorString()); leave(); emit errorOccurred(error);
    });
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        if (m_joined) stopSharing(); else leave();
        emit errorOccurred(tr("Tempo di connessione scaduto. Verifica server, firewall e TURN."));
    });
    connect(&m_peer, &WebRtcPeer::signalMessage, this, [this](QJsonObject message) {
        if (m_session.isEmpty()) return;
        message["session"] = m_session; send(message);
    });
    connect(&m_peer, &WebRtcPeer::frameReady, this, &RoomClient::remoteFrame);
    connect(&m_peer, &WebRtcPeer::connected, this, [this] {
        m_timeout.stop(); emit statusChanged(m_sender
            ? (m_sessionAudio ? tr("Stai condividendo schermo e audio del PC · connessione cifrata") : tr("Stai condividendo lo schermo · connessione cifrata"))
            : (m_sessionAudio ? tr("Stai guardando schermo e audio · connessione cifrata") : tr("Stai guardando lo schermo · connessione cifrata"))); emit transportConnected();
    });
    connect(&m_peer, &WebRtcPeer::errorOccurred, this, [this](const QString& error) {
        stopSharing(); emit errorOccurred(error);
    });
}
RoomClient::~RoomClient() { m_socket.disconnect(this); m_peer.disconnect(this); m_socket.abort(); m_peer.stop(); }
void RoomClient::send(QJsonObject message) {
    if (m_socket.state() == QAbstractSocket::ConnectedState)
        m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}
void RoomClient::join(const QUrl& url, const QString& room, const QString& token,
                      const VideoSettings& preferences, const WebRtcPeer::IceConfig& ice, const QString& caFile) {
    connectRoom(false, url, room, token, preferences, ice, caFile);
}
void RoomClient::create(const QUrl& url, const QString& serverKey, const VideoSettings& preferences,
                        const WebRtcPeer::IceConfig& ice) {
    connectRoom(true, url, {}, serverKey, preferences, ice, {});
}
void RoomClient::joinInvitation(const QString& invitation, const VideoSettings& preferences,
                                const WebRtcPeer::IceConfig& ice) {
    const QUrl link(invitation.trimmed());
    const QUrlQuery query(link);
    if (link.scheme() != "streamingapp" || link.host() != "join" || invitation.size() > 8192) {
        emit errorOccurred(tr("Incolla l’invito completo ricevuto dall’altra persona.")); return;
    }
    join(QUrl(query.queryItemValue("server", QUrl::FullyDecoded)),
         query.queryItemValue("room", QUrl::FullyDecoded), query.queryItemValue("key", QUrl::FullyDecoded), preferences, ice);
}
void RoomClient::connectRoom(bool create, const QUrl& url, const QString& room, const QString& token,
                            const VideoSettings& preferences, const WebRtcPeer::IceConfig& ice, const QString& caFile) {
    if (m_joined || m_connecting) return;
    // Cleartext signaling is only allowed on literal loopback, never a LAN address.
    const bool loopback = url.host() == "127.0.0.1" || url.host() == "::1" || url.host() == "localhost";
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
        || (url.scheme() != "wss" && !(url.scheme() == "ws" && loopback))) {
        emit errorOccurred(tr("Usa wss:// per la rete; ws:// è consentito solo su localhost.")); return;
    }
    if (token.size() < 16 || token.size() > 1024 || (!create && (room.isEmpty() || room.size() > 64))) {
        emit errorOccurred(tr("Verifica l’invito o configura la chiave del server (almeno 16 caratteri) per creare una stanza.")); return;
    }
    auto ssl = QSslConfiguration::defaultConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyPeer); ssl.setProtocol(QSsl::TlsV1_2OrLater);
    if (!caFile.isEmpty()) {
        QFile file(caFile);
        if (!file.open(QIODevice::ReadOnly)) { emit errorOccurred(tr("Impossibile leggere il certificato CA.")); return; }
        const auto certificates = QSslCertificate::fromData(file.readAll());
        if (certificates.isEmpty()) { emit errorOccurred(tr("Certificato CA non valido.")); return; }
        auto roots = ssl.caCertificates(); roots.append(certificates); ssl.setCaCertificates(roots);
    }
    m_socket.setSslConfiguration(ssl); // TLS errors are never ignored.
    m_ice = ice;
    m_joinMessage = {{"type", create ? "create" : "join"}, {"room", room}, {"token", token},
                    {"settings", QJsonObject{{"fps", preferences.fps}, {"width", preferences.maxSize.width()},
                        {"height", preferences.maxSize.height()}, {"bitrate", preferences.bitrateKbps}}}};
    m_connecting = true; m_timeout.start(); emit changed(); emit statusChanged(tr("Connessione alla stanza…"));
    m_socket.open(url);
}
void RoomClient::resetSession() {
    m_timeout.stop(); m_peer.stop(); m_session.clear(); m_requested = false; m_sender = false;
    emit sessionStopped(); emit changed();
}
void RoomClient::leave() {
    m_joinMessage = {}; m_socket.abort(); m_joined = false; m_connecting = false; m_participants = 0;
    resetSession(); emit changed();
}
void RoomClient::publish() {
    if (!m_joined || busy() || m_participants != 2) return;
    m_requested = true; send({{"type", "publish"}, {"audio", m_shareAudio}}); m_timeout.start(); emit changed();
}
void RoomClient::stopSharing() {
    if (!m_session.isEmpty()) send({{"type", "stop"}, {"session", m_session}});
    resetSession();
}
void RoomClient::receive(const QString& text) {
    const auto message = QJsonDocument::fromJson(text.toUtf8()).object();
    const auto type = message.value("type").toString();
    if (type == "joined") {
        m_joined = true; m_connecting = false; m_timeout.stop(); emit changed();
        QUrl invitation;
        invitation.setScheme("streamingapp"); invitation.setHost("join");
        QUrlQuery query;
        query.addQueryItem("server", m_socket.requestUrl().toString(QUrl::FullyEncoded));
        query.addQueryItem("room", message.value("room").toString());
        query.addQueryItem("key", message.value("key").toString());
        invitation.setQuery(query);
        emit invitationReady(invitation.toString(QUrl::FullyEncoded));
        emit statusChanged(tr("Stanza pronta. Condividi l’invito con l’altra persona."));
    } else if (type == "presence") {
        m_participants = message.value("peers").toInt(); emit changed();
        if (!busy()) emit statusChanged(m_participants == 2 ? tr("Due partecipanti: chiunque può condividere.") : tr("In attesa del secondo partecipante."));
    } else if (type == "error") {
        m_requested = false; m_timeout.stop(); emit changed(); emit errorOccurred(message.value("message").toString());
    } else if (type == "stopped") {
        resetSession(); emit statusChanged(tr("Condivisione terminata. Puoi avviarne una nuova."));
    } else if (type == "session") {
        resetSession();
        m_session = message.value("session").toString(); m_sender = message.value("sender").toBool();
        const auto settings = message.value("settings").toObject();
        const VideoSettings video{settings.value("fps").toInt(), {settings.value("width").toInt(), settings.value("height").toInt()}, settings.value("bitrate").toInt()};
        QString error;
        m_sessionAudio = message.value("audio").toBool();
        if (!m_peer.start(m_sender, m_ice, WebRtcPeer::AudioConfig{m_sessionAudio}, error)) { stopSharing(); emit errorOccurred(error); return; }
        m_timeout.start(); emit changed();
        emit statusChanged(tr("Collegamento WebRTC · %1p · %2 FPS · %3 Mbit/s")
                           .arg(video.maxSize.height()).arg(video.fps).arg(video.bitrateKbps / 1000));
        if (m_sender) emit captureRequested(video);
    } else if (!m_session.isEmpty() && message.value("session").toString() == m_session) {
        m_peer.receive(message);
    }
}

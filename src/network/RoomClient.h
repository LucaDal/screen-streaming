#pragma once
#include "WebRtcPeer.h"
#include <QTimer>
#include <QVideoFrame>
#include <QWebSocket>
#include <QUrl>

class RoomClient final : public QObject
{
    Q_OBJECT
public:
    explicit RoomClient(QObject* parent = nullptr);
    ~RoomClient() override;
    void join(const QUrl& url, const QString& room, const QString& token,
              const VideoSettings& preferences, const WebRtcPeer::IceConfig& ice, const QString& caFile = {});
    void create(const QUrl& url, const QString& serverKey, const VideoSettings& preferences,
                const WebRtcPeer::IceConfig& ice);
    void joinInvitation(const QString& invitation, const VideoSettings& preferences,
                        const WebRtcPeer::IceConfig& ice);
    void leave();
    void publish();
    void setShareAudio(bool enabled) { m_shareAudio = enabled; }
    void setVolume(double volume) { m_peer.setVolume(volume); }
    void stopSharing();
    bool joined() const { return m_joined; }
    bool connecting() const { return m_connecting; }
    bool busy() const { return m_requested || !m_session.isEmpty(); }
    bool sending() const { return !m_session.isEmpty() && m_sender; }
    int participants() const { return m_participants; }
    quint64 receivedFrames() const { return m_peer.receivedFrames(); }
    VideoPipeline::EncodedHandler encodedHandler() const { return m_peer.encodedHandler(); }

signals:
    void invitationReady(const QString& invitation);
    void changed();
    void statusChanged(const QString& status);
    void errorOccurred(const QString& error);
    void captureRequested(const VideoSettings& settings);
    void sessionStopped();
    void remoteFrame(const QVideoFrame& frame);
    void transportConnected();

private:
    QWebSocket m_socket;
    WebRtcPeer m_peer;
    QTimer m_timeout;
    QJsonObject m_joinMessage;
    WebRtcPeer::IceConfig m_ice;
    QString m_session;
    bool m_shareAudio = true;
    bool m_sessionAudio = false;
    bool m_joined = false, m_connecting = false, m_sender = false, m_requested = false;
    int m_participants = 0;
    void connectRoom(bool create, const QUrl& url, const QString& room, const QString& token,
                     const VideoSettings& preferences, const WebRtcPeer::IceConfig& ice, const QString& caFile);
    void send(QJsonObject message);
    void receive(const QString& text);
    void resetSession();
};

#pragma once
#include "VideoPipeline.h"
#include <QList>
#include <QObject>
#include <QJsonObject>
#include <QTimer>
#include <QVideoFrame>
#include <memory>

// One WebRTC connection. Qt owns signaling; GStreamer owns ICE/DTLS/SRTP.
class WebRtcPeer final : public QObject
{
    Q_OBJECT
public:
    struct IceConfig { QString stun; QString turn; bool relayOnly = false; };
    struct AudioConfig {
        bool enabled = false;
        // Explicit synthetic endpoints for integration tests, never selected by the UI.
        bool testTone = false;
        bool discardOutput = false;
    };
    struct MediaStats {
        quint64 audioBuffers = 0;
        qint64 videoPtsNs = -1, audioPtsNs = -1;
    };
    explicit WebRtcPeer(QObject* parent = nullptr);
    ~WebRtcPeer() override;
    bool start(bool sender, const IceConfig& ice, QString& error);
    bool start(bool sender, const IceConfig& ice, const AudioConfig& audio, QString& error);
    void setVolume(double volume);
    MediaStats mediaStats() const;
    void stop();
    void receive(const QJsonObject& message);
    VideoPipeline::EncodedHandler encodedHandler() const;
    quint64 receivedFrames() const;

signals:
    void signalMessage(const QJsonObject& message);
    void frameReady(const QVideoFrame& frame);
    void connected();
    void errorOccurred(const QString& message);

private:
    struct Context;
    std::shared_ptr<Context> m_context;
    QTimer m_poll;
    void poll();
    void createDescription(bool offer);
    void setDescription(bool local, const QString& type, const QString& sdp);
    double m_volume = 1.0;
    bool m_sender = false;
    bool m_remoteReady = false;
    bool m_remotePending = false;
    bool m_reportedConnected = false;
    QList<QJsonObject> m_pendingIce;
    struct _GstElement* m_pipeline = nullptr;
    struct _GstElement* m_rtc = nullptr;
};

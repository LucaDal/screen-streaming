#pragma once

#include "VideoPipeline.h"

#include <QElapsedTimer>
#include <QMediaCaptureSession>
#include <QObject>
#include <QPointer>
#include <QScreenCapture>
#include <QSize>
#include <QTimer>
#include <QVideoSink>

#include <memory>

class PortalSessionGuard;
class QScreen;
class QVideoSink;

class ScreenCaptureController final : public QObject
{
    Q_OBJECT

public:
    enum class State { Idle, Starting, Capturing, Error };
    Q_ENUM(State)

    explicit ScreenCaptureController(QVideoSink* previewSink, QObject* parent = nullptr);
    ~ScreenCaptureController() override;

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] bool usesPortal() const { return m_usesPortal; }

    void start(QScreen* screen, const VideoSettings& settings = {}, VideoPipeline::EncodedHandler handler = {});
    void stop();

signals:
    void stateChanged(ScreenCaptureController::State state);
    void statisticsChanged(double capturedFps, double decodedFps, QSize frameSize,
                           double latencyMs, quint64 skipped);
    void timingsChanged(double prepareMs, double encodeMs, double decodeMs, double outputMs);
    void errorOccurred(const QString& message);

private:
    void setState(State state);
    void resetStatistics();
    void fail(const QString& message);
    void createCaptureSession();
    void destroyCaptureSession();

#ifdef STREAMING_PORTAL_GUARD
    std::unique_ptr<PortalSessionGuard> m_portal;
#endif
    quint64 m_captureGeneration = 0;
    QVideoSink m_captureSink;
    std::unique_ptr<QScreenCapture> m_capture;
    std::unique_ptr<QMediaCaptureSession> m_session;
    VideoPipeline m_pipeline;
    QPointer<QVideoSink> m_previewSink;
    QTimer m_previewTimer;
    QTimer m_statisticsTimer;
    QElapsedTimer m_sampleClock;
    VideoPipeline::Snapshot m_latest;
    quint64 m_previousCaptured = 0;
    quint64 m_previousDecoded = 0;
    State m_state = State::Idle;
    const bool m_usesPortal;
};

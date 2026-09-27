#include "ScreenCaptureController.h"
#ifdef STREAMING_PORTAL_GUARD
#include "PortalSessionGuard.h"
#endif

#include <QGuiApplication>
#include <QScreen>
#include <QVideoFrame>
#include <QVideoSink>

ScreenCaptureController::ScreenCaptureController(QVideoSink* previewSink, QObject* parent)
    : QObject(parent),
      m_previewSink(previewSink),
      m_usesPortal(QGuiApplication::platformName().startsWith(QStringLiteral("wayland")))
{
#ifdef STREAMING_PORTAL_GUARD
    if (m_usesPortal) {
        m_portal = std::make_unique<PortalSessionGuard>();
        connect(m_portal.get(), &PortalSessionGuard::cancelled, this, [this] {
            const auto generation = m_captureGeneration;
            QTimer::singleShot(0, this, [this, generation] {
                if (generation == m_captureGeneration) stop();
            });
        });
    }
#endif
    m_previewTimer.setInterval(8);
    m_previewTimer.setTimerType(Qt::PreciseTimer);
    m_statisticsTimer.setInterval(1000);

    // This callback may run on the capture thread: only call the thread-safe
    // handoff. A queued connection could accumulate raw frames on the GUI.
    connect(&m_captureSink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame& frame) { m_pipeline.submit(frame); }, Qt::DirectConnection);

    connect(&m_previewTimer, &QTimer::timeout, this, [this] {
        m_latest = m_pipeline.takeLatest();
        if (!m_latest.error.isEmpty()) {
            const auto error = m_latest.error;
            fail(error);
            return;
        }
        if (m_latest.frame.isValid()) {
            if (m_previewSink) m_previewSink->setVideoFrame(m_latest.frame);
            m_latest.frame = {};
            setState(State::Capturing);
        }
    });

    connect(&m_statisticsTimer, &QTimer::timeout, this, [this] {
        const auto elapsedMs = m_sampleClock.restart();
        const double scale = elapsedMs > 0 ? 1000.0 / static_cast<double>(elapsedMs) : 0.0;
        emit statisticsChanged(static_cast<double>(m_latest.captured - m_previousCaptured) * scale,
                               static_cast<double>(m_latest.decoded - m_previousDecoded) * scale,
                               m_latest.size, m_latest.latencyMs, m_latest.skipped);
        m_previousCaptured = m_latest.captured;
        m_previousDecoded = m_latest.decoded;
        emit timingsChanged(m_latest.prepareMs, m_latest.encodeMs, m_latest.decodeMs, m_latest.outputMs);
    });

    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* screen) {
        if (!m_usesPortal && m_capture && m_capture->screen() == screen) {
            fail(tr("Il monitor condiviso è stato scollegato."));
            if (m_capture)
                m_capture->setScreen(nullptr);
        }
    });
}

ScreenCaptureController::~ScreenCaptureController()
{
    // Backend callbacks are queued; teardown never deletes their sender
    // while Qt is still executing a capture callback.
    m_captureSink.disconnect(this);
    m_previewTimer.stop();
    m_statisticsTimer.stop();
    destroyCaptureSession();
    m_pipeline.stop();
#ifdef STREAMING_PORTAL_GUARD
    if (m_portal) m_portal->stop();
#endif
}

void ScreenCaptureController::start(QScreen* screen, const VideoSettings& settings, VideoPipeline::EncodedHandler handler)
{
    if (m_state == State::Starting || m_state == State::Capturing)
        return;

    if (!m_usesPortal && !screen)
        screen = QGuiApplication::primaryScreen();
    if (!m_usesPortal && !screen) {
        fail(tr("Nessuno schermo disponibile."));
        return;
    }

    resetStatistics();
    QString error;
    if (!m_pipeline.start(settings, error, std::move(handler))) {
        fail(error);
        return;
    }
#ifdef STREAMING_PORTAL_GUARD
    if (m_portal && !m_portal->begin(error)) {
        fail(error);
        return;
    }
#endif
    createCaptureSession();
    setState(State::Starting);
    m_sampleClock.start();
    m_statisticsTimer.start();
    m_previewTimer.start();
    // Wayland requires the system portal; setScreen() is unsupported there.
    if (!m_usesPortal)
        m_capture->setScreen(screen);
    // Every sharing attempt owns a fresh capture object.
    m_session->setScreenCapture(m_capture.get());
    m_session->setVideoSink(&m_captureSink);
    m_capture->start();
}

void ScreenCaptureController::stop()
{
    m_previewTimer.stop();
    m_statisticsTimer.stop();
    destroyCaptureSession();
    m_pipeline.stop();
#ifdef STREAMING_PORTAL_GUARD
    if (m_portal) m_portal->stop();
#endif
    m_captureSink.setVideoFrame(QVideoFrame{});
    resetStatistics();
    if (m_previewSink) m_previewSink->setVideoFrame(QVideoFrame{});
    // Only advertise Idle after the old session has been released. A new
    // start must never reuse it or be destroyed by an older deferred stop.
    setState(State::Idle);
}

void ScreenCaptureController::createCaptureSession()
{
    if (m_capture || m_session)
        return;
    m_capture = std::make_unique<QScreenCapture>();
    m_session = std::make_unique<QMediaCaptureSession>();
    m_session->setScreenCapture(m_capture.get());
    m_session->setVideoSink(&m_captureSink);
    const auto generation = ++m_captureGeneration;
    connect(m_capture.get(), &QScreenCapture::activeChanged, this, [this, generation](bool active) {
        if (generation != m_captureGeneration) return;
        if (!active && (m_state == State::Starting || m_state == State::Capturing)) stop();
    }, Qt::QueuedConnection);
    connect(m_capture.get(), &QScreenCapture::errorOccurred, this,
            [this, generation](QScreenCapture::Error error, const QString& message) {
        if (generation != m_captureGeneration) return;
        if (error != QScreenCapture::NoError) {
#ifdef STREAMING_PORTAL_GUARD
            if (m_portal) m_portal->creationFailed();
#endif
            fail(message.isEmpty() ? tr("La cattura dello schermo non è disponibile.") : message);
        }
    }, Qt::QueuedConnection);
}

void ScreenCaptureController::destroyCaptureSession()
{
    ++m_captureGeneration;
    if (m_capture) {
        m_capture->disconnect(this);
        m_capture->stop();
    }
    if (m_session) {
        m_session->setScreenCapture(nullptr);
        m_session->setVideoSink(nullptr);
    }
    m_session.reset();
    m_capture.reset();
}

void ScreenCaptureController::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

void ScreenCaptureController::resetStatistics()
{
    m_statisticsTimer.stop();
    m_sampleClock.invalidate();
    m_latest = {};
    m_previousCaptured = 0;
    m_previousDecoded = 0;
    emit statisticsChanged(0.0, 0.0, {}, 0.0, 0);
    emit timingsChanged(0.0, 0.0, 0.0, 0.0);
}

void ScreenCaptureController::fail(const QString& message)
{
    stop();
    setState(State::Error);
    emit errorOccurred(message);
}

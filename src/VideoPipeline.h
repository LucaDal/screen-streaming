#pragma once

#include <QSize>
#include <QString>
#include <QVideoFrame>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>

typedef struct _GstSample GstSample;

struct VideoSettings
{
    int fps = 60;
    QSize maxSize{1920, 1080};
    int bitrateKbps = 12000;
};

// A bounded handoff between Qt capture, the codec worker and the GUI.
// start/stop belong to the owner thread; submit/takeLatest are thread-safe.
class VideoPipeline final
{
public:
    using EncodedHandler = std::function<void(GstSample*, std::chrono::steady_clock::time_point)>;
    struct Snapshot
    {
        QVideoFrame frame;
        QString error;
        quint64 captured = 0;
        quint64 decoded = 0;
        quint64 skipped = 0;
        double latencyMs = 0;
        double prepareMs = 0;
        double codecMs = 0;
        double encodeMs = 0;
        double decodeMs = 0;
        double outputMs = 0;
        QSize size;
    };

    VideoPipeline() = default;
    ~VideoPipeline();
    VideoPipeline(const VideoPipeline&) = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;

    bool start(const VideoSettings& settings, QString& error, EncodedHandler handler = {});
    void stop();
    void submit(const QVideoFrame& frame);
    Snapshot takeLatest();

private:
    using Clock = std::chrono::steady_clock;
    void run(VideoSettings settings, EncodedHandler handler);
    void reportError(const QString& error);
    bool stopping();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_worker;
    bool m_stopping = true;
    QVideoFrame m_pending;
    Clock::time_point m_receivedAt;
    Snapshot m_result;
};

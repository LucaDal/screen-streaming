#include "VideoPipeline.h"

#include <QGuiApplication>
#include <QImage>
#include <QVideoFrameFormat>

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

QVideoFrame makeFrame(QSize size, QVideoFrameFormat::PixelFormat format = QVideoFrameFormat::Format_RGBA8888)
{
    QVideoFrame frame(QVideoFrameFormat(size, format));
    require(frame.map(QVideoFrame::WriteOnly), "Cannot map synthetic input");
    for (int y = 0; y < size.height(); ++y) {
        auto* row = frame.bits(0) + y * frame.bytesPerLine(0);
        for (int x = 0; x < size.width(); ++x) {
            const int red = format == QVideoFrameFormat::Format_BGRA8888 ? 2 : 0;
            const int blue = format == QVideoFrameFormat::Format_BGRA8888 ? 0 : 2;
            row[4 * x + red] = x < size.width() / 2 ? 220 : 20;
            row[4 * x + 1] = y < size.height() / 2 ? 180 : 30;
            row[4 * x + blue] = 70;
            row[4 * x + 3] = 255;
        }
    }
    frame.unmap();
    return frame;
}

VideoPipeline::Snapshot awaitFrame(VideoPipeline& pipeline)
{
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline) {
        auto result = pipeline.takeLatest();
        if (!result.error.isEmpty())
            throw std::runtime_error(result.error.toStdString());
        if (result.frame.isValid())
            return result;
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("Timed out waiting for decoded frame");
}

void checkPixels(const QVideoFrame& frame)
{
    const auto image = frame.toImage();
    require(!image.isNull(), "Decoded image is empty");
    for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 2; ++column) {
            const QColor pixel = image.pixelColor(image.width() * (2 * column + 1) / 4,
                                                  image.height() * (2 * row + 1) / 4);
            if (std::abs(pixel.red() - (column == 0 ? 220 : 20)) >= 20
                || std::abs(pixel.green() - (row == 0 ? 180 : 30)) >= 20
                || std::abs(pixel.blue() - 70) >= 20)
                std::cerr << "Pixel at quadrant " << column << ',' << row << ": "
                          << pixel.red() << ',' << pixel.green() << ',' << pixel.blue()
                          << "; color space=" << frame.surfaceFormat().colorSpace()
                          << ", range=" << frame.surfaceFormat().colorRange() << '\n';
            require(std::abs(pixel.red() - (column == 0 ? 220 : 20)) < 20,
                    "H.264 roundtrip changed red channel or image orientation");
            require(std::abs(pixel.green() - (row == 0 ? 180 : 30)) < 20,
                    "H.264 roundtrip changed green channel or image orientation");
            require(std::abs(pixel.blue() - 70) < 20, "H.264 roundtrip changed blue channel");
        }
    }
}

void roundtripAndRestart()
{
    VideoPipeline pipeline;
    for (int fps : {30, 60}) {
        QString error;
        require(pipeline.start({fps, QSize(1280, 720), 8000}, error), qPrintable(error));
        pipeline.submit(makeFrame(QSize(1920, 1080)));
        auto result = awaitFrame(pipeline);
        require(result.size == QSize(1280, 720), "1080p input must scale to 720p");
        require(result.decoded == 1 && result.captured == 1, "Wrong initial frame counters");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        require(result.frame.surfaceFormat().streamFrameRate() == fps, "Negotiated framerate differs from settings");
#else
        require(result.frame.surfaceFormat().frameRate() == fps, "Negotiated framerate differs from settings");
#endif
        require(result.latencyMs > 0, "Latency must include codec work");
        require(result.prepareMs >= 0 && result.encodeMs > 0 && result.decodeMs > 0 && result.outputMs >= 0,
                "Processing timings must be valid for a decoded frame");
        checkPixels(result.frame);

        // Renegotiate for a different aspect ratio, including odd input dimensions.
        pipeline.submit(makeFrame(QSize(301, 601)));
        result = awaitFrame(pipeline);
        require(result.size == QSize(300, 600), "Portrait source must keep proportions and even dimensions");
        checkPixels(result.frame);
        pipeline.submit(makeFrame(QSize(1200, 2400)));
        result = awaitFrame(pipeline);
        require(result.size == QSize(360, 720), "Portrait downscale must fit the selected bounds");
        checkPixels(result.frame);
        // Verify byte order and renegotiation with a different input format.
        pipeline.submit(makeFrame(QSize(318, 182), QVideoFrameFormat::Format_BGRA8888));
        result = awaitFrame(pipeline);
        require(result.size == QSize(318, 182), "BGRA input size changed");
        checkPixels(result.frame);
        pipeline.stop();
        pipeline.submit(makeFrame(QSize(32, 32)));
        result = pipeline.takeLatest();
        require(!result.frame.isValid() && result.captured == 0 && result.decoded == 0,
                "Stop must clear results and reject late input");
    }
}

void rateLimit(int fps)
{
    VideoPipeline pipeline;
    QString error;
    require(pipeline.start({fps, QSize(320, 180), 2000}, error), qPrintable(error));
    const auto frame = makeFrame(QSize(320, 180));
    const auto start = Clock::now();
    const auto deadline = start + 1200ms;
    VideoPipeline::Snapshot last;
    while (Clock::now() < deadline) {
        pipeline.submit(frame); // Deliberately faster than both selected frame rates.
        last = pipeline.takeLatest();
        require(last.error.isEmpty(), qPrintable(last.error));
        std::this_thread::sleep_for(1ms);
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    require(last.decoded > 0, "No output under load");
    require(last.decoded <= static_cast<quint64>(std::ceil(elapsed * fps)) + 1,
            "Output exceeded the selected FPS limit");
    require(last.skipped > 0 && last.captured > last.decoded,
            "Excess input must be dropped rather than queued");
    std::cout << fps << " FPS target: " << last.decoded / elapsed << " decoded FPS, "
              << last.skipped << " input frames skipped\n";
    pipeline.stop();
}

void retainedPreviewLifetime()
{
    QVideoFrame retained;
    {
        VideoPipeline pipeline;
        QString error;
        const bool started = pipeline.start({60, QSize(320, 180), 4000}, error);
        require(started, qPrintable(error));
        // Submit a temporary: its backing pixels must outlive this expression.
        pipeline.submit(makeFrame(QSize(318, 178), QVideoFrameFormat::Format_BGRA8888));
        retained = awaitFrame(pipeline).frame;
        pipeline.stop();
        const bool restarted = pipeline.start({30, QSize(320, 180), 4000}, error);
        require(restarted, qPrintable(error));
        pipeline.submit(makeFrame(QSize(160, 90)));
        require(awaitFrame(pipeline).size == QSize(160, 90), "Restart failed with an outstanding preview");
    }
    // Read only after stop, restart and destruction, before toImage can cache it.
    for (int i = 0; i < 3; ++i) {
        require(retained.map(QVideoFrame::ReadOnly), "Retained preview cannot be mapped");
        require(retained.bits(0) && retained.bytesPerLine(0) >= 318 * 4,
                "Retained preview has invalid pixel storage");
        retained.unmap();
    }
    checkPixels(retained);
}

void invalidSettingsAndEarlyStop()
{
    VideoPipeline pipeline;
    QString error;
    for (const VideoSettings settings : {VideoSettings{0, {1920, 1080}, 12000},
                                        VideoSettings{60, {0, 1080}, 12000},
                                        VideoSettings{60, {1920, 1080}, 0}}) {
        require(!pipeline.start(settings, error) && !error.isEmpty(), "Invalid settings accepted");
    }
    for (int i = 0; i < 3; ++i) {
        require(pipeline.start({}, error), qPrintable(error));
        pipeline.stop(); // No input yet: must not wait indefinitely for a sample.
    }
}

void benchmark()
{
    for (QSize size : {QSize(1920, 1080), QSize(2560, 1440), QSize(3840, 2160)}) {
        // Prebuild changing frames so input generation does not limit the codec.
        QList<QVideoFrame> frames;
        for (int phase = 0; phase < 8; ++phase) {
            auto frame = makeFrame(size);
            require(frame.map(QVideoFrame::WriteOnly), "Cannot map benchmark frame");
            for (int y = 0; y < size.height(); ++y) {
                auto* row = frame.bits(0) + y * frame.bytesPerLine(0);
                for (int x = 0; x < size.width(); ++x) {
                    row[x * 4] = static_cast<unsigned char>((x / 8 + phase * 10) % 256);
                    row[x * 4 + 1] = static_cast<unsigned char>((y / 8 + phase * 10) % 256);
                }
            }
            frame.unmap();
            frames.append(frame);
        }
        VideoPipeline pipeline;
        QString error;
        const bool started = pipeline.start({}, error);
        require(started, qPrintable(error));
        const auto start = Clock::now();
        quint64 count = 0;
        double prepare = 0, encode = 0, decode = 0, output = 0;
        VideoPipeline::Snapshot last;
        int phase = 0;
        while (Clock::now() - start < 3s) {
            pipeline.submit(frames.at(phase++ % frames.size()));
            last = pipeline.takeLatest();
            require(last.error.isEmpty(), qPrintable(last.error));
            if (last.frame.isValid()) {
                ++count;
                prepare += last.prepareMs;
                encode += last.encodeMs;
                decode += last.decodeMs;
                output += last.outputMs;
            }
            std::this_thread::sleep_for(4ms);
        }
        require(count > 0, "Benchmark produced no frames");
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        std::cout << size.width() << 'x' << size.height() << " -> 1080p: "
                  << last.decoded / seconds << " FPS; prepare=" << prepare / count
                  << " ms, encode=" << encode / count << " ms, decode=" << decode / count
                  << " ms, output=" << output / count << " ms\n";
    }
}
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    try {
        if (app.arguments().contains(QStringLiteral("--missing-plugins"))) {
            VideoPipeline pipeline;
            QString error;
            require(!pipeline.start({}, error) && error.contains(QStringLiteral("Plugin GStreamer mancante")),
                    "Missing plugins must produce an actionable error");
        } else if (app.arguments().contains(QStringLiteral("--benchmark"))) {
            benchmark();
        } else {
            invalidSettingsAndEarlyStop();
            roundtripAndRestart();
            retainedPreviewLifetime();
            rateLimit(30);
            rateLimit(60);
        }
        std::cout << "PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

#include "VideoPipeline.h"
#include "GstFrameBridge.h"

#include <QImage>
#include <QVideoFrameFormat>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAbstractVideoBuffer>
#endif

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <utility>

namespace {
// GStreamer references use RAII even on error paths.
struct GstUnref {
    void operator()(GstElement* p) const { gst_object_unref(p); }
    void operator()(GstBus* p) const { gst_object_unref(p); }
    void operator()(GstSample* p) const { gst_sample_unref(p); }
    void operator()(GstCaps* p) const { gst_caps_unref(p); }
    void operator()(GstMessage* p) const { gst_message_unref(p); }
    void operator()(GstPad* p) const { gst_object_unref(p); }
};
template<typename T> using GstPtr = std::unique_ptr<T, GstUnref>;

struct PipelineOwner {
    GstElement* value = nullptr;
    ~PipelineOwner() {
        if (value) {
            gst_element_set_state(value, GST_STATE_NULL);
            gst_object_unref(value);
        }
    }
};

QString busError(GstBus* bus)
{
    GstPtr<GstMessage> message(gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR));
    if (!message)
        return {};
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message.get(), &error, &debug);
    const QString text = error ? QString::fromUtf8(error->message)
                               : QStringLiteral("Errore nella pipeline video.");
    g_clear_error(&error);
    g_free(debug);
    return text;
}

QSize outputSize(QSize input, QSize maximum)
{
    if (input.width() > maximum.width() || input.height() > maximum.height())
        input.scale(maximum, Qt::KeepAspectRatio);
    // I420 needs even dimensions. Keep aspect ratio within a rounding pixel.
    return {std::max(2, input.width() & ~1), std::max(2, input.height() & ~1)};
}

const char* imageFormat(QImage& image)
{
    // QImage's ARGB32/RGB32 names describe words, GStreamer names describe bytes.
    switch (image.format()) {
    case QImage::Format_RGBA8888: return "RGBA";
    case QImage::Format_RGBX8888: return "RGBx";
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    case QImage::Format_ARGB32: return "BGRA";
    case QImage::Format_RGB32: return "BGRx";
#else
    case QImage::Format_ARGB32: return "ARGB";
    case QImage::Format_RGB32: return "xRGB";
#endif
    default:
        image = image.convertToFormat(QImage::Format_RGBA8888);
        return "RGBA";
    }
}

struct MappedCaptureFrame {
    explicit MappedCaptureFrame(const QVideoFrame& source) : frame(source) {}
    QVideoFrame frame;
    bool mapped = false;
    ~MappedCaptureFrame() { if (mapped) frame.unmap(); }
};

QImage captureImage(const QVideoFrame& frame)
{
    const auto format = QVideoFrameFormat::imageFormatFromPixelFormat(frame.pixelFormat());
    if (format != QImage::Format_Invalid
        && frame.surfaceFormat().scanLineDirection() == QVideoFrameFormat::TopToBottom) {
        auto mapped = std::make_unique<MappedCaptureFrame>(frame);
        mapped->mapped = mapped->frame.map(QVideoFrame::ReadOnly);
        if (mapped->mapped) {
            // Read-only view: the cleanup retains the mapped capture frame until
            // scaling or GStreamer releases the last reference to this image.
            QImage image(static_cast<const uchar*>(mapped->frame.bits(0)), frame.width(), frame.height(),
                         mapped->frame.bytesPerLine(0), format,
                         [](void* data) { delete static_cast<MappedCaptureFrame*>(data); }, mapped.get());
            mapped.release();
            return image;
        }
    }
    return frame.toImage();
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
class GstPreviewBuffer final : public QAbstractVideoBuffer
{
public:
    GstPreviewBuffer(GstSample* sample, GstVideoInfo info, QVideoFrameFormat format)
        : m_sample(gst_sample_ref(sample)), m_info(info), m_format(std::move(format)) {}
    ~GstPreviewBuffer() override { unmap(); }

    QVideoFrameFormat format() const override { return m_format; }

    MapData map(QVideoFrame::MapMode mode) override
    {
        // The preview is read-only. Never write into GStreamer's buffer pool.
        if (mode != QVideoFrame::ReadOnly)
            return {};
        if (!m_mapped) {
            m_mapped = gst_video_frame_map(&m_frame, &m_info,
                                          gst_sample_get_buffer(m_sample.get()), GST_MAP_READ);
            if (!m_mapped)
                return {};
        }
        const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&m_frame, 0);
        if (stride < m_format.frameWidth() * 4) {
            unmap();
            return {};
        }
        MapData result;
        result.planeCount = 1;
        result.bytesPerLine[0] = stride;
        result.data[0] = static_cast<uchar*>(GST_VIDEO_FRAME_PLANE_DATA(&m_frame, 0));
        result.dataSize[0] = stride * (m_format.frameHeight() - 1) + m_format.frameWidth() * 4;
        return result;
    }

    void unmap() override
    {
        if (m_mapped) {
            gst_video_frame_unmap(&m_frame);
            m_mapped = false;
        }
    }

private:
    GstPtr<GstSample> m_sample;
    GstVideoInfo m_info;
    QVideoFrameFormat m_format;
    GstVideoFrame m_frame{};
    bool m_mapped = false;
};
#endif

QVideoFrame decodedFrame(GstSample* sample)
{
    GstVideoInfo info;
    gst_video_info_init(&info);
    if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample))
        || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGBA)
        return {};

    QVideoFrameFormat format(
        QSize(GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info)),
        QVideoFrameFormat::Format_RGBA8888);
    if (GST_VIDEO_INFO_FPS_D(&info) > 0) {
        const double fps = static_cast<double>(GST_VIDEO_INFO_FPS_N(&info)) / GST_VIDEO_INFO_FPS_D(&info);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        format.setStreamFrameRate(fps);
#else
        format.setFrameRate(fps);
#endif
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Retain the decoded sample instead of allocating and copying an RGBA frame.
    QVideoFrame result(std::make_unique<GstPreviewBuffer>(sample, info, format));
    if (!result.map(QVideoFrame::ReadOnly))
        return {};
    result.unmap();
    return result;
#else
    // Qt 6.5–6.7 have no public custom-buffer API: retain the compatible copy path.
    GstVideoFrame source{};
    if (!gst_video_frame_map(&source, &info, gst_sample_get_buffer(sample), GST_MAP_READ))
        return {};
    QVideoFrame result(format);
    if (!result.map(QVideoFrame::WriteOnly)) {
        gst_video_frame_unmap(&source);
        return {};
    }
    const auto* pixels = static_cast<const unsigned char*>(GST_VIDEO_FRAME_PLANE_DATA(&source, 0));
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&source, 0);
    for (int y = 0; y < result.height(); ++y)
        std::memcpy(result.bits(0) + y * result.bytesPerLine(0), pixels + y * stride,
                    static_cast<std::size_t>(result.width()) * 4);
    result.unmap();
    gst_video_frame_unmap(&source);
    return result;
#endif
}
} // namespace

QVideoFrame videoFrameFromGstSample(GstSample* sample) { return decodedFrame(sample); }

VideoPipeline::~VideoPipeline()
{
    stop();
}

bool VideoPipeline::start(const VideoSettings& settings, QString& error, EncodedHandler handler)
{
    stop();
    error.clear();
    if ((settings.fps != 30 && settings.fps != 60)
        || settings.maxSize.width() < 2 || settings.maxSize.height() < 2
        || settings.maxSize.width() > 3840 || settings.maxSize.height() > 2160
        || settings.bitrateKbps < 1000 || settings.bitrateKbps > 50000) {
        error = QStringLiteral("Parametri video non validi (30/60 FPS, bitrate 1–50 Mbit/s).");
        return false;
    }

    GError* initError = nullptr;
    if (!gst_init_check(nullptr, nullptr, &initError)) {
        error = initError ? QString::fromUtf8(initError->message)
                          : QStringLiteral("Impossibile inizializzare GStreamer.");
        g_clear_error(&initError);
        return false;
    }
    for (const char* name : {"appsrc", "videoconvert", "x264enc", "h264parse", "avdec_h264", "appsink"}) {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (!factory) {
            error = QStringLiteral("Plugin GStreamer mancante: %1. Consulta i requisiti nel README.")
                        .arg(QString::fromUtf8(name));
            return false;
        }
        gst_object_unref(factory);
    }

    {
        const std::lock_guard lock(m_mutex);
        m_stopping = false;
        m_result = {};
    }
    m_worker = std::thread(&VideoPipeline::run, this, settings, std::move(handler));
    return true;
}

void VideoPipeline::stop()
{
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_pending = {};
    }
    m_wake.notify_all();
    if (m_worker.joinable())
        m_worker.join();
    const std::lock_guard lock(m_mutex);
    m_result = {};
}

void VideoPipeline::submit(const QVideoFrame& frame)
{
    if (!frame.isValid())
        return;
    {
        const std::lock_guard lock(m_mutex);
        if (m_stopping)
            return;
        ++m_result.captured;
        if (m_pending.isValid())
            ++m_result.skipped;
        m_pending = frame;
        m_receivedAt = Clock::now();
    }
    m_wake.notify_one();
}

VideoPipeline::Snapshot VideoPipeline::takeLatest()
{
    const std::lock_guard lock(m_mutex);
    Snapshot result = m_result;
    m_result.frame = {};
    return result;
}

bool VideoPipeline::stopping()
{
    const std::lock_guard lock(m_mutex);
    return m_stopping;
}

void VideoPipeline::reportError(const QString& error)
{
    const std::lock_guard lock(m_mutex);
    m_result.error = error;
    m_stopping = true;
    m_pending = {};
}

void VideoPipeline::run(VideoSettings settings, EncodedHandler handler)
{
    // Declared before PipelineOwner: the pad callback stops before this dies.
    std::atomic<Clock::duration::rep> encodedAt{0};
    // Only one frame is in flight. Preserve capture time independently of
    // x264's timestamp offset and time spent scaling/encoding.
    std::atomic<Clock::duration::rep> capturedAt{0};
    struct Delivery { EncodedHandler& handler; std::atomic<Clock::duration::rep>& captured; };
    Delivery delivery{handler, capturedAt};
    const QByteArray description = QStringLiteral(
        "appsrc name=input is-live=true format=time block=false max-buffers=1 max-bytes=0 "
        "! videoconvert n-threads=4 ! video/x-raw,format=I420 "
        "! x264enc name=encoder tune=zerolatency speed-preset=veryfast bitrate=%1 key-int-max=%2 "
        "! h264parse name=encoded ! video/x-h264,stream-format=byte-stream,alignment=au "
        "! avdec_h264 max-threads=4 thread-type=slice "
        "! videoconvert n-threads=4 ! video/x-raw,format=RGBA "
        "! appsink name=output sync=false max-buffers=1 drop=true enable-last-sample=false")
        .arg(settings.bitrateKbps).arg(settings.fps * 2).toUtf8();
    GError* parseError = nullptr;
    PipelineOwner pipeline{gst_parse_launch(description.constData(), &parseError)};
    if (parseError || !pipeline.value) {
        reportError(parseError ? QString::fromUtf8(parseError->message)
                               : QStringLiteral("Creazione della pipeline fallita."));
        g_clear_error(&parseError);
        return;
    }
    GstPtr<GstElement> input(gst_bin_get_by_name(GST_BIN(pipeline.value), "input"));
    GstPtr<GstElement> output(gst_bin_get_by_name(GST_BIN(pipeline.value), "output"));
    GstPtr<GstElement> encoder(gst_bin_get_by_name(GST_BIN(pipeline.value), "encoder"));
    GstPtr<GstPad> encodedPad(gst_element_get_static_pad(encoder.get(), "src"));
    gst_pad_add_probe(encodedPad.get(), GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad*, GstPadProbeInfo*, gpointer data) {
            auto* timestamp = static_cast<std::atomic<Clock::duration::rep>*>(data);
            timestamp->store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
            return GST_PAD_PROBE_OK;
        }, &encodedAt, nullptr);
    GstPtr<GstBus> bus(gst_element_get_bus(pipeline.value));
    GstPtr<GstElement> parsed(gst_bin_get_by_name(GST_BIN(pipeline.value), "encoded"));
    GstPtr<GstPad> parsedPad(gst_element_get_static_pad(parsed.get(), "src"));
    if (handler) {
        gst_pad_add_probe(parsedPad.get(), GST_PAD_PROBE_TYPE_BUFFER,
            [](GstPad* pad, GstPadProbeInfo* probe, gpointer data) {
                auto* callback = static_cast<Delivery*>(data);
                GstCaps* caps = gst_pad_get_current_caps(pad);
                GstSample* sample = gst_sample_new(GST_PAD_PROBE_INFO_BUFFER(probe), caps, nullptr, nullptr);
                callback->handler(sample, Clock::time_point{Clock::duration{callback->captured.load()}});
                gst_sample_unref(sample);
                if (caps) gst_caps_unref(caps);
                return GST_PAD_PROBE_OK;
            }, &delivery, nullptr);
    }
    if (!input || !output || gst_element_set_state(pipeline.value, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        const auto error = busError(bus.get());
        reportError(error.isEmpty() ? QStringLiteral("Avvio della pipeline fallito.") : error);
        return;
    }

    const auto period = std::chrono::nanoseconds(GST_SECOND / settings.fps);
    const auto origin = Clock::now();
    auto nextFrame = origin;
    QSize negotiatedSize;
    QByteArray negotiatedFormat;
    while (!stopping()) {
        QVideoFrame frame;
        Clock::time_point receivedAt;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait_until(lock, nextFrame, [this] { return m_stopping; });
            // Wake periodically even on a static desktop to check the bus.
            m_wake.wait_for(lock, std::chrono::milliseconds(50),
                            [this] { return m_stopping || m_pending.isValid(); });
            if (m_stopping)
                break;
            frame = std::exchange(m_pending, {});
            receivedAt = m_receivedAt;
        }
        const auto error = busError(bus.get());
        if (!error.isEmpty()) {
            reportError(error);
            return;
        }
        if (!frame.isValid())
            continue;
        const auto started = Clock::now();
        nextFrame = started + period;

        // Mapping, resizing and codec work happen outside the GUI thread.
        // Qt's converted image has an efficient smooth-scaling path. Use the
        // mapped view when no scaling is needed, without slowing down 4K input.
        const bool needsScaling = frame.size() != outputSize(frame.size(), settings.maxSize);
        QImage image = needsScaling ? frame.toImage() : captureImage(frame);
        frame = {};
        if (image.isNull()) {
            reportError(QStringLiteral("Impossibile leggere i pixel del frame catturato."));
            return;
        }
        const QSize size = outputSize(image.size(), settings.maxSize);
        if (image.size() != size)
            image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const char* format = imageFormat(image);
        if (image.isNull()) {
            reportError(QStringLiteral("Impossibile preparare il frame video."));
            return;
        }
        if (size != negotiatedSize || negotiatedFormat != format) {
            GstPtr<GstCaps> caps(gst_caps_new_simple("video/x-raw",
                "format", G_TYPE_STRING, format, "width", G_TYPE_INT, size.width(),
                "height", G_TYPE_INT, size.height(), "framerate", GST_TYPE_FRACTION, settings.fps, 1,
                "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, nullptr));
            gst_app_src_set_caps(GST_APP_SRC(input.get()), caps.get());
            negotiatedSize = size;
            negotiatedFormat = format;
        }
        // Retain QImage storage until GStreamer releases it, avoiding an entire
        // frame allocation/copy. Read-only memory prevents downstream mutation.
        auto* retainedImage = new QImage(std::move(image));
        const auto bytes = static_cast<gsize>(retainedImage->sizeInBytes());
        GstBuffer* buffer = gst_buffer_new_wrapped_full(GST_MEMORY_FLAG_READONLY,
            const_cast<uchar*>(retainedImage->constBits()), bytes, 0, bytes, retainedImage,
            [](gpointer data) { delete static_cast<QImage*>(data); });
        if (!buffer) {
            delete retainedImage;
            reportError(QStringLiteral("Impossibile allocare il buffer video."));
            return;
        }
        gsize offsets[GST_VIDEO_MAX_PLANES]{};
        // Output width is bounded to 3840 pixels by settings validation.
        gint strides[GST_VIDEO_MAX_PLANES]{static_cast<gint>(retainedImage->bytesPerLine()), 0, 0, 0};
        gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE,
            gst_video_format_from_string(format), size.width(), size.height(), 1, offsets, strides);
        GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(started - origin).count());
        GST_BUFFER_DURATION(buffer) = static_cast<GstClockTime>(period.count());
        capturedAt.store(receivedAt.time_since_epoch().count());
        const auto prepared = Clock::now();
        // push_buffer takes ownership. One frame is in flight at a time.
        if (gst_app_src_push_buffer(GST_APP_SRC(input.get()), buffer) != GST_FLOW_OK) {
            reportError(QStringLiteral("La pipeline non accetta nuovi frame."));
            return;
        }

        GstPtr<GstSample> sample;
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!stopping() && !sample) {
            sample.reset(gst_app_sink_try_pull_sample(GST_APP_SINK(output.get()), 20 * GST_MSECOND));
            const auto decodeError = busError(bus.get());
            if (!decodeError.isEmpty()) {
                reportError(decodeError);
                return;
            }
            if (!sample && Clock::now() >= deadline) {
                reportError(QStringLiteral("Nessun frame decodificato entro 3 secondi."));
                return;
            }
        }
        if (stopping())
            break;
        const auto codecDone = Clock::now();
        auto decoded = decodedFrame(sample.get());
        if (!decoded.isValid()) {
            reportError(QStringLiteral("Il frame decodificato non è leggibile."));
            return;
        }
        const double latency = std::chrono::duration<double, std::milli>(Clock::now() - receivedAt).count();
        const std::lock_guard lock(m_mutex);
        m_result.frame = std::move(decoded);
        m_result.size = size;
        m_result.latencyMs = latency;
        m_result.prepareMs = std::chrono::duration<double, std::milli>(prepared - started).count();
        m_result.codecMs = std::chrono::duration<double, std::milli>(codecDone - prepared).count();
        const Clock::time_point encoded{Clock::duration{encodedAt.load(std::memory_order_relaxed)}};
        m_result.encodeMs = std::chrono::duration<double, std::milli>(encoded - prepared).count();
        m_result.decodeMs = std::chrono::duration<double, std::milli>(codecDone - encoded).count();
        m_result.outputMs = std::chrono::duration<double, std::milli>(Clock::now() - codecDone).count();
        ++m_result.decoded;
    }
}

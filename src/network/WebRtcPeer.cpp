#include "WebRtcPeer.h"
#include "GstFrameBridge.h"
#include <deque>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <algorithm>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <mutex>
#include <utility>

struct WebRtcPeer::Context {
    std::mutex mutex;
    bool active = true;
    bool connected = false;
    bool needKeyframe = true;
    GstClockTime runningOrigin = 0;
    std::chrono::steady_clock::time_point steadyOrigin;
    MediaStats stats;
    GstElement *source =
        nullptr; // Owned reference, shared with the capture callback.
    GstPad *decoderSink = nullptr;
    GstPad *audioSink = nullptr;
    GstElement *audioGate = nullptr;
    std::deque<QJsonObject> events;
    QVideoFrame latest;
    quint64 frames = 0;
    ~Context() {
        if (source)
            gst_object_unref(source);
        if (decoderSink)
            gst_object_unref(decoderSink);
        if (audioSink) gst_object_unref(audioSink);
        if (audioGate) gst_object_unref(audioGate);
    }
    void post(QJsonObject event) {
        const std::lock_guard lock(mutex);
        if (active && events.size() < 256)
            events.push_back(std::move(event));
    }
    struct PromiseData {
        std::shared_ptr<Context> context;
        QString action, type, sdp;
    };
    static void promiseDone(GstPromise *promise, gpointer data) {
        const auto &task = *static_cast<PromiseData *>(data);
        const auto result = gst_promise_wait(promise);
        const GstStructure *reply = result == GST_PROMISE_RESULT_REPLIED
                                        ? gst_promise_get_reply(promise)
                                        : nullptr;
        if (result != GST_PROMISE_RESULT_REPLIED ||
            (reply && gst_structure_has_field(reply, "error"))) {
            task.context->post({{"event", "error"},
                                {"message", "Negoziazione WebRTC fallita."}});
        } else if (task.action == "created") {
            GstWebRTCSessionDescription *description = nullptr;
            const auto key = task.type.toUtf8();
            if (!reply ||
                !gst_structure_get(reply, key.constData(),
                                   GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                                   &description, nullptr)) {
                task.context->post(
                    {{"event", "error"},
                     {"message", "Descrizione SDP non disponibile."}});
            } else {
                gchar *sdp = gst_sdp_message_as_text(description->sdp);
                task.context->post({{"event", "created"},
                                    {"type", task.type},
                                    {"sdp", QString::fromUtf8(sdp)}});
                g_free(sdp);
                gst_webrtc_session_description_free(description);
            }
        } else {
            task.context->post({{"event", task.action},
                                {"type", task.type},
                                {"sdp", task.sdp}});
        }
        gst_promise_unref(promise);
    }
    static GstPromise *promise(std::shared_ptr<Context> context, QString action,
                               QString type, QString sdp = {}) {
        auto *data = new PromiseData{std::move(context), std::move(action),
                                     std::move(type), std::move(sdp)};
        return gst_promise_new_with_change_func(
            promiseDone, data,
            [](gpointer p) { delete static_cast<PromiseData *>(p); });
    }
    static void release(gpointer data, GClosure *) {
        delete static_cast<std::shared_ptr<Context> *>(data);
    }
};

WebRtcPeer::WebRtcPeer(QObject *parent) : QObject(parent) {
    m_poll.setInterval(8);
    m_poll.setTimerType(Qt::PreciseTimer);
    connect(&m_poll, &QTimer::timeout, this, &WebRtcPeer::poll);
}
WebRtcPeer::~WebRtcPeer() { stop(); }

bool WebRtcPeer::start(bool sender, const IceConfig &ice, QString &error) {
    return start(sender, ice, AudioConfig{}, error);
}

bool WebRtcPeer::start(bool sender, const IceConfig &ice, const AudioConfig &audio, QString &error) {
    stop();
    error.clear();
    GError *initError = nullptr;
    if (!gst_init_check(nullptr, nullptr, &initError)) {
        error = initError ? QString::fromUtf8(initError->message)
                          : tr("GStreamer non disponibile.");
        g_clear_error(&initError);
        return false;
    }
    for (const char *name :
         {"webrtcbin", "nicesrc", "nicesink", "rtpbin", "rtph264pay",
          "rtph264depay", "dtlssrtpenc", "dtlssrtpdec", "appsrc", "appsink",
          "h264parse", "avdec_h264", "videoconvert"}) {
        auto *factory = gst_element_factory_find(name);
        if (!factory) {
            error =
                tr("Plugin WebRTC mancante: %1").arg(QString::fromUtf8(name));
            return false;
        }
        gst_object_unref(factory);
    }
    QString audioSource;
    QString monitorDevice;
    if (audio.enabled && sender) {
        if (audio.testTone) {
            audioSource = QStringLiteral("audiotestsrc is-live=true wave=sine freq=440");
        } else {
#ifdef Q_OS_WIN
            audioSource = QStringLiteral("wasapi2src loopback=true low-latency=true provide-clock=false");
#elif defined(Q_OS_LINUX)
            // Resolve the output's monitor explicitly: never fall back to a microphone.
            QProcess pactl;
            auto runPactl = [&](const QStringList& arguments) {
                pactl.start(QStringLiteral("pactl"), arguments);
                QString detail;
                if (!pactl.waitForStarted(2000)) {
                    detail = tr("Impossibile avviare pactl: %1").arg(pactl.errorString());
                } else if (!pactl.waitForFinished(2000)) {
                    detail = tr("pactl non risponde: %1").arg(pactl.errorString());
                    pactl.kill();
                    pactl.waitForFinished(1000);
                } else if (pactl.exitStatus() != QProcess::NormalExit || pactl.exitCode() != 0) {
                    detail = QString::fromUtf8(pactl.readAllStandardError()).trimmed().left(1000);
                    if (detail.isEmpty()) detail = tr("pactl terminato con errore (codice %1).").arg(pactl.exitCode());
                } else {
                    return true;
                }
                error = tr("Audio di sistema non disponibile.\n%1\nVerifica pactl e PulseAudio/PipeWire nella sessione desktop, oppure disattiva Audio del PC.").arg(detail);
                return false;
            };
            if (!runPactl({QStringLiteral("get-default-sink")})) return false;
            const QString defaultSink = QString::fromUtf8(pactl.readAllStandardOutput()).trimmed();
            if (!runPactl({QStringLiteral("--format=json"), QStringLiteral("list"), QStringLiteral("sinks")})) return false;
            const auto sinks = QJsonDocument::fromJson(pactl.readAllStandardOutput()).array();
            for (const auto& sink : sinks) {
                const auto object = sink.toObject();
                if (object.value("name").toString() == defaultSink) {
                    monitorDevice = object.value("monitor_source").toString();
                    if (monitorDevice.isEmpty()) monitorDevice = object.value("monitor_source_name").toString();
                }
            }
            if (monitorDevice.isEmpty()) {
                error = tr("Monitor audio dell’uscita predefinita non disponibile. Verifica PulseAudio/PipeWire oppure disattiva Audio del PC.");
                return false;
            }
            audioSource = QStringLiteral("pulsesrc name=systemaudio provide-clock=false");
#else
            error = tr("Cattura audio di sistema supportata su Linux e Windows.");
            return false;
#endif
        }
    }
    m_context = std::make_shared<Context>();
    m_sender = sender;
    QString description = sender
        ? QStringLiteral("webrtcbin name=rtc bundle-policy=max-bundle latency=100 "
            "appsrc name=encoded is-live=true format=time block=false max-buffers=4 max-bytes=0 "
            "caps=video/x-h264,stream-format=byte-stream,alignment=au "
            "! rtph264pay pt=96 config-interval=-1 aggregate-mode=zero-latency "
            "! application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000 ! rtc. ")
        : QStringLiteral("webrtcbin name=rtc bundle-policy=max-bundle latency=100 "
            "rtph264depay name=depay ! h264parse ! avdec_h264 max-threads=4 thread-type=slice "
            "! videoconvert n-threads=4 ! video/x-raw,format=RGBA "
            "! appsink name=decoded sync=true async=false max-buffers=1 drop=true "
            "emit-signals=true enable-last-sample=false ");
    if (audio.enabled) {
        description += sender
            ? audioSource + QStringLiteral(" ! audioconvert ! audioresample "
                "! audio/x-raw,rate=48000,channels=2 ! opusenc bitrate=192000 frame-size=20 "
                "! valve name=audiogate drop=true ! rtpopuspay pt=97 ! application/x-rtp,media=audio,encoding-name=OPUS,"
                "payload=97,clock-rate=48000,encoding-params=(string)2 ! rtc. ")
            : QStringLiteral("rtpopusdepay name=audiodepay ! opusdec ! audioconvert ! audioresample "
                "! volume name=playbackvolume ! ") + (audio.discardOutput
                    ? QStringLiteral("fakesink name=audioout sync=true async=false signal-handoffs=true")
#ifdef Q_OS_WIN
                    : QStringLiteral("wasapi2sink name=audioout sync=true async=false provide-clock=false"));
#else
                    : QStringLiteral("pulsesink name=audioout sync=true async=false provide-clock=false"));
#endif
    }
    GError *parseError = nullptr;
    m_pipeline = gst_parse_launch(description.toUtf8().constData(), &parseError);
    if (!m_pipeline || parseError) {
        error = parseError ? QString::fromUtf8(parseError->message)
                           : tr("Pipeline WebRTC non disponibile.");
        g_clear_error(&parseError);
        stop();
        return false;
    }
    if (!monitorDevice.isEmpty()) {
        auto* source = gst_bin_get_by_name(GST_BIN(m_pipeline), "systemaudio");
        g_object_set(source, "device", monitorDevice.toUtf8().constData(), nullptr);
        gst_object_unref(source);
    }
    // A single monotonic clock for capture timestamps, RTP and playback.
    auto* clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(m_pipeline), clock);
    gst_object_unref(clock);
    m_rtc = gst_bin_get_by_name(GST_BIN(m_pipeline), "rtc");
    if (!ice.stun.isEmpty())
        g_object_set(m_rtc, "stun-server", ice.stun.toUtf8().constData(),
                     nullptr);
    if (!ice.turn.isEmpty()) {
        gboolean accepted = FALSE;
        g_signal_emit_by_name(m_rtc, "add-turn-server",
                              ice.turn.toUtf8().constData(), &accepted);
        if (!accepted) {
            error = tr("Indirizzo TURN non valido.");
            stop();
            return false;
        }
    }
    if (ice.relayOnly) {
        if (ice.turn.isEmpty()) {
            error = tr("La modalità relay richiede un server TURN.");
            stop();
            return false;
        }
        g_object_set(m_rtc, "ice-transport-policy",
                     GST_WEBRTC_ICE_TRANSPORT_POLICY_RELAY, nullptr);
    }
    g_signal_connect_data(
        m_rtc, "on-ice-candidate",
        G_CALLBACK(+[](GstElement *, guint line, gchar *candidate,
                       gpointer data) {
            const auto context = *static_cast<std::shared_ptr<Context> *>(data);
            if (!candidate || !*candidate)
                return;
            context->post({{"event", "ice"},
                           {"type", "ice"},
                           {"mline", static_cast<int>(line)},
                           {"candidate", QString::fromUtf8(candidate)}});
        }),
        new std::shared_ptr<Context>(m_context), Context::release,
        GConnectFlags(0));
    if (sender) {
        m_context->source = gst_bin_get_by_name(GST_BIN(m_pipeline), "encoded");
        m_context->audioGate = gst_bin_get_by_name(GST_BIN(m_pipeline), "audiogate");
    } else {
        auto *depay = gst_bin_get_by_name(GST_BIN(m_pipeline), "depay");
        m_context->decoderSink = gst_element_get_static_pad(depay, "sink");
        gst_object_unref(depay);
        if (audio.enabled) {
            auto* audioDepay = gst_bin_get_by_name(GST_BIN(m_pipeline), "audiodepay");
            m_context->audioSink = gst_element_get_static_pad(audioDepay, "sink");
            gst_object_unref(audioDepay);
            setVolume(m_volume);
            auto* output = gst_bin_get_by_name(GST_BIN(m_pipeline), "playbackvolume");
            auto* pad = gst_element_get_static_pad(output, "src");
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                +[](GstPad*, GstPadProbeInfo* info, gpointer data) {
                    const auto context = *static_cast<std::shared_ptr<Context>*>(data);
                    const std::lock_guard lock(context->mutex);
                    ++context->stats.audioBuffers;
                    context->stats.audioPtsNs = static_cast<qint64>(GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info)));
                    return GST_PAD_PROBE_OK;
                }, new std::shared_ptr<Context>(m_context),
                +[](gpointer p) { delete static_cast<std::shared_ptr<Context>*>(p); });
            gst_object_unref(pad);
            gst_object_unref(output);
        }
        g_signal_connect_data(
            m_rtc, "pad-added",
            G_CALLBACK(+[](GstElement *, GstPad *pad, gpointer data) {
                if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC)
                    return;
                const auto context =
                    *static_cast<std::shared_ptr<Context> *>(data);
                auto* caps = gst_pad_get_current_caps(pad);
                if (!caps) caps = gst_pad_query_caps(pad, nullptr);
                const char* media = caps && gst_caps_get_size(caps)
                    ? gst_structure_get_string(gst_caps_get_structure(caps, 0), "media") : nullptr;
                auto* target = g_strcmp0(media, "video") == 0 ? context->decoderSink
                    : g_strcmp0(media, "audio") == 0 ? context->audioSink : nullptr;
                if (caps) gst_caps_unref(caps);
                if (!target || gst_pad_link(pad, target) != GST_PAD_LINK_OK)
                    context->post(
                        {{"event", "error"},
                         {"message",
                          "Impossibile collegare il flusso ricevuto."}});
            }),
            new std::shared_ptr<Context>(m_context), Context::release,
            GConnectFlags(0));
        auto *sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "decoded");
        g_signal_connect_data(
            sink, "new-sample",
            G_CALLBACK(+[](GstAppSink *sink, gpointer data) -> GstFlowReturn {
                const auto context =
                    *static_cast<std::shared_ptr<Context> *>(data);
                GstSample *sample = gst_app_sink_pull_sample(sink);
                if (!sample)
                    return GST_FLOW_EOS;
                const auto pts = GST_BUFFER_PTS(gst_sample_get_buffer(sample));
                auto frame = videoFrameFromGstSample(sample);
                gst_sample_unref(sample);
                if (!frame.isValid()) {
                    context->post({{"event", "error"},
                                   {"message", "Frame WebRTC non leggibile."}});
                    return GST_FLOW_ERROR;
                }
                const std::lock_guard lock(context->mutex);
                if (context->active) {
                    context->latest = std::move(frame);
                    ++context->frames;
                    context->stats.videoPtsNs = static_cast<qint64>(pts);
                }
                return GST_FLOW_OK;
            }),
            new std::shared_ptr<Context>(m_context), Context::release,
            GConnectFlags(0));
        gst_object_unref(sink);
    }
    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        error = tr("Avvio WebRTC fallito. Verifica i plugin ICE/DTLS/RTP.");
        stop();
        return false;
    }
    auto* activeClock = gst_system_clock_obtain();
    m_context->steadyOrigin = std::chrono::steady_clock::now();
    m_context->runningOrigin = gst_clock_get_time(activeClock) - gst_element_get_base_time(m_pipeline);
    gst_object_unref(activeClock);
    m_poll.start();
    if (sender)
        createDescription(true);
    return true;
}

void WebRtcPeer::stop() {
    m_poll.stop();
    if (m_context) {
        const std::lock_guard lock(m_context->mutex);
        m_context->active = false;
        m_context->connected = false;
        m_context->latest = {};
    }
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    if (m_rtc) {
        gst_object_unref(m_rtc);
        m_rtc = nullptr;
    }
    m_context.reset();
    m_pendingIce.clear();
    m_remoteReady = false;
    m_remotePending = false;
    m_reportedConnected = false;
}
void WebRtcPeer::createDescription(bool offer) {
    if (!m_rtc)
        return;
    auto *promise =
        Context::promise(m_context, "created", offer ? "offer" : "answer");
    g_signal_emit_by_name(m_rtc, offer ? "create-offer" : "create-answer",
                          nullptr, promise);
}
void WebRtcPeer::setDescription(bool local, const QString &type,
                                const QString &sdp) {
    GstSDPMessage *parsed = nullptr;
    const auto bytes = sdp.toUtf8();
    if (gst_sdp_message_new(&parsed) != GST_SDP_OK)
        return;
    if (gst_sdp_message_parse_buffer(
            reinterpret_cast<const guint8 *>(bytes.constData()), bytes.size(),
            parsed) != GST_SDP_OK) {
        gst_sdp_message_free(parsed);
        emit errorOccurred(tr("SDP non valido."));
        return;
    }
    auto *description = gst_webrtc_session_description_new(
        type == "offer" ? GST_WEBRTC_SDP_TYPE_OFFER
                        : GST_WEBRTC_SDP_TYPE_ANSWER,
        parsed);
    auto *promise = Context::promise(
        m_context, local ? "local-set" : "remote-set", type, sdp);
    g_signal_emit_by_name(
        m_rtc, local ? "set-local-description" : "set-remote-description",
        description, promise);
    gst_webrtc_session_description_free(description);
}
void WebRtcPeer::receive(const QJsonObject &message) {
    if (!m_rtc)
        return;
    const auto type = message.value("type").toString();
    if (type == "ice") {
        const int line = message.value("mline").toInt(-1);
        if (line < 0 || line > 8 ||
            message.value("candidate").toString().size() > 8192)
            return;
        if (!m_remoteReady) {
            if (m_pendingIce.size() < 256)
                m_pendingIce.append(message);
            return;
        }
        const auto candidate = message.value("candidate").toString().toUtf8();
        g_signal_emit_by_name(m_rtc, "add-ice-candidate",
                              static_cast<guint>(line), candidate.constData());
    } else if ((type == "offer" && !m_sender) ||
               (type == "answer" && m_sender)) {
        if (m_remoteReady || m_remotePending)
            return;
        const auto sdp = message.value("sdp").toString();
        if (sdp.isEmpty() || sdp.size() > 100000)
            return;
        m_remotePending = true;
        setDescription(false, type, sdp);
    }
}
void WebRtcPeer::poll() {
    if (!m_context)
        return;
    // Hold the context locally: a Qt signal may synchronously stop the
    // connection.
    const auto context = m_context;
    std::deque<QJsonObject> events;
    QVideoFrame frame;
    {
        const std::lock_guard lock(context->mutex);
        events.swap(context->events);
        frame = std::exchange(context->latest, {});
    }
    for (auto event : events) {
        if (context != m_context)
            return;
        const auto kind = event.take("event").toString();
        if (kind == "error") {
            emit errorOccurred(event.value("message").toString());
            return;
        }
        if (kind == "created")
            setDescription(true, event.value("type").toString(),
                           event.value("sdp").toString());
        else if (kind == "local-set" || kind == "ice")
            emit signalMessage(event);
        else if (kind == "remote-set") {
            m_remoteReady = true;
            m_remotePending = false;
            const auto candidates = std::exchange(m_pendingIce, {});
            for (const auto &candidate : candidates)
                receive(candidate);
            if (event.value("type") == "offer")
                createDescription(false);
        }
    }
    if (context != m_context)
        return;
    auto *bus = gst_element_get_bus(m_pipeline);
    GstMessage* message = nullptr;
    while ((message = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_LATENCY)))) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) break;
        gst_bin_recalculate_latency(GST_BIN(m_pipeline));
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    if (message) {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        // Avoid leaking TURN credentials from backend diagnostics into the
        // UI/logs.
        const auto text =
            tr("Errore nel trasporto WebRTC (%1).")
                .arg(QString::fromUtf8(GST_OBJECT_NAME(message->src)));
        g_clear_error(&error);
        g_free(debug);
        gst_message_unref(message);
        emit errorOccurred(text);
        return;
    }
    GstWebRTCPeerConnectionState state;
    g_object_get(m_rtc, "connection-state", &state, nullptr);
    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED) {
        emit errorOccurred(
            tr("Connessione WebRTC fallita. Verifica rete e TURN."));
        return;
    }
    {
        const std::lock_guard lock(context->mutex);
        context->connected =
            state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED;
    }
    if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED &&
        !m_reportedConnected) {
        m_reportedConnected = true;
        emit connected();
    }
    if (context != m_context)
        return;
    if (frame.isValid())
        emit frameReady(frame);
}
quint64 WebRtcPeer::receivedFrames() const {
    if (!m_context)
        return 0;
    const std::lock_guard lock(m_context->mutex);
    return m_context->frames;
}
VideoPipeline::EncodedHandler WebRtcPeer::encodedHandler() const {
    const std::weak_ptr<Context> weak = m_context;
    return [weak](GstSample *sample, std::chrono::steady_clock::time_point capturedAt) {
        const auto context = weak.lock();
        if (!context)
            return;
        const std::lock_guard lock(context->mutex);
        if (!context->active || !context->source || !context->connected)
            return;
        GstBuffer *original = gst_sample_get_buffer(sample);
        const bool keyframe =
            !GST_BUFFER_FLAG_IS_SET(original, GST_BUFFER_FLAG_DELTA_UNIT);
        guint64 queued = 0;
        g_object_get(context->source, "current-level-buffers", &queued,
                     nullptr);
        if (queued >= 4) {
            context->needKeyframe = true;
            return;
        }
        if (context->needKeyframe && !keyframe)
            return;
        context->needKeyframe = false;
        if (context->audioGate) g_object_set(context->audioGate, "drop", FALSE, nullptr);
        GstBuffer *buffer = gst_buffer_copy(
            original); // Share encoded memory; change timestamps only.
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            capturedAt - context->steadyOrigin).count();
        GST_BUFFER_PTS(buffer) = context->runningOrigin + static_cast<GstClockTime>(std::max<qint64>(0, elapsed));
        GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
        GstSample *output = gst_sample_new(buffer, gst_sample_get_caps(sample),
                                           nullptr, nullptr);
        gst_buffer_unref(buffer);
        gst_app_src_push_sample(GST_APP_SRC(context->source), output);
        gst_sample_unref(output);
    };
}

void WebRtcPeer::setVolume(double volume) {
    m_volume = std::clamp(volume, 0.0, 1.0);
    if (!m_pipeline) return;
    auto* element = gst_bin_get_by_name(GST_BIN(m_pipeline), "playbackvolume");
    if (element) {
        g_object_set(element, "volume", m_volume, nullptr);
        gst_object_unref(element);
    }
}
WebRtcPeer::MediaStats WebRtcPeer::mediaStats() const {
    if (!m_context) return {};
    const std::lock_guard lock(m_context->mutex);
    return m_context->stats;
}

#include "MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QSslSocket>
#include <QPluginLoader>
#include <QTextStream>
#include <gst/gst.h>

int main(int argc, char* argv[])
{
    // Screen capture requires Qt Multimedia's FFmpeg backend.
    // Keep an explicit environment override available for diagnostics.
    if (qEnvironmentVariableIsEmpty("QT_MEDIA_BACKEND"))
        qputenv("QT_MEDIA_BACKEND", "ffmpeg");

    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("StreamingApp");
    QCoreApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.addOption({"check-runtime", "Verifica TLS e plugin multimediali senza aprire la finestra"});
    parser.addHelpOption(); parser.addVersionOption(); parser.process(app);

#ifdef Q_OS_WIN
    const QDir runtime(QCoreApplication::applicationDirPath());
    if (runtime.exists("gstreamer-1.0")) {
        qputenv("PATH", runtime.absolutePath().toUtf8() + ';' + qgetenv("PATH"));
        qputenv("GST_PLUGIN_PATH_1_0", runtime.filePath("gstreamer-1.0").toUtf8());
        qputenv("GST_PLUGIN_SYSTEM_PATH_1_0", runtime.filePath("gstreamer-1.0").toUtf8());
        qputenv("GST_PLUGIN_SCANNER_1_0", runtime.filePath("libexec/gstreamer-1.0/gst-plugin-scanner.exe").toUtf8());
        if (qEnvironmentVariableIsEmpty("GST_REGISTRY_1_0")) {
            const QDir cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
            if (QDir().mkpath(cache.absolutePath()))
                qputenv("GST_REGISTRY_1_0", cache.filePath("gstreamer-registry.bin").toUtf8());
        }
    }
#endif

    // Development builds may keep optional GStreamer plugins in the ignored
    // .local directory. Use them automatically when no explicit path exists.
    if (qEnvironmentVariableIsEmpty("GST_PLUGIN_PATH_1_0")) {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QStringList candidates{
            QDir::current().filePath(QStringLiteral(".local/gstreamer-1.0")),
            QDir(appDir).filePath(QStringLiteral(".local/gstreamer-1.0")),
            QDir(appDir).filePath(QStringLiteral("../.local/gstreamer-1.0"))};
        for (const auto& candidate : candidates) {
            if (QFileInfo(candidate).isDir()) {
                qputenv("GST_PLUGIN_PATH_1_0", QDir::cleanPath(candidate).toUtf8());
                break;
            }
        }
    }

    if (parser.isSet("check-runtime")) {
        QTextStream errors(stderr);
        if (!QSslSocket::supportsSsl()) {
            errors << "TLS non disponibile nel pacchetto.\n";
            return 1;
        }
#ifdef Q_OS_WIN
        QPluginLoader multimedia(runtime.filePath("multimedia/ffmpegmediaplugin.dll"));
        if (!multimedia.load()) {
            errors << "Backend Qt FFmpeg non disponibile: " << multimedia.errorString() << '\n';
            return 1;
        }
#endif
        GError* error = nullptr;
        if (!gst_init_check(nullptr, nullptr, &error)) {
            errors << "GStreamer: " << (error ? error->message : "inizializzazione fallita") << '\n';
            g_clear_error(&error);
            return 1;
        }
        const char* elements[] = {
            "appsrc", "appsink", "videoconvert", "x264enc", "h264parse", "avdec_h264",
            "webrtcbin", "nicesrc", "nicesink", "rtpbin", "rtph264pay", "rtph264depay",
            "dtlssrtpenc", "dtlssrtpdec", "srtpenc", "srtpdec", "opusenc", "opusdec",
            "rtpopuspay", "rtpopusdepay", "audioconvert", "audioresample", "volume", "valve",
#ifdef Q_OS_WIN
            "wasapi2src", "wasapi2sink"
#elif defined(Q_OS_LINUX)
            "pulsesrc", "pulsesink"
#endif
        };
        bool complete = true;
        for (const auto* name : elements) {
            auto* element = gst_element_factory_make(name, nullptr);
            if (element) gst_object_unref(element);
            else { errors << "Plugin non caricabile: " << name << '\n'; complete = false; }
        }
        if (!complete) return 1;
        QTextStream(stdout) << "Runtime TLS e GStreamer disponibile.\n";
        return 0;
    }

    MainWindow window;
    window.show();
    return app.exec();
}

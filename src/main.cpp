#include "MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QCommandLineParser>

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
    parser.addHelpOption(); parser.addVersionOption(); parser.process(app);

#ifdef Q_OS_WIN
    const QDir runtime(QCoreApplication::applicationDirPath());
    if (runtime.exists("gstreamer-1.0")) {
        qputenv("PATH", runtime.absolutePath().toUtf8() + ';' + qgetenv("PATH"));
        qputenv("GST_PLUGIN_PATH_1_0", runtime.filePath("gstreamer-1.0").toUtf8());
        qputenv("GST_PLUGIN_SYSTEM_PATH_1_0", runtime.filePath("gstreamer-1.0").toUtf8());
        qputenv("GST_PLUGIN_SCANNER_1_0", runtime.filePath("libexec/gstreamer-1.0/gst-plugin-scanner.exe").toUtf8());
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

    MainWindow window;
    window.show();
    return app.exec();
}

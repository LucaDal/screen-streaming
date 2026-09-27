#include "MainWindow.h"
#include <QApplication>
#include <QKeyEvent>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QMouseEvent>
#include <QStackedWidget>
#include <QVideoWidget>
#include <QWindow>
#include <iostream>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir config;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
    if (app.arguments().contains("--settings")) {
        const QString key = QStringLiteral("test-key-for-local-settings-only");
        {
            MainWindow settingsWindow;
            auto* edit = settingsWindow.findChild<QLineEdit*>("serverKeyEdit");
            auto* remember = settingsWindow.findChild<QCheckBox*>("rememberServerKey");
            auto* save = settingsWindow.findChild<QPushButton*>("saveSettingsButton");
            if (!edit || !remember || !save || remember->isChecked()) return 1;
            edit->setText(key); save->click();
            QSettings stored("StreamingApp", "StreamingApp");
            if (stored.contains("serverKey")) return 1;
            remember->setChecked(true); save->click();
        }
        {
            MainWindow restored;
            if (restored.findChild<QLineEdit*>("serverKeyEdit")->text() != key) return 1;
            restored.findChild<QCheckBox*>("rememberServerKey")->setChecked(false);
            QSettings stored("StreamingApp", "StreamingApp");
            if (stored.contains("serverKey")) return 1;
        }
        {
            MainWindow forgotten;
            if (!forgotten.findChild<QLineEdit*>("serverKeyEdit")->text().isEmpty()) return 1;
        }
        std::cout << "PASS: key storage opt-in, save, reload and immediate removal\n";
        return 0;
    }
    MainWindow window;
    window.show(); window.activateWindow(); app.processEvents();
    if (app.arguments().contains("--screenshot")) window.grab().save("/tmp/streaming-app-ui.png");
    auto* video = window.findChild<QVideoWidget*>();
    auto* stack = window.findChild<QStackedWidget*>();
    if (!video || !stack) return 1;
    stack->setCurrentWidget(video); app.processEvents();
    QWindow* surface = nullptr;
    for (auto* candidate : QApplication::allWindows()) {
        if (candidate->metaObject()->className() == QByteArray("QVideoWindow")) surface = candidate;
    }
    if (!surface) { std::cerr << "Missing native video surface\n"; return 1; }
    auto doubleClick = [&] {
        const QPointF local = video->rect().center();
        const QPointF global = video->mapToGlobal(local.toPoint());
        QMouseEvent event(QEvent::MouseButtonDblClick, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &event); app.processEvents();
    };
    doubleClick();
    if (!video->isFullScreen()) { std::cerr << "Double click did not enter fullscreen\n"; return 1; }
    video->activateWindow(); app.processEvents();
    doubleClick();
    if (video->isFullScreen()) { std::cerr << "Double click did not exit fullscreen\n"; return 1; }
    window.activateWindow(); app.processEvents(); doubleClick();
    video->activateWindow(); app.processEvents();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(surface, &escape); app.processEvents();
    if (video->isFullScreen()) return 1;
    std::cout << "PASS: native video window double click enters/exits fullscreen, Esc exits\n";
}

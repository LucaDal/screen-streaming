#pragma once

#include "ScreenCaptureController.h"
#include "RoomClient.h"

#include <QMainWindow>
#include <QList>
#include <QPointer>

#include <memory>

class QScreen;
class QStackedWidget;
class QSlider;
class QComboBox;
class QCheckBox;
class QEvent;
class QLineEdit;
class QLabel;
class QPushButton;
class QSpinBox;
class QTimer;
class QVideoWidget;

class MainWindow final : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    bool saveSettings();
    void refreshScreens();
    void updateState(ScreenCaptureController::State state);
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showFullscreenControl();

    QComboBox* m_screenSelector;
    QCheckBox* m_shareAudio;
    QSlider* m_volume;
    QStackedWidget* m_videoArea;
    QList<QPointer<QScreen>> m_screens;
    QComboBox* m_fpsSelector;
    QComboBox* m_resolutionSelector;
    QSpinBox* m_bitrate;
    QPushButton* m_stopButton;
    QLabel* m_statusLabel;
    QLabel* m_errorLabel;
    QLabel* m_statisticsLabel;
    QLabel* m_timingsLabel;
    QVideoWidget* m_remoteWidget;
    QLineEdit* m_signalingUrl;
    QLineEdit* m_roomName;
    QLineEdit* m_roomToken;
    QCheckBox* m_rememberServerKey;
    QLineEdit* m_stunServer;
    QLineEdit* m_turnServer;
    QCheckBox* m_relayOnly;
    QPushButton* m_joinRoomButton;
    QPushButton* m_createRoomButton;
    QPushButton* m_copyInviteButton;
    QPushButton* m_leaveRoomButton;
    QPushButton* m_shareButton;
    QPushButton* m_fullscreenExitButton;
    QLabel* m_roomStatusLabel;
    QTimer* m_fullscreenHideTimer;
    // Destroy the controller before QMainWindow destroys its child widgets.
    std::unique_ptr<ScreenCaptureController> m_controller;
    std::unique_ptr<RoomClient> m_room;
};
